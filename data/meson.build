install_data(
  'com.ustudio.VideoEditor.desktop',
  install_dir: get_option('datadir') / 'applications',
)

install_data(
  'com.ustudio.VideoEditor.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)

install_data(
  'icons/hicolor/scalable/apps/com.ustudio.VideoEditor.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps',
)

install_data(
  'com.ustudio.VideoEditor.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0/schemas',
)

# Compiles gschemas.compiled into THIS build subdir (builddir/data), for
# running the app uninstalled -- glib-compile-schemas' own targetdir
# convention, matching what `meson devenv -C builddir` auto-adds to
# GSETTINGS_SCHEMA_DIR. Outside of `meson devenv`, launching the binary
# directly (this project's normal dev loop -- CLAUDE.md) needs that same
# path exported by hand: GSETTINGS_SCHEMA_DIR=builddir/data. settings.cpp
# degrades to hardcoded defaults (no crash, no persistence) if it isn't set
# and the schema also isn't installed system-wide -- see its own comment.
gnome = import('gnome')
# Captured (not a bare statement) so tests/app/meson.build's settings test
# can order itself after this compiles, via test()'s own depends: -- see
# that test's own comment for why an ordering dependency, not just an env
# var, is needed.
ustudio_gschemas_compiled = gnome.compile_schemas(build_by_default: true,
                                                   depend_files: files('com.ustudio.VideoEditor.gschema.xml'))

# GResource compilation happens in src/app/meson.build, not here -- see the
# rationale comment there. This file only owns packaging metadata.
