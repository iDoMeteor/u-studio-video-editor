doctest_dep = dependency(
  'doctest',
  required: get_option('tests'),
  fallback: ['doctest', 'doctest_dep'],
)

if doctest_dep.found()
  subdir('core')
  subdir('engine')
endif
