gnome = import('gnome')

# The manifest lives under data/ (packaging-owned), but the CSS it bundles
# lives under src/app/style/ (this target's own concern) -- calling
# compile_resources() from here, with source_dir pointing at the local
# style/ directory, keeps "where do my resource files live" colocated with
# the target that consumes them; only the single, rarely-moving XML
# manifest path has to reach across into data/.
app_resources = gnome.compile_resources(
  'ustudio-resources',
  meson.project_source_root() / 'data' / 'ustudio.gresource.xml',
  source_dir: 'style',
  c_name: 'ustudio',
)

# Its own tiny static library (not just folded into app_sources below) so
# tests/app/ can link it without pulling in GTK4/libadwaita -- autosave.cpp
# only needs GLib (XDG dirs, SHA1) and std::filesystem.
app_autosave_lib = static_library(
  'ustudio_app_autosave',
  files('autosave.cpp'),
  dependencies: [glib_dep, libustudio_platform_dep],
  include_directories: src_inc,
  install: false,
)
app_autosave_dep = declare_dependency(
  link_with: app_autosave_lib,
  include_directories: src_inc,
  dependencies: [glib_dep, libustudio_platform_dep],
)

# Same rationale as app_autosave_lib above: settings.cpp only needs GIO
# (GSettings), not GTK4/libadwaita, so tests/app/ can exercise its
# schema-found/schema-missing degrade behaviour (settings.cpp's own
# comment) without linking the whole GTK app.
app_settings_lib = static_library(
  'ustudio_app_settings',
  files('settings.cpp', 'render_profiles.cpp', 'snap_env.cpp'),
  dependencies: [gio_dep, glib_dep, libustudio_core_dep, libustudio_platform_dep],
  include_directories: src_inc,
  install: false,
)
app_settings_dep = declare_dependency(
  link_with: app_settings_lib,
  include_directories: src_inc,
  dependencies: [gio_dep, glib_dep, libustudio_core_dep, libustudio_platform_dep],
)

# GLib-only, like the two libraries above, so tests/app/ can exercise it
# against the real Documents portal.
app_portal_lib = static_library(
  'ustudio_app_portal',
  files('portal_path.cpp'),
  dependencies: [glib_dep, libustudio_core_dep],
  include_directories: src_inc,
  install: false,
)
app_portal_dep = declare_dependency(
  link_with: app_portal_lib,
  include_directories: src_inc,
  dependencies: [glib_dep, libustudio_core_dep],
)

# The render queue and its quit-time persistence: no GTK, so tests/app/ can
# drive them with a fake backend.
app_render_queue_lib = static_library(
  'ustudio_app_render_queue',
  files('render_queue.cpp', 'pending_renders.cpp'),
  dependencies: [libustudio_core_dep, app_settings_dep, glib_dep],
  include_directories: src_inc,
  install: false,
)
app_render_queue_dep = declare_dependency(
  link_with: app_render_queue_lib,
  include_directories: src_inc,
  dependencies: [libustudio_core_dep, app_settings_dep, glib_dep],
)

# The timeline's GTK-free parts (doc 06: Viewport, and the
# TimelineController to come), unit-tested in tests/app/.
app_timeline_lib = static_library(
  'ustudio_app_timeline',
  files('timeline/selection.cpp', 'timeline/timeline_controller.cpp', 'timeline/viewport.cpp'),
  dependencies: [libustudio_core_dep],
  include_directories: src_inc,
  install: false,
)
app_timeline_dep = declare_dependency(
  link_with: app_timeline_lib,
  include_directories: src_inc,
  dependencies: [libustudio_core_dep],
)

# The timeline's colours, generated from style.css so the C++-drawn
# timeline and the CSS-styled chrome share one set of tokens.
app_tokens_h = custom_target(
  'app-tokens-h',
  input: 'style' / 'style.css',
  output: 'tokens.h',
  command: [find_program('python3'), meson.project_source_root() / 'tools' / 'gen_tokens.py', '@INPUT@', '@OUTPUT@'],
)

# Everything but main.cpp, as a library, so tests/app/ can check the real
# hint and action tables against each other (test_ui_hints.cpp).
app_window_lib = static_library(
  'ustudio_app_window',
  files('action_registry.cpp', 'app_window.cpp', 'dropins_page.cpp', 'import_queue.cpp', 'missing_media.cpp', 'project_loader.cpp', 'render_profiles_page.cpp', 'save_queue.cpp', 'shell_hosts.cpp', 'stall_monitor.cpp', 'ui_hints.cpp', 'timeline/timeline_renderer.cpp',
        'timeline/us_timeline_view.cpp'),
  app_tokens_h,
  dependencies: [libustudio_engine_dep, libustudio_dropins_dep, app_timeline_dep, app_autosave_dep, app_settings_dep, app_portal_dep, app_render_queue_dep, libustudio_platform_dep, gtk4_dep, adwaita_dep, gio_dep],
  cpp_args: ['-DUSTUDIO_VERSION="' + meson.project_version() + '"'],
  install: false,
)
app_window_dep = declare_dependency(
  link_with: app_window_lib,
  include_directories: src_inc,
  dependencies: [libustudio_engine_dep, libustudio_dropins_dep, app_timeline_dep, app_autosave_dep, app_settings_dep, app_portal_dep, app_render_queue_dep, libustudio_platform_dep, gtk4_dep, adwaita_dep, gio_dep],
)

# --- Dependency-boundary guard: app/ must never see MLT or Pulse headers
# directly (doc 02). It only talks to the engine through the plain-C++ API
# in engine/*.h, which forward-declares the MLT/Pulse types it needs.
# Enforced by the build itself (build_by_default), not just by CI --
# matches the M0 acceptance criterion `grep -rn '<mlt' src/app` returning
# nothing, but anchored to actual #include lines (not a bare substring) so
# a comment mentioning "mlt" can't false-positive it.
app_boundary_check = custom_target(
  'app-boundary-check',
  output: 'app-boundary-check.stamp',
  command: [
    find_program('sh'), '-c',
    'grep -rnE "$1" "$2" && exit 1; touch "$3"',
    'sh',
    '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](mlt|pulse)',
    meson.current_source_dir(),
    '@OUTPUT@',
  ],
  build_by_default: true,
)

executable(
  'u-studio-video-editor',
  files('main.cpp'),
  app_resources,
  dependencies: [app_window_dep, drop_ins_dep],
  export_dynamic: dropin_any_module,
  install: true,
  cpp_args: ['-DUSTUDIO_VERSION="' + meson.project_version() + '"'],
)
