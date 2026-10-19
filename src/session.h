/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Public interface of the logind session monitor.
 *
 * Connect to the "interrupted" signal to learn when the keyboard should be
 * released (lock, suspend, inactive session).
 */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define CYLK_TYPE_SESSION (cylk_session_get_type())
G_DECLARE_FINAL_TYPE(CylkSession, cylk_session, CYLK, SESSION, GObject)

CylkSession *cylk_session_new(void);

G_END_DECLS
