/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * User settings persistence.
 *
 * Stores the chosen duration, the manually selected keyboard and the touchpad
 * preference in $XDG_CONFIG_HOME/clean-your-lap-keyboard/settings.ini.  Loading
 * is best-effort: a missing or unreadable file just falls back to the defaults.
 */

#include "config.h"

#include <glib/gstdio.h>

#define CONFIG_DIR "clean-your-lap-keyboard"
#define CONFIG_FILE "settings.ini"

static gchar *
config_path(void)
{
  return g_build_filename(g_get_user_config_dir(), CONFIG_DIR, CONFIG_FILE, NULL);
}

static gint
normalize_duration(gint duration)
{
  if (duration == 30 || duration == 60 || duration == 120)
    return duration;
  return 60;
}

CylkConfig *
cylk_config_load(void)
{
  CylkConfig *config = g_new0(CylkConfig, 1);
  GKeyFile *key_file = g_key_file_new();
  gchar *path = config_path();

  config->duration = 60;
  config->disable_touchpad = FALSE;

  if (g_file_test(path, G_FILE_TEST_IS_REGULAR) &&
      g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL)) {
    GError *error = NULL;
    gint duration = g_key_file_get_integer(key_file, "general", "duration", &error);
    if (error == NULL)
      config->duration = normalize_duration(duration);
    g_clear_error(&error);

    config->disable_touchpad =
        g_key_file_get_boolean(key_file, "general", "touchpad", NULL);

    gchar *device_id = g_key_file_get_string(key_file, "general", "device-id", NULL);
    if (device_id != NULL && *device_id != '\0')
      config->device_id = device_id;
    else
      g_free(device_id);
  }

  g_key_file_unref(key_file);
  g_free(path);
  return config;
}

void
cylk_config_save(CylkConfig *config)
{
  GKeyFile *key_file;
  gchar *path;
  gchar *contents;
  gsize length;
  gchar *directory;
  GError *error = NULL;

  if (config == NULL)
    return;

  key_file = g_key_file_new();
  g_key_file_set_integer(key_file, "general", "duration", config->duration);
  g_key_file_set_boolean(key_file, "general", "touchpad",
                         config->disable_touchpad);
  if (config->device_id != NULL)
    g_key_file_set_string(key_file, "general", "device-id", config->device_id);

  contents = g_key_file_to_data(key_file, &length, NULL);
  path = config_path();
  directory = g_path_get_dirname(path);
  g_mkdir_with_parents(directory, 0700);

  if (!g_file_set_contents(path, contents, (gssize) length, &error))
    g_warning("Could not save settings: %s", error->message);

  g_clear_error(&error);
  g_free(directory);
  g_free(path);
  g_free(contents);
  g_key_file_unref(key_file);
}

void
cylk_config_set_device(CylkConfig *config, const gchar *device_id)
{
  if (config == NULL)
    return;

  g_free(config->device_id);
  config->device_id = g_strdup(device_id);
}

void
cylk_config_free(CylkConfig *config)
{
  if (config == NULL)
    return;

  g_free(config->device_id);
  g_free(config);
}
