test_core_exe = executable(
  'test_core',
  files('test_log.cpp', 'test_model.cpp', 'test_commands.cpp', 'test_timeline_edits.cpp', 'test_xml.cpp',
        'test_audio_align.cpp', 'test_backup.cpp', 'test_render_profile.cpp', 'test_profile_match.cpp',
        'test_retime.cpp', 'test_import_batch.cpp'),
  dependencies: [doctest_dep, libustudio_core_dep],
  include_directories: include_directories('../common'),
  cpp_args: ['-DTEST_DATA_DIR="' + meson.current_source_dir() / 'data' + '"'],
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

# doc 19 MT0: Model::snapshot() on a 5,000-clip project. A benchmark, not a
# test: `meson test -C builddir --benchmark core-snapshot` (or run it).
bench_snapshot_exe = executable(
  'bench_snapshot',
  files('bench_snapshot.cpp'),
  dependencies: [libustudio_core_dep],
  install: false,
)
benchmark('core-snapshot', bench_snapshot_exe)

# Model's release-build safety: core compiled again, here, with NDEBUG, so
# the precondition asserts are gone and the safe paths behind them run.
test_model_release_exe = executable(
  'test_model_release',
  files('test_model_release.cpp'),
  libustudio_core_sources,
  cpp_args: ['-DNDEBUG'],
  dependencies: [doctest_dep, libxml2_dep, threads_dep],
  include_directories: [src_inc, include_directories('../common')],
  install: false,
)
test('core-model-release', test_model_release_exe)
