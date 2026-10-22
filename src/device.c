/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Input device discovery and classification.
 *
 * Walks /sys/class/input through libudev, reads each device's capabilities from
 * sysfs and builds the list of usable keyboards.  It also finds the auxiliary
 * "companion" devices (volume, brightness, power button, ...) that belong to
 * the same hardware as a given keyboard and the internal touchpad that the
 * user may choose to disable together with it, so the helper can grab them
 * together.
 *
 * Everything here is read-only: no device is opened or grabbed from this file.
 */

#include "device.h"

#include "companion-keys.h"

#include <errno.h>
#include <gio/gio.h>
#include <linux/input.h>
#include <libudev.h>
#include <string.h>

#define INTERNAL_SCORE_THRESHOLD 100

static gchar *
read_sysfs_string(const gchar *syspath, const gchar *relative)
{
  gchar *path = g_build_filename(syspath, relative, NULL);
  gchar *contents = NULL;

  if (g_file_get_contents(path, &contents, NULL, NULL))
    g_strstrip(contents);

  g_free(path);
  return contents;
}

static gboolean
contains_ci(const gchar *haystack, const gchar *needle)
{
  gchar *lower_haystack;
  gchar *lower_needle;
  gboolean found;

  if (haystack == NULL || needle == NULL)
    return FALSE;

  lower_haystack = g_ascii_strdown(haystack, -1);
  lower_needle = g_ascii_strdown(needle, -1);
  found = strstr(lower_haystack, lower_needle) != NULL;

  g_free(lower_haystack);
  g_free(lower_needle);
  return found;
}

/*
 * Test a single bit in a sysfs capability bitmap.
 *
 * The kernel exposes these bitmaps as space-separated hexadecimal words, with
 * the most significant word first.  Each word holds 8 * sizeof(gulong) bits.
 */
gboolean
cylk_bitmap_has_key(const gchar *capabilities, guint key)
{
  gchar *copy;
  gchar **words;
  guint n_words;
  guint word_bits = 8 * sizeof(gulong);
  guint word_index;
  gint index;
  gulong value;
  gboolean result;

  if (capabilities == NULL || *capabilities == '\0')
    return FALSE;

  copy = g_strdup(capabilities);
  g_strstrip(copy);
  words = g_strsplit(copy, " ", -1);
  n_words = g_strv_length(words);

  word_index = key / word_bits;
  if (word_index >= n_words) {
    g_strfreev(words);
    g_free(copy);
    return FALSE;
  }

  /* The kernel prints the most significant word first. */
  index = (gint) n_words - 1 - (gint) word_index;
  value = (gulong) g_ascii_strtoull(words[index], NULL, 16);
  result = ((value >> (key % word_bits)) & 1UL) != 0;

  g_strfreev(words);
  g_free(copy);
  return result;
}

gboolean
cylk_bitmap_has_any_key(const gchar *capabilities,
                        const guint *keys,
                        gsize n_keys)
{
  if (capabilities == NULL)
    return FALSE;

  for (gsize i = 0; i < n_keys; i++) {
    if (cylk_bitmap_has_key(capabilities, keys[i]))
      return TRUE;
  }

  return FALSE;
}

static gboolean
has_full_keyboard_capabilities(const gchar *capabilities)
{
  return cylk_bitmap_has_key(capabilities, KEY_A) &&
         cylk_bitmap_has_key(capabilities, KEY_Z) &&
         cylk_bitmap_has_key(capabilities, KEY_0) &&
         cylk_bitmap_has_key(capabilities, KEY_ENTER) &&
         cylk_bitmap_has_key(capabilities, KEY_SPACE) &&
         cylk_bitmap_has_key(capabilities, KEY_LEFTSHIFT);
}

static gboolean
has_pointer_axes(const CylkDevice *device)
{
  return cylk_bitmap_has_key(device->rel_caps, REL_X) ||
         cylk_bitmap_has_key(device->rel_caps, REL_Y) ||
         cylk_bitmap_has_key(device->abs_caps, ABS_X) ||
         cylk_bitmap_has_key(device->abs_caps, ABS_Y) ||
         cylk_bitmap_has_key(device->abs_caps, ABS_MT_POSITION_X) ||
         cylk_bitmap_has_key(device->abs_caps, ABS_MT_POSITION_Y);
}

