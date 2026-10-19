cylk_helper = executable(
  'cylk-helper',
  'cylk-helper.c',
  include_directories: include_directories('..'),
  c_args: [
    '-Wall',
    '-Wextra',
    '-Wpedantic',
    '-Wformat=2',
    '-Wshadow',
    '-Wstrict-prototypes',
    '-Wmissing-prototypes',
  ],
  install: true,
  install_dir: helper_install_dir,
)
