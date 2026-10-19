/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "helper.h"

#include "cylk-config.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#define STDERR_LIMIT 4096

struct _CylkHelper {
  GObject parent_instance;
};

typedef struct {
  GSubprocess *subprocess;
  GDataInputStream *stdout_stream;
  GDataInputStream *stderr_stream;
  GString *stderr_text;
  GCancellable *cancellable;
  guint force_timeout;
  gboolean ready;
  gboolean running;
  gboolean grabbed;
  gboolean released;
  gboolean errored;
  gboolean stopping;
} CylkHelperPrivate;

enum {
  SIGNAL_READY,
  SIGNAL_GRABBED,
  SIGNAL_RELEASED,
  SIGNAL_ERROR,
  SIGNAL_EXITED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE_WITH_PRIVATE(CylkHelper, cylk_helper, G_TYPE_OBJECT)

CylkHelper *
cylk_helper_new(void)
{
  return g_object_new(CYLK_TYPE_HELPER, NULL);
}

static void read_next_stdout_line(CylkHelper *self);
static void read_next_stderr_line(CylkHelper *self);

static void
emit_error(CylkHelper *self, const gchar *message)
{
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);

  if (priv->errored || priv->stopping)
    return;

  priv->errored = TRUE;
  g_signal_emit(self, signals[SIGNAL_ERROR], 0, message);
}

static void
handle_line(CylkHelper *self, const gchar *line)
{
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);

  if (g_strcmp0(line, "READY") == 0) {
    priv->ready = TRUE;
    g_signal_emit(self, signals[SIGNAL_READY], 0);
    return;
  }

  if (g_str_has_prefix(line, "OK\t")) {
    priv->grabbed = TRUE;
    g_signal_emit(self, signals[SIGNAL_GRABBED], 0, line + 3);
    return;
  }

  if (g_str_has_prefix(line, "RELEASED\t")) {
    priv->released = TRUE;
    g_signal_emit(self, signals[SIGNAL_RELEASED], 0, line + 9);
    return;
  }

  if (g_str_has_prefix(line, "ERR\t")) {
    const gchar *rest = line + 4;
    const gchar *tab = strchr(rest, '\t');
    gchar *message;

    if (tab != NULL)
      message = g_strdup(tab + 1);
    else
      message = g_strdup(rest);

    emit_error(self, message);
    g_free(message);
    return;
  }

  g_debug("helper: %s", line);
}

static void
on_stdout_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
  CylkHelper *self = CYLK_HELPER(user_data);
  GError *error = NULL;
  gsize length = 0;
  gchar *line;

  line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source),
                                              result, &length, &error);

  if (error != NULL) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_debug("helper stdout: %s", error->message);
    g_clear_error(&error);
  } else if (line != NULL) {
    handle_line(self, line);
    g_free(line);
    read_next_stdout_line(self);
  } else {
    g_debug("helper stdout closed");
  }

  g_object_unref(self);
}

static void
read_next_stdout_line(CylkHelper *self)
{
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);

  if (priv->stdout_stream == NULL)
    return;

  g_object_ref(self);
  g_data_input_stream_read_line_async(priv->stdout_stream,
                                      G_PRIORITY_DEFAULT,
                                      priv->cancellable,
                                      on_stdout_line,
                                      self);
}

static void
on_stderr_line(GObject *source, GAsyncResult *result, gpointer user_data)
{
  CylkHelper *self = CYLK_HELPER(user_data);
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);
  GError *error = NULL;
  gsize length = 0;
  gchar *line;

  line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source),
                                              result, &length, &error);

  if (error != NULL) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_debug("helper stderr: %s", error->message);
    g_clear_error(&error);
  } else if (line != NULL) {
    if (priv->stderr_text->len < STDERR_LIMIT) {
      g_string_append(priv->stderr_text, line);
      g_string_append_c(priv->stderr_text, '\n');
    }
    g_free(line);
    read_next_stderr_line(self);
  }

  g_object_unref(self);
}

static void
read_next_stderr_line(CylkHelper *self)
{
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);

  if (priv->stderr_stream == NULL)
    return;

  g_object_ref(self);
  g_data_input_stream_read_line_async(priv->stderr_stream,
                                      G_PRIORITY_DEFAULT,
                                      priv->cancellable,
                                      on_stderr_line,
                                      self);
}

