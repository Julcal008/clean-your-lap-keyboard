project(
  'clean-your-lap-keyboard',
  'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>=0.62.0',
  default_options: [
    'warning_level=2',
    'c_std=gnu11',
    'buildtype=release',
  ],
)

gnome = import('gnome')
i18n = import('i18n')
fs = import('fs')

cc = meson.get_compiler('c')

prefix = get_option('prefix')
datadir = get_option('datadir')
localedir_opt = get_option('localedir')
libexecdir_opt = get_option('libexecdir')

if fs.is_absolute(localedir_opt)
  localedir = localedir_opt
else
  localedir = join_paths(prefix, localedir_opt)
endif

if fs.is_absolute(libexecdir_opt)
  libexecdir = libexecdir_opt
else
  libexecdir = join_paths(prefix, libexecdir_opt)
endif

helper_dir = join_paths(libexecdir, meson.project_name())
helper_path = join_paths(helper_dir, 'cylk-helper')
helper_install_dir = join_paths(libexecdir_opt, meson.project_name())

conf = configuration_data()
conf.set_quoted('VERSION', meson.project_version())
conf.set_quoted('APP_ID', 'io.github.julcal008.CleanYourLapKeyboard')
conf.set_quoted('GETTEXT_PACKAGE', meson.project_name())
conf.set_quoted('LOCALEDIR', localedir)
conf.set_quoted('HELPER_PATH', helper_path)
conf.set_quoted('PKGDATADIR', join_paths(prefix, datadir, meson.project_name()))

config_h = configure_file(
  output: 'cylk-config.h',
  configuration: conf,
)

add_project_arguments('-DHAVE_CONFIG_H', language: 'c')
add_project_arguments('-D_GNU_SOURCE', language: 'c')
add_project_arguments(
  '-I' + meson.project_source_root(),
  '-I' + meson.project_build_root(),
  language: 'c',
)

subdir('src')
subdir('helper')
subdir('data')
subdir('po')

if get_option('tests')
  subdir('tests')
endif
