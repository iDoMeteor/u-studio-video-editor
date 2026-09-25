# Drop-in loading and the host they register with (ADR-013, ADR-014). Core,
# engine and GModule; no GTK, so the render tool uses it too.
gmodule_dep = dependency('gmodule-2.0', include_type: 'system')

dropin_install_dir = get_option('prefix') / get_option('libdir') / 'u-studio' / 'drop-ins'

libustudio_dropins = static_library(
  'ustudio_dropins',
  files('registry.cpp'),
  cpp_args: [
    '-DUSTUDIO_VERSION="' + meson.project_version() + '"',
    '-DUSTUDIO_DROPIN_DIR="' + dropin_install_dir + '"',
  ],
  dependencies: [libustudio_core_dep, gmodule_dep],
  include_directories: src_inc,
  install: false,
)
libustudio_dropins_dep = declare_dependency(
  link_with: libustudio_dropins,
  include_directories: src_inc,
  dependencies: [libustudio_core_dep, gmodule_dep],
)
