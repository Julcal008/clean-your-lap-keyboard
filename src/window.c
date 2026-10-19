/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Main window and user interface.
 *
 * Owns the whole interaction: it lists the detected keyboards, remembers the
 * chosen one, asks device.c which companion hotkey devices belong to it, drives
 * the countdown and cleaning states, and tells helper.c when to grab or release.
 * The window itself never touches /dev/input; only the privileged helper does.
 */

#include "window.h"

#include "cylk-config.h"
#include "config.h"
#include "device.h"
#include "helper.h"
#include "session.h"

#include <glib-unix.h>
#include <glib/gi18n.h>
#include <math.h>
#include <signal.h>
#include <string.h>

#define HOLD_ESCAPE_MS 2000

struct _CylkWindow {
  AdwApplicationWindow parent_instance;
};

typedef struct {
  CylkConfig *config;
  GPtrArray *devices;
  GPtrArray *companions;
  CylkDevice *selected;
  CylkHelper *helper;
  CylkSession *session;

  GtkWidget *stack;
  GtkWidget *toast_overlay;
  GtkWidget *idle_page;
  GtkWidget *countdown_page;
  GtkWidget *cleaning_page;
  GtkWidget *done_page;

  GtkWidget *device_button;
  GtkWidget *device_name;
  GtkWidget *device_type;
  GtkWidget *companion_note;
  GtkWidget *device_popover;
  GtkWidget *device_list;
  GtkWidget *start_button;

  GtkWidget *countdown_number;
  GtkWidget *countdown_subtitle;

  GtkWidget *ring;
  GtkWidget *ring_time;
  GtkWidget *stop_button;

  GtkWidget *done_icon;
  GtkWidget *done_title;
  GtkWidget *done_subtitle;

  guint countdown_source;
  guint tick_source;
  guint stop_timeout_source;
  guint sigterm_source;
  guint sigint_source;

  gint countdown_value;
  gint duration;
  gint64 clean_start_us;

  gboolean grabbed;
  gboolean release_sent;
  gboolean waiting_ready;
  gboolean closing;
  gboolean demo_cleaning;
  gboolean completed;

  gchar *demo_mode;
  gchar *device_override;
} CylkWindowPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(CylkWindow, cylk_window, ADW_TYPE_APPLICATION_WINDOW)

static void on_duration_toggled(GtkToggleButton *button, gpointer user_data);
static void on_start_clicked(GtkButton *button, gpointer user_data);
static void on_cancel_clicked(GtkButton *button, gpointer user_data);
static void on_stop_clicked(GtkButton *button, gpointer user_data);
static void on_done_clicked(GtkButton *button, gpointer user_data);
static void on_device_row_activated(GtkListBox *list, GtkListBoxRow *row,
                                    gpointer user_data);
static void on_device_popover_show(GtkWidget *widget, gpointer user_data);
static gboolean on_countdown_tick(gpointer user_data);
static gboolean on_cleaning_tick(gpointer user_data);
static gboolean on_stop_timeout(gpointer user_data);
static gboolean on_sigterm(gpointer user_data);
static gboolean on_sigint(gpointer user_data);
static gboolean on_key_pressed(GtkEventControllerKey *controller,
                               guint keyval, guint keycode,
                               GdkModifierType state, gpointer user_data);
static void on_style_dark_changed(GObject *object, GParamSpec *pspec,
                                  gpointer user_data);
static gboolean on_close_request(GtkWindow *window, gpointer user_data);
static void on_helper_ready(CylkHelper *helper, gpointer user_data);
static void on_helper_grabbed(CylkHelper *helper, const gchar *name,
                              gpointer user_data);
static void on_helper_released(CylkHelper *helper, const gchar *reason,
                               gpointer user_data);
static void on_helper_error(CylkHelper *helper, const gchar *message,
                            gpointer user_data);
static void on_helper_exited(CylkHelper *helper, gint status,
                             gpointer user_data);
static void on_session_interrupted(CylkSession *session, const gchar *reason,
                                   gpointer user_data);

static void update_device_card(CylkWindow *self);
static void update_companions(CylkWindow *self);
static void rebuild_device_list(CylkWindow *self);
static void cancel_countdown(CylkWindow *self);
static void reset_to_idle(CylkWindow *self);
static void stop_cleaning(CylkWindow *self);
static void show_done(CylkWindow *self, const gchar *title,
                      const gchar *subtitle, gboolean success);
static void show_toast(CylkWindow *self, const gchar *message);
static void start_tick(CylkWindow *self);
static void start_demo(CylkWindow *self);
static void begin_countdown(CylkWindow *self);
static void begin_grab(CylkWindow *self);

static gint
normalize_duration(gint duration)
{
  if (duration == 30 || duration == 60 || duration == 120)
    return duration;
  return 60;
}

static gchar *
format_remaining(gint remaining)
{
  if (remaining < 0)
    remaining = 0;
  return g_strdup_printf("%d:%02d", remaining / 60, remaining % 60);
}

static GtkWidget *
make_label(const gchar *text, const gchar *css_class)
{
  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
  gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
  if (css_class != NULL)
    gtk_widget_add_css_class(label, css_class);
  return label;
}

static GtkWidget *
make_icon(const gchar *icon_name, gint size)
{
  GtkWidget *image = gtk_image_new_from_icon_name(icon_name);
  gtk_image_set_pixel_size(GTK_IMAGE(image), size);
  return image;
}

static GtkWidget *
make_page(void)
{
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(page, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand(page, TRUE);
  gtk_widget_set_margin_start(page, 20);
  gtk_widget_set_margin_end(page, 20);
  gtk_widget_set_margin_top(page, 8);
  gtk_widget_set_margin_bottom(page, 16);
  return page;
}

static GtkWidget *
make_icon_label(const gchar *icon_name, const gchar *text)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *icon = make_icon(icon_name, 13);
  GtkWidget *label = make_label(text, "cylk-footnote");

  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 34);
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), label);
  return box;
}