static gboolean
has_companion_keys(const gchar *key_caps)
{
  return cylk_bitmap_has_any_key(key_caps, cylk_companion_keys,
                                 G_N_ELEMENTS(cylk_companion_keys)) ||
         cylk_bitmap_has_any_key(key_caps, cylk_button_keys,
                                 G_N_ELEMENTS(cylk_button_keys));
}

/*
 * Touchpads are named after the hardware in almost every laptop, which is the
 * most reliable signal available in sysfs.  Reject mice and touchscreens
 * explicitly so an external pointer or a touch-enabled panel is never grabbed.
 */
static gboolean
has_touchpad_name(const gchar *name)
{
  if (name == NULL)
    return FALSE;

  if (contains_ci(name, "touchscreen") || contains_ci(name, "touch screen") ||
      contains_ci(name, "mouse"))
    return FALSE;

  return contains_ci(name, "touchpad") || contains_ci(name, "touch pad") ||
         contains_ci(name, "trackpad") || contains_ci(name, "track pad") ||
         contains_ci(name, "clickpad") || contains_ci(name, "glidepoint");
}

/*
 * A touchpad is a pointer that reports touches through BTN_TOUCH/BTN_TOOL_* or
 * through multitouch axes.  A plain mouse has axes but none of these.
 */
static gboolean
has_touchpad_capabilities(const CylkDevice *device)
{
  static const guint touch_keys[] = {
    BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
  };
  static const guint multitouch_axes[] = {
    ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
  };

  if (!has_pointer_axes(device))
    return FALSE;

  return cylk_bitmap_has_any_key(device->key_caps, touch_keys,
                                 G_N_ELEMENTS(touch_keys)) ||
         cylk_bitmap_has_any_key(device->abs_caps, multitouch_axes,
                                 G_N_ELEMENTS(multitouch_axes));
}

static gboolean
is_excluded_name(const gchar *name)
{
  static const gchar *const excluded[] = {
    "Video Bus",
    "Power Button",
    "Sleep Button",
    "Lid Switch",
    "PC Speaker",
    "HD-Audio",
    "Consumer Control",
    "System Control",
    "Wireless Radio",
    "WMI",
    "Hotkeys",
    "Touchpad",
    "TrackPoint",
    "Extra Buttons",
    NULL,
  };

  for (guint i = 0; excluded[i] != NULL; i++) {
    if (contains_ci(name, excluded[i]))
      return TRUE;
  }
  return FALSE;
}

/*
 * Heuristic score used to guess which device is the built-in keyboard.
 *
 * The i8042/serio bus and the "AT Translated Set 2 keyboard" name are strong
 * hints; anything on the USB bus is pushed down.  A score >= 100 means the
 * device is considered internal.
 */
gint
cylk_device_score(const gchar *name,
                  const gchar *syspath,
                  const gchar *id_path,
                  const gchar *id_bus,
                  guint16 bustype)
{
  gint score = 0;
  gboolean usb = FALSE;

  if (bustype == BUS_I8042)
    score += 120;

  if (id_bus != NULL && *id_bus != '\0') {
    if (g_ascii_strcasecmp(id_bus, "i8042") == 0)
      score += 100;
    else if (g_ascii_strcasecmp(id_bus, "serio") == 0)
      score += 90;
    else if (g_ascii_strcasecmp(id_bus, "usb") == 0)
      usb = TRUE;
  }

  if (id_path != NULL && *id_path != '\0') {
    if (strstr(id_path, "i8042") != NULL || strstr(id_path, "serio") != NULL)
      score += 80;
    if (strstr(id_path, "usb") != NULL)
      usb = TRUE;
  }

  if (syspath != NULL) {
    if (strstr(syspath, "i8042") != NULL || strstr(syspath, "serio") != NULL)
      score += 60;
    if (strstr(syspath, "usb") != NULL)
      usb = TRUE;
  }

  if (name != NULL) {
    if (contains_ci(name, "AT Translated Set 2 keyboard") ||
        contains_ci(name, "AT Translated Set 3 keyboard"))
      score += 100;

    if (contains_ci(name, "internal keyboard") ||
        contains_ci(name, "laptop keyboard"))
      score += 150;
  }

  if (usb)
    score -= 60;

  return score;
}

