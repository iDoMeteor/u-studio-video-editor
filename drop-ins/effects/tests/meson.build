effects_test_core = executable(
  'test_effects_core',
  files('test_core.cpp'),
  dependencies: [effects_doctest_dep, effects_core_dep],
  install: false,
)
test('effects-core', effects_test_core)

# IP3/IP4/IP6 against real MLT and the installed frei0r plugins.
effects_engine_args = effects_args
# Stock melt, for "melt renders a saved project with effects" (FX1); the
# check is skipped without it.
effects_melt = find_program('melt-7', 'melt', required: false)
if effects_melt.found()
  effects_engine_args += '-DEFFECTS_MELT="' + effects_melt.full_path() + '"'
endif
effects_test_engine = executable(
  'test_effects_engine',
  files('test_engine.cpp', '../register.cpp') + effects_engine_sources + effects_app_sources,
  cpp_args: effects_engine_args,
  include_directories: [src_inc, effects_inc],
  dependencies: [effects_doctest_dep, effects_core_dep, libustudio_engine_dep, libustudio_dropins_dep,
                 libustudio_platform_dep, glib_dep, gio_dep, gtk4_dep],
  install: false,
)
test('effects-engine', effects_test_engine, timeout: 300)

# Two frei0r plugins broken on purpose (frei0r/broken_plugin.cpp), alone in
# their directory, which the scan test uses as the whole FREI0R_PATH.
effects_broken_plugins = []
foreach plugin : [['crashy', []], ['hangy', ['-DBROKEN_HANG']]]
  effects_broken_plugins += shared_module(
    plugin[0],
    files('frei0r/broken_plugin.cpp'),
    cpp_args: plugin[1],
    name_prefix: '',
    build_by_default: true,
    install: false,
  )
endforeach
effects_broken_dir = meson.current_build_dir()

# The editor's scan against the real render tool: a crashing plugin and a
# hanging one are quarantined, a working effect isn't, and the test (the
# editor's stand-in) keeps running.
if get_option('render_tool')
  effects_scan_args = effects_args + [
    '-DEFFECTS_RENDER_TOOL="' + meson.project_build_root() / 'src' / 'render' / 'u-studio-render' + '"',
    '-DEFFECTS_BROKEN_PLUGIN_DIR="' + effects_broken_dir + '"',
  ]
  if get_option('dropin_effects') == 'module'
    # An uninstalled render tool finds the module only through the
    # development override (ADR-014).
    effects_scan_args += '-DEFFECTS_DROPIN_MODULE_DIR="' + meson.current_build_dir() / '..' + '"'
  endif
  effects_test_scan = executable(
    'test_effects_scan',
    files('test_scan.cpp') + effects_engine_sources + effects_app_sources,
    cpp_args: effects_scan_args,
    include_directories: [src_inc, effects_inc],
    dependencies: [effects_doctest_dep, effects_core_dep, libustudio_engine_dep, libustudio_platform_dep, glib_dep,
                   gio_dep, gtk4_dep],
    install: false,
  )
  test('effects-scan', effects_test_scan, depends: effects_broken_plugins, timeout: 180)
endif
