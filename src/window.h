/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define CYLK_TYPE_WINDOW (cylk_window_get_type())
G_DECLARE_FINAL_TYPE(CylkWindow, cylk_window, CYLK, WINDOW, AdwApplicationWindow)

CylkWindow *cylk_window_new(AdwApplication *app,
                            const gchar *demo_mode,
                            const gchar *device_override,
                            gint duration_override);

G_END_DECLS
