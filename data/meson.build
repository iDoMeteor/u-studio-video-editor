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
# ADR-019: a packaged build that starts with GPU acceleration off installs a
# vendor override next to the schema; glib-compile-schemas (post-install
# below) folds it into the default. It lives in a subdirectory so the
# uninstalled build's compile_schemas() (this directory only) leaves dev
# builds on "auto".
if get_option('gpu_acceleration_default') == 'off'
  install_data(
    'gsettings-overrides' / 'ustudio-gpu-off.gschema.override',
    install_dir: get_option('datadir') / 'glib-2.0/schemas',
  )
endif

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

# An installed build (the Flatpak, `meson install` into a prefix) needs the
# schema compiled where it was installed, or settings.cpp finds no schema and
# the app shows "Settings can't be saved" (seen in the first Flatpak build).
# meson skips these when DESTDIR is set, leaving it to the distro packaging.
gnome.post_install(
  glib_compile_schemas: true,
  gtk_update_icon_cache: true,
  update_desktop_database: true,
)

# GResource compilation happens in src/app/meson.build, not here -- see the
# rationale comment there. This file only owns packaging metadata.
