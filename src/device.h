/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <glib.h>

typedef struct {
  gchar *name;
  gchar *path;
  gchar *syspath;
  gchar *id_path;
  gchar *id_bus;
  gchar *phys;
  guint16 bustype;
  gint score;
  gboolean internal;
} CylkDevice;

GPtrArray *cylk_device_scan(GError **error);
void cylk_device_free(CylkDevice *device);

const gchar *cylk_device_id(const CylkDevice *device);

gboolean cylk_bitmap_has_key(const gchar *capabilities, guint key);
gint cylk_device_score(const gchar *name,
                       const gchar *syspath,
                       const gchar *id_path,
                       const gchar *id_bus,
                       guint16 bustype);