static GtkWidget *
make_device_popover(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  GtkWidget *popover = gtk_popover_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *title = make_label(_("Keyboard to disable"), "cylk-popover-title");
  GtkWidget *list = gtk_list_box_new();

  gtk_widget_add_css_class(popover, "cylk-popover");
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_size_request(popover, 310, -1);

  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_widget_add_css_class(list, "cylk-device-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);

  gtk_box_append(GTK_BOX(box), title);
  gtk_box_append(GTK_BOX(box), list);
  gtk_popover_set_child(GTK_POPOVER(popover), box);

  priv->device_list = list;
  priv->device_popover = popover;

  g_signal_connect(list, "row-activated",
                   G_CALLBACK(on_device_row_activated), self);
  g_signal_connect(popover, "map",
                   G_CALLBACK(on_device_popover_show), self);

  return popover;
}

static GtkWidget *
make_device_card(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  GtkWidget *button = gtk_menu_button_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
  GtkWidget *icon = make_icon("input-keyboard-symbolic", 24);
  GtkWidget *arrow = make_icon("go-next-symbolic", 14);
  GtkWidget *name;
  GtkWidget *type;
  GtkWidget *popover;

  gtk_widget_add_css_class(button, "cylk-card");
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_halign(button, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand(button, TRUE);
  gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(button), FALSE);

  gtk_widget_set_hexpand(text_box, TRUE);
  gtk_widget_set_halign(text_box, GTK_ALIGN_START);

  name = gtk_label_new(NULL);
  gtk_widget_add_css_class(name, "cylk-card-title");
  gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(name), 28);
  gtk_widget_set_hexpand(name, TRUE);

  type = gtk_label_new(NULL);
  gtk_widget_add_css_class(type, "cylk-card-subtitle");
  gtk_label_set_xalign(GTK_LABEL(type), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(type), PANGO_ELLIPSIZE_END);

  gtk_box_append(GTK_BOX(text_box), name);
  gtk_box_append(GTK_BOX(text_box), type);
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), text_box);
  gtk_box_append(GTK_BOX(box), arrow);
  gtk_menu_button_set_child(GTK_MENU_BUTTON(button), box);

  popover = make_device_popover(self);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);

  priv->device_button = button;
  priv->device_name = name;
  priv->device_type = type;

  return button;
}

static GtkWidget *
make_duration_selector(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *label = make_label(_("Cleaning time"), "cylk-muted");
  GtkWidget *segment = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *first = NULL;
  const gint durations[] = { 30, 60, 120 };
  const gchar *const labels[] = { _("30 s"), _("60 s"), _("2 min") };

  gtk_widget_add_css_class(segment, "linked");
  gtk_widget_add_css_class(segment, "cylk-segmented");
  gtk_widget_set_halign(segment, GTK_ALIGN_CENTER);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);

  for (guint i = 0; i < G_N_ELEMENTS(durations); i++) {
    GtkWidget *button = gtk_toggle_button_new_with_label(labels[i]);
    g_object_set_data(G_OBJECT(button), "cylk-duration",
                      GINT_TO_POINTER(durations[i]));
    if (first == NULL)
      first = button;
    else
      gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(button),
                                  GTK_TOGGLE_BUTTON(first));
    gtk_box_append(GTK_BOX(segment), button);
  }

  for (GtkWidget *child = gtk_widget_get_first_child(segment);
       child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    gint value = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child),
                                                   "cylk-duration"));
    if (value == priv->duration)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(child), TRUE);
  }

  for (GtkWidget *child = gtk_widget_get_first_child(segment);
       child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    g_signal_connect(child, "toggled",
                     G_CALLBACK(on_duration_toggled), self);
  }

  gtk_box_append(GTK_BOX(wrapper), label);
  gtk_box_append(GTK_BOX(wrapper), segment);
  return wrapper;
}

static void
on_duration_toggled(GtkToggleButton *button, gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  if (!gtk_toggle_button_get_active(button))
    return;

  priv->duration = GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(button), "cylk-duration"));
  priv->config->duration = priv->duration;
  cylk_config_save(priv->config);
}

static void
build_idle_page(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  GtkWidget *page = make_page();
  GtkWidget *hero;
  GtkWidget *title;
  GtkWidget *subtitle;
  GtkWidget *device_card;
  GtkWidget *duration;
  GtkWidget *start;
  GtkWidget *notes = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  hero = gtk_image_new_from_resource(
      "/io/github/julcal008/CleanYourLapKeyboard/icons/hero.svg");
  gtk_image_set_pixel_size(GTK_IMAGE(hero), 84);
  gtk_widget_set_halign(hero, GTK_ALIGN_CENTER);

  title = make_label(_("Clean your keyboard"), "cylk-title");
  subtitle = make_label(
      _("Disable the built-in keyboard so you can wipe it safely."),
      "cylk-subtitle");
  gtk_label_set_wrap(GTK_LABEL(subtitle), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(subtitle), 34);

  device_card = make_device_card(self);
  duration = make_duration_selector(self);

  start = gtk_button_new_with_label(_("Start cleaning"));
  gtk_widget_add_css_class(start, "suggested-action");
  gtk_widget_add_css_class(start, "cylk-primary");
  gtk_widget_set_hexpand(start, TRUE);
  g_signal_connect(start, "clicked", G_CALLBACK(on_start_clicked), self);

  gtk_box_append(GTK_BOX(notes),
                 make_icon_label("input-keyboard-symbolic",
                                 _("External keyboards and the touchpad stay active.")));

  priv->companion_note =
      make_icon_label("dialog-information-symbolic",
                      _("Media keys and the power button are blocked too."));
  gtk_widget_set_visible(priv->companion_note, FALSE);
  gtk_box_append(GTK_BOX(notes), priv->companion_note);

  gtk_box_append(GTK_BOX(notes),
                 make_icon_label("dialog-information-symbolic",
                                 _("Hold Esc for 2 seconds to stop.")));

  priv->start_button = start;

  gtk_box_append(GTK_BOX(page), hero);
  gtk_box_append(GTK_BOX(page), title);
  gtk_box_append(GTK_BOX(page), subtitle);
  gtk_box_append(GTK_BOX(page), device_card);
  gtk_box_append(GTK_BOX(page), duration);
  gtk_box_append(GTK_BOX(page), start);
  gtk_box_append(GTK_BOX(page), notes);

  priv->idle_page = page;
  gtk_stack_add_named(GTK_STACK(priv->stack), page, "idle");
}

