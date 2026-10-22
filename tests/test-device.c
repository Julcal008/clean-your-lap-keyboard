/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Unit tests for the device logic in src/device.c.
 *
 * The tests build CylkDevice structures by hand, so they do not need any real
 * evdev device or root privileges.
 */

#include "device.h"

#include <glib.h>
#include <linux/input.h>

/* Build a capability bitmap string in the same format the kernel uses. */
static gchar *
make_caps(const guint *keys, gsize n_keys)
{
  guint word_bits = 8 * sizeof(gulong);
  guint max_key = 0;
  guint n_words;
  gulong *words;
  GString *caps;

  for (gsize i = 0; i < n_keys; i++) {
    if (keys[i] > max_key)
      max_key = keys[i];
  }

  n_words = max_key / word_bits + 1;
  words = g_new0(gulong, n_words);

  for (gsize i = 0; i < n_keys; i++)
    words[keys[i] / word_bits] |= 1UL << (keys[i] % word_bits);

  caps = g_string_new(NULL);
  for (guint i = n_words; i > 0; i--) {
    if (i != n_words)
      g_string_append_c(caps, ' ');
    g_string_append_printf(caps, "%lx", words[i - 1]);
  }

  g_free(words);
  return g_string_free(caps, FALSE);
}

static CylkDevice *
make_device(const gchar *name,
            const gchar *path,
            const gchar *syspath,
            const gchar *id_path,
            const gchar *id_bus,
            CylkDeviceKind kind,
            const guint *keys,
            gsize n_keys)
{
  CylkDevice *device = g_new0(CylkDevice, 1);

  device->name = g_strdup(name);
  device->path = g_strdup(path);
  device->syspath = g_strdup(syspath);
  device->id_path = g_strdup(id_path);
  device->id_bus = g_strdup(id_bus);
  device->key_caps = make_caps(keys, n_keys);
  device->kind = kind;

  return device;
}

static CylkDevice *
make_internal_keyboard(void)
{
  static const guint keys[] = {
    KEY_A, KEY_Z, KEY_0, KEY_ENTER, KEY_SPACE, KEY_LEFTSHIFT,
  };
  CylkDevice *device;

  device = make_device("AT Translated Set 2 keyboard", "/dev/input/event4",
                       "/sys/devices/platform/i8042/serio0/input/input4",
                       "platform-i8042-serio-0", "i8042", CYLK_DEVICE_KEYBOARD,
                       keys, G_N_ELEMENTS(keys));
  device->internal = TRUE;
  return device;
}

static CylkDevice *
make_touchpad(const gchar *name,
              const gchar *syspath,
              const gchar *id_path,
              const gchar *id_bus)
{
  static const guint keys[] = { BTN_TOUCH, BTN_TOOL_FINGER };
  static const guint axes[] = {
    ABS_X, ABS_Y, ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
  };
  CylkDevice *device;

  device = make_device(name, "/dev/input/event8", syspath, id_path, id_bus,
                       CYLK_DEVICE_TOUCHPAD, keys, G_N_ELEMENTS(keys));
  device->abs_caps = make_caps(axes, G_N_ELEMENTS(axes));
  return device;
}

static void
test_internal_keyboard_scores_high(void)
{
  gint score = cylk_device_score(
      "AT Translated Set 2 keyboard",
      "/sys/devices/platform/i8042/serio0/input/input4",
      "platform-i8042-serio-0",
      "i8042",
      BUS_I8042);

  g_assert_cmpint(score, >=, 100);
}

static void
test_usb_keyboard_scores_low(void)
{
  gint score = cylk_device_score(
      "MOSART Semi. 2.4G Keyboard Mouse",
      "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/0003:0000:0000.0001",
      "pci-0000:00:14.0-usb-0:2:1.0",
      "usb",
      BUS_USB);

  g_assert_cmpint(score, <, 100);
}

static void
test_bitmap_last_word(void)
{
  /* 12 words is the usual size of the kernel key capability bitmap.
   * The kernel prints the most significant word first. */
  const gchar *caps = "0 0 0 0 0 0 0 0 0 0 0 40000000";

  g_assert_true(cylk_bitmap_has_key(caps, KEY_A));
  g_assert_false(cylk_bitmap_has_key(caps, KEY_Z));
}

