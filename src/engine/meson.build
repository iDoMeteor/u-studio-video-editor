libustudio_engine_sources = files(
  'factory_policy.cpp',
  'dispatcher.cpp',
  'playback_controller.cpp',
  'waveform_cache.cpp',
  'engine_sync.cpp',
)

# The real MLT module directory (e.g. /usr/lib64/mlt-7), baked in at build
# time so FactoryPolicy (ADR-007) knows what to curate without needing to
# ask pkg-config at runtime. mlt-framework-7.pc is the one that defines
# `moduledir` -- mlt++-7.pc does not (verified: empty output querying it).
mlt_module_dir = mlt_dep.get_variable(pkgconfig: 'moduledir')
factory_policy_args = ['-DUSTUDIO_MLT_MODULE_DIR="' + mlt_module_dir + '"']

libustudio_engine = static_library(
  'ustudio_engine',
  libustudio_engine_sources,
  cpp_args: factory_policy_args,
  dependencies: [libustudio_core_dep, mltpp_dep, mlt_dep, glib_dep],
  include_directories: src_inc,
  install: false,
)

libustudio_engine_dep = declare_dependency(
  link_with: libustudio_engine,
  include_directories: src_inc,
  dependencies: [libustudio_core_dep, mltpp_dep, mlt_dep, glib_dep],
)
