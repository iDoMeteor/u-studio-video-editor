test_core_exe = executable(
  'test_core',
  files('test_log.cpp', 'test_model.cpp', 'test_commands.cpp', 'test_timeline_edits.cpp', 'test_xml.cpp'),
  dependencies: [doctest_dep, libustudio_core_dep],
  include_directories: include_directories('../common'),
  install: false,
)

# The 10k-iteration random-command property test alone runs ~28-30s on the
# dev machine, already against meson's 30s default before this file added
# any commands to the fuzzer's repertoire; give the whole binary headroom
# rather than let it flake on machine load.
test('core', test_core_exe, timeout: 90)

# doc 19 MT0: the worker pool, its own binary so `just tsan` can run it on
# its own quickly.
test_thread_pool_exe = executable(
  'test_thread_pool',
  files('test_thread_pool.cpp'),
  dependencies: [doctest_dep, libustudio_core_dep],
  install: false,
)

test('core-thread-pool', test_thread_pool_exe, timeout: 60)