static void
on_process_exited(GObject *source, GAsyncResult *result, gpointer user_data)
{
  CylkHelper *self = CYLK_HELPER(user_data);
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);
  GError *error = NULL;
  gint status = -1;
  gboolean normal = FALSE;

  if (!g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error)) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_debug("helper wait: %s", error->message);
    g_clear_error(&error);
  } else {
    normal = g_subprocess_get_if_exited(priv->subprocess);
    if (normal)
      status = g_subprocess_get_exit_status(priv->subprocess);
  }

  priv->running = FALSE;

  if (!priv->stopping) {
    if (priv->grabbed && !priv->released)
      g_signal_emit(self, signals[SIGNAL_RELEASED], 0, "disconnected");

    if (!priv->errored && !priv->released) {
      if (status == 126 || status == 127) {
        emit_error(self,
                   "Permission denied. The keyboard was not disabled. "
                   "Make sure polkit (pkexec) is installed and try again.");
      } else if (status > 0) {
        gchar *message;

        if (priv->stderr_text->len > 0) {
          g_strchomp(priv->stderr_text->str);
          message = g_strdup_printf("The helper exited with status %d: %s",
                                    status, priv->stderr_text->str);
        } else {
          message = g_strdup_printf("The helper exited unexpectedly (status %d).",
                                    status);
        }
        emit_error(self, message);
        g_free(message);
      }
    }
  }

  g_signal_emit(self, signals[SIGNAL_EXITED], 0, status);
  g_object_unref(self);
}

static gboolean
force_exit_cb(gpointer user_data)
{
  CylkHelper *self = CYLK_HELPER(user_data);
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);

  priv->force_timeout = 0;
  if (priv->running && priv->subprocess != NULL)
    g_subprocess_force_exit(priv->subprocess);

  return G_SOURCE_REMOVE;
}

static gboolean
write_command(CylkHelper *self, const gchar *command, GError **error)
{
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);
  GOutputStream *stdin_stream;
  gsize written = 0;

  if (priv->subprocess == NULL)
    return FALSE;

  stdin_stream = g_subprocess_get_stdin_pipe(priv->subprocess);
  if (stdin_stream == NULL)
    return FALSE;

  if (!g_output_stream_write_all(stdin_stream, command, strlen(command),
                                 &written, NULL, error))
    return FALSE;

  return g_output_stream_flush(stdin_stream, NULL, error);
}

gboolean
cylk_helper_start(CylkHelper *self, GError **error)
{
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);
  GSubprocessLauncher *launcher;
  const gchar *argv[] = { "pkexec", HELPER_PATH, NULL };
  gchar *pkexec;

  g_return_val_if_fail(CYLK_IS_HELPER(self), FALSE);

  if (priv->running)
    return TRUE;

  pkexec = g_find_program_in_path("pkexec");
  if (pkexec == NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "pkexec was not found. Install polkit and try again.");
    return FALSE;
  }
  g_free(pkexec);

  if (!g_file_test(HELPER_PATH, G_FILE_TEST_IS_EXECUTABLE)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "The helper is not installed at %s. Reinstall the package.",
                HELPER_PATH);
    return FALSE;
  }

  launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                       G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                       G_SUBPROCESS_FLAGS_STDERR_PIPE);

  priv->subprocess = g_subprocess_launcher_spawnv(launcher,
                                                  (const gchar *const *) argv,
                                                  error);
  g_object_unref(launcher);

  if (priv->subprocess == NULL)
    return FALSE;

  priv->running = TRUE;
  priv->ready = FALSE;
  priv->grabbed = FALSE;
  priv->released = FALSE;
  priv->errored = FALSE;
  priv->stopping = FALSE;
  g_string_set_size(priv->stderr_text, 0);

  priv->stdout_stream = g_data_input_stream_new(
      g_subprocess_get_stdout_pipe(priv->subprocess));
  priv->stderr_stream = g_data_input_stream_new(
      g_subprocess_get_stderr_pipe(priv->subprocess));

  g_data_input_stream_set_newline_type(priv->stdout_stream,
                                       G_DATA_STREAM_NEWLINE_TYPE_ANY);
  g_data_input_stream_set_newline_type(priv->stderr_stream,
                                       G_DATA_STREAM_NEWLINE_TYPE_ANY);

  read_next_stdout_line(self);
  read_next_stderr_line(self);

  g_object_ref(self);
  g_subprocess_wait_async(priv->subprocess, priv->cancellable,
                          on_process_exited, self);

  return TRUE;
}

