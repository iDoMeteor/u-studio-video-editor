test_factory_policy_exe = executable(
  'test_factory_policy',
  files('test_factory_policy.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep],
  install: false,
)

test('engine-factory-policy', test_factory_policy_exe)

# core::easedValue() against MLT's own interpolation, every easing.
test_eased_value_exe = executable(
  'test_eased_value',
  files('test_eased_value.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)
test('engine-eased-value', test_eased_value_exe)

subdir('factory_paths')
test_factory_paths_exe = executable(
  'test_factory_paths',
  files('test_factory_paths.cpp'),
  cpp_args: ['-DTEST_MODULE_DIR="' + factory_paths_module_dir + '"'],
  dependencies: [doctest_dep, libustudio_engine_dep],
  install: false,
)
test('engine-factory-paths', test_factory_paths_exe, depends: factory_paths_module)

test_engine_sync_exe = executable(
  'test_engine_sync',
  files('test_engine_sync.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  include_directories: include_directories('../common'),
  install: false,
)

# 9 s alone, but 31 s (past meson's default 30) on the owner's machine under
# load (2026-09-25): headroom like the other decoding tests.
test('engine-sync', test_engine_sync_exe, timeout: 90)

test_xml_playback_exe = executable(
  'test_xml_playback',
  files('test_xml_playback.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

test('engine-xml-playback', test_xml_playback_exe, timeout: 90)

test_playback_controller_exe = executable(
  'test_playback_controller',
  files('test_playback_controller.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep],
  install: false,
)

# Runs a live sdl2_audio/rtaudio/null consumer and repeats a 100-iteration
# shutdown loop -- slower than the other engine tests, hence its own timeout.
#
# SDL's dummy audio driver: silent, but paced at the profile fps like a real
# device, so the test behaves the same with or without one (and plays nothing
# through the owner's speakers). Without it, on a machine with no audio
# device (a clean Fedora 44 container, 2026-09-24): the first sdl2_audio
# start succeeds but fails to open audio, every later SDL init in the process
# fails, rtaudio's start() returns 0 but never shows a frame, and the test
# times out.
test('engine-playback-controller', test_playback_controller_exe, timeout: 60,
     env: ['SDL_AUDIODRIVER=dummy'])

test_probe_media_exe = executable(
  'test_probe_media',
  files('test_probe_media.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

test('engine-probe-media', test_probe_media_exe)

# M4 B: missing media plays a placeholder; relink restores it exactly.
test_missing_media_exe = executable(
  'test_missing_media',
  files('test_missing_media.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep, libustudio_platform_dep],
  install: false,
)
test('engine-missing-media', test_missing_media_exe, timeout: 60)

# composite's fill (a picture smaller than the project fills the frame).
test_composite_fill_exe = executable(
  'test_composite_fill',
  files('test_composite_fill.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep, libustudio_platform_dep],
  install: false,
)
test('engine-composite-fill', test_composite_fill_exe, timeout: 60)

# M4 C: proxies (render, toggle, fallback, originals for export).
test_proxy_exe = executable(
  'test_proxy',
  files('test_proxy.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep, libustudio_platform_dep],
  install: false,
)
test('engine-proxy', test_proxy_exe, timeout: 120)

# ADR-019 G1: per-producer hardware decode, worker producers off the GPU
# chain. The device is one that can't exist, so the software fallback is
# what runs everywhere (docs/developer/notes/gpu.md).
test_hardware_decode_exe = executable(
  'test_hardware_decode',
  files('test_hardware_decode.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep, libustudio_platform_dep],
  install: false,
)
test('engine-hardware-decode', test_hardware_decode_exe, timeout: 120,
     env: ['MLT_AVFORMAT_HWACCEL_DEVICE=/nonexistent/ustudio-render-node'])

# ADR-019 G2: our own GL context and the GPU probe; again with EGL pointed
# nowhere, where the probe must fail cleanly.
test_gpu_probe_exe = executable(
  'test_gpu_probe',
  files('test_gpu_probe.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep, libustudio_platform_dep],
  install: false,
)
test('engine-gpu-probe', test_gpu_probe_exe, timeout: 60)
test('engine-gpu-probe-no-egl', test_gpu_probe_exe, timeout: 60,
     env: ['USTUDIO_EGL_LIBRARY=/nonexistent/libEGL.so.1'])

# ADR-019 G3: the GPU graph against the CPU one, scene by scene.
test_gpu_pipeline_exe = executable(
  'test_gpu_pipeline',
  files('test_gpu_pipeline.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep, libustudio_platform_dep],
  install: false,
)
test('engine-gpu-pipeline', test_gpu_pipeline_exe, timeout: 180)

# ADR-019 G3: the GPU pipeline through Engine and the real consumer.
test_gpu_engine_exe = executable(
  'test_gpu_engine',
  files('test_gpu_engine.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep, glib_dep],
  install: false,
)
test('engine-gpu-engine', test_gpu_engine_exe, timeout: 120, is_parallel: false,
     env: ['SDL_AUDIODRIVER=dummy'])

# ADR-018 (M4 F1): clip transforms in the engine.
test_transform_exe = executable(
  'test_transform',
  files('test_transform.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep, libustudio_platform_dep],
  install: false,
)
test('engine-transform', test_transform_exe, timeout: 180)

# BT.709 sources keep their colours (the 601-tagged background).
test_colour_exe = executable(
  'test_colour',
  files('test_colour.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep, libustudio_platform_dep],
  install: false,
)
test('engine-colour', test_colour_exe, timeout: 120)

# M4 E: image sequences through pixbuf.
test_image_sequence_exe = executable(
  'test_image_sequence',
  files('test_image_sequence.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep, libustudio_platform_dep],
  install: false,
)
test('engine-image-sequence', test_image_sequence_exe, timeout: 60)

test_waveform_cache_exe = executable(
  'test_waveform_cache',
  files('test_waveform_cache.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

# Serial: a perf case waits a bounded time for a long clip's peaks, which a
# machine loaded with the rest of the suite (or two sanitizer suites) can miss.
test('engine-waveform-cache', test_waveform_cache_exe, is_parallel: false)

test_thumbnail_cache_exe = executable(
  'test_thumbnail_cache',
  files('test_thumbnail_cache.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

# Decodes real frames on its worker; ~11 s alone, past meson's default 30 s
# on a loaded machine (2026-09-24), hence its own timeout.
test('engine-thumbnail-cache', test_thumbnail_cache_exe, timeout: 90)

test_render_exe = executable(
  'test_render',
  files('test_render.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

# Runs a real libx264/aac avformat encode (short, 5 frames) -- a bit
# slower than the synthetic-only engine tests, hence its own timeout.
test('engine-render', test_render_exe, timeout: 90)

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

# doc 19 MT2 piece 0b: chunked track playlists against the flat graph --
# frames, audio, verify(), paused seeks/steps, loops and a render.
test_chunked_playlist_exe = executable(
  'test_chunked_playlist',
  files('test_chunked_playlist.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep, glib_dep],
  install: false,
)

# 178 s under ASan (2026-09-24): the render and every-frame hashing.
test('engine-chunked-playlist', test_chunked_playlist_exe, timeout: 400)

# doc 19 MT2 piece 2: the engine thread through its facade.
test_engine_thread_exe = executable(
  'test_engine_thread',
  files('test_engine_thread.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep, glib_dep],
  install: false,
)

# Serial: one case holds façade calls to a 1 ms median, which a machine
# loaded with the rest of the suite can't meet. The budget's meaning stays.
test('engine-thread', test_engine_thread_exe, timeout: 180, is_parallel: false)

# "Sync tracks (audio)": decoding and matching on a rendered file.
test_audio_sync_exe = executable(
  'test_audio_sync',
  files('test_audio_sync.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

test('engine-audio-sync', test_audio_sync_exe, timeout: 120)

# Clips of eight frame rates in 30 and 24 fps projects (doc 12, "Frame rate"):
# generates its own lossless sources, so give it room.
test_mixed_rates_exe = executable(
  'test_mixed_rates',
  files('test_mixed_rates.cpp'),
  dependencies: [doctest_dep, libustudio_engine_dep, libustudio_core_dep],
  install: false,
)

test('engine-mixed-rates', test_mixed_rates_exe, timeout: 180)
