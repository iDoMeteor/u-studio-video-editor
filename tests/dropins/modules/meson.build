test_dropin_module_dir = meson.current_build_dir()
test_dropin_modules = []
foreach variant : [
    ['moduledropin', []],
    ['wrongapi', ['-DTEST_WRONG_API']],
    ['wrongapp', ['-DTEST_WRONG_APP']],
    ['nodescribe', ['-DTEST_NO_DESCRIBE']],
  ]
  test_dropin_modules += shared_module(
    'ustudio-dropin-' + variant[0],
    files('../test_dropin.cpp', '../engine/test_extension.cpp', '../engine/test_probe.cpp', '../app/test_shell.cpp'), app_tokens_h,
    cpp_args: test_dropin_args + variant[1] + ['-DTEST_DROPIN_MODULE', '-DTEST_DROPIN_NAME="' + variant[0] + '"'],
    gnu_symbol_visibility: 'hidden',
    include_directories: [src_inc, app_inc],
    dependencies: [mltpp_dep, mlt_dep, gtk4_dep],
    install: false,
  )
endforeach
