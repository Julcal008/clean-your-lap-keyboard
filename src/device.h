/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Input device model shared by the UI and the CLI.
 *
 * A CylkDevice is either a full keyboard (something the user can select to
 * clean) or a companion device (a hotkey/media/button node that is grabbed at
 * the same time).  The capability bitmaps are kept as the raw sysfs strings so
 * they can be queried with cylk_bitmap_has_key().
 */
#pragma once

#include <glib.h>

typedef enum {
  CYLK_DEVICE_KEYBOARD,
  CYLK_DEVICE_COMPANION,
} CylkDeviceKind;

typedef struct {
  gchar *name;
  gchar *path;
  gchar *syspath;
  gchar *id_path;
  gchar *id_bus;
  gchar *phys;
  gchar *key_caps;
  gchar *rel_caps;
  gchar *abs_caps;
  guint16 bustype;
  gint score;
  gboolean internal;
  CylkDeviceKind kind;
} CylkDevice;

GPtrArray *cylk_device_scan(GError **error);
GPtrArray *cylk_device_scan_all(GError **error);
GPtrArray *cylk_device_scan_companions(const CylkDevice *primary,
                                       GError **error);
void cylk_device_free(CylkDevice *device);

const gchar *cylk_device_id(const CylkDevice *device);

gboolean cylk_bitmap_has_key(const gchar *capabilities, guint key);
gboolean cylk_bitmap_has_any_key(const gchar *capabilities,
                                 const guint *keys,
                                 gsize n_keys);

gboolean cylk_device_is_companion_candidate(const CylkDevice *candidate,
                                            const CylkDevice *primary);

gint cylk_device_score(const gchar *name,
                       const gchar *syspath,
                       const gchar *id_path,
                       const gchar *id_bus,
                       guint16 bustype);
