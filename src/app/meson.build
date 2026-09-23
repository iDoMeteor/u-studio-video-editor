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
  dependencies: [glib_dep],
  include_directories: src_inc,
  install: false,
)
app_autosave_dep = declare_dependency(
  link_with: app_autosave_lib,
  include_directories: src_inc,
  dependencies: [glib_dep],
)

# Same rationale as app_autosave_lib above: settings.cpp only needs GIO
# (GSettings), not GTK4/libadwaita, so tests/app/ can exercise its
# schema-found/schema-missing degrade behaviour (settings.cpp's own
# comment) without linking the whole GTK app.
app_settings_lib = static_library(
  'ustudio_app_settings',
  files('settings.cpp'),
  dependencies: [gio_dep, glib_dep, libustudio_core_dep],
  include_directories: src_inc,
  install: false,
)
app_settings_dep = declare_dependency(
  link_with: app_settings_lib,
  include_directories: src_inc,
  dependencies: [gio_dep, glib_dep, libustudio_core_dep],
)

app_sources = files(
  'action_registry.cpp',
  'app_window.cpp',
  'main.cpp',
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
  app_sources,
  app_resources,
  dependencies: [libustudio_engine_dep, app_autosave_dep, app_settings_dep, gtk4_dep, adwaita_dep, gio_dep],
  install: true,
  cpp_args: ['-DUSTUDIO_VERSION="' + meson.project_version() + '"'],
)
