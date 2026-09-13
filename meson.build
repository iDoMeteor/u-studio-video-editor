project(
  'u-studio-video-editor', 'cpp',
  version: '0.0.1',
  license: 'GPL-3.0-or-later',
  default_options: ['cpp_std=c++17', 'warning_level=2'],
)

gtk4_dep = dependency('gtk4', version: '>=4.10')
adwaita_dep = dependency('libadwaita-1')
mlt_dep = dependency('mlt-framework-7')
mltpp_dep = dependency('mlt++-7')
pulse_dep = dependency('libpulse-simple')
glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
gobject_dep = dependency('gobject-2.0')

subdir('src')
