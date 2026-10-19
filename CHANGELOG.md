# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.1] - 2026-10-19

### Fixed

- Media and function keys (volume, microphone mute, brightness) no longer keep
  working during cleaning. The helper now grabs the companion hotkey devices
  (`Video Bus`, `ThinkPad Extra Buttons`, and similar) in addition to the
  selected keyboard.
- The power and sleep buttons are grabbed as well while cleaning, so they cannot
  be pressed by accident. Holding the power button for several seconds still
  forces a hardware power-off.

### Added

- `cylk --list-all` shows keyboards and companion hotkey devices for
  troubleshooting.
- The helper protocol reports its version so an outdated helper is detected.

## [0.1.0] - 2026-10-19

### Added

- Automatic detection of internal laptop keyboards using udev and sysfs.
- Manual keyboard selector with persisted choice.
- Compact GTK4/libadwaita interface with idle, countdown, cleaning, and done
  states.
- 30, 60, and 120 second cleaning durations, defaulting to 60 seconds.
- Three-second countdown and Stop button.
- Release by holding Esc for two seconds.
- Automatic release on timer expiry, window close, process exit, screen lock,
  suspend, and inactive session.
- Privileged `cylk-helper` launched on demand through `pkexec`, with a polkit
  policy for active local sessions.
- English and Spanish translations.
- Manual page for `cylk`.
- Fedora RPM spec, Debian packaging, and Arch Linux PKGBUILD.
- Unit tests for keyboard scoring and capability bitmap parsing.
- GitHub Actions CI for Fedora, Debian, Ubuntu, and Arch Linux.

[0.1.1]: https://github.com/julcal008/clean-your-lap-keyboard/releases/tag/v0.1.1
[0.1.0]: https://github.com/julcal008/clean-your-lap-keyboard/releases/tag/v0.1.0
