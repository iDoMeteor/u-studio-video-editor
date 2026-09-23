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
  },
)
