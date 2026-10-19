/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "device.h"

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
  g_free(device);
}

static gint
compare_devices(gconstpointer a, gconstpointer b)
{
  const CylkDevice *da = *(CylkDevice *const *) a;
  const CylkDevice *db = *(CylkDevice *const *) b;

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

GPtrArray *
cylk_device_scan(GError **error)
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
    gchar *capabilities;
    gchar *id_path;
    gchar *id_bus;
    gchar *phys;
    gchar *bustype_string;
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

    capabilities = read_sysfs_string(syspath, "device/capabilities/key");
    if (capabilities == NULL || !has_full_keyboard_capabilities(capabilities)) {
      g_free(name);
      g_free(capabilities);
      udev_device_unref(udev_device);
      continue;
    }

    if (is_excluded_name(name)) {
      g_free(name);
      g_free(capabilities);
      udev_device_unref(udev_device);
      continue;
    }

    id_path = g_strdup(udev_device_get_property_value(udev_device, "ID_PATH"));
    id_bus = g_strdup(udev_device_get_property_value(udev_device, "ID_BUS"));
    phys = read_sysfs_string(syspath, "device/phys");
    bustype_string = read_sysfs_string(syspath, "device/id/bustype");

    device = g_new0(CylkDevice, 1);
    device->name = g_strdup(name);
    device->path = g_strdup(devnode);
    device->syspath = g_strdup(syspath);
    device->id_path = id_path;
    device->id_bus = id_bus;
    device->phys = phys;
    device->bustype = parse_bustype(bustype_string);
    device->score = cylk_device_score(device->name, device->syspath,
                                      device->id_path, device->id_bus,
                                      device->bustype);
    device->internal = device->score >= INTERNAL_SCORE_THRESHOLD;

    g_ptr_array_add(devices, device);

    g_free(name);
    g_free(capabilities);
    g_free(bustype_string);
    udev_device_unref(udev_device);
  }

  udev_enumerate_unref(enumerate);
  udev_unref(udev);

  g_ptr_array_sort(devices, compare_devices);
  return devices;
}
