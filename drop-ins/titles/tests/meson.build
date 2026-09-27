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
