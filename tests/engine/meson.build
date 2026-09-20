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

test_playback_controller_exe = executable(
  'test_playback_controller',
  files('test_playback_controller.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep],
  install: false,
)

# Runs a live sdl2_audio/rtaudio/null consumer and repeats a 100-iteration
# shutdown loop -- slower than the other engine tests, hence its own timeout.
test('engine-playback-controller', test_playback_controller_exe, timeout: 60)

test_probe_media_exe = executable(
  'test_probe_media',
  files('test_probe_media.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

test('engine-probe-media', test_probe_media_exe)

test_waveform_cache_exe = executable(
  'test_waveform_cache',
  files('test_waveform_cache.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

test('engine-waveform-cache', test_waveform_cache_exe)
