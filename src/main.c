/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Application entry point.
 *
 * Parses the command line options (--list, --list-all, --device, --duration,
 * --demo, --quit-after), sets up gettext, creates the libadwaita application
 * and opens the main window.  This file only wires things together; the actual
 * UI lives in window.c.
 */

#include "cylk-config.h"

#include "device.h"
#include "window.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <locale.h>
#include <stdlib.h>

static gboolean opt_list = FALSE;
static gboolean opt_list_all = FALSE;
static gchar *opt_device = NULL;
static gint opt_duration = 0;
static gint opt_quit_after = 0;
static gchar *opt_demo = NULL;
static gboolean opt_demo_set = FALSE;

static gboolean
parse_demo_option(const gchar *option_name,
                  const gchar *value,
                  gpointer data,
                  GError **error)
{
  (void) option_name;
  (void) data;

  if (value == NULL || *value == '\0')
    value = "idle";

  if (g_strcmp0(value, "idle") != 0 &&
      g_strcmp0(value, "countdown") != 0 &&
      g_strcmp0(value, "cleaning") != 0 &&
      g_strcmp0(value, "done") != 0) {
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                _("Unknown demo state “%s”. Use idle, countdown, cleaning or done."),
                value);
    return FALSE;
  }

  g_free(opt_demo);
  opt_demo = g_strdup(value);
  opt_demo_set = TRUE;
  return TRUE;
}

static gboolean
validate_duration(const gchar *option_name,
                  const gchar *value,
                  gpointer data,
                  GError **error)
{
  gchar *end = NULL;
  gint64 duration;

  (void) option_name;
  (void) data;

  if (value == NULL || *value == '\0') {
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                _("Duration must be 30, 60 or 120 seconds."));
    return FALSE;
  }

  duration = g_ascii_strtoll(value, &end, 10);
  if (end == value || *end != '\0' ||
      (duration != 30 && duration != 60 && duration != 120)) {
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                _("Duration must be 30, 60 or 120 seconds."));
    return FALSE;
  }

  opt_duration = (gint) duration;
  return TRUE;
}

static int
list_keyboards(gboolean all)
{
  GError *error = NULL;
  GPtrArray *devices = all ? cylk_device_scan_all(&error)
                           : cylk_device_scan(&error);

  if (devices == NULL) {
    g_printerr(_("Could not scan input devices: %s\n"),
               error != NULL ? error->message : _("unknown error"));
    g_clear_error(&error);
    return 1;
  }

  if (devices->len == 0) {
    g_printerr("%s\n", _("No keyboards detected."));
    g_ptr_array_unref(devices);
    return 1;
  }

  for (guint i = 0; i < devices->len; i++) {
    CylkDevice *device = g_ptr_array_index(devices, i);
    const gchar *kind;

    if (device->kind == CYLK_DEVICE_COMPANION)
      kind = "companion";
    else if (device->kind == CYLK_DEVICE_TOUCHPAD)
      kind = "touchpad";
    else
      kind = device->internal ? "internal" : "external";

    g_print("%s\t%s\t%s\t%d\t%s\t%s\n",
            kind,
            device->path,
            device->name,
            device->score,
            device->id_bus != NULL ? device->id_bus : "-",
            device->id_path != NULL ? device->id_path : "-");
  }

  g_ptr_array_unref(devices);
  return 0;
}

static void
on_quit_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  GApplication *app = G_APPLICATION(user_data);

  (void) action;
  (void) parameter;

  g_application_quit(app);
}

static gboolean
quit_after_cb(gpointer user_data)
{
  gtk_window_close(GTK_WINDOW(user_data));
  return G_SOURCE_REMOVE;
}

static gint
on_handle_local_options(GApplication *app, GVariantDict *options, gpointer user_data)
{
  (void) app;
  (void) options;
  (void) user_data;

  if (opt_list_all)
    return list_keyboards(TRUE);

  if (opt_list)
    return list_keyboards(FALSE);

  return -1;
}

static void
load_stylesheet(void)
{
  GtkCssProvider *provider;
  GdkDisplay *display = gdk_display_get_default();

  if (display == NULL)
    return;

  provider = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(
      provider,
      "/io/github/julcal008/CleanYourLapKeyboard/css/style.css");
  gtk_style_context_add_provider_for_display(
      display, GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

static void
on_startup(GApplication *app, gpointer user_data)
{
  GSimpleAction *quit_action;

  (void) user_data;

  quit_action = g_simple_action_new("quit", NULL);
  g_signal_connect(quit_action, "activate",
                   G_CALLBACK(on_quit_action), app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit_action));
  g_object_unref(quit_action);

  gtk_application_set_accels_for_action(
      GTK_APPLICATION(app), "app.quit",
      (const gchar *const[]) { "<Control>q", NULL });
}

static void
on_activate(GtkApplication *app, gpointer user_data)
{
  GtkWindow *window;

  (void) user_data;

  window = g_object_get_data(G_OBJECT(app), "cylk-window");
  if (window == NULL) {
    load_stylesheet();

    window = GTK_WINDOW(cylk_window_new(ADW_APPLICATION(app),
                                        opt_demo_set ? opt_demo : NULL,
                                        opt_device,
                                        opt_duration));
    g_object_set_data(G_OBJECT(app), "cylk-window", window);
  }

  gtk_window_present(window);

  if (opt_quit_after > 0)
    g_timeout_add_seconds((guint) opt_quit_after, quit_after_cb, window);
}

int
main(int argc, char *argv[])
{
  AdwApplication *app;
  gint status;
  const GOptionEntry entries[] = {
    { "list", 'l', 0, G_OPTION_ARG_NONE, &opt_list,
      N_("List detected keyboards and exit"), NULL },
    { "list-all", 0, 0, G_OPTION_ARG_NONE, &opt_list_all,
      N_("List keyboards, touchpads and companion hotkey devices and exit"), NULL },
    { "device", 'd', 0, G_OPTION_ARG_STRING, &opt_device,
      N_("Use the keyboard at PATH"), N_("PATH") },
    { "duration", 0, 0, G_OPTION_ARG_CALLBACK, validate_duration,
      N_("Cleaning duration: 30, 60 or 120 seconds"), N_("SECONDS") },
    { "demo", 0, G_OPTION_FLAG_OPTIONAL_ARG, G_OPTION_ARG_CALLBACK,
      parse_demo_option,
      N_("Preview a UI state: idle, countdown, cleaning or done"),
      N_("STATE") },
    { "quit-after", 0, 0, G_OPTION_ARG_INT, &opt_quit_after,
      N_("Close automatically after SECONDS (testing)"), N_("SECONDS") },
    { NULL }
  };

  setlocale(LC_ALL, "");
  bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  app = adw_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_application_add_main_option_entries(G_APPLICATION(app), entries);
  g_application_set_option_context_summary(
      G_APPLICATION(app),
      _("Temporarily disable the built-in laptop keyboard so it can be cleaned."));

  g_signal_connect(app, "handle-local-options",
                   G_CALLBACK(on_handle_local_options), NULL);
  g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  status = g_application_run(G_APPLICATION(app), argc, argv);

  g_free(opt_device);
  g_free(opt_demo);
  g_object_unref(app);
  return status;
}