/*
 * Heuristic score used to recognise the built-in touchpad.
 *
 * The name is the strongest hint, while i8042/serio and I2C paths identify
 * integrated hardware.  USB and Bluetooth pointers are pushed below the
 * internal threshold so an external mouse or trackpad is never captured.
 */
gint
cylk_touchpad_score(const gchar *name,
                    const gchar *syspath,
                    const gchar *id_path,
                    const gchar *id_bus)
{
  gint score = 0;
  gboolean external = FALSE;

  if (name != NULL) {
    if (contains_ci(name, "touchpad") || contains_ci(name, "touch pad") ||
        contains_ci(name, "trackpad") || contains_ci(name, "track pad") ||
        contains_ci(name, "clickpad") || contains_ci(name, "glidepoint"))
      score += 120;

    if (contains_ci(name, "synaptics") || contains_ci(name, "elan") ||
        contains_ci(name, "alps") || contains_ci(name, "cypress") ||
        contains_ci(name, "precision"))
      score += 60;

    if (contains_ci(name, "mouse") || contains_ci(name, "touchscreen") ||
        contains_ci(name, "touch screen"))
      score -= 200;
  }

  if (id_bus != NULL &&
      (g_ascii_strcasecmp(id_bus, "usb") == 0 ||
       g_ascii_strcasecmp(id_bus, "bluetooth") == 0))
    external = TRUE;

  if (id_path != NULL) {
    if (strstr(id_path, "i8042") != NULL || strstr(id_path, "serio") != NULL)
      score += 80;
    if (strstr(id_path, "i2c") != NULL)
      score += 70;
    if (strstr(id_path, "usb") != NULL || strstr(id_path, "bluetooth") != NULL)
      external = TRUE;
  }

  if (syspath != NULL) {
    if (strstr(syspath, "i8042") != NULL || strstr(syspath, "serio") != NULL)
      score += 80;
    if (strstr(syspath, "i2c") != NULL)
      score += 70;
    if (strstr(syspath, "usb") != NULL || strstr(syspath, "bluetooth") != NULL)
      external = TRUE;
  }

  if (external)
    score -= 150;

  return score;
}

const gchar *
cylk_device_id(const CylkDevice *device)
{
  if (device->id_path != NULL && *device->id_path != '\0')
    return device->id_path;
  if (device->syspath != NULL && *device->syspath != '\0')
    return device->syspath;
  return device->path;
}

void
cylk_device_free(CylkDevice *device)
{
  if (device == NULL)
    return;

  g_free(device->name);
  g_free(device->path);
  g_free(device->syspath);
  g_free(device->id_path);
  g_free(device->id_bus);
  g_free(device->phys);
  g_free(device->key_caps);
  g_free(device->rel_caps);
  g_free(device->abs_caps);
  g_free(device);
}

static gint
device_kind_rank(CylkDeviceKind kind)
{
  switch (kind) {
  case CYLK_DEVICE_KEYBOARD:
    return 0;
  case CYLK_DEVICE_TOUCHPAD:
    return 1;
  case CYLK_DEVICE_COMPANION:
    return 2;
  }

  return 3;
}

static gint
compare_devices(gconstpointer a, gconstpointer b)
{
  const CylkDevice *da = *(CylkDevice *const *) a;
  const CylkDevice *db = *(CylkDevice *const *) b;

  if (da->kind != db->kind)
    return device_kind_rank(da->kind) - device_kind_rank(db->kind);

  if (da->score != db->score)
    return db->score - da->score;

  if (da->internal != db->internal)
    return da->internal ? -1 : 1;

  return g_ascii_strcasecmp(da->name, db->name);
}

static guint16
parse_bustype(const gchar *value)
{
  if (value == NULL || *value == '\0')
    return 0;
  return (guint16) g_ascii_strtoull(value, NULL, 16);
}

