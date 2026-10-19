/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "device.h"

#include <glib.h>
#include <linux/input.h>

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
  const gchar *caps =
      "0 0 0 0 0 0 0 0 0 0 0 40000000";

  g_assert_true(cylk_bitmap_has_key(caps, KEY_A));
  g_assert_false(cylk_bitmap_has_key(caps, KEY_Z));
}

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/device/internal-score", test_internal_keyboard_scores_high);
  g_test_add_func("/device/usb-score", test_usb_keyboard_scores_low);
  g_test_add_func("/device/bitmap", test_bitmap_last_word);

  return g_test_run();
}
