# The test drop-in (tests/dropins/test_dropin.cpp), built linked-in and as
# modules, and the loader tests that use it (ADR-014; doc 17, "Testing").
test_dropin_args = ['-DUSTUDIO_VERSION="' + meson.project_version() + '"']
# The shell layer includes the generated tokens.h from src/app's build dir.
app_inc = include_directories('../../src/app')

test_dropin_builtin = static_library(
  'ustudio_test_dropin',
  files('test_dropin.cpp', 'engine/test_extension.cpp', 'engine/test_probe.cpp', 'app/test_shell.cpp'), app_tokens_h,
  cpp_args: test_dropin_args + ['-DTEST_DROPIN_NAME="testdropin"'],
  include_directories: [src_inc, app_inc],
  dependencies: [mltpp_dep, mlt_dep, gtk4_dep, app_timeline_dep],
  install: false,
)

# Modules in their own directory, so the loader sees exactly these.
subdir('modules')

test_dropins_exe = executable(
  'test_dropins',
  files('test_dropins.cpp'),
  cpp_args: ['-DTEST_MODULE_DIR="' + test_dropin_module_dir + '"', '-DUSTUDIO_VERSION="' + meson.project_version() + '"'],
  link_with: test_dropin_builtin,
  dependencies: [doctest_dep, libustudio_dropins_dep],
  install: false,
)
test('dropins', test_dropins_exe, depends: test_dropin_modules)

# The test drop-in's core layer: effect commands on Model's IP1 mutators,
# and IP1's property test.
test_dropin_core = static_library(
  'ustudio_test_dropin_core',
  files('core/effect_commands.cpp'),
  dependencies: [libustudio_core_dep],
  install: false,
)
test_dropin_model_exe = executable(
  'test_dropin_model',
  files('test_dropin_model.cpp'),
  include_directories: include_directories('core'),
  link_with: test_dropin_core,
  dependencies: [doctest_dep, libustudio_core_dep],
  install: false,
)
test('dropin-model', test_dropin_model_exe, timeout: 120)

# IP3: EngineSync with the test drop-in's EngineExtension, built in and as a
# module. The module resolves core and engine symbols against this program,
# so it exports them and links those libraries whole, as the editor must
# for real modules (doc 15, step 0's REVIEW note).
test_dropin_engine_exe = executable(
  'test_dropin_engine',
  files('test_dropin_engine.cpp'),
  cpp_args: ['-DTEST_MODULE_DIR="' + test_dropin_module_dir + '"'],
  include_directories: include_directories('.'),
  link_with: test_dropin_builtin,
  link_whole: [libustudio_core, libustudio_engine, libustudio_dropins],
  export_dynamic: true,
  dependencies: [doctest_dep, libustudio_dropins_dep],
  install: false,
)
test('dropin-engine', test_dropin_engine_exe, depends: test_dropin_modules, timeout: 120)

# IP6: u-studio-render's command line with the test drop-in's subcommand,
# built in and as a module (linked and exported as for dropin-engine).
if get_option('render_tool')
  test_dropin_render_exe = executable(
    'test_dropin_render',
    files('test_dropin_render.cpp'),
    cpp_args: ['-DTEST_MODULE_DIR="' + test_dropin_module_dir + '"'],
    include_directories: include_directories('.'),
    link_with: [test_dropin_builtin, libustudio_render_cli],
    link_whole: [libustudio_core, libustudio_engine, libustudio_dropins],
    export_dynamic: true,
    dependencies: [doctest_dep, libustudio_dropins_dep],
    install: false,
  )
  test('dropin-render', test_dropin_render_exe, depends: test_dropin_modules, timeout: 120)
endif

# IP5: the editor with the test drop-in built in -- the real main.cpp and
# window, for trying every shell host live (never installed). The module
# build of the drop-in loads into it too (USTUDIO_DROPIN_PATH), since it
# links the libraries whole and exports them as a module-mode editor must.
test_editor_exe = executable(
  'u-studio-video-editor-testdropin',
  files('../../src/app/main.cpp'),
  app_resources,
  cpp_args: ['-DUSTUDIO_VERSION="' + meson.project_version() + '"'],
  include_directories: [include_directories('editor'), include_directories('../../src/app')],
  link_with: test_dropin_builtin,
  link_whole: [libustudio_core, libustudio_engine, libustudio_dropins, app_timeline_lib],
  export_dynamic: true,
  dependencies: [app_window_dep],
  install: false,
)

# IP5: the test drop-in's shell layer against a recording ShellHost, built
# in and as a module (linked and exported as for dropin-engine).
test_dropin_shell_exe = executable(
  'test_dropin_shell',
  files('test_dropin_shell.cpp'),
  cpp_args: ['-DTEST_MODULE_DIR="' + test_dropin_module_dir + '"'],
  include_directories: include_directories('.'),
  link_with: test_dropin_builtin,
  link_whole: [libustudio_core, libustudio_engine, libustudio_dropins, app_timeline_lib],
  export_dynamic: true,
  dependencies: [doctest_dep, libustudio_dropins_dep, gtk4_dep],
  install: false,
)
test('dropin-shell', test_dropin_shell_exe, depends: test_dropin_modules, timeout: 60)
