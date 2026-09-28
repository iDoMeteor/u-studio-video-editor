# The effects and transitions drop-in (doc 15, ADR-011, ADR-013, ADR-014):
# descriptors, commands and probe results (core/), the registry, frei0r
# curation, the engine extension and the render tool's subcommands
# (engine/). Built when -Ddropin_effects isn't 'disabled'. frei0r-plugins is
# this drop-in's runtime dependency, never the editor's.
effects_inc = include_directories('.')

# core/: std and src/core only (tools/dropin_boundary_check.sh).
effects_core = static_library(
  'ustudio_effects_core',
  files('core/commands.cpp', 'core/descriptor.cpp', 'core/health.cpp', 'core/json.cpp', 'core/keyframes.cpp', 'core/looks.cpp',
        'core/transitions.cpp', 'core/blocks.cpp'),
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
install_subdir('data/looks', install_dir: effects_data_install_dir)
install_subdir('data/transitions', install_dir: effects_data_install_dir)
effects_args = [
  '-DUSTUDIO_VERSION="' + meson.project_version() + '"',
  '-DEFFECTS_DATA_INSTALL_DIR="' + get_option('prefix') / effects_data_install_dir + '"',
  # A package that brings its own MLT frei0r module and frei0r plugins (the
  # Flatpak extension: the core app's MLT is built without frei0r, ADR-014)
  # installs them here; used when they exist, else MLT's and the system's.
  '-DEFFECTS_MLT_INSTALL_DIR="' + get_option('prefix') / get_option('libdir') / 'u-studio' / 'mlt' + '"',
  '-DEFFECTS_FREI0R_INSTALL_DIR="' + get_option('prefix') / get_option('libdir') / 'frei0r-1' + '"',
  # Running from a build directory: this source tree's data.
  '-DEFFECTS_DATA_SOURCE_DIR="' + meson.current_source_dir() / 'data' + '"',
]

effects_engine_sources = files('engine/effects_extension.cpp', 'engine/frame_renderer.cpp', 'engine/plugins.cpp',
                               'engine/probe.cpp', 'engine/registry.cpp')
# app/: GTK for the Rack, GIO for the scan's children; no MLT
# (tools/dropin_boundary_check.sh).
# The Transitions page's tile and outline colours, from the editor's
# style.css (drop-ins/ builds before src/app, so its own copy, as titles').
effects_app_tokens_h = custom_target(
  'effects-app-tokens-h',
  input: meson.project_source_root() / 'src' / 'app' / 'style' / 'style.css',
  output: 'tokens.h',
  command: [find_program('python3'), meson.project_source_root() / 'tools' / 'gen_tokens.py', '@INPUT@', '@OUTPUT@'],
)
effects_app_sources = files('app/browser.cpp', 'app/catalog.cpp', 'app/compare.cpp', 'app/health_scan.cpp',
                              'app/rack.cpp', 'app/transitions_page.cpp', 'app/curve_lanes.cpp', 'app/fx_lane.cpp') + [effects_app_tokens_h]
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
      mltpp_dep, mlt_dep, glib_dep, gio_dep, gtk4_dep, libxml2_dep,
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
                   gio_dep, gtk4_dep],
    install: false,
  )
  dropin_effects_dep = declare_dependency(link_with: effects_builtin, dependencies: [effects_core_dep, glib_dep, gio_dep, gtk4_dep])
endif

# The drop-in's own tests. tests/meson.build runs after drop-ins/, so look
# doctest up here (meson caches the lookup).
effects_doctest_dep = dependency('doctest', required: get_option('tests'), fallback: ['doctest', 'doctest_dep'])
if effects_doctest_dep.found()
  subdir('tests')
endif