static void
build_countdown_page(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  GtkWidget *page = make_page();
  GtkWidget *number = make_label("3", "cylk-countdown");
  GtkWidget *title = make_label(_("Get ready…"), "cylk-page-title");
  GtkWidget *subtitle =
      make_label(_("The keyboard will be disabled in a moment."), "cylk-subtitle");
  GtkWidget *cancel = gtk_button_new_with_label(_("Cancel"));

  gtk_widget_add_css_class(cancel, "cylk-secondary");
  gtk_widget_set_halign(cancel, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(cancel, 150, -1);
  gtk_label_set_wrap(GTK_LABEL(subtitle), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(subtitle), 34);

  g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel_clicked), self);

  priv->countdown_number = number;
  priv->countdown_subtitle = subtitle;

  gtk_box_append(GTK_BOX(page), number);
  gtk_box_append(GTK_BOX(page), title);
  gtk_box_append(GTK_BOX(page), subtitle);
  gtk_box_append(GTK_BOX(page), cancel);

  priv->countdown_page = page;
  gtk_stack_add_named(GTK_STACK(priv->stack), page, "countdown");
}

static void
draw_ring(GtkDrawingArea *area,
          cairo_t *cr,
          int width,
          int height,
          gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  gboolean dark = adw_style_manager_get_dark(adw_style_manager_get_default());
  gdouble size = MIN(width, height);
  gdouble center_x = width / 2.0;
  gdouble center_y = height / 2.0;
  gdouble radius = size / 2.0 - 10.0;
  gdouble fraction = 0.0;
  gint remaining = priv->duration;

  (void) area;

  if (priv->grabbed || priv->demo_cleaning) {
    gint64 elapsed = (g_get_monotonic_time() - priv->clean_start_us) /
                     G_USEC_PER_SEC;
    remaining = priv->duration - (gint) elapsed;
    if (remaining < 0)
      remaining = 0;
  }

  if (priv->duration > 0)
    fraction = (gdouble) remaining / (gdouble) priv->duration;

  cairo_set_line_width(cr, 9.0);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  if (dark)
    cairo_set_source_rgba(cr, 0.65, 0.65, 0.70, 0.20);
  else
    cairo_set_source_rgba(cr, 0.45, 0.45, 0.50, 0.14);

  cairo_arc(cr, center_x, center_y, radius, -G_PI / 2.0,
            -G_PI / 2.0 + 2.0 * G_PI);
  cairo_stroke(cr);

  if (fraction > 0.0) {
    if (remaining <= 10) {
      cairo_set_source_rgba(cr, 1.0, 0.58, 0.0, 1.0);
    } else if (dark) {
      cairo_set_source_rgba(cr, 0.25, 0.62, 1.0, 1.0);
    } else {
      cairo_set_source_rgba(cr, 0.04, 0.52, 1.0, 1.0);
    }

    cairo_arc(cr, center_x, center_y, radius, -G_PI / 2.0,
              -G_PI / 2.0 + 2.0 * G_PI * fraction);
    cairo_stroke(cr);
  }
}

static void
build_cleaning_page(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  GtkWidget *page = make_page();
  GtkWidget *overlay = gtk_overlay_new();
  GtkWidget *ring = gtk_drawing_area_new();
  GtkWidget *time = make_label("1:00", "cylk-ring-time");
  GtkWidget *title = make_label(_("Keyboard disabled"), "cylk-page-title");
  GtkWidget *subtitle = make_label(
      _("Wipe the keys gently.\nTouchpad and external keyboards still work."),
      "cylk-subtitle");
  GtkWidget *chip = make_label(_("Cleaning mode active"), "cylk-chip");
  GtkWidget *stop = gtk_button_new_with_label(_("Stop cleaning"));
  GtkWidget *hint = make_label(_("Hold Esc for 2 seconds to stop."),
                               "cylk-footnote");

  gtk_widget_set_size_request(overlay, 176, 176);
  gtk_widget_set_halign(overlay, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(ring, 176, 176);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ring), draw_ring, self, NULL);

  gtk_widget_set_halign(time, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(time, GTK_ALIGN_CENTER);

  gtk_overlay_set_child(GTK_OVERLAY(overlay), ring);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), time);

  gtk_label_set_wrap(GTK_LABEL(subtitle), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(subtitle), 36);
  gtk_widget_set_halign(chip, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(hint, GTK_ALIGN_CENTER);
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(hint), 34);

  gtk_widget_add_css_class(stop, "cylk-secondary");
  gtk_widget_set_size_request(stop, 190, -1);
  gtk_widget_set_halign(stop, GTK_ALIGN_CENTER);
  g_signal_connect(stop, "clicked", G_CALLBACK(on_stop_clicked), self);

  priv->ring = ring;
  priv->ring_time = time;
  priv->stop_button = stop;

  gtk_box_append(GTK_BOX(page), overlay);
  gtk_box_append(GTK_BOX(page), title);
  gtk_box_append(GTK_BOX(page), subtitle);
  gtk_box_append(GTK_BOX(page), chip);
  gtk_box_append(GTK_BOX(page), stop);
  gtk_box_append(GTK_BOX(page), hint);

  priv->cleaning_page = page;
  gtk_stack_add_named(GTK_STACK(priv->stack), page, "cleaning");
}

