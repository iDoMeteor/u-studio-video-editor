test_factory_policy_exe = executable(
  'test_factory_policy',
  files('test_factory_policy.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep],
  install: false,
)

test('engine-factory-policy', test_factory_policy_exe)

test_engine_sync_exe = executable(
  'test_engine_sync',
  files('test_engine_sync.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

test('engine-sync', test_engine_sync_exe)

test_xml_playback_exe = executable(
  'test_xml_playback',
  files('test_xml_playback.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

test('engine-xml-playback', test_xml_playback_exe)
