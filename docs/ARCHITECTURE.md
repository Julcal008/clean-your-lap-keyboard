# Architecture

This document explains how Clean Your Lap Keyboard is put together. It is meant
to be readable by anyone, even without previous experience with evdev, udev or
GTK. Every statement here matches the code in `src/` and `helper/`.

## The problem

Laptop keyboards cannot be "turned off" from a normal application: the keys are
handled by the kernel. What we can do is take an **exclusive grab** on the evdev
device with the `EVIOCGRAB` ioctl. While the grab is held, the kernel delivers
the key events only to us (and we simply ignore them), so nothing is typed.

Taking that grab needs `CAP_SYS_ADMIN`, so it cannot be done by the user-facing
application directly. That is the only reason a small privileged helper exists.

## The two halves

```text
+------------------------------+          +-----------------------------+
|  cylk  (normal user)         |          |  cylk-helper  (root)        |
|                              |  stdin   |                             |
|  GTK4 / libadwaita UI        | -------> |  opens /dev/input/eventN    |
|  device discovery (read-only)|  stdout  |  EVIOCGRAB on every device  |
|  state machine + countdown   | <------- |  drops privileges           |
+------------------------------+          +-----------------------------+
              ^                                        |
              | D-Bus (logind)                         | evdev
              |                                        v
        screen locked?                          /dev/input/event*
        session inactive?
```

The UI never opens an input device. The helper never draws anything and never
reads (or logs) key events: it only grabs and waits.

## Source files

| File | Responsibility |
| --- | --- |
| `src/main.c` | Command line options, gettext, application startup. |
| `src/window.c` | The whole UI: device list, countdown, cleaning, done, toasts. |
| `src/device.c` / `src/device.h` | Discover and classify keyboards and companions. |
| `src/companion-keys.h` | The single list of "blocked" key codes. |
| `src/config.c` / `src/config.h` | Persist duration and chosen keyboard. |
| `src/helper.c` / `src/helper.h` | Spawn the helper and speak the line protocol. |
| `src/session.c` / `src/session.h` | Watch logind (lock / suspend / inactive). |
| `helper/cylk-helper.c` | The privileged grab/release helper. |
| `tests/test-device.c` | Unit tests for the device logic. |

## The device model

`CylkDevice` describes one evdev node. Each one is either:

- a **keyboard** (`CYLK_DEVICE_KEYBOARD`): a device with full keyboard
  capabilities. These are the devices the user can pick to clean.
- a **companion** (`CYLK_DEVICE_COMPANION`): an auxiliary node that reports
  hotkeys (volume, mute, brightness, radio switches) or physical buttons
  (power, sleep). Companions are grabbed together with a keyboard.
- a **touchpad** (`CYLK_DEVICE_TOUCHPAD`): an internal pointer that the user can
  optionally disable together with the keyboard.

### How a keyboard is recognized

`cylk_device_scan_all()` walks `/sys/class/input` with libudev and, for each
`/dev/input/eventN`, reads the raw capability bitmaps from sysfs
(`device/capabilities/key`, `.../ev`, `.../rel`, `.../abs`). A device is a full
keyboard when its key bitmap has `KEY_A`, `KEY_Z`, `KEY_0`, `KEY_ENTER`,
`KEY_SPACE` and `KEY_LEFTSHIFT`, and its name is not in the excluded list
(touchpads, video, power buttons, ...).

### How the internal keyboard is guessed

`cylk_device_score()` adds points for the i8042/serio bus and for the classic
"AT Translated Set 2 keyboard" name, and subtracts points for USB. A score of
100 or more marks the device as **internal**. The user can always override the
choice; the selection is remembered in `~/.config/clean-your-lap-keyboard/`.

### How companions are found

`cylk_device_scan_companions()` keeps the companions that share hardware with
the chosen keyboard (`cylk_device_is_companion_candidate()`):

- **Same USB interface**: the `ID_PATH` of the two nodes only differs in the
  last index, e.g. `...-usb-0:2:1.0` (keyboard) and `...-usb-0:2:1.1`
  (consumer control). This is how external keyboards are handled: their media
  keys are only blocked if that keyboard was selected.
- **Built-in keyboard**: any ACPI/platform device is considered part of the
  laptop, which covers `Video Bus` (brightness), `ThinkPad Extra Buttons`
  (volume, mic, radio) and the `Power Button` / `Sleep Button` nodes.

A pointer, a second keyboard or a device without hotkey/button keys is never
chosen as a companion.

### How the touchpad is found