static void
build_done_page(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  GtkWidget *page = make_page();
  GtkWidget *icon = gtk_image_new_from_resource(
      "/io/github/julcal008/CleanYourLapKeyboard/icons/check.svg");
  GtkWidget *title = make_label(_("All clean"), "cylk-page-title");
  GtkWidget *subtitle = make_label(_("Your keyboard is back."), "cylk-subtitle");
  GtkWidget *done = gtk_button_new_with_label(_("Done"));

  gtk_image_set_pixel_size(GTK_IMAGE(icon), 56);
  gtk_widget_add_css_class(icon, "cylk-done-icon");
  gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(done, "cylk-primary");
  gtk_widget_set_size_request(done, 190, -1);
  gtk_widget_set_halign(done, GTK_ALIGN_CENTER);
  g_signal_connect(done, "clicked", G_CALLBACK(on_done_clicked), self);

  priv->done_icon = icon;
  priv->done_title = title;
  priv->done_subtitle = subtitle;

  gtk_box_append(GTK_BOX(page), icon);
  gtk_box_append(GTK_BOX(page), title);
  gtk_box_append(GTK_BOX(page), subtitle);
  gtk_box_append(GTK_BOX(page), done);

  priv->done_page = page;
  gtk_stack_add_named(GTK_STACK(priv->stack), page, "done");
}

static void
build_ui(CylkWindow *self)
{
  GtkWindow *window = GTK_WINDOW(self);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = adw_header_bar_new();
  GtkWidget *clamp = adw_clamp_new();
  GtkWidget *stack = gtk_stack_new();
  GtkEventController *key_controller;
  AdwStyleManager *style_manager;

  gtk_window_set_title(window, _("Clean Your Lap Keyboard"));
  gtk_window_set_icon_name(window, APP_ID);
  gtk_window_set_default_size(window, 400, 580);
  gtk_window_set_resizable(window, TRUE);
  gtk_widget_set_size_request(GTK_WIDGET(window), 360, 520);
  gtk_widget_add_css_class(GTK_WIDGET(window), "cylk-window");

  gtk_widget_add_css_class(header, "cylk-header");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                  gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), TRUE);
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), TRUE);

  priv->toast_overlay = adw_toast_overlay_new();
  gtk_widget_set_vexpand(priv->toast_overlay, TRUE);

  priv->stack = stack;
  gtk_stack_set_transition_type(GTK_STACK(stack),
                                GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration(GTK_STACK(stack), 180);
  gtk_widget_set_vexpand(stack, TRUE);
  gtk_widget_set_hexpand(stack, TRUE);

  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 340);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 300);
  adw_clamp_set_child(ADW_CLAMP(clamp), stack);
  gtk_widget_set_vexpand(clamp, TRUE);

  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(priv->toast_overlay), clamp);

  build_idle_page(self);
  build_countdown_page(self);
  build_cleaning_page(self);
  build_done_page(self);

  gtk_stack_set_visible_child_name(GTK_STACK(stack), "idle");

  gtk_box_append(GTK_BOX(outer), header);
  gtk_box_append(GTK_BOX(outer), priv->toast_overlay);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), outer);

  key_controller = gtk_event_controller_key_new();
  g_signal_connect(key_controller, "key-pressed",
                   G_CALLBACK(on_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(window), key_controller);

  style_manager = adw_style_manager_get_default();
  g_signal_connect(style_manager, "notify::dark",
                   G_CALLBACK(on_style_dark_changed), self);
  on_style_dark_changed(G_OBJECT(style_manager), NULL, self);

  priv->sigterm_source = g_unix_signal_add(SIGTERM, on_sigterm, self);
  priv->sigint_source = g_unix_signal_add(SIGINT, on_sigint, self);

  g_signal_connect(window, "close-request",
                   G_CALLBACK(on_close_request), self);
}

/*
 * Recompute the companion devices that must be grabbed together with the
 * currently selected keyboard.  Called whenever the selection changes.
 */
