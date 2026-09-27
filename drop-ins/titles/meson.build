# The titles drop-in (doc 16, ADR-012, ADR-013): a .ustitle document model
# (core/), its Pango/Cairo renderer (render/), and the editor's side of it
# (register.cpp). Built when -Ddropin_titles isn't 'disabled'.
#
# Pango, PangoCairo, Cairo and fontconfig are already in the editor's
# process through GTK4; ADR-012 records linking them into non-GTK layers.
titles_pangocairo_dep = dependency('pangocairo', include_type: 'system')
titles_cairo_dep = dependency('cairo', include_type: 'system')
titles_fontconfig_dep = dependency('fontconfig', include_type: 'system')
titles_inc = include_directories('.')

# core/: std, libxml2 and src/core only (tools/dropin_boundary_check.sh).
titles_core = static_library(
  'ustudio_titles_core',
  files('core/evaluate.cpp', 'core/title_document.cpp', 'core/title_xml.cpp'),
  include_directories: titles_inc,
  dependencies: [libustudio_core_dep],
  pic: true,
  install: false,
)
titles_core_dep = declare_dependency(
  link_with: titles_core,
  include_directories: titles_inc,
  dependencies: [libustudio_core_dep],
)

# render/ (titlerender): Pango, Cairo, fontconfig; no GTK, no MLT.
titles_render = static_library(
  'ustudio_titlerender',
  files('render/blur.cpp', 'render/title_renderer.cpp'),
  include_directories: titles_inc,
  dependencies: [titles_core_dep, titles_pangocairo_dep, titles_cairo_dep, titles_fontconfig_dep],
  pic: true,
  install: false,
)
titles_render_dep = declare_dependency(
  link_with: titles_render,
  include_directories: titles_inc,
  dependencies: [titles_core_dep, titles_pangocairo_dep, titles_cairo_dep, titles_fontconfig_dep],
)

titles_register_args = ['-DUSTUDIO_VERSION="' + meson.project_version() + '"']
if get_option('dropin_titles') == 'module'
  shared_module(
    'ustudio-dropin-titles',
    files('register.cpp'),
    cpp_args: titles_register_args + ['-DTITLES_DROPIN_MODULE'],
    gnu_symbol_visibility: 'hidden',
    include_directories: [src_inc, titles_inc],
    dependencies: [titles_render_dep, mltpp_dep, mlt_dep],
    install: true,
    install_dir: dropin_install_dir,
  )
else
  titles_builtin = static_library(
    'ustudio_dropin_titles',
    files('register.cpp'),
    cpp_args: titles_register_args,
    include_directories: [src_inc, titles_inc],
    dependencies: [titles_render_dep, mltpp_dep, mlt_dep],
    install: false,
  )
  dropin_titles_dep = declare_dependency(link_with: titles_builtin, dependencies: [titles_render_dep])
endif

# The drop-in's own tests. tests/meson.build runs after drop-ins/, so look
# doctest up here (meson caches the lookup).
titles_doctest_dep = dependency('doctest', required: get_option('tests'), fallback: ['doctest', 'doctest_dep'])
if titles_doctest_dep.found()
  subdir('tests')
endif