void
cylk_helper_grab(CylkHelper *self,
                 const gchar *device_path,
                 guint hold_ms,
                 guint max_seconds)
{
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);
  GError *error = NULL;
  gchar *command;

  g_return_if_fail(CYLK_IS_HELPER(self));
  g_return_if_fail(device_path != NULL);

  if (priv->subprocess == NULL) {
    emit_error(self, "The helper is not running.");
    return;
  }

  if (!priv->ready) {
    emit_error(self, "The helper is not ready.");
    return;
  }

  command = g_strdup_printf("GRAB\t%s\t%u\t%u\n",
                            device_path, hold_ms, max_seconds);
  if (!write_command(self, command, &error)) {
    emit_error(self, error != NULL ? error->message : "Could not send the GRAB command.");
    g_clear_error(&error);
  }
  g_free(command);
}

void
cylk_helper_release(CylkHelper *self)
{
  GError *error = NULL;

  g_return_if_fail(CYLK_IS_HELPER(self));

  if (!write_command(self, "RELEASE\n", &error)) {
    emit_error(self, error != NULL ? error->message : "Could not send the RELEASE command.");
    g_clear_error(&error);
  }
}

void
cylk_helper_stop(CylkHelper *self)
{
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);
  GError *error = NULL;

  g_return_if_fail(CYLK_IS_HELPER(self));

  if (!priv->running)
    return;

  priv->stopping = TRUE;

  if (priv->grabbed)
    write_command(self, "RELEASE\n", &error);
  else if (priv->ready)
    write_command(self, "QUIT\n", &error);
  g_clear_error(&error);

  if (priv->stdout_stream != NULL)
    g_object_unref(priv->stdout_stream);
  priv->stdout_stream = NULL;
  if (priv->stderr_stream != NULL)
    g_object_unref(priv->stderr_stream);
  priv->stderr_stream = NULL;

  if (priv->force_timeout == 0)
    priv->force_timeout = g_timeout_add(600, force_exit_cb, self);
}

gboolean
cylk_helper_is_running(CylkHelper *self)
{
  CylkHelperPrivate *priv;

  g_return_val_if_fail(CYLK_IS_HELPER(self), FALSE);
  priv = cylk_helper_get_instance_private(self);
  return priv->running;
}

static void
cylk_helper_dispose(GObject *object)
{
  CylkHelper *self = CYLK_HELPER(object);
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);

  if (priv->force_timeout != 0) {
    g_source_remove(priv->force_timeout);
    priv->force_timeout = 0;
  }

  if (priv->cancellable != NULL)
    g_cancellable_cancel(priv->cancellable);

  if (priv->running && priv->subprocess != NULL)
    g_subprocess_force_exit(priv->subprocess);

  g_clear_object(&priv->stdout_stream);
  g_clear_object(&priv->stderr_stream);
  g_clear_object(&priv->subprocess);

  G_OBJECT_CLASS(cylk_helper_parent_class)->dispose(object);
}

static void
cylk_helper_finalize(GObject *object)
{
  CylkHelper *self = CYLK_HELPER(object);
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);

  g_string_free(priv->stderr_text, TRUE);
  g_clear_object(&priv->cancellable);

  G_OBJECT_CLASS(cylk_helper_parent_class)->finalize(object);
}

static void
cylk_helper_class_init(CylkHelperClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = cylk_helper_dispose;
  object_class->finalize = cylk_helper_finalize;

  signals[SIGNAL_READY] =
      g_signal_new("ready", G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                   G_TYPE_NONE, 0);
  signals[SIGNAL_GRABBED] =
      g_signal_new("grabbed", G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                   G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_RELEASED] =
      g_signal_new("released", G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                   G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_ERROR] =
      g_signal_new("error", G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                   G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_EXITED] =
      g_signal_new("exited", G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                   G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
cylk_helper_init(CylkHelper *self)
{
  CylkHelperPrivate *priv = cylk_helper_get_instance_private(self);

  priv->cancellable = g_cancellable_new();
  priv->stderr_text = g_string_new(NULL);
}
