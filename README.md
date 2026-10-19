# Clean Your Lap Keyboard

A tiny Linux app that temporarily disables the **built-in keyboard** of a
laptop, so you can wipe the keys while the computer is still on. Your touchpad
and any external keyboard keep working.

[Español](README.es.md)

![Idle](docs/screenshots/idle.png)

## Why this exists

Turning a laptop off to clean the keyboard is slow, and turning it on again is
not always convenient. Disabling only the internal keyboard lets you clean the
keys with the machine running, without random keystrokes opening menus,
launching programs, or changing your session.

The app uses `EVIOCGRAB` on the evdev device. X11/Wayland global grabs are not
enough because they do not stop the compositor or a virtual terminal from
receiving the key events.

## Features

- Temporary, exclusive grab of the internal keyboard.
- Also grabs the companion hotkey devices that carry volume, microphone,
  brightness, and power/sleep button events.
- Automatic detection of the internal keyboard, with a manual selector.
- Compact 400 × 580 GTK4/libadwaita window that follows the system light/dark
  theme and the system language.
- Three durations: 30 seconds, 60 seconds, and 2 minutes.
- Three-second countdown before the grab.
- Stop button, plus **hold Esc for 2 seconds** to release.
- Automatic release when the timer ends, the window closes, the screen locks,
  the machine suspends, or the session becomes inactive.
- A hard maximum duration in the privileged helper as a second line of defense.
- No daemon, no key logging, no key event reading.
- English and Spanish translations.

| Countdown | Cleaning | Done |
| --- | --- | --- |
| <img src="docs/screenshots/countdown.png" width="250" alt="Countdown"> | <img src="docs/screenshots/cleaning.png" width="250" alt="Cleaning"> | <img src="docs/screenshots/done.png" width="250" alt="Done"> |

## What it does not do

- It does not disable the touchpad or an external USB/Bluetooth keyboard. An
  external keyboard is only affected if you select it for cleaning.
- It cannot block keys that the firmware or embedded controller handles
  entirely in hardware without emitting any evdev event.
- It does not read, record, or transmit key presses. The helper only grabs the
  devices and polls the key state to detect a held Esc.
- It does not replace a screen lock. Lock your session before cleaning if
  someone else has physical access to the laptop.
- It cannot place the window at absolute coordinates on Wayland; the compositor
  controls placement. Most compositors center small windows.

## Requirements

- Linux with evdev (`/dev/input/event*`) and logind.
- GTK 4.8 or newer.
- libadwaita 1.2 or newer.
- `pkexec` and a working polkit authentication agent.
- A desktop session on Wayland or X11.

Tested target distributions:

| Distribution | Notes |
| --- | --- |
| Fedora 44 | Primary target. |
| Ubuntu 24.04+ | GTK 4.8 / libadwaita 1.2 are available. |
| Debian 12+ | Bookworm has the required library versions. |
| Arch Linux | Rolling packages are sufficient. |

## Install

### Fedora

