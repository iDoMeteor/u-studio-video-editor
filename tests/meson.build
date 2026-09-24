doctest_dep = dependency(
  'doctest',
  required: get_option('tests'),
  fallback: ['doctest', 'doctest_dep'],
)

if doctest_dep.found()
  subdir('core')
  subdir('engine')
  subdir('app')
endif

# The `slow` suite (tests that take many minutes) stays out of plain
# `meson test`; run it explicitly with `meson test --suite slow`.
add_test_setup('default', is_default: true, exclude_suites: ['slow'])
