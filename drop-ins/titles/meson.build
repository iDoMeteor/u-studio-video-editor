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
  files('core/animation.cpp', 'core/brand_kit.cpp', 'core/clip_fields.cpp', 'core/evaluate.cpp', 'core/export_formats.cpp', 'core/snapping.cpp', 'core/title_document.cpp', 'core/title_edit.cpp', 'core/title_xml.cpp'),
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
  dependencies: [titles_core_dep, libustudio_platform_dep, titles_pangocairo_dep, titles_cairo_dep,
                 titles_fontconfig_dep],
  pic: true,
  install: false,
)
titles_render_dep = declare_dependency(
  link_with: titles_render,
  include_directories: titles_inc,
  dependencies: [titles_core_dep, libustudio_platform_dep, titles_pangocairo_dep, titles_cairo_dep,
                 titles_fontconfig_dep],
)

subdir('mltmodule')
subdir('app')

# Where the editor and u-studio-render find libmltustudio.so (IP4): the
# installed copy, or this build's when running from the build directory.
titles_register_args = [
  '-DUSTUDIO_VERSION="' + meson.project_version() + '"',
  '-DTITLES_MLT_INSTALL_DIR="' + titles_mlt_install_dir + '"',
  '-DTITLES_MLT_BUILD_DIR="' + titles_mlt_build_dir + '"',
  # Running from a build directory: the designer built next to it.
  '-DTITLES_APP_BUILD_PATH="' + titles_app_exe.full_path() + '"',
]
# The editor's side: the engine extension (IP3), the render tool's
# --title-frames (IP6) and the shell's import and file watch (IP5).
titles_dropin_sources = files('register.cpp', 'editor/jobs.cpp', 'editor/title_bake.cpp', 'editor/title_launch.cpp', 'editor/title_page.cpp', 'editor/title_shell.cpp',
                              'engine/backdrop.cpp', 'engine/title_export.cpp', 'engine/title_extension.cpp',
                              'engine/title_frames.cpp')
if get_option('dropin_titles') == 'module'
  # A module resolves core, engine and drop-in host symbols against the
  # program (built with export_dynamic and those libraries linked whole,
  # src/app and src/render), so it takes their headers only: a second copy
  # of core's globals (the log) inside the module would be a different one.
  shared_module(
    'ustudio-dropin-titles',
    titles_dropin_sources,
    cpp_args: titles_register_args + ['-DTITLES_DROPIN_MODULE'],
    gnu_symbol_visibility: 'hidden',
    link_with: titles_core,
    include_directories: [src_inc, titles_inc],
    dependencies: [
      libustudio_engine_dep.partial_dependency(compile_args: true, includes: true),
      libustudio_dropins_dep.partial_dependency(compile_args: true, includes: true),
      libustudio_platform_dep.partial_dependency(compile_args: true, includes: true),
      mltpp_dep, mlt_dep, gtk4_dep, libxml2_dep,
    ],
    install: true,
    install_dir: dropin_install_dir,
  )
else
  titles_builtin = static_library(
    'ustudio_dropin_titles',
    titles_dropin_sources,
    cpp_args: titles_register_args,
    include_directories: [src_inc, titles_inc],
    dependencies: [titles_core_dep, libustudio_engine_dep, libustudio_dropins_dep, libustudio_platform_dep, gtk4_dep],
    install: false,
  )
  dropin_titles_dep = declare_dependency(link_with: titles_builtin, dependencies: [titles_core_dep, gtk4_dep])
endif

# u-studio-titles' desktop entry, the .ustitle MIME type and its AppStream
# metadata (drafts until the owner's new logos; Icon= is a placeholder).
install_data('data/com.ustudio.Titles.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/com.ustudio.Titles.metainfo.xml', install_dir: get_option('datadir') / 'metainfo')
install_data('data/com.ustudio.Titles.mime.xml', install_dir: get_option('datadir') / 'mime' / 'packages',
             rename: 'com.ustudio.Titles.xml')

# The drop-in's own tests. tests/meson.build runs after drop-ins/, so look
# doctest up here (meson caches the lookup).
titles_doctest_dep = dependency('doctest', required: get_option('tests'), fallback: ['doctest', 'doctest_dep'])
if titles_doctest_dep.found()
  subdir('tests')
endif
