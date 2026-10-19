# SPDX-License-Identifier: GPL-3.0-or-later

po_dir = join_paths(meson.project_source_root(), 'po')

desktop_file = i18n.merge_file(
  input: 'io.github.julcal008.CleanYourLapKeyboard.desktop.in',
  output: 'io.github.julcal008.CleanYourLapKeyboard.desktop',
  type: 'desktop',
  po_dir: po_dir,
  install: true,
  install_dir: join_paths(datadir, 'applications'),
)

metainfo_file = i18n.merge_file(
  input: 'io.github.julcal008.CleanYourLapKeyboard.metainfo.xml.in',
  output: 'io.github.julcal008.CleanYourLapKeyboard.metainfo.xml',
  type: 'xml',
  po_dir: po_dir,
  install: true,
  install_dir: join_paths(datadir, 'metainfo'),
)

install_data(
  'icons/hicolor/scalable/apps/io.github.julcal008.CleanYourLapKeyboard.svg',
  install_dir: join_paths(datadir, 'icons', 'hicolor', 'scalable', 'apps'),
)

policy = configure_file(
  input: 'io.github.julcal008.CleanYourLapKeyboard.policy.in',
  output: 'io.github.julcal008.CleanYourLapKeyboard.policy',
  configuration: {'HELPER_PATH': helper_path},
)

install_data(
  policy,
  install_dir: join_paths(datadir, 'polkit-1', 'actions'),
)

install_man('cylk.1')
