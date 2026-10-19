/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Public interface of the helper client object.
 *
 * The window uses cylk_helper_start() to launch the privileged helper,
 * cylk_helper_grab()/cylk_helper_release() to control the grab, and connects to
 * its "ready", "grabbed", "released", "error" and "exited" signals.
 */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define CYLK_TYPE_HELPER (cylk_helper_get_type())
G_DECLARE_FINAL_TYPE(CylkHelper, cylk_helper, CYLK, HELPER, GObject)

CylkHelper *cylk_helper_new(void);

gboolean cylk_helper_start(CylkHelper *self, GError **error);
void cylk_helper_grab(CylkHelper *self,
                      const gchar *device_path,
                      GPtrArray *companion_paths,
                      guint hold_ms,
                      guint max_seconds);
void cylk_helper_release(CylkHelper *self);
void cylk_helper_stop(CylkHelper *self);
gboolean cylk_helper_is_running(CylkHelper *self);
guint cylk_helper_protocol_version(CylkHelper *self);

G_END_DECLS
