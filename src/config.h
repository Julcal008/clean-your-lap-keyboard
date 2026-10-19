/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <glib.h>

typedef struct {
  gint duration;
  gchar *device_id;
} CylkConfig;

CylkConfig *cylk_config_load(void);
void cylk_config_save(CylkConfig *config);
void cylk_config_free(CylkConfig *config);

void cylk_config_set_device(CylkConfig *config, const gchar *device_id);
