/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "session.h"

#include <gio/gio.h>
#include <unistd.h>

struct _CylkSession {
  GObject parent_instance;
};

typedef struct {
  GDBusConnection *bus;
  gchar *session_path;
  guint lock_subscription;
  guint active_subscription;
  guint sleep_subscription;
  guint properties_subscription;
} CylkSessionPrivate;

enum {
  SIGNAL_INTERRUPTED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE_WITH_PRIVATE(CylkSession, cylk_session, G_TYPE_OBJECT)

static void
emit_interrupted(CylkSession *self, const gchar *reason)
{
  g_signal_emit(self, signals[SIGNAL_INTERRUPTED], 0, reason);
}

static void
on_lock(GDBusConnection *connection,
        const gchar *sender_name,
        const gchar *object_path,
        const gchar *interface_name,
        const gchar *signal_name,
        GVariant *parameters,
        gpointer user_data)
{
  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;
  (void) parameters;

  emit_interrupted(CYLK_SESSION(user_data), "locked");
}

static void
on_active_changed(GDBusConnection *connection,
                  const gchar *sender_name,
                  const gchar *object_path,
                  const gchar *interface_name,
                  const gchar *signal_name,
                  GVariant *parameters,
                  gpointer user_data)
{
  gboolean active = TRUE;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;

  g_variant_get(parameters, "(b)", &active);
  if (!active)
    emit_interrupted(CYLK_SESSION(user_data), "inactive");
}

static void
on_properties_changed(GDBusConnection *connection,
                      const gchar *sender_name,
                      const gchar *object_path,
                      const gchar *interface_name,
                      const gchar *signal_name,
                      GVariant *parameters,
                      gpointer user_data)
{
  const gchar *changed_interface = NULL;
  GVariant *changed = NULL;
  GVariant *invalidated = NULL;
  GVariant *locked_hint;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;

  g_variant_get(parameters, "(&s@a{sv}@as)",
                &changed_interface, &changed, &invalidated);

  if (g_strcmp0(changed_interface, "org.freedesktop.login1.Session") == 0) {
    locked_hint = g_variant_lookup_value(changed, "LockedHint",
                                         G_VARIANT_TYPE_BOOLEAN);
    if (locked_hint != NULL) {
      if (g_variant_get_boolean(locked_hint))
        emit_interrupted(CYLK_SESSION(user_data), "locked");
      g_variant_unref(locked_hint);
    }
  }

  g_variant_unref(changed);
  g_variant_unref(invalidated);
}

static void
on_prepare_for_sleep(GDBusConnection *connection,
                     const gchar *sender_name,
                     const gchar *object_path,
                     const gchar *interface_name,
                     const gchar *signal_name,
                     GVariant *parameters,
                     gpointer user_data)
{
  gboolean sleeping = FALSE;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;

  g_variant_get(parameters, "(b)", &sleeping);
  if (sleeping)
    emit_interrupted(CYLK_SESSION(user_data), "sleep");
}

static void
cylk_session_init(CylkSession *self)
{
  CylkSessionPrivate *priv = cylk_session_get_instance_private(self);
  GError *error = NULL;
  GVariant *reply;

  priv->bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
  if (priv->bus == NULL) {
    g_debug("logind: cannot connect to the system bus: %s", error->message);
    g_clear_error(&error);
    return;
  }

  reply = g_dbus_connection_call_sync(
      priv->bus,
      "org.freedesktop.login1",
      "/org/freedesktop/login1",
      "org.freedesktop.login1.Manager",
      "GetSessionByPID",
      g_variant_new("(u)", (guint) getpid()),
      G_VARIANT_TYPE("(o)"),
      G_DBUS_CALL_FLAGS_NONE,
      1000,
      NULL,
      &error);

  if (reply == NULL) {
    g_debug("logind: cannot find the current session: %s", error->message);
    g_clear_error(&error);
    return;
  }

  g_variant_get(reply, "(o)", &priv->session_path);
  g_variant_unref(reply);

  priv->lock_subscription = g_dbus_connection_signal_subscribe(
      priv->bus,
      "org.freedesktop.login1",
      "org.freedesktop.login1.Session",
      "Lock",
      priv->session_path,
      NULL,
      G_DBUS_SIGNAL_FLAGS_NONE,
      on_lock,
      self,
      NULL);

  priv->active_subscription = g_dbus_connection_signal_subscribe(
      priv->bus,
      "org.freedesktop.login1",
      "org.freedesktop.login1.Session",
      "ActiveChanged",
      priv->session_path,
      NULL,
      G_DBUS_SIGNAL_FLAGS_NONE,
      on_active_changed,
      self,
      NULL);

  priv->sleep_subscription = g_dbus_connection_signal_subscribe(
      priv->bus,
      "org.freedesktop.login1",
      "org.freedesktop.login1.Manager",
      "PrepareForSleep",
      "/org/freedesktop/login1",
      NULL,
      G_DBUS_SIGNAL_FLAGS_NONE,
      on_prepare_for_sleep,
      self,
      NULL);

  priv->properties_subscription = g_dbus_connection_signal_subscribe(
      priv->bus,
      "org.freedesktop.login1",
      "org.freedesktop.DBus.Properties",
      "PropertiesChanged",
      priv->session_path,
      "org.freedesktop.login1.Session",
      G_DBUS_SIGNAL_FLAGS_NONE,
      on_properties_changed,
      self,
      NULL);
}

CylkSession *
cylk_session_new(void)
{
  return g_object_new(CYLK_TYPE_SESSION, NULL);
}

static void
cylk_session_dispose(GObject *object)
{
  CylkSession *self = CYLK_SESSION(object);
  CylkSessionPrivate *priv = cylk_session_get_instance_private(self);

  if (priv->bus != NULL) {
    if (priv->lock_subscription != 0)
      g_dbus_connection_signal_unsubscribe(priv->bus, priv->lock_subscription);
    if (priv->active_subscription != 0)
      g_dbus_connection_signal_unsubscribe(priv->bus, priv->active_subscription);
    if (priv->sleep_subscription != 0)
      g_dbus_connection_signal_unsubscribe(priv->bus, priv->sleep_subscription);
    if (priv->properties_subscription != 0)
      g_dbus_connection_signal_unsubscribe(priv->bus,
                                           priv->properties_subscription);
  }

  g_clear_object(&priv->bus);
  g_free(priv->session_path);
  priv->session_path = NULL;

  G_OBJECT_CLASS(cylk_session_parent_class)->dispose(object);
}

static void
cylk_session_class_init(CylkSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = cylk_session_dispose;

  signals[SIGNAL_INTERRUPTED] =
      g_signal_new("interrupted", G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                   G_TYPE_NONE, 1, G_TYPE_STRING);
}
