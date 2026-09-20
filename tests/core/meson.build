test_core_exe = executable(
  'test_core',
  files('test_log.cpp', 'test_model.cpp', 'test_commands.cpp', 'test_xml.cpp'),
  dependencies: [doctest_dep, libustudio_core_dep],
  install: false,
)

# The 10k-iteration random-command property test alone runs ~28-30s on the
# dev machine, already against meson's 30s default before this file added
# any commands to the fuzzer's repertoire; give the whole binary headroom
# rather than let it flake on machine load.
test('core', test_core_exe, timeout: 90)
