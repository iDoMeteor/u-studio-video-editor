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

app_sources = files(
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
  dependencies: [libustudio_engine_dep, gtk4_dep, adwaita_dep, gio_dep],
  install: true,
)
