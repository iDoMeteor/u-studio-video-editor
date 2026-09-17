test_core_exe = executable(
  'test_core',
  files('test_log.cpp'),
  dependencies: [doctest_dep, libustudio_core_dep],
  install: false,
)

test('core', test_core_exe)