static gchar *
id_path_prefix(const gchar *id_path)
{
  const gchar *colon;

  if (id_path == NULL || *id_path == '\0')
    return NULL;

  /* "pci-0000:07:00.4-usb-0:2:1.0" and "...:1.1" share "...-usb-0:2:1". */
  colon = strrchr(id_path, ':');
  if (colon == NULL || colon == id_path)
    return g_strdup(id_path);

  return g_strndup(id_path, (gsize) (colon - id_path));
}

/*
 * Decide whether a companion device belongs to the same laptop as the primary
 * keyboard.
 *
 * Two cases are supported:
 *  - USB multi-interface devices: the ID_PATH only differs in the last index,
 *    so both nodes share the same physical interface ("...-usb-0:2:1.0" and
 *    "...-usb-0:2:1.1").
 *  - Built-in keyboards: any ACPI platform device, or a device with a known
 *    hotkey name, is considered part of the laptop.
 */
static gboolean
devices_share_hardware(const CylkDevice *primary, const CylkDevice *candidate)
{
  gchar *primary_prefix;
  gchar *candidate_prefix;
  gboolean same_interface;

  primary_prefix = id_path_prefix(primary->id_path);
  candidate_prefix = id_path_prefix(candidate->id_path);
  same_interface = g_strcmp0(primary_prefix, candidate_prefix) == 0 &&
                   primary_prefix != NULL;
  g_free(primary_prefix);
  g_free(candidate_prefix);

  if (same_interface)
    return TRUE;

  /*
   * The built-in keyboard lives on the i8042 controller or on the ACPI
   * platform bus.  Auxiliary hotkey devices and the power/sleep buttons are
   * exposed through the same firmware, so they are part of the laptop too.
   */
  if (primary->internal) {
    if (candidate->syspath != NULL &&
        (contains_ci(candidate->syspath, "acpi") ||
         contains_ci(candidate->syspath, "/platform/")))
      return TRUE;

    if (candidate->name != NULL &&
        (contains_ci(candidate->name, "hotkeys") ||
         contains_ci(candidate->name, "extra buttons") ||
         contains_ci(candidate->name, "thinkpad") ||
         contains_ci(candidate->name, "wmi")))
      return TRUE;
  }

  return FALSE;
}

/*
 * Pure predicate: is "candidate" a companion device to grab alongside
 * "primary"?  It is safe to call with hand-built structs, which is what the
 * unit tests do.  A candidate must be a companion kind, must not be the primary
 * itself, must not be a full keyboard or a pointer, must expose hotkey or
 * button keys and must share hardware with the primary.
 */
gboolean
cylk_device_is_companion_candidate(const CylkDevice *candidate,
                                   const CylkDevice *primary)
{
  g_return_val_if_fail(candidate != NULL, FALSE);

  if (candidate->kind != CYLK_DEVICE_COMPANION)
    return FALSE;

  if (primary != NULL && g_strcmp0(candidate->path, primary->path) == 0)
    return FALSE;

  if (has_full_keyboard_capabilities(candidate->key_caps))
    return FALSE;

  if (has_pointer_axes(candidate))
    return FALSE;

  if (!has_companion_keys(candidate->key_caps))
    return FALSE;

  if (primary == NULL)
    return TRUE;

  return devices_share_hardware(primary, candidate);
}

/*
 * Pure predicate: is "candidate" the built-in touchpad of the laptop?
 *
 * Besides the name and capabilities, the score must clear the internal
 * threshold.  A laptop whose touchpad is wired through USB is still accepted
 * when it shares the same physical interface as the selected keyboard.
 */
gboolean
cylk_device_is_touchpad_candidate(const CylkDevice *candidate,
                                  const CylkDevice *primary)
{
  g_return_val_if_fail(candidate != NULL, FALSE);

  if (primary != NULL && g_strcmp0(candidate->path, primary->path) == 0)
    return FALSE;

  if (has_full_keyboard_capabilities(candidate->key_caps))
    return FALSE;

  if (!has_touchpad_name(candidate->name) ||
      !has_touchpad_capabilities(candidate))
    return FALSE;

  if (cylk_touchpad_score(candidate->name, candidate->syspath,
                          candidate->id_path, candidate->id_bus) >=
      INTERNAL_SCORE_THRESHOLD)
    return TRUE;

  return primary != NULL && devices_share_hardware(primary, candidate);
}