static void
update_companions(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  GError *error = NULL;

  g_clear_pointer(&priv->companions, g_ptr_array_unref);

  if (priv->selected == NULL)
    return;

  priv->companions = cylk_device_scan_companions(priv->selected, &error);
  if (priv->companions == NULL) {
    g_debug("companions: %s",
            error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
  }
}

static void
update_device_card(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  gboolean has_companions;

  has_companions = priv->companions != NULL && priv->companions->len > 0;

  if (priv->companion_note != NULL)
    gtk_widget_set_visible(priv->companion_note, has_companions);

  if (priv->selected == NULL) {
    gtk_label_set_text(GTK_LABEL(priv->device_name), _("No keyboard detected"));
    gtk_label_set_text(GTK_LABEL(priv->device_type),
                       _("Connect a keyboard and reopen the app."));
    gtk_widget_set_tooltip_text(priv->device_button, NULL);
    if (priv->start_button != NULL)
      gtk_widget_set_sensitive(priv->start_button, FALSE);
    return;
  }

  gtk_label_set_text(GTK_LABEL(priv->device_name), priv->selected->name);
  gtk_label_set_text(GTK_LABEL(priv->device_type),
                     priv->selected->internal
                         ? _("Internal keyboard")
                         : _("External keyboard"));

  if (has_companions) {
    GString *tip = g_string_new(priv->selected->path);

    for (guint i = 0; i < priv->companions->len; i++) {
      CylkDevice *companion = g_ptr_array_index(priv->companions, i);
      g_string_append_printf(tip, "\n• %s", companion->name);
    }

    gtk_widget_set_tooltip_text(priv->device_button, tip->str);
    g_string_free(tip, TRUE);
  } else {
    gtk_widget_set_tooltip_text(priv->device_button, priv->selected->path);
  }

  if (priv->start_button != NULL)
    gtk_widget_set_sensitive(priv->start_button, TRUE);
}

static void
on_device_row_activated(GtkListBox *list,
                        GtkListBoxRow *row,
                        gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  CylkDevice *device = g_object_get_data(G_OBJECT(row), "cylk-device");

  (void) list;

  if (device == NULL)
    return;

  priv->selected = device;
  cylk_config_set_device(priv->config, cylk_device_id(device));
  cylk_config_save(priv->config);
  update_companions(self);
  update_device_card(self);
  gtk_popover_popdown(GTK_POPOVER(priv->device_popover));
}

static void
on_device_popover_show(GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  rebuild_device_list(CYLK_WINDOW(user_data));
}

static void
rebuild_device_list(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  GtkWidget *child;

  if (priv->device_list == NULL)
    return;

  while ((child = gtk_widget_get_first_child(priv->device_list)) != NULL)
    gtk_list_box_remove(GTK_LIST_BOX(priv->device_list), child);

  if (priv->devices == NULL || priv->devices->len == 0) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *label = make_label(_("No keyboards found."), "cylk-device-meta");
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_append(GTK_LIST_BOX(priv->device_list), row);
    return;
  }

  for (guint i = 0; i < priv->devices->len; i++) {
    CylkDevice *device = g_ptr_array_index(priv->devices, i);
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = make_icon("input-keyboard-symbolic", 20);
    GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *name = gtk_label_new(device->name);
    GtkWidget *meta = gtk_label_new(device->path);
    GtkWidget *badge = gtk_label_new(device->internal ? _("Internal")
                                                      : _("External"));
    GtkWidget *check = make_icon("object-select-symbolic", 16);

    gtk_widget_add_css_class(name, "cylk-device-name");
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 26);
    gtk_widget_add_css_class(meta, "cylk-device-meta");
    gtk_label_set_xalign(GTK_LABEL(meta), 0.0f);
    gtk_widget_add_css_class(badge, "cylk-badge");
    if (!device->internal)
      gtk_widget_add_css_class(badge, "cylk-badge-external");

    gtk_widget_set_hexpand(text_box, TRUE);
    gtk_widget_set_halign(text_box, GTK_ALIGN_START);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(box, 4);
    gtk_widget_set_margin_bottom(box, 4);
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box, 4);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(text_box), name);
    gtk_box_append(GTK_BOX(text_box), meta);
    gtk_box_append(GTK_BOX(box), text_box);
    gtk_box_append(GTK_BOX(box), badge);
    gtk_box_append(GTK_BOX(box), check);

    gtk_widget_set_visible(check, device == priv->selected);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data(G_OBJECT(row), "cylk-device", device);
    gtk_list_box_append(GTK_LIST_BOX(priv->device_list), row);
  }
}

static void
load_devices(CylkWindow *self, const gchar *override)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  GError *error = NULL;
  gboolean found_override = FALSE;

  if (priv->devices != NULL) {
    g_ptr_array_unref(priv->devices);
    priv->devices = NULL;
  }

  priv->devices = cylk_device_scan(&error);
  if (priv->devices == NULL) {
    show_toast(self, error->message);
    g_clear_error(&error);
    priv->selected = NULL;
    g_clear_pointer(&priv->companions, g_ptr_array_unref);
    update_device_card(self);
    rebuild_device_list(self);
    return;
  }

  priv->selected = NULL;

  if (override != NULL) {
    for (guint i = 0; i < priv->devices->len; i++) {
      CylkDevice *device = g_ptr_array_index(priv->devices, i);
      if (g_strcmp0(device->path, override) == 0 ||
          g_strcmp0(cylk_device_id(device), override) == 0) {
        priv->selected = device;
        found_override = TRUE;
        break;
      }
    }
  }

  if (priv->selected == NULL && priv->config->device_id != NULL) {
    for (guint i = 0; i < priv->devices->len; i++) {
      CylkDevice *device = g_ptr_array_index(priv->devices, i);
      if (g_strcmp0(cylk_device_id(device), priv->config->device_id) == 0) {
        priv->selected = device;
        break;
      }
    }
  }

  if (priv->selected == NULL) {
    for (guint i = 0; i < priv->devices->len; i++) {
      CylkDevice *device = g_ptr_array_index(priv->devices, i);
      if (device->internal) {
        priv->selected = device;
        break;
      }
    }
  }

  if (priv->selected == NULL && priv->devices->len > 0)
    priv->selected = g_ptr_array_index(priv->devices, 0);

  if (override != NULL && !found_override)
    show_toast(self, _("The requested keyboard was not found."));

  update_companions(self);
  update_device_card(self);
  rebuild_device_list(self);
}

static void
show_toast(CylkWindow *self, const gchar *message)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  AdwToast *toast;

  if (priv->toast_overlay == NULL)
    return;

  toast = adw_toast_new(message != NULL ? message : _("Something went wrong."));
  adw_toast_set_timeout(toast, 5);
  adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(priv->toast_overlay), toast);
}

static void
cancel_countdown(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  if (priv->countdown_source != 0) {
    g_source_remove(priv->countdown_source);
    priv->countdown_source = 0;
  }

  priv->waiting_ready = FALSE;

  if (priv->helper != NULL) {
    cylk_helper_stop(priv->helper);
    g_clear_object(&priv->helper);
  }

  gtk_stack_set_visible_child_name(GTK_STACK(priv->stack), "idle");
}

static void
reset_to_idle(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  if (priv->countdown_source != 0) {
    g_source_remove(priv->countdown_source);
    priv->countdown_source = 0;
  }
  if (priv->tick_source != 0) {
    g_source_remove(priv->tick_source);
    priv->tick_source = 0;
  }
  if (priv->stop_timeout_source != 0) {
    g_source_remove(priv->stop_timeout_source);
    priv->stop_timeout_source = 0;
  }

  if (priv->helper != NULL) {
    cylk_helper_stop(priv->helper);
    g_clear_object(&priv->helper);
  }

  priv->grabbed = FALSE;
  priv->demo_cleaning = FALSE;
  priv->release_sent = FALSE;
  priv->waiting_ready = FALSE;
  priv->completed = FALSE;

  gtk_button_set_label(GTK_BUTTON(priv->stop_button), _("Stop cleaning"));
  gtk_widget_set_sensitive(priv->stop_button, TRUE);
  gtk_stack_set_visible_child_name(GTK_STACK(priv->stack), "idle");
  update_device_card(self);
}

