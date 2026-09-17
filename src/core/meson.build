libustudio_core_sources = files(
  'log.cpp',
)

libustudio_core = static_library(
  'ustudio_core',
  libustudio_core_sources,
  dependencies: [libxml2_dep],
  include_directories: src_inc,
  install: false,
)

# Consumers link against this, not against libustudio_core directly, so the
# include path (and, once core/ actually uses libxml2 from M1 onward) the
# dependency propagate automatically -- a plain static_library()'s own
# dependencies: do NOT auto-propagate to whatever links it;
# declare_dependency() is the mechanism that does.
libustudio_core_dep = declare_dependency(
  link_with: libustudio_core,
  include_directories: src_inc,
  dependencies: [libxml2_dep],
)
