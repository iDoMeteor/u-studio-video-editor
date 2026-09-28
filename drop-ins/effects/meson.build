# The effects and transitions drop-in (doc 15, ADR-011, ADR-013, ADR-014):
# descriptors, commands and probe results (core/), the registry, frei0r
# curation, the engine extension and the render tool's subcommands
# (engine/). Built when -Ddropin_effects isn't 'disabled'. frei0r-plugins is
# this drop-in's runtime dependency, never the editor's.
effects_inc = include_directories('.')

# core/: std and src/core only (tools/dropin_boundary_check.sh).
effects_core = static_library(
  'ustudio_effects_core',
  files('core/commands.cpp', 'core/descriptor.cpp', 'core/health.cpp', 'core/json.cpp'),
  include_directories: effects_inc,
  dependencies: [libustudio_core_dep],
  pic: true,
  install: false,
)
effects_core_dep = declare_dependency(
  link_with: effects_core,
  include_directories: effects_inc,
  dependencies: [libustudio_core_dep],
)

# Curated overlays (and, from FX3, transition recipes and luma maps).
effects_data_install_dir = get_option('datadir') / 'u-studio' / 'drop-ins' / 'effects'
install_subdir('data/overlays', install_dir: effects_data_install_dir)
effects_args = [
  '-DUSTUDIO_VERSION="' + meson.project_version() + '"',
  '-DEFFECTS_DATA_INSTALL_DIR="' + get_option('prefix') / effects_data_install_dir + '"',
  # Running from a build directory: this source tree's data.
  '-DEFFECTS_DATA_SOURCE_DIR="' + meson.current_source_dir() / 'data' + '"',
]

effects_engine_sources = files('engine/effects_extension.cpp', 'engine/plugins.cpp', 'engine/probe.cpp',
                               'engine/registry.cpp')
# app/: GIO for the scan's children; no MLT (tools/dropin_boundary_check.sh).
effects_app_sources = files('app/health_scan.cpp')
effects_dropin_sources = files('register.cpp') + effects_engine_sources + effects_app_sources

if get_option('dropin_effects') == 'module'
  # A module resolves core, engine and drop-in host symbols against
  # the program (export_dynamic, those libraries linked whole: src/app and
  # src/render), so it takes their headers only; a second copy of core's
  # globals (the log) inside the module would be a different one.
  shared_module(
    'ustudio-dropin-effects',
    effects_dropin_sources,
    cpp_args: effects_args + ['-DEFFECTS_DROPIN_MODULE'],
    gnu_symbol_visibility: 'hidden',
    # platform/ is stateless and the programs don't link it whole, so a
    # platform function the program itself never calls (the curated frei0r
    # directory's) must come with the module.
    link_with: [effects_core, libustudio_platform],
    include_directories: [src_inc, effects_inc],
    dependencies: [
      libustudio_engine_dep.partial_dependency(compile_args: true, includes: true),
      libustudio_dropins_dep.partial_dependency(compile_args: true, includes: true),
      mltpp_dep, mlt_dep, glib_dep, gio_dep, libxml2_dep,
    ],
    install: true,
    install_dir: dropin_install_dir,
  )
else
  effects_builtin = static_library(
    'ustudio_dropin_effects',
    effects_dropin_sources,
    cpp_args: effects_args,
    include_directories: [src_inc, effects_inc],
    dependencies: [effects_core_dep, libustudio_engine_dep, libustudio_dropins_dep, libustudio_platform_dep, glib_dep,
                   gio_dep],
    install: false,
  )
  dropin_effects_dep = declare_dependency(link_with: effects_builtin, dependencies: [effects_core_dep, glib_dep, gio_dep])
endif

# The drop-in's own tests. tests/meson.build runs after drop-ins/, so look
# doctest up here (meson caches the lookup).
effects_doctest_dep = dependency('doctest', required: get_option('tests'), fallback: ['doctest', 'doctest_dep'])
if effects_doctest_dep.found()
  subdir('tests')
endif
