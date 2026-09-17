sources = files(
  'main.cpp',
  'engine/mlt_engine.cpp',
  'engine/waveform_cache.cpp',
  'ui/app_window.cpp',
  'util/log.cpp',
)

executable(
  'u-studio-video-editor',
  sources,
  dependencies: [gtk4_dep, adwaita_dep, mlt_dep, mltpp_dep, glib_dep, gio_dep, gobject_dep, pulse_dep],
  install: true,
)
