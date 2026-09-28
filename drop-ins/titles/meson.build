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
  files('core/animation.cpp', 'core/brand_kit.cpp', 'core/clip_fields.cpp', 'core/evaluate.cpp', 'core/template_library.cpp', 'core/export_formats.cpp', 'core/snapping.cpp', 'core/title_document.cpp', 'core/title_edit.cpp', 'core/title_xml.cpp'),
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

# package/ (doc 20, T4b): template packs. The manifest, validator and
# library install; std, libxml2 and GLib (SHA-256). No network.
# Archives through libarchive (ADR-020: this drop-in only). Without its
# headers the drop-in still builds, and opening or saving a pack says so.
titles_archive_dep = dependency('libarchive', required: false)
titles_package_args = titles_archive_dep.found() ? ['-DTITLES_HAVE_LIBARCHIVE'] : []
titles_package = static_library(
  'ustudio_titles_package',
  files('package/archive.cpp', 'package/pack.cpp'),
  include_directories: titles_inc,
  cpp_args: titles_package_args,
  dependencies: [titles_core_dep, glib_dep, libxml2_dep, titles_archive_dep],
  pic: true,
  install: false,
)
titles_package_dep = declare_dependency(
  link_with: titles_package,
  include_directories: titles_inc,
  compile_args: titles_package_args,
  dependencies: [titles_core_dep, glib_dep, libxml2_dep, titles_archive_dep],
)

# share/ (ADR-020, T7): u-studio-share, the only code with network access,
# in a program of its own (never linked into the editor or u-studio-titles).
titles_share_option = get_option('titles_share')
titles_soup_dep = dependency('libsoup-3.0', required: titles_share_option)
titles_json_dep = dependency('json-glib-1.0', required: titles_share_option)
titles_secret_dep = dependency('libsecret-1', required: false)
titles_share_enabled = titles_share_option.allowed() and titles_soup_dep.found() and titles_json_dep.found() and \
                       titles_archive_dep.found()
if titles_share_enabled
  titles_share_args = titles_secret_dep.found() ? ['-DTITLES_HAVE_LIBSECRET'] : []
  titles_share = static_library(
    'ustudio_titles_share',
    files('share/client.cpp', 'share/handoff.cpp', 'share/signin.cpp'),
    cpp_args: titles_share_args,
    include_directories: [src_inc, titles_inc],
    dependencies: [titles_package_dep, titles_soup_dep, titles_json_dep, titles_secret_dep, gio_dep],
    install: false,
  )
  titles_share_dep = declare_dependency(
    link_with: titles_share,
    include_directories: [src_inc, titles_inc],
    dependencies: [titles_package_dep, titles_soup_dep, titles_json_dep, titles_secret_dep, gio_dep],
  )
  # The helper: its own program (its own Flatpak app, com.ustudio.Share).
  titles_share_resources = import('gnome').compile_resources(
    'share-resources',
    'share/share.gresource.xml',
    source_dir: meson.project_source_root() / 'src' / 'app' / 'style',
    c_name: 'ustudio_share',
  )
  titles_share_exe = executable(
    'u-studio-share',
    files('share/main.cpp'),
    titles_share_resources,
    dependencies: [titles_share_dep, libustudio_core_dep, gtk4_dep, adwaita_dep],
    install: true,
  )
endif

subdir('mltmodule')
subdir('app')

# Where the editor and u-studio-render find libmltustudio.so (IP4): the
# installed copy, or this build's when running from the build directory.
titles_register_args = [
  '-DUSTUDIO_VERSION="' + meson.project_version() + '"',
  '-DTITLES_MLT_INSTALL_DIR="' + titles_mlt_install_dir + '"',
  '-DTITLES_MLT_BUILD_DIR="' + titles_mlt_build_dir + '"',
  # Packaged apart from the editor (a Flatpak extension): its own bindir.
  '-DTITLES_APP_INSTALL_DIR="' + get_option('prefix') / get_option('bindir') + '"',
  # Running from a build directory: the designer built next to it.
  '-DTITLES_APP_BUILD_PATH="' + titles_app_exe.full_path() + '"',
]
# The editor's side: the engine extension (IP3), the render tool's
# --title-frames (IP6) and the shell's import and file watch (IP5).
# The drop-in's own resources in the editor (its header button's icon).
titles_editor_resources = titles_gnome.compile_resources(
  'titles-editor-resources',
  'editor/titles-editor.gresource.xml',
  source_dir: 'data',
  c_name: 'ustudio_titles_editor',
)
titles_dropin_sources = [titles_editor_resources] + files('register.cpp', 'editor/jobs.cpp', 'editor/title_bake.cpp', 'editor/title_launch.cpp', 'editor/title_page.cpp', 'editor/title_shell.cpp',
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
    gnu_symbol_visibility: 'hidden',
    link_with: [titles_core, titles_package],
    include_directories: [src_inc, titles_inc],
    dependencies: [
      libustudio_engine_dep.partial_dependency(compile_args: true, includes: true),
      libustudio_dropins_dep.partial_dependency(compile_args: true, includes: true),
      libustudio_platform_dep.partial_dependency(compile_args: true, includes: true),
      mltpp_dep, mlt_dep, gtk4_dep, libxml2_dep, titles_archive_dep,
    ],
    cpp_args: titles_register_args + ['-DTITLES_DROPIN_MODULE'] + titles_package_args,
    install: true,
    install_dir: dropin_install_dir,
  )
else
  titles_builtin = static_library(
    'ustudio_dropin_titles',
    titles_dropin_sources,
    cpp_args: titles_register_args,
    include_directories: [src_inc, titles_inc],
    dependencies: [titles_core_dep, titles_package_dep, libustudio_engine_dep, libustudio_dropins_dep,
                   libustudio_platform_dep, gtk4_dep],
    install: false,
  )
  dropin_titles_dep = declare_dependency(link_with: titles_builtin, dependencies: [titles_core_dep, gtk4_dep])
endif

# u-studio-titles' desktop entry, the .ustitle MIME type and its AppStream
# metadata (drafts until the owner's new logos; Icon= is a placeholder).
install_data('data/com.ustudio.Titles.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/com.ustudio.Titles.metainfo.xml', install_dir: get_option('datadir') / 'metainfo')
# The built-in templates (doc 16, T4.2), found by u-studio-titles at
# <prefix>/share/u-studio/titles/templates.
install_subdir('data/templates', install_dir: get_option('datadir') / 'u-studio' / 'titles')
install_data('data/com.ustudio.Titles.mime.xml', install_dir: get_option('datadir') / 'mime' / 'packages',
             rename: 'com.ustudio.Titles.xml')

# The drop-in's own tests. tests/meson.build runs after drop-ins/, so look
# doctest up here (meson caches the lookup).
titles_doctest_dep = dependency('doctest', required: get_option('tests'), fallback: ['doctest', 'doctest_dep'])
if titles_doctest_dep.found()
  subdir('tests')
endif
