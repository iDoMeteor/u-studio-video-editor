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