Build the RPM from this repository (see [Packaging](#packaging)), or install a
release RPM:

```bash
sudo dnf install ./clean-your-lap-keyboard-0.1.1-1.fc44.x86_64.rpm
```

### Debian and Ubuntu

```bash
sudo apt install ./clean-your-lap-keyboard_0.1.1-1_amd64.deb
```

### Arch Linux

The `PKGBUILD` is in `packaging/arch/`. Copy it next to the release tarball and
run:

```bash
makepkg -si
```

The app appears in the application menu as **Clean Your Lap Keyboard**. You can
also launch it from a terminal:

```bash
cylk
```

Packages also install a manual page:

```bash
man cylk
```

## Build from source

Install the development packages first.

### Fedora

```bash
sudo dnf install gcc meson ninja-build gettext desktop-file-utils appstream \
  gtk4-devel libadwaita-devel systemd-devel
```

### Debian / Ubuntu

```bash
sudo apt install build-essential meson ninja-build pkg-config gettext \
  desktop-file-utils appstream libgtk-4-dev libadwaita-1-dev libudev-dev \
  libglib2.0-dev
```

### Arch Linux

```bash
sudo pacman -S --needed base-devel meson ninja gettext desktop-file-utils \
  appstream gtk4 libadwaita systemd-libs
```

Then:

```bash
meson setup build --buildtype=release
meson compile -C build
meson test -C build --print-errorlogs
sudo meson install -C build
```

For a staging install without touching the system:

```bash
meson install -C build --destdir "$PWD/pkgroot"
```

The compiled binaries are also directly runnable from the build tree:

```bash
./build/src/cylk --list
./build/helper/cylk-helper
```

## Usage

```text
cylk [OPTION…]

  -l, --list                 List detected keyboards and exit
      --list-all             List keyboards and companion hotkey devices
  -d, --device=PATH          Use the keyboard at PATH
      --duration=SECONDS     Cleaning duration: 30, 60 or 120 seconds
      --demo=STATE           Preview idle, countdown, cleaning or done
      --quit-after=SECONDS   Close automatically (testing)
  -h, --help                 Show help
```

### Find the right keyboard

```bash
cylk --list
```

Example output:

```text
internal	/dev/input/event4	AT Translated Set 2 keyboard	460	i8042	platform-i8042-serio-0
external	/dev/input/event21	MOSART Semi. 2.4G Keyboard Mouse	-60	usb	pci-0000:07:00.4-usb-0:2:1.0
```

The first field is the heuristic classification, followed by the device path,
name, score, bus, and hardware path. The app remembers a manually selected
keyboard in `~/.config/clean-your-lap-keyboard/settings.ini`.

To see the companion hotkey devices that are grabbed together with the
keyboard, use:

```bash
cylk --list-all
```

```text
internal	/dev/input/event4	AT Translated Set 2 keyboard	460	i8042	platform-i8042-serio-0
companion	/dev/input/event10	Video Bus	0	-	pci-0000:00:08.1
companion	/dev/input/event13	ThinkPad Extra Buttons	0	-	platform-thinkpad_acpi
external	/dev/input/event21	MOSART Semi. 2.4G Keyboard Mouse	-60	usb	pci-0000:07:00.4-usb-0:2:1.0
```

If the automatic choice is wrong, select the keyboard from the card in the app,
or start it with an explicit path:

```bash
cylk --device /dev/input/event4 --duration 60
```

### Preview the interface without grabbing anything

```bash
cylk --demo=idle
cylk --demo=countdown
cylk --demo=cleaning
cylk --demo=done
```

## Safety model

The normal flow is:

1. The user presses **Start cleaning**.
2. The app starts `pkexec` and asks polkit for permission.
3. The helper opens the requested evdev device plus the companion hotkey
   devices (volume, microphone, brightness, power/sleep buttons), checks each
   one, waits for all keys to be released, and takes `EVIOCGRAB` on all of them.
4. The helper drops privileges to the user who invoked `pkexec`, sets
   `PR_SET_NO_NEW_PRIVS` and `PR_SET_DUMPABLE=0`, and reports success.
5. The UI shows the countdown and then the cleaning screen.
6. The keyboard is released on any of these events:
   - the selected duration ends;
   - the **Stop cleaning** button is pressed;
   - Esc is held for two seconds;
   - the helper reaches its own maximum duration (selected duration + 5 s);
   - the window is closed, or the app receives `SIGTERM`/`SIGINT`;
   - the helper's stdin closes or the app dies;
   - logind reports a screen lock, suspend, or inactive session.

Closing the helper's file descriptors is enough for the kernel to drop the
`EVIOCGRAB`, so the keyboard is restored even if the UI crashes. Holding the
power button for several seconds still forces a hardware power-off at the
firmware level, so the machine can always be turned off.

The helper does **not** read the evdev event stream. It uses `EVIOCGKEY` only to
check whether keys are currently down, including the held-Esc release gesture.

## Permissions and polkit

The helper is installed in the distribution's `libexec` directory and is
launched on demand through `pkexec`. There is no always-running privileged
daemon.

The polkit action is:

```text
io.github.julcal008.CleanYourLapKeyboard.grab-keyboard
```

Its defaults allow active local sessions. This avoids a password prompt every
time the app starts. If your system requires stricter authentication, edit the
policy file in `/usr/share/polkit-1/actions/` or add a local override under
`/etc/polkit-1/rules.d/`.

The helper validates that the requested path is a real evdev event device with
the expected major/minor numbers and a full keyboard capability bitmap. Every
companion path must expose hotkey or button keys, must not be a pointer or a
second keyboard, and must belong to the same hardware as the selected keyboard.
Arbitrary files and unsupported devices are rejected.

## Troubleshooting

### `--list` shows no internal keyboard

The detection is a heuristic based on `ID_BUS`, `ID_PATH`, the sysfs path, the
bus type, and the device name. Some laptops expose the keyboard through a
non-i8042 controller or a USB HID bridge.

1. Run `cylk --list` and look for a likely device.
2. Run `udevadm info --query=property --name=/dev/input/eventN`.
3. Select the device manually in the app.
4. If it is not listed at all, check that it exposes the usual letter,
   number, Enter, Space, and Shift keys. The helper refuses partial devices.

### `pkexec` is not found, or authentication fails

Install polkit and make sure an authentication agent is running in your desktop
session. On minimal window managers, start one such as
`polkit-gnome-authentication-agent-1` or `lxpolkit`.

### `Cannot grab … Device or resource busy`

Another program already holds `EVIOCGRAB` on the device. Close virtual-machine
input grabbers, remote-desktop tools, or other keyboard-remapping software and
try again.

### Volume, brightness, or microphone keys still work during cleaning

Those keys are usually reported by a separate evdev device, not by the main
keyboard, so the app grabs it too. To see which devices are used:

```bash
cylk --list-all
```

If a key still works, check which event device emits it:

```bash
sudo evtest
```

Keys handled entirely inside the firmware cannot be blocked from userspace.
Keys of an external keyboard are intentionally left working unless you selected
that keyboard for cleaning.

### Wayland window placement

Wayland does not let applications set absolute window coordinates. The size is
fixed, but the compositor decides where the window appears. KDE Plasma and
GNOME normally center a new small window; other compositors may not.

### SELinux

On Fedora, install the package with `dnf` so the helper and policy receive the
correct SELinux labels. Running binaries from an unlabeled build directory
through `pkexec` can be blocked. If needed, check with:

```bash
sudo ausearch -m avc -ts recent
```

### The keyboard is still disabled

The kernel releases the grab when the helper exits. If a helper process is
stuck, terminate it:

```bash
sudo pkill -TERM cylk-helper
```

As a last resort, switch to a virtual terminal or reboot.

## Under the hood

```text
src/main.c          CLI, application startup, stylesheet loading
src/window.c        GTK4/libadwaita UI and state machine
src/device.c        udev/sysfs keyboard discovery, scoring, and companions
src/companion-keys.h  shared list of blocked hotkey and button codes
src/config.c        persisted duration and manual device choice
src/helper.c        pkexec subprocess and line protocol
src/session.c       logind lock, suspend, and active-session monitor
helper/cylk-helper.c  privileged grab/release helper
```

The helper protocol is line based:

```text
UI -> helper:  GRAB\t<path>\t<hold_ms>\t<max_seconds>[\t<companion>…]
UI -> helper:  RELEASE
UI -> helper:  QUIT
helper -> UI:  READY\t2
helper -> UI:  OK\t<device name>
helper -> UI:  RELEASED\t<reason>
helper -> UI:  ERR\t<code>\t<message>
```

This keeps the privileged code small and auditable.

For a complete walkthrough (device model, companion detection, grab lifecycle
and how to add a blocked key), read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
(also available in Spanish: [docs/ARQUITECTURA.es.md](docs/ARQUITECTURA.es.md)).
See [CONTRIBUTING.md](CONTRIBUTING.md) for build, packaging and debugging notes.

## Packaging

Build recipes live in:

- `packaging/fedora/clean-your-lap-keyboard.spec`
- `debian/` (Debian and Ubuntu)
- `packaging/arch/PKGBUILD`

Fedora RPM:

```bash
mkdir -p ~/rpmbuild/SOURCES
git archive --format=tar.gz \
  --prefix=clean-your-lap-keyboard-0.1.1/ \
  -o ~/rpmbuild/SOURCES/clean-your-lap-keyboard-0.1.1.tar.gz HEAD
rpmbuild -ba packaging/fedora/clean-your-lap-keyboard.spec
```

Debian/Ubuntu:

```bash
dpkg-buildpackage -us -uc -b
```

Arch:

```bash
cp packaging/arch/PKGBUILD .
makepkg -si
```

## Contributing

Pull requests and hardware reports are welcome. If automatic detection fails on
your laptop, include the output of:

```bash
cylk --list
udevadm info --query=property --name=/dev/input/eventN
```

Please do not include your polkit password or any private session data.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
