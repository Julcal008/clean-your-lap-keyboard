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

## Pull requests

- Keep the privileged helper small and auditable.
- Do not add key logging or event-stream reading to the helper.
- Add tests for detection or parsing changes when possible.
- Update both `README.md` and `README.es.md` for user-facing changes.
- Run `meson test` before opening a pull request.

## License

By contributing, you agree that your changes are licensed under
GPL-3.0-or-later.