static void
show_done(CylkWindow *self, const gchar *title,
          const gchar *subtitle, gboolean success)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  gtk_label_set_text(GTK_LABEL(priv->done_title), title);
  gtk_label_set_text(GTK_LABEL(priv->done_subtitle), subtitle);
  if (success) {
    gtk_image_set_from_resource(
        GTK_IMAGE(priv->done_icon),
        "/io/github/julcal008/CleanYourLapKeyboard/icons/check.svg");
  } else {
    gtk_image_set_from_icon_name(GTK_IMAGE(priv->done_icon),
                                 "dialog-information-symbolic");
  }
  if (success) {
    gtk_widget_add_css_class(priv->done_icon, "cylk-done-icon");
    gtk_widget_remove_css_class(priv->done_icon, "cylk-info-icon");
  } else {
    gtk_widget_remove_css_class(priv->done_icon, "cylk-done-icon");
    gtk_widget_add_css_class(priv->done_icon, "cylk-info-icon");
  }
  gtk_stack_set_visible_child_name(GTK_STACK(priv->stack), "done");
}

static void
start_tick(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  if (priv->tick_source != 0)
    g_source_remove(priv->tick_source);
  priv->tick_source = g_timeout_add(200, on_cleaning_tick, self);
}

static void
start_demo(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  gchar *text;

  priv->demo_cleaning = TRUE;
  priv->duration = normalize_duration(priv->duration);
  priv->clean_start_us = g_get_monotonic_time();
  priv->release_sent = FALSE;
  priv->completed = FALSE;

  text = format_remaining(priv->duration);
  gtk_label_set_text(GTK_LABEL(priv->ring_time), text);
  g_free(text);

  gtk_button_set_label(GTK_BUTTON(priv->stop_button), _("Stop cleaning"));
  gtk_widget_set_sensitive(priv->stop_button, TRUE);
  gtk_stack_set_visible_child_name(GTK_STACK(priv->stack), "cleaning");
  start_tick(self);
}

static void
stop_cleaning(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  if (priv->demo_cleaning) {
    priv->demo_cleaning = FALSE;
    if (priv->tick_source != 0) {
      g_source_remove(priv->tick_source);
      priv->tick_source = 0;
    }
    show_done(self, _("Cleaning stopped"), _("Your keyboard is back."), FALSE);
    return;
  }

  if (!priv->grabbed || priv->release_sent)
    return;

  priv->release_sent = TRUE;
  gtk_button_set_label(GTK_BUTTON(priv->stop_button), _("Stopping…"));
  gtk_widget_set_sensitive(priv->stop_button, FALSE);

  if (priv->helper != NULL)
    cylk_helper_release(priv->helper);

  if (priv->stop_timeout_source != 0)
    g_source_remove(priv->stop_timeout_source);
  priv->stop_timeout_source = g_timeout_add(2500, on_stop_timeout, self);
}

static gboolean
on_stop_timeout(gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  priv->stop_timeout_source = 0;

  if (priv->grabbed) {
    priv->grabbed = FALSE;
    if (priv->tick_source != 0) {
      g_source_remove(priv->tick_source);
      priv->tick_source = 0;
    }
    if (priv->helper != NULL)
      cylk_helper_stop(priv->helper);
    show_done(self, _("Keyboard restored"), _("Your keyboard is back."), FALSE);
  }

  return G_SOURCE_REMOVE;
}

static void
begin_countdown(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  if (priv->countdown_source != 0)
    g_source_remove(priv->countdown_source);

  priv->countdown_value = 3;
  gtk_label_set_text(GTK_LABEL(priv->countdown_number), "3");
  gtk_label_set_text(GTK_LABEL(priv->countdown_subtitle),
                     _("The keyboard will be disabled in a moment."));
  priv->countdown_source = g_timeout_add(1000, on_countdown_tick, self);
}

static void
begin_grab(CylkWindow *self)
{
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  GPtrArray *paths;

  if (priv->helper == NULL || priv->selected == NULL) {
    show_toast(self, _("No keyboard detected."));
    reset_to_idle(self);
    return;
  }

  paths = g_ptr_array_new();
  if (priv->companions != NULL) {
    for (guint i = 0; i < priv->companions->len; i++) {
      CylkDevice *companion = g_ptr_array_index(priv->companions, i);
      g_ptr_array_add(paths, companion->path);
    }
  }

  cylk_helper_grab(priv->helper, priv->selected->path, paths,
                   HOLD_ESCAPE_MS, priv->duration + 5);
  g_ptr_array_unref(paths);
}

static gboolean
on_countdown_tick(gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  if (priv->closing) {
    priv->countdown_source = 0;
    return G_SOURCE_REMOVE;
  }

  priv->countdown_value--;

  if (priv->countdown_value > 0) {
    gchar *text = g_strdup_printf("%d", priv->countdown_value);
    gtk_label_set_text(GTK_LABEL(priv->countdown_number), text);
    g_free(text);
    return G_SOURCE_CONTINUE;
  }

  priv->countdown_source = 0;
  gtk_label_set_text(GTK_LABEL(priv->countdown_number), "…");
  gtk_label_set_text(GTK_LABEL(priv->countdown_subtitle), _("Starting…"));
  begin_grab(self);
  return G_SOURCE_REMOVE;
}