static gboolean
ev_has_key(const gchar *ev_caps)
{
  if (ev_caps == NULL || *ev_caps == '\0')
    return TRUE;
  return cylk_bitmap_has_key(ev_caps, EV_KEY);
}

/*
 * Enumerate every usable keyboard, every internal touchpad and every companion
 * hotkey/button device.  The returned array is sorted and owns its CylkDevice
 * objects.
 */
GPtrArray *
cylk_device_scan_all(GError **error)
{
  struct udev *udev;
  struct udev_enumerate *enumerate;
  struct udev_list_entry *entries;
  struct udev_list_entry *entry;
  GPtrArray *devices;

  udev = udev_new();
  if (udev == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Could not initialize libudev");
    return NULL;
  }

  devices = g_ptr_array_new_with_free_func((GDestroyNotify) cylk_device_free);

  enumerate = udev_enumerate_new(udev);
  if (enumerate == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Could not enumerate input devices");
    udev_unref(udev);
    g_ptr_array_unref(devices);
    return NULL;
  }

  udev_enumerate_add_match_subsystem(enumerate, "input");
  udev_enumerate_scan_devices(enumerate);
  entries = udev_enumerate_get_list_entry(enumerate);

  udev_list_entry_foreach(entry, entries) {
    const gchar *syspath = udev_list_entry_get_name(entry);
    struct udev_device *udev_device;
    const gchar *devnode;
    gchar *name;
    gchar *key_caps;
    gchar *ev_caps;
    gchar *rel_caps;
    gchar *abs_caps;
    gchar *id_path;
    gchar *id_bus;
    gchar *phys;
    gchar *bustype_string;
    gboolean is_keyboard;
    gboolean is_touchpad;
    CylkDevice *device;

    udev_device = udev_device_new_from_syspath(udev, syspath);
    if (udev_device == NULL)
      continue;

    devnode = udev_device_get_devnode(udev_device);
    if (devnode == NULL || !g_str_has_prefix(devnode, "/dev/input/event")) {
      udev_device_unref(udev_device);
      continue;
    }

    name = read_sysfs_string(syspath, "device/name");
    if (name == NULL) {
      const gchar *attr = udev_device_get_sysattr_value(udev_device, "name");
      name = g_strdup(attr != NULL ? attr : "Unknown keyboard");
    }

    key_caps = read_sysfs_string(syspath, "device/capabilities/key");
    ev_caps = read_sysfs_string(syspath, "device/capabilities/ev");
    rel_caps = read_sysfs_string(syspath, "device/capabilities/rel");
    abs_caps = read_sysfs_string(syspath, "device/capabilities/abs");

    if (key_caps == NULL || !ev_has_key(ev_caps)) {
      g_free(name);
      g_free(key_caps);
      g_free(ev_caps);
      g_free(rel_caps);
      g_free(abs_caps);
      udev_device_unref(udev_device);
      continue;
    }

    is_keyboard = has_full_keyboard_capabilities(key_caps) &&
                  !is_excluded_name(name);

    id_path = g_strdup(udev_device_get_property_value(udev_device, "ID_PATH"));
    id_bus = g_strdup(udev_device_get_property_value(udev_device, "ID_BUS"));
    phys = read_sysfs_string(syspath, "device/phys");
    bustype_string = read_sysfs_string(syspath, "device/id/bustype");

    is_touchpad = FALSE;

    if (!is_keyboard) {
      CylkDevice probe;
      gboolean is_companion;

      memset(&probe, 0, sizeof probe);
      probe.name = name;
      probe.path = (gchar *) devnode;
      probe.syspath = (gchar *) syspath;
      probe.id_path = id_path;
      probe.id_bus = id_bus;
      probe.key_caps = key_caps;
      probe.rel_caps = rel_caps;
      probe.abs_caps = abs_caps;

      /*
       * Classify by name and capabilities here.  The internal/external
       * decision needs the selected keyboard, so it is applied later by
       * cylk_device_is_touchpad_candidate().  This keeps USB-wired internal
       * touchpads in the list so they can match the keyboard's interface.
       */
      is_touchpad = !has_full_keyboard_capabilities(key_caps) &&
                    has_touchpad_name(name) && has_touchpad_capabilities(&probe);
      is_companion = !is_touchpad && !has_full_keyboard_capabilities(key_caps) &&
                     !has_pointer_axes(&probe) && has_companion_keys(key_caps);

      if (!is_touchpad && !is_companion) {
        g_free(name);
        g_free(key_caps);
        g_free(ev_caps);
        g_free(rel_caps);
        g_free(abs_caps);
        g_free(id_path);
        g_free(id_bus);
        g_free(phys);
        g_free(bustype_string);
        udev_device_unref(udev_device);
        continue;
      }
    }

    device = g_new0(CylkDevice, 1);
    device->name = g_strdup(name);
    device->path = g_strdup(devnode);
    device->syspath = g_strdup(syspath);
    device->id_path = id_path;
    device->id_bus = id_bus;
    device->phys = phys;
    device->key_caps = key_caps;
    device->rel_caps = rel_caps;
    device->abs_caps = abs_caps;
    device->bustype = parse_bustype(bustype_string);
    device->kind = is_keyboard  ? CYLK_DEVICE_KEYBOARD
                   : is_touchpad ? CYLK_DEVICE_TOUCHPAD
                                 : CYLK_DEVICE_COMPANION;
    if (device->kind == CYLK_DEVICE_TOUCHPAD)
      device->score = cylk_touchpad_score(device->name, device->syspath,
                                          device->id_path, device->id_bus);
    else
      device->score = cylk_device_score(device->name, device->syspath,
                                        device->id_path, device->id_bus,
                                        device->bustype);
    device->internal = device->score >= INTERNAL_SCORE_THRESHOLD;

    g_ptr_array_add(devices, device);

    g_free(name);
    g_free(ev_caps);
    g_free(bustype_string);
    udev_device_unref(udev_device);
  }

  udev_enumerate_unref(enumerate);
  udev_unref(udev);

  g_ptr_array_sort(devices, compare_devices);
  return devices;
}