`cylk_device_scan_touchpads()` keeps the nodes accepted by
`cylk_device_is_touchpad_candidate()`:

- they must have pointer axes (`REL_X/Y` or `ABS_X/Y`) and touch capabilities
  (`BTN_TOUCH`, `BTN_TOOL_FINGER`, multitouch axes);
- the name must look like a touchpad (`Touchpad`, `Trackpad`, `ClickPad`,
  `GlidePoint`, ...) and must not say mouse or touchscreen;
- `cylk_touchpad_score()` must reach the internal threshold: names and
  i8042/serio/I2C paths add points, USB and Bluetooth subtract. A laptop that
  wires the touchpad through USB is still accepted when it shares the physical
  interface of the selected keyboard.

The touchpad is only grabbed when the user ticks the checkbox, and the choice
is persisted as `touchpad=true` in the settings file.

### Why the power button is included

The power button lives on its own ACPI device (`event0`, `event3`, ...). If it
is not grabbed, it can be pressed by accident while wiping the keyboard and the
machine would shut down in the middle of the operation. Grabbing it is safe
because holding the button for several seconds still forces a **hardware**
power-off handled by the embedded controller, which does not depend on the
operating system.

## The grab

When the user presses **Start cleaning**, the UI starts the helper. Once the
helper prints `READY`, the UI runs the three-second countdown. When the
countdown ends:

1. `src/window.c` uses the companion and touchpad paths that were computed when
   the keyboard was selected.
2. It sends `GRAB <path> <hold_ms> <max_seconds> <companion> ... T:<touchpad> ...`
   to the helper.
3. `helper/cylk-helper.c` opens every path, validates each one with
   `is_full_keyboard()`, `is_companion_device()` or `is_touchpad_device()`,
   waits up to two seconds for all keys to be released and takes `EVIOCGRAB` on
   all of them. Touchpads are excluded from the key-release wait because a
   resting finger is not a pressed key.
4. It then drops root, sets `PR_SET_NO_NEW_PRIVS` and clears the dumpable flag.
5. It reports `OK` and enters a tiny loop that only waits for `RELEASE`, EOF,
   a held **Esc** or the maximum duration.

The keyboard is released when any of these happens:

- the chosen duration ends (UI + hard limit in the helper);
- the user presses **Stop**;
- the user holds **Esc** for 2 seconds (emergency release);
- the window is closed or the app exits;
- logind reports a lock, a suspend or an inactive session;
- the helper's stdin closes (its file descriptors are closed by the kernel).

Closing the file descriptors is enough for the kernel to drop the grab, so a
crash in the UI can never leave the keyboard disabled.

When the touchpad is grabbed too, the **Stop** button cannot be clicked because
the pointer is disabled. The emergency Esc hold, the timer and the helper's hard
limit still release everything.

## Helper protocol

Tab-separated lines over stdin/stdout:

```text
UI -> helper : GRAB <path> <hold_ms> <max_seconds> [<companion> ...] [T:<touchpad> ...]
UI -> helper : RELEASE
UI -> helper : QUIT
helper -> UI : READY <protocol-version>
helper -> UI : OK <device-name>
helper -> UI : RELEASED <reason>
helper -> UI : ERR <code> <message>
```

`READY 3` is the current version. The version lets the UI detect an old helper
installed next to a new application and refuse to continue instead of silently
grabbing only the main keyboard or ignoring the touchpad.

## Adding a new blocked key

1. Add the `KEY_*` macro to `CYLK_COMPANION_KEY_LIST` (media/hotkey) or to
   `CYLK_BUTTON_KEY_LIST` (physical buttons) in `src/companion-keys.h`.
2. Add a unit test in `tests/test-device.c` if the behavior is not obvious.
3. Run `meson test -C build`.

Because both the UI (via `device.c`) and the helper (via `cylk-helper.c`)
include the same header, the key is then recognized on both sides.

## Known trade-offs

- Grabbing `ThinkPad Extra Buttons` also blocks Fn+suspend, WLAN/Bluetooth
  toggles, battery and notification keys **during cleaning**. They come back as
  soon as the grab is released.
- Keys that the firmware handles entirely in hardware (without any evdev event)
  cannot be blocked from userspace.
- Touchpad detection is heuristic and intentionally limited to built-in
  touchpads. Disabling it also disables the pointer, so the in-app Stop button
  cannot be clicked; use the Esc hold or let the timer finish.
- Automatic detection is heuristic; the manual selector and `cylk --list-all`
  exist for the cases where it guesses wrong.