static gboolean
on_cleaning_tick(gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  gint64 elapsed;
  gint remaining;
  gchar *text;

  if (priv->closing || (!priv->grabbed && !priv->demo_cleaning)) {
    priv->tick_source = 0;
    return G_SOURCE_REMOVE;
  }

  elapsed = (g_get_monotonic_time() - priv->clean_start_us) / G_USEC_PER_SEC;
  remaining = priv->duration - (gint) elapsed;
  if (remaining < 0)
    remaining = 0;

  text = format_remaining(remaining);
  gtk_label_set_text(GTK_LABEL(priv->ring_time), text);
  g_free(text);
  gtk_widget_queue_draw(priv->ring);

  if (remaining == 0) {
    if (priv->demo_cleaning) {
      priv->demo_cleaning = FALSE;
      priv->tick_source = 0;
      show_done(self, _("All clean"), _("Your keyboard is back."), TRUE);
      return G_SOURCE_REMOVE;
    }
    if (!priv->release_sent) {
      priv->completed = TRUE;
      priv->tick_source = 0;
      stop_cleaning(self);
      return G_SOURCE_REMOVE;
    }
  }

  return G_SOURCE_CONTINUE;
}

static void
on_start_clicked(GtkButton *button, gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  GError *error = NULL;

  (void) button;

  if (priv->selected == NULL) {
    show_toast(self, _("No keyboard detected."));
    return;
  }

  if (priv->demo_mode != NULL) {
    start_demo(self);
    return;
  }

  if (priv->helper != NULL) {
    cylk_helper_stop(priv->helper);
    g_clear_object(&priv->helper);
  }

  priv->helper = cylk_helper_new();
  g_signal_connect(priv->helper, "ready", G_CALLBACK(on_helper_ready), self);
  g_signal_connect(priv->helper, "grabbed", G_CALLBACK(on_helper_grabbed), self);
  g_signal_connect(priv->helper, "released", G_CALLBACK(on_helper_released), self);
  g_signal_connect(priv->helper, "error", G_CALLBACK(on_helper_error), self);
  g_signal_connect(priv->helper, "exited", G_CALLBACK(on_helper_exited), self);

  gtk_label_set_text(GTK_LABEL(priv->countdown_number), "…");
  gtk_label_set_text(GTK_LABEL(priv->countdown_subtitle),
                     _("Waiting for permission…"));
  priv->waiting_ready = TRUE;
  gtk_stack_set_visible_child_name(GTK_STACK(priv->stack), "countdown");

  if (!cylk_helper_start(priv->helper, &error)) {
    show_toast(self, error->message);
    g_clear_error(&error);
    g_clear_object(&priv->helper);
    priv->waiting_ready = FALSE;
    gtk_stack_set_visible_child_name(GTK_STACK(priv->stack), "idle");
  }
}

static void
on_cancel_clicked(GtkButton *button, gpointer user_data)
{
  (void) button;
  cancel_countdown(CYLK_WINDOW(user_data));
}

static void
on_stop_clicked(GtkButton *button, gpointer user_data)
{
  (void) button;
  stop_cleaning(CYLK_WINDOW(user_data));
}

static void
on_done_clicked(GtkButton *button, gpointer user_data)
{
  (void) button;
  reset_to_idle(CYLK_WINDOW(user_data));
}

static void
on_helper_ready(CylkHelper *helper, gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  if (helper != priv->helper || priv->closing || !priv->waiting_ready)
    return;

  if (priv->companions != NULL && priv->companions->len > 0 &&
      cylk_helper_protocol_version(helper) < 2) {
    show_toast(self, _("The helper is outdated. Reinstall the application."));
    cancel_countdown(self);
    return;
  }

  priv->waiting_ready = FALSE;
  begin_countdown(self);
}

static void
on_helper_grabbed(CylkHelper *helper, const gchar *name, gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  (void) name;

  if (helper != priv->helper || priv->closing)
    return;

  priv->grabbed = TRUE;
  priv->demo_cleaning = FALSE;
  priv->release_sent = FALSE;
  priv->completed = FALSE;
  priv->clean_start_us = g_get_monotonic_time();

  gtk_button_set_label(GTK_BUTTON(priv->stop_button), _("Stop cleaning"));
  gtk_widget_set_sensitive(priv->stop_button, TRUE);
  gtk_stack_set_visible_child_name(GTK_STACK(priv->stack), "cleaning");
  start_tick(self);
}

static void
on_helper_released(CylkHelper *helper, const gchar *reason, gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  const gchar *title;
  gboolean success;

  if (helper != priv->helper || priv->closing)
    return;

  if (!priv->grabbed && !priv->demo_cleaning)
    return;

  priv->grabbed = FALSE;
  priv->demo_cleaning = FALSE;

  if (priv->tick_source != 0) {
    g_source_remove(priv->tick_source);
    priv->tick_source = 0;
  }
  if (priv->stop_timeout_source != 0) {
    g_source_remove(priv->stop_timeout_source);
    priv->stop_timeout_source = 0;
  }

  success = g_strcmp0(reason, "timeout") == 0 ||
            (priv->completed && g_strcmp0(reason, "user") == 0);
  priv->completed = FALSE;
  if (success)
    title = _("All clean");
  else if (g_strcmp0(reason, "user") == 0 || g_strcmp0(reason, "escape") == 0)
    title = _("Cleaning stopped");
  else
    title = _("Keyboard restored");

  show_done(self, title, _("Your keyboard is back."), success);
  cylk_helper_stop(priv->helper);
}

static void
on_helper_error(CylkHelper *helper, const gchar *message, gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  if (helper != priv->helper || priv->closing)
    return;

  show_toast(self, message);

  if (!priv->grabbed) {
    if (priv->countdown_source != 0) {
      g_source_remove(priv->countdown_source);
      priv->countdown_source = 0;
    }
    priv->waiting_ready = FALSE;
    gtk_stack_set_visible_child_name(GTK_STACK(priv->stack), "idle");
  }
}