/*
 * Enumerate only the selectable keyboards (candidates to clean).
 */
GPtrArray *
cylk_device_scan(GError **error)
{
  GPtrArray *devices = cylk_device_scan_all(error);

  if (devices == NULL)
    return NULL;

  for (guint i = devices->len; i > 0; i--) {
    CylkDevice *device = g_ptr_array_index(devices, i - 1);
    if (device->kind != CYLK_DEVICE_KEYBOARD)
      g_ptr_array_remove_index(devices, i - 1);
  }

  return devices;
}

/*
 * Enumerate the companion devices that should be grabbed together with the
 * given primary keyboard.
 */
GPtrArray *
cylk_device_scan_companions(const CylkDevice *primary, GError **error)
{
  GPtrArray *devices;

  g_return_val_if_fail(primary != NULL, NULL);

  devices = cylk_device_scan_all(error);
  if (devices == NULL)
    return NULL;

  for (guint i = devices->len; i > 0; i--) {
    CylkDevice *device = g_ptr_array_index(devices, i - 1);
    if (!cylk_device_is_companion_candidate(device, primary))
      g_ptr_array_remove_index(devices, i - 1);
  }

  return devices;
}

/*
 * Enumerate the internal touchpads that belong to the same laptop as the given
 * primary keyboard.  Usually there is exactly one, but some machines expose
 * both a legacy and a multitouch node.
 */
GPtrArray *
cylk_device_scan_touchpads(const CylkDevice *primary, GError **error)
{
  GPtrArray *devices;

  g_return_val_if_fail(primary != NULL, NULL);

  devices = cylk_device_scan_all(error);
  if (devices == NULL)
    return NULL;

  for (guint i = devices->len; i > 0; i--) {
    CylkDevice *device = g_ptr_array_index(devices, i - 1);
    if (!cylk_device_is_touchpad_candidate(device, primary))
      g_ptr_array_remove_index(devices, i - 1);
  }

  return devices;
}
