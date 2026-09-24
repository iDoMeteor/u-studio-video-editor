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
  include_directories: include_directories('../common'),
  install: false,
)

test('engine-sync', test_engine_sync_exe)

# doc 12, M1: verify() across the whole 10k-command property test. Takes
# tens of minutes, so it's the `slow` suite, excluded by tests/meson.build's
# default setup: run it with `meson test -C builddir --suite slow`.
test('engine-sync-property-10k', test_engine_sync_exe, args: ['--no-skip', '--test-case=*10k-command*'],
     suite: 'slow', timeout: 0)

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

test_thumbnail_cache_exe = executable(
  'test_thumbnail_cache',
  files('test_thumbnail_cache.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

test('engine-thumbnail-cache', test_thumbnail_cache_exe)

test_render_exe = executable(
  'test_render',
  files('test_render.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

# Runs a real libx264/aac avformat encode (short, 5 frames) -- a bit
# slower than the synthetic-only engine tests, hence its own timeout.
test('engine-render', test_render_exe, timeout: 30)

test_av_sync_exe = executable(
  'test_av_sync',
  files('test_av_sync.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

# doc 12, M2: the automated half of the A/V sync box (renders a clip and
# plays it in real time, so a few seconds).
test('engine-av-sync', test_av_sync_exe, timeout: 60)

# Writes the same clip for the by-eye half: build/tests/engine/make_sync_clip out.mp4
executable(
  'make_sync_clip',
  files('make_sync_clip.cpp'),
  dependencies: [libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

# doc 12, M2 soak (minutes of real playback): run by hand, not by meson test.
#   build/tests/engine/playback_soak <4k60.mp4> 10
executable(
  'playback_soak',
  files('playback_soak.cpp'),
  dependencies: [libustudio_engine_dep, libustudio_core_dep, glib_dep],
  install: false,
)
