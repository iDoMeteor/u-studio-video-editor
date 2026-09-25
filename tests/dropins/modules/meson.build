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
    files('../test_dropin.cpp'),
    cpp_args: test_dropin_args + variant[1] + ['-DTEST_DROPIN_MODULE', '-DTEST_DROPIN_NAME="' + variant[0] + '"'],
    gnu_symbol_visibility: 'hidden',
    include_directories: src_inc,
    install: false,
  )
endforeach
