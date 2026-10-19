# SPDX-License-Identifier: GPL-3.0-or-later

gtk_dep = dependency('gtk4', version: '>=4.8')
adw_dep = dependency('libadwaita-1', version: '>=1.2')
gio_dep = dependency('gio-2.0')
udev_dep = dependency('libudev')
m_dep = cc.find_library('m', required: false)

cylk_core = static_library(
  'cylk-core',
  [
    'config.c',
    'device.c',
    'helper.c',
    'session.c',
  ],
  dependencies: [
    gio_dep,
    udev_dep,
  ],
  install: false,
)

cylk_resources = gnome.compile_resources(
  'cylk-resources',
  'resources/cylk.gresource.xml',
  source_dir: 'resources',
  c_name: 'cylk',
)

cylk = executable(
  'cylk',
  [
    'main.c',
    'window.c',
    cylk_resources,
  ],
  dependencies: [
    gtk_dep,
    adw_dep,
    gio_dep,
    m_dep,
  ],
  c_args: [
    '-Wall',
    '-Wextra',
    '-Wformat=2',
    '-Wshadow',
    '-Wstrict-prototypes',
    '-Wmissing-prototypes',
  ],
  link_with: cylk_core,
  install: true,
)
