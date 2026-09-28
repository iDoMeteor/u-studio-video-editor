# u-studio-titles (doc 16, T2): the title designer. GTK4 + libadwaita and
# titlerender; no MLT (tools/dropin_boundary_check.sh).
titles_gnome = import('gnome')
titles_app_resources = titles_gnome.compile_resources(
  'titles-resources',
  'titles.gresource.xml',
  # The editor's stylesheet: both apps share the Unicorn Tears tokens.
  source_dir: [meson.project_source_root() / 'src' / 'app' / 'style', meson.current_source_dir() / '..' / 'data'],
  c_name: 'ustudio_titles',
)
# The canvas's overlay colours, from the same style.css (as the editor's
# timeline takes its own).
titles_app_tokens_h = custom_target(
  'titles-app-tokens-h',
  input: meson.project_source_root() / 'src' / 'app' / 'style' / 'style.css',
  output: 'tokens.h',
  command: [find_program('python3'), meson.project_source_root() / 'tools' / 'gen_tokens.py', '@INPUT@', '@OUTPUT@'],
)
# Export runs u-studio-render (the designer has no MLT): installed next to
# it, else this build's (defined after drop-ins/, so named by path).
titles_app_args = ['-DTITLES_RENDER_BUILD_PATH="' + meson.project_build_root() / 'src' / 'render' / 'u-studio-render' + '"']
# The built-in templates: installed beside the program's share/, else this
# source tree's (running from the build directory).
titles_app_args += '-DTITLES_TEMPLATES_SOURCE_DIR="' + meson.current_source_dir() / '..' / 'data' / 'templates' + '"'
if get_option('dropin_titles') == 'module'
  # An uninstalled render tool finds the module only through the
  # development override (ADR-014).
  titles_app_args += '-DTITLES_DROPIN_MODULE_DIR="' + meson.current_build_dir() / '..' + '"'
endif
titles_app_lib = static_library(
  'ustudio_titles_app',
  files('animation_strip.cpp', 'behavior_drawer.cpp', 'canvas.cpp', 'export.cpp', 'gallery.cpp', 'inspector.cpp', 'packs.cpp', 'layers_panel.cpp', 'render_worker.cpp', 'titles_window.cpp',
        'view_settings.cpp'),
  titles_app_tokens_h,
  include_directories: [src_inc, titles_inc],
  cpp_args: titles_app_args,
  dependencies: [titles_render_dep, titles_package_dep, libustudio_platform_dep, gtk4_dep, adwaita_dep],
  install: false,
)
titles_app_dep = declare_dependency(
  link_with: titles_app_lib,
  sources: titles_app_tokens_h,
  include_directories: [src_inc, titles_inc],
  dependencies: [titles_render_dep, titles_package_dep, libustudio_platform_dep, gtk4_dep, adwaita_dep],
)
titles_app_exe = executable(
  'u-studio-titles',
  files('main.cpp'),
  titles_app_resources,
  dependencies: [titles_app_dep],
  install: true,
)
