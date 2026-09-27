titles_test_core = executable(
  'test_titles_core',
  files('test_core.cpp'),
  dependencies: [titles_doctest_dep, titles_core_dep],
  install: false,
)
test('titles-core', titles_test_core)

titles_test_render = executable(
  'test_titles_render',
  files('test_render.cpp'),
  cpp_args: ['-DTITLES_TEST_FONT_DIR="' + meson.current_build_dir() / 'fonts' + '"'],
  dependencies: [titles_doctest_dep, titles_render_dep],
  install: false,
)
test('titles-render', titles_test_render, timeout: 120)

# IP3/IP4/IP6: the producer module in real graphs, and u-studio-render's
# --title-frames (defined after drop-ins/, so named by path; `meson test`
# builds everything first).
titles_engine_args = ['-DTITLES_MLT_BUILD_DIR="' + titles_mlt_build_dir + '"']
if get_option('render_tool')
  titles_engine_args += '-DTITLES_RENDER_TOOL="' + meson.project_build_root() / 'src' / 'render' / 'u-studio-render' + '"'
  if get_option('dropin_titles') == 'module'
    # An uninstalled render tool finds the module only through the
    # development override (ADR-014).
    titles_engine_args += '-DTITLES_DROPIN_MODULE_DIR="' + meson.current_build_dir() / '..' + '"'
  endif
endif
titles_test_engine = executable(
  'test_titles_engine',
  files('test_engine.cpp', '../engine/backdrop.cpp', '../engine/title_export.cpp', '../engine/title_extension.cpp',
        '../engine/title_frames.cpp'),
  cpp_args: titles_engine_args,
  include_directories: [src_inc, titles_inc],
  dependencies: [titles_doctest_dep, titles_render_dep, libustudio_engine_dep, libustudio_platform_dep, glib_dep],
  install: false,
)
test('titles-engine', titles_test_engine, depends: titles_mlt_module, timeout: 180)

# IP5: import and the file watch, with a fake ShellHost.
titles_test_shell = executable(
  'test_titles_shell',
  files('test_shell.cpp', '../editor/title_shell.cpp', '../editor/title_launch.cpp', '../engine/backdrop.cpp'),
  cpp_args: ['-DTITLES_APP_BUILD_PATH="' + titles_app_exe.full_path() + '"'],
  include_directories: [src_inc, titles_inc],
  dependencies: [titles_doctest_dep, titles_core_dep, libustudio_engine_dep, libustudio_platform_dep, gtk4_dep],
  install: false,
)
test('titles-shell', titles_test_shell, timeout: 60)

titles_test_edit = executable(
  'test_titles_edit',
  files('test_edit.cpp'),
  cpp_args: ['-DTITLES_BRAND_XML="' + meson.current_source_dir() / '..' / 'data' / 'brand.xml' + '"'],
  dependencies: [titles_doctest_dep, titles_core_dep],
  install: false,
)
test('titles-edit', titles_test_edit)

# The app's render worker (the app's only thread), without a display.
titles_test_worker = executable(
  'test_titles_worker',
  files('test_worker.cpp', '../app/render_worker.cpp', '../app/behavior_drawer.cpp'),
  include_directories: [src_inc, titles_inc],
  dependencies: [titles_doctest_dep, titles_render_dep, gtk4_dep],
  install: false,
)
test('titles-worker', titles_test_worker)

# The packaging drafts stay valid (when the validators are installed).
desktop_file_validate = find_program('desktop-file-validate', required: false)
if desktop_file_validate.found()
  test('titles-desktop-file', desktop_file_validate, args: [meson.current_source_dir() / '..' / 'data' / 'com.ustudio.Titles.desktop'])
endif
appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('titles-metainfo', appstreamcli,
       args: ['validate', '--no-net', meson.current_source_dir() / '..' / 'data' / 'com.ustudio.Titles.metainfo.xml'])
endif

# T3: animators, behaviours, loops, the scramble, the cursor, detaching.
titles_test_animation = executable(
  'test_titles_animation',
  files('test_animation.cpp'),
  dependencies: [titles_doctest_dep, titles_core_dep],
  install: false,
)
test('titles-animation', titles_test_animation)
