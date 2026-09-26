test_autosave_exe = executable(
  'test_autosave',
  files('test_autosave.cpp'),
  dependencies: [doctest_dep, app_autosave_dep],
  install: false,
)

# autosave::directory() is $XDG_STATE_HOME/ustudio/autosave -- without this
# override that's the owner's real autosave/recovery directory (CLAUDE.md:
# verification output never lands next to real app/user state). Pointing
# XDG_STATE_HOME at a build-dir-local scratch path isolates the test
# completely instead of relying on "doctest-" filename prefixes alone.
test(
  'app-autosave',
  test_autosave_exe,
  env: {'XDG_STATE_HOME': meson.current_build_dir() / 'autosave-test-state'},
)

test_snap_env_exe = executable(
  'test_snap_env',
  files('test_snap_env.cpp'),
  dependencies: [doctest_dep, app_settings_dep],
  install: false,
)
test('app-snap-env', test_snap_env_exe)

test_render_queue_exe = executable(
  'test_render_queue',
  files('test_render_queue.cpp'),
  dependencies: [doctest_dep, app_render_queue_dep],
  install: false,
)
test('app-render-queue', test_render_queue_exe)

test_render_profiles_exe = executable(
  'test_render_profiles',
  files('test_render_profiles.cpp'),
  dependencies: [doctest_dep, app_settings_dep],
  install: false,
)
test('app-render-profiles', test_render_profiles_exe)

test_settings_exe = executable(
  'test_settings',
  files('test_settings.cpp'),
  dependencies: [doctest_dep, app_settings_dep],
  install: false,
)

# depends: orders this after data/'s gschemas.compiled actually exists on
# disk (build-graph ordering, not just meson-configure-time variable
# visibility) -- GSETTINGS_BACKEND=memory keeps every read/write isolated
# from the real system dconf database (see test_settings.cpp's own
# comment).
test(
  'app-settings',
  test_settings_exe,
  env: {
    'GSETTINGS_SCHEMA_DIR': meson.project_build_root() / 'data',
    'GSETTINGS_BACKEND': 'memory',
  },
  depends: ustudio_gschemas_compiled,
)

test_settings_missing_schema_exe = executable(
  'test_settings_missing_schema',
  files('test_settings_missing_schema.cpp'),
  dependencies: [doctest_dep, app_settings_dep],
  install: false,
)

test(
  'app-settings-missing-schema',
  test_settings_missing_schema_exe,
  env: {
    'GSETTINGS_SCHEMA_DIR': meson.current_build_dir() / 'no-such-schema-dir',
    'XDG_DATA_DIRS': meson.current_build_dir() / 'empty-xdg-data-dir',
    # The build-dir fallback case writes a setting: keep it off dconf.
    'GSETTINGS_BACKEND': 'memory',
  },
  depends: ustudio_gschemas_compiled,
)

test_portal_path_exe = executable(
  'test_portal_path',
  files('test_portal_path.cpp'),
  dependencies: [doctest_dep, app_portal_dep, gio_dep],
  install: false,
)

# Talks to the real session's xdg-document-portal (skips itself when there
# isn't one, e.g. in a CI container).
test('app-portal-path', test_portal_path_exe, workdir: meson.current_build_dir())

test_ui_hints_exe = executable(
  'test_ui_hints',
  files('test_ui_hints.cpp'),
  dependencies: [doctest_dep, app_window_dep],
  cpp_args: ['-DUSTUDIO_APP_WINDOW_CPP="' + meson.project_source_root() / 'src' / 'app' / 'app_window.cpp' + '"'],
  install: false,
)

test('app-ui-hints', test_ui_hints_exe)

# Doc 15 IP5: row lanes, the preview mapping, contributed actions.
test_shell_hosts_exe = executable(
  'test_shell_hosts',
  files('test_shell_hosts.cpp'),
  dependencies: [doctest_dep, app_window_dep],
  install: false,
)
test('app-shell-hosts', test_shell_hosts_exe)

# M4 C: the proxy queue, with a fake u-studio-render.
test_proxy_queue_exe = executable(
  'test_proxy_queue',
  files('test_proxy_queue.cpp'),
  dependencies: [doctest_dep, app_window_dep],
  install: false,
)
test('app-proxy-queue', test_proxy_queue_exe)

test_viewport_exe = executable(
  'test_viewport',
  files('test_viewport.cpp'),
  dependencies: [doctest_dep, app_timeline_dep],
  install: false,
)

test('app-viewport', test_viewport_exe)

# M4 F2: the preview's transform handles, as arithmetic.
test_transform_gestures_exe = executable(
  'test_transform_gestures',
  files('test_transform_gestures.cpp'),
  dependencies: [doctest_dep, app_timeline_dep],
  install: false,
)
test('app-transform-gestures', test_transform_gestures_exe)

# M4 D: the media browser's badges.
test_media_badges_exe = executable(
  'test_media_badges',
  files('test_media_badges.cpp'),
  dependencies: [doctest_dep, app_timeline_dep],
  install: false,
)
test('app-media-badges', test_media_badges_exe)

test_timeline_controller_exe = executable(
  'test_timeline_controller',
  files('test_timeline_controller.cpp'),
  dependencies: [doctest_dep, app_timeline_dep],
  install: false,
)

test('app-timeline-controller', test_timeline_controller_exe)

test_timeline_render_exe = executable(
  'test_timeline_render',
  files('test_timeline_render.cpp'),
  dependencies: [doctest_dep, app_window_dep],
  install: false,
)

# doc 12, M3's snapshot budget. Prints the measured times.
test('app-timeline-render', test_timeline_render_exe)

test_timeline_fuzz_exe = executable(
  'test_timeline_fuzz',
  files('test_timeline_fuzz.cpp'),
  dependencies: [doctest_dep, app_timeline_dep],
  install: false,
)

test('app-timeline-fuzz', test_timeline_fuzz_exe, timeout: 120)

test_stall_monitor_exe = executable(
  'test_stall_monitor',
  files('test_stall_monitor.cpp'),
  dependencies: [doctest_dep, app_window_dep],
  install: false,
)

# USTUDIO_LOG_LEVEL=debug turns the monitor on in release test builds too.
test('app-stall-monitor', test_stall_monitor_exe, env: ['USTUDIO_LOG_LEVEL=debug'])

test_import_queue_exe = executable(
  'test_import_queue',
  files('test_import_queue.cpp'),
  dependencies: [doctest_dep, app_window_dep],
  install: false,
)

test('app-import-queue', test_import_queue_exe, timeout: 60)

test_save_queue_exe = executable(
  'test_save_queue',
  files('test_save_queue.cpp'),
  dependencies: [doctest_dep, app_window_dep],
  install: false,
)

test('app-save-queue', test_save_queue_exe, timeout: 60)

test_project_loader_exe = executable(
  'test_project_loader',
  files('test_project_loader.cpp'),
  dependencies: [doctest_dep, app_window_dep],
  install: false,
)

test('app-project-loader', test_project_loader_exe, timeout: 60)