static void
test_bitmap_has_any_key(void)
{
  static const guint present[] = { KEY_VOLUMEUP };
  static const guint wanted[] = { KEY_VOLUMEUP, KEY_MUTE };
  static const guint missing[] = { KEY_A, KEY_BRIGHTNESSUP };
  gchar *caps = make_caps(present, G_N_ELEMENTS(present));

  g_assert_true(cylk_bitmap_has_any_key(caps, wanted, G_N_ELEMENTS(wanted)));
  g_assert_false(cylk_bitmap_has_any_key(caps, missing, G_N_ELEMENTS(missing)));

  g_free(caps);
}

static void
test_companion_video_bus(void)
{
  static const guint keys[] = { KEY_BRIGHTNESSDOWN, KEY_BRIGHTNESSUP };
  CylkDevice *primary = make_internal_keyboard();
  CylkDevice *candidate;

  candidate = make_device(
      "Video Bus", "/dev/input/event10",
      "/sys/devices/pci0000:00/0000:00:08.1/acpi.video_bus.0/input/input12",
      "pci-0000:00:08.1", NULL, CYLK_DEVICE_COMPANION, keys,
      G_N_ELEMENTS(keys));

  g_assert_true(cylk_device_is_companion_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

static void
test_companion_thinkpad_extra_buttons(void)
{
  static const guint keys[] = {
    KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE, KEY_MICMUTE, KEY_SLEEP,
  };
  CylkDevice *primary = make_internal_keyboard();
  CylkDevice *candidate;

  candidate = make_device("ThinkPad Extra Buttons", "/dev/input/event13",
                          "/sys/devices/platform/thinkpad_acpi/input/input14",
                          "platform-thinkpad_acpi", NULL,
                          CYLK_DEVICE_COMPANION, keys, G_N_ELEMENTS(keys));

  g_assert_true(cylk_device_is_companion_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

static void
test_companion_power_button(void)
{
  static const guint keys[] = { KEY_POWER };
  CylkDevice *primary = make_internal_keyboard();
  CylkDevice *candidate;

  candidate = make_device("Power Button", "/dev/input/event0",
                          "/sys/devices/platform/PNP0C0C:00/input/input0",
                          "platform-PNP0C0C:00", NULL, CYLK_DEVICE_COMPANION,
                          keys, G_N_ELEMENTS(keys));

  g_assert_true(cylk_device_is_companion_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

static void
test_companion_external_consumer_control_rejected(void)
{
  static const guint keys[] = { KEY_VOLUMEUP, KEY_MUTE };
  CylkDevice *primary = make_internal_keyboard();
  CylkDevice *candidate;

  candidate = make_device(
      "MOSART Semi. 2.4G Keyboard Mouse Consumer Control", "/dev/input/event23",
      "/sys/devices/pci0000:00/0000:00:08.1/usb3/3-2/3-2:1.1/0003:062A:4101.000F/input/input44",
      "pci-0000:07:00.4-usb-0:2:1.1", "usb", CYLK_DEVICE_COMPANION, keys,
      G_N_ELEMENTS(keys));

  g_assert_false(cylk_device_is_companion_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

static void
test_companion_usb_sibling_accepted(void)
{
  static const guint keyboard_keys[] = {
    KEY_A, KEY_Z, KEY_0, KEY_ENTER, KEY_SPACE, KEY_LEFTSHIFT,
  };
  static const guint media_keys[] = { KEY_VOLUMEUP, KEY_MUTE };
  CylkDevice *primary;
  CylkDevice *candidate;

  primary = make_device(
      "Internal USB keyboard", "/dev/input/event3",
      "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/0003:0000:0000.0001/input/input9",
      "pci-0000:00:14.0-usb-0:2:1.0", "usb", CYLK_DEVICE_KEYBOARD,
      keyboard_keys, G_N_ELEMENTS(keyboard_keys));

  candidate = make_device(
      "Internal USB Consumer Control", "/dev/input/event4",
      "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.1/0003:0000:0000.0002/input/input10",
      "pci-0000:00:14.0-usb-0:2:1.1", "usb", CYLK_DEVICE_COMPANION,
      media_keys, G_N_ELEMENTS(media_keys));

  g_assert_true(cylk_device_is_companion_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

static void
test_companion_pointer_rejected(void)
{
  static const guint keys[] = { KEY_VOLUMEUP };
  static const guint axes[] = { REL_X, REL_Y };
  CylkDevice *primary = make_internal_keyboard();
  CylkDevice *candidate;

  candidate = make_device("Wireless Mouse Consumer Control", "/dev/input/event6",
                          "/sys/devices/platform/wireless_mouse/input/input8",
                          "platform-wireless_mouse", NULL,
                          CYLK_DEVICE_COMPANION, keys, G_N_ELEMENTS(keys));
  candidate->rel_caps = make_caps(axes, G_N_ELEMENTS(axes));

  g_assert_false(cylk_device_is_companion_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

static void
test_companion_second_keyboard_rejected(void)
{
  static const guint keys[] = {
    KEY_A, KEY_Z, KEY_0, KEY_ENTER, KEY_SPACE, KEY_LEFTSHIFT, KEY_VOLUMEUP,
  };
  CylkDevice *primary = make_internal_keyboard();
  CylkDevice *candidate;

  candidate = make_device("Second keyboard", "/dev/input/event20",
                          "/sys/devices/platform/second_keyboard/input/input20",
                          "platform-second_keyboard", NULL,
                          CYLK_DEVICE_COMPANION, keys, G_N_ELEMENTS(keys));

  g_assert_false(cylk_device_is_companion_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

static void
test_companion_lid_switch_rejected(void)
{
  CylkDevice *primary = make_internal_keyboard();
  CylkDevice *candidate;

  candidate = make_device("Lid Switch", "/dev/input/event1",
                          "/sys/devices/platform/PNP0C0D:00/input/input1",
                          "platform-PNP0C0D:00", NULL, CYLK_DEVICE_COMPANION,
                          NULL, 0);

  g_assert_false(cylk_device_is_companion_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

static void
test_touchpad_internal_accepted(void)
{
  CylkDevice *primary = make_internal_keyboard();
  CylkDevice *candidate;

  candidate = make_touchpad(
      "MSFT0001:01 06CB:CE44 Touchpad",
      "/sys/devices/pci0000:00/0000:00:15.0/i2c-1/i2c-MSHW0001:00/input/input20",
      "pci-0000:00:15.0-i2c-1", NULL);

  g_assert_cmpint(cylk_touchpad_score(candidate->name, candidate->syspath,
                                      candidate->id_path, candidate->id_bus),
                  >=, 100);
  g_assert_true(cylk_device_is_touchpad_candidate(candidate, primary));
  /* A touchpad must never be treated as a companion hotkey device. */
  g_assert_false(cylk_device_is_companion_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

static void
test_touchpad_ps2_accepted(void)
{
  CylkDevice *primary = make_internal_keyboard();
  CylkDevice *candidate;

  candidate = make_touchpad(
      "SynPS/2 Synaptics TouchPad",
      "/sys/devices/platform/i8042/serio1/input/input8",
      "platform-i8042-serio-1", "i8042");

  g_assert_true(cylk_device_is_touchpad_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

static void
test_touchpad_external_usb_rejected(void)
{
  CylkDevice *primary = make_internal_keyboard();
  CylkDevice *candidate;

  candidate = make_touchpad(
      "Wireless Touchpad",
      "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/0003:0000:0000.0001/input/input30",
      "pci-0000:00:14.0-usb-0:2:1.0", "usb");

  g_assert_cmpint(cylk_touchpad_score(candidate->name, candidate->syspath,
                                      candidate->id_path, candidate->id_bus),
                  <, 100);
  g_assert_false(cylk_device_is_touchpad_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

static void
test_touchpad_usb_shared_interface_accepted(void)
{
  static const guint keys[] = {
    KEY_A, KEY_Z, KEY_0, KEY_ENTER, KEY_SPACE, KEY_LEFTSHIFT,
  };
  CylkDevice *primary;
  CylkDevice *candidate;

  /* A laptop can wire both the keyboard and the touchpad through one USB
   * composite device.  They only differ in the last interface index. */
  primary = make_device(
      "USB keyboard", "/dev/input/event5",
      "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/input/input5",
      "pci-0000:00:14.0-usb-0:2:1.0", "usb", CYLK_DEVICE_KEYBOARD, keys,
      G_N_ELEMENTS(keys));
  candidate = make_touchpad(
      "Touchpad",
      "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.1/input/input6",
      "pci-0000:00:14.0-usb-0:2:1.1", "usb");

  g_assert_cmpint(cylk_touchpad_score(candidate->name, candidate->syspath,
                                      candidate->id_path, candidate->id_bus),
                  <, 100);
  g_assert_true(cylk_device_is_touchpad_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

static void
test_touchpad_mouse_rejected(void)
{
  static const guint keys[] = { BTN_LEFT };
  static const guint axes[] = { REL_X, REL_Y };
  CylkDevice *primary = make_internal_keyboard();
  CylkDevice *candidate;

  candidate = make_device(
      "Logitech USB Optical Mouse", "/dev/input/event9",
      "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-3/1-3:1.0/0003:046D:C077.0002/input/input12",
      "pci-0000:00:14.0-usb-0:3:1.0", "usb", CYLK_DEVICE_COMPANION, keys,
      G_N_ELEMENTS(keys));
  candidate->rel_caps = make_caps(axes, G_N_ELEMENTS(axes));

  g_assert_false(cylk_device_is_touchpad_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

static void
test_touchpad_touchscreen_rejected(void)
{
  CylkDevice *primary = make_internal_keyboard();
  CylkDevice *candidate;

  candidate = make_touchpad(
      "ELAN2514:00 04F3:2D5A Touchscreen",
      "/sys/devices/pci0000:00/0000:00:15.0/i2c-2/i2c-ELAN2514:00/input/input18",
      "pci-0000:00:15.0-i2c-2", NULL);

  g_assert_false(cylk_device_is_touchpad_candidate(candidate, primary));

  cylk_device_free(primary);
  cylk_device_free(candidate);
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/device/internal-score", test_internal_keyboard_scores_high);
  g_test_add_func("/device/usb-score", test_usb_keyboard_scores_low);
  g_test_add_func("/device/bitmap", test_bitmap_last_word);
  g_test_add_func("/device/bitmap-any", test_bitmap_has_any_key);
  g_test_add_func("/device/companion-video-bus", test_companion_video_bus);
  g_test_add_func("/device/companion-thinkpad",
                  test_companion_thinkpad_extra_buttons);
  g_test_add_func("/device/companion-power-button",
                  test_companion_power_button);
  g_test_add_func("/device/companion-external-rejected",
                  test_companion_external_consumer_control_rejected);
  g_test_add_func("/device/companion-usb-sibling",
                  test_companion_usb_sibling_accepted);
  g_test_add_func("/device/companion-pointer-rejected",
                  test_companion_pointer_rejected);
  g_test_add_func("/device/companion-keyboard-rejected",
                  test_companion_second_keyboard_rejected);
  g_test_add_func("/device/companion-lid-rejected",
                  test_companion_lid_switch_rejected);
  g_test_add_func("/device/touchpad-internal", test_touchpad_internal_accepted);
  g_test_add_func("/device/touchpad-ps2", test_touchpad_ps2_accepted);
  g_test_add_func("/device/touchpad-external-rejected",
                  test_touchpad_external_usb_rejected);
  g_test_add_func("/device/touchpad-usb-shared-interface",
                  test_touchpad_usb_shared_interface_accepted);
  g_test_add_func("/device/touchpad-mouse-rejected",
                  test_touchpad_mouse_rejected);
  g_test_add_func("/device/touchpad-touchscreen-rejected",
                  test_touchpad_touchscreen_rejected);

  return g_test_run();
}
