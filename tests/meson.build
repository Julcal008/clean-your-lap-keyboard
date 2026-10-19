test_device = executable(
  'test-device',
  'test-device.c',
  link_with: cylk_core,
  include_directories: include_directories('../src'),
  dependencies: [
    gio_dep,
    udev_dep,
  ],
)

test('device', test_device)
