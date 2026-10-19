/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <linux/input.h>

/*
 * Keys that identify an auxiliary "companion" input device.
 *
 * On many laptops the firmware or embedded controller reports volume,
 * microphone mute, brightness and radio switches through a separate evdev
 * device (for example "Video Bus" or "ThinkPad Extra Buttons"), not through
 * the main keyboard device.  A device that exposes any of these keys, is not a
 * full keyboard and is not a pointer is grabbed together with the selected
 * keyboard.
 *
 * Keep this list in sync with the keys the application advertises as blocked.
 */
#define CYLK_COMPANION_KEY_LIST \
  X(KEY_VOLUMEUP)               \
  X(KEY_VOLUMEDOWN)             \
  X(KEY_MUTE)                   \
  X(KEY_MICMUTE)                \
  X(KEY_BRIGHTNESSDOWN)         \
  X(KEY_BRIGHTNESSUP)           \
  X(KEY_BRIGHTNESS_CYCLE)       \
  X(KEY_BRIGHTNESS_AUTO)        \
  X(KEY_BRIGHTNESS_MIN)         \
  X(KEY_BRIGHTNESS_MAX)         \
  X(KEY_DISPLAY_OFF)            \
  X(KEY_SWITCHVIDEOMODE)        \
  X(KEY_VIDEO_NEXT)             \
  X(KEY_VIDEO_PREV)             \
  X(KEY_KBDILLUMTOGGLE)         \
  X(KEY_KBDILLUMDOWN)           \
  X(KEY_KBDILLUMUP)             \
  X(KEY_WLAN)                   \
  X(KEY_BLUETOOTH)              \
  X(KEY_RFKILL)                 \
  X(KEY_WWAN)                   \
  X(KEY_UWB)                    \
  X(KEY_TOUCHPAD_TOGGLE)        \
  X(KEY_TOUCHPAD_ON)            \
  X(KEY_TOUCHPAD_OFF)           \
  X(KEY_NOTIFICATION_CENTER)    \
  X(KEY_SELECTIVE_SCREENSHOT)   \
  X(KEY_VOICECOMMAND)           \
  X(KEY_PLAYPAUSE)              \
  X(KEY_NEXTSONG)               \
  X(KEY_PREVIOUSSONG)           \
  X(KEY_STOPCD)                 \
  X(KEY_MEDIA)                  \
  X(KEY_CALC)                   \
  X(KEY_MAIL)                   \
  X(KEY_HOMEPAGE)               \
  X(KEY_BACK)                   \
  X(KEY_FORWARD)                \
  X(KEY_REFRESH)                \
  X(KEY_BOOKMARKS)              \
  X(KEY_SEARCH)

/*
 * Physical buttons that live on their own ACPI platform device on most
 * laptops.  They are grabbed too so that they cannot be pressed while wiping
 * the keyboard.  The firmware still performs a forced power-off when the power
 * button is held down for several seconds, so the computer can always be
 * turned off.
 */
#define CYLK_BUTTON_KEY_LIST \
  X(KEY_POWER)              \
  X(KEY_SLEEP)              \
  X(KEY_SUSPEND)

#define X(key) key,
static const unsigned int cylk_companion_keys[] = { CYLK_COMPANION_KEY_LIST };
static const unsigned int cylk_button_keys[] = { CYLK_BUTTON_KEY_LIST };
#undef X