static void
on_helper_exited(CylkHelper *helper, gint status, gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  (void) status;

  if (helper != priv->helper)
    return;

  if (priv->closing) {
    g_clear_object(&priv->helper);
    return;
  }

  if (priv->grabbed) {
    priv->grabbed = FALSE;
    if (priv->tick_source != 0) {
      g_source_remove(priv->tick_source);
      priv->tick_source = 0;
    }
    if (priv->stop_timeout_source != 0) {
      g_source_remove(priv->stop_timeout_source);
      priv->stop_timeout_source = 0;
    }
    show_done(self, _("Keyboard restored"), _("Your keyboard is back."), FALSE);
  } else if (priv->waiting_ready ||
             g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(priv->stack)),
                       "countdown") == 0) {
    if (priv->countdown_source != 0) {
      g_source_remove(priv->countdown_source);
      priv->countdown_source = 0;
    }
    priv->waiting_ready = FALSE;
    gtk_stack_set_visible_child_name(GTK_STACK(priv->stack), "idle");
  }

  g_clear_object(&priv->helper);
}

static void
on_session_interrupted(CylkSession *session, const gchar *reason,
                       gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  const gchar *message;

  (void) session;

  if (priv->closing || priv->demo_mode != NULL)
    return;

  if (!priv->grabbed &&
      g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(priv->stack)),
                "countdown") != 0)
    return;

  if (g_strcmp0(reason, "locked") == 0)
    message = _("Cleaning stopped because the screen was locked.");
  else if (g_strcmp0(reason, "sleep") == 0)
    message = _("Cleaning stopped before the computer went to sleep.");
  else
    message = _("Cleaning stopped because the session became inactive.");

  show_toast(self, message);

  if (priv->grabbed)
    stop_cleaning(self);
  else
    cancel_countdown(self);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller,
               guint keyval,
               guint keycode,
               GdkModifierType state,
               gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  (void) controller;
  (void) keycode;
  (void) state;

  if (keyval == GDK_KEY_Escape &&
      g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(priv->stack)),
                "countdown") == 0) {
    cancel_countdown(self);
    return TRUE;
  }

  return FALSE;
}

static void
on_style_dark_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);
  gboolean dark = adw_style_manager_get_dark(ADW_STYLE_MANAGER(object));

  (void) pspec;

  if (dark)
    gtk_widget_add_css_class(GTK_WIDGET(self), "cylk-dark");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(self), "cylk-dark");

  if (priv->ring != NULL)
    gtk_widget_queue_draw(priv->ring);
}

static gboolean
on_close_request(GtkWindow *window, gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  (void) window;

  priv->closing = TRUE;
  if (priv->helper != NULL)
    cylk_helper_stop(priv->helper);

  return FALSE;
}

static gboolean
on_sigterm(gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  priv->sigterm_source = 0;
  gtk_window_close(GTK_WINDOW(self));
  return G_SOURCE_REMOVE;
}

static gboolean
on_sigint(gpointer user_data)
{
  CylkWindow *self = CYLK_WINDOW(user_data);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  priv->sigint_source = 0;
  gtk_window_close(GTK_WINDOW(self));
  return G_SOURCE_REMOVE;
}

CylkWindow *
cylk_window_new(AdwApplication *app,
                const gchar *demo_mode,
                const gchar *device_override,
                gint duration_override)
{
  CylkWindow *self;
  CylkWindowPrivate *priv;

  g_return_val_if_fail(ADW_IS_APPLICATION(app), NULL);

  self = g_object_new(CYLK_TYPE_WINDOW, "application", app, NULL);
  priv = cylk_window_get_instance_private(self);

  priv->config = cylk_config_load();
  priv->duration = priv->config->duration;
  if (duration_override > 0)
    priv->duration = normalize_duration(duration_override);
  priv->demo_mode = g_strdup(demo_mode);
  priv->device_override = g_strdup(device_override);

  build_ui(self);
  load_devices(self, priv->device_override);

  priv->session = cylk_session_new();
  g_signal_connect(priv->session, "interrupted",
                   G_CALLBACK(on_session_interrupted), self);

  if (g_strcmp0(priv->demo_mode, "countdown") == 0) {
    gtk_label_set_text(GTK_LABEL(priv->countdown_number), "3");
    gtk_label_set_text(GTK_LABEL(priv->countdown_subtitle),
                       _("The keyboard will be disabled in a moment."));
    gtk_stack_set_visible_child_name(GTK_STACK(priv->stack), "countdown");
  } else if (g_strcmp0(priv->demo_mode, "cleaning") == 0) {
    start_demo(self);
  } else if (g_strcmp0(priv->demo_mode, "done") == 0) {
    show_done(self, _("All clean"), _("Your keyboard is back."), TRUE);
  }

  return self;
}

static void
cylk_window_dispose(GObject *object)
{
  CylkWindow *self = CYLK_WINDOW(object);
  CylkWindowPrivate *priv = cylk_window_get_instance_private(self);

  priv->closing = TRUE;

  if (priv->sigterm_source != 0)
    g_source_remove(priv->sigterm_source);
  if (priv->sigint_source != 0)
    g_source_remove(priv->sigint_source);
  if (priv->countdown_source != 0)
    g_source_remove(priv->countdown_source);
  if (priv->tick_source != 0)
    g_source_remove(priv->tick_source);
  if (priv->stop_timeout_source != 0)
    g_source_remove(priv->stop_timeout_source);

  g_signal_handlers_disconnect_by_data(adw_style_manager_get_default(), self);

  if (priv->helper != NULL) {
    cylk_helper_stop(priv->helper);
    g_clear_object(&priv->helper);
  }

  g_clear_object(&priv->session);
  g_clear_pointer(&priv->devices, g_ptr_array_unref);
  g_clear_pointer(&priv->companions, g_ptr_array_unref);
  g_clear_pointer(&priv->config, cylk_config_free);
  g_free(priv->demo_mode);
  g_free(priv->device_override);
  priv->demo_mode = NULL;
  priv->device_override = NULL;

  G_OBJECT_CLASS(cylk_window_parent_class)->dispose(object);
}

static void
cylk_window_class_init(CylkWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = cylk_window_dispose;
}

static void
cylk_window_init(CylkWindow *self)
{
  (void) self;
}
