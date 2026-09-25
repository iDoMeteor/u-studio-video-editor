# The test drop-in (tests/dropins/test_dropin.cpp), built linked-in and as
# modules, and the loader tests that use it (ADR-014; doc 17, "Testing").
test_dropin_args = ['-DUSTUDIO_VERSION="' + meson.project_version() + '"']

test_dropin_builtin = static_library(
  'ustudio_test_dropin',
  files('test_dropin.cpp'),
  cpp_args: test_dropin_args + ['-DTEST_DROPIN_NAME="testdropin"'],
  include_directories: src_inc,
  install: false,
)

# Modules in their own directory, so the loader sees exactly these.
subdir('modules')

test_dropins_exe = executable(
  'test_dropins',
  files('test_dropins.cpp'),
  cpp_args: ['-DTEST_MODULE_DIR="' + test_dropin_module_dir + '"', '-DUSTUDIO_VERSION="' + meson.project_version() + '"'],
  link_with: test_dropin_builtin,
  dependencies: [doctest_dep, libustudio_dropins_dep],
  install: false,
)
test('dropins', test_dropins_exe, depends: test_dropin_modules)
