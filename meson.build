project(
  'u-studio-video-editor', 'cpp',
  version: '0.0.1',
  license: 'GPL-3.0-or-later',
  default_options: [
    'cpp_std=c++23',
    'warning_level=3',
  ],
)

cpp = meson.get_compiler('cpp')

# include_type: 'system' makes meson pass these via -isystem instead of -I,
# so GCC's default suppression of warnings from system headers applies --
# needed because -Wold-style-cast etc. otherwise fire on GTK/GLib's own
# macros (G_TYPE_CHECK_INSTANCE_CAST, G_CALLBACK, G_DEFINE_AUTOPTR_CLEANUP_FUNC)
# at every call site, which we don't control and can't fix upstream.
gtk4_dep = dependency('gtk4', version: '>=4.10', include_type: 'system')
adwaita_dep = dependency('libadwaita-1', include_type: 'system')
mlt_dep = dependency('mlt-framework-7', include_type: 'system')
mltpp_dep = dependency('mlt++-7', include_type: 'system')
glib_dep = dependency('glib-2.0', include_type: 'system')
gio_dep = dependency('gio-2.0', include_type: 'system')
gobject_dep = dependency('gobject-2.0', include_type: 'system')
libxml2_dep = dependency('libxml-2.0', include_type: 'system')

# Extra warnings beyond warning_level=3 (doc 11). -Werror is deliberately
# NOT part of default_options -- it's gated behind -Dwerror so local dev
# builds don't hard-fail the first time these flags surface pre-existing
# warnings; CI sets -Dwerror=true.
extra_warning_flags = cpp.get_supported_arguments(
  '-Wshadow',
  '-Wconversion',
  '-Wold-style-cast',
  '-Wnon-virtual-dtor',
)
add_project_arguments(extra_warning_flags, language: 'cpp')

# -Werror itself is meson's own built-in option (-Dwerror=true); no need to
# add it manually -- meson already does when that's set. CI sets it; local
# dev builds default to false.

subdir('src')
subdir('data')
subdir('tests')
