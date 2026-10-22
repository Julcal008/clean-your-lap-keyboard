# Contributing

Thanks for taking the time to improve Clean Your Lap Keyboard.

## Reporting hardware problems

Automatic detection is heuristic. If the wrong keyboard is selected, please
include:

```bash
cylk --list
udevadm info --query=property --name=/dev/input/eventN
cat /sys/class/input/eventN/device/name
```

Do not include passwords, polkit credentials, or private session data.

## Development

```bash
meson setup build --buildtype=debug
meson compile -C build
meson test -C build --print-errorlogs
```

The UI can be previewed without touching any input device:

```bash
./build/src/cylk --demo=idle
./build/src/cylk --demo=cleaning
```

Real grab tests require root or `pkexec`, and should always include an automatic
release. Never leave a keyboard grabbed while editing or debugging.

## Repository layout

```text
src/                  Application and shared logic
  main.c              CLI options and startup
  window.c            GTK4/libadwaita UI and state machine
  device.c/.h         Keyboard and companion discovery
  companion-keys.h    The list of blocked key codes
  config.c/.h         Persisted settings
  helper.c/.h         Helper client (line protocol)
  session.c/.h        logind monitor
helper/cylk-helper.c  Privileged grab/release helper
tests/                Unit tests (run with meson test)
data/                 Desktop file, icon, man page, metainfo, polkit policy
docs/                 Architecture notes (English and Spanish)
po/                   Translations
packaging/            RPM, DEB and Arch packaging files
```

See `docs/ARCHITECTURE.md` for the full design and data flow.

## Adding a blocked key

The keys that must not work while cleaning are listed in one place,
`src/companion-keys.h`:

- `CYLK_COMPANION_KEY_LIST` for media and hotkey codes (volume, mute,
  brightness, radio switches, ...);
- `CYLK_BUTTON_KEY_LIST` for physical buttons (power, sleep, suspend).

Add the `KEY_*` macro to the right list, add a test in `tests/test-device.c` if
needed, and run `meson test`. Both the UI and the helper include the same header,
so the key is recognized on both sides.

## Diagnosing devices

```bash
cylk --list           # selectable keyboards
cylk --list-all       # keyboards plus companion hotkey devices
sudo evtest           # which device emits a given key
```

If a key still works during cleaning, find the event device that emits it with
`evtest` and check whether the companion detection should consider it. Report
the output of `cylk --list-all` and `evtest` (never include passwords).

## Packaging and releasing

The packages are built by GitHub Actions when a tag is pushed:

```bash
git tag -a v0.1.2 -m "Clean Your Lap Keyboard 0.1.2"
git push origin v0.1.2
```

The `Release` workflow builds the Fedora RPM, the Debian package and the Arch
package and publishes them in a GitHub Release. Remember to bump the version in
`meson.build`, `CHANGELOG.md`, `debian/changelog`, `packaging/` and the metainfo
before tagging.

## Pull requests

- Keep the privileged helper small and auditable.
- Do not add key logging or event-stream reading to the helper.
- Add tests for detection or parsing changes when possible.
- Update both `README.md` and `README.es.md` for user-facing changes.
- Run `meson test` before opening a pull request.

## License

By contributing, you agree that your changes are licensed under
GPL-3.0-or-later.
