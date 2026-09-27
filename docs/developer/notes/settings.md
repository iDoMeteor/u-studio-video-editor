# Settings and GSettings notes

[Developer docs](../README.md) › [Implementation notes](README.md)

`Settings` (`src/app/settings.h`/`.cpp`) wraps the `com.ustudio.VideoEditor`
GSettings schema for the Settings dialog's General/Playback tabs. One
sharp edge drove its whole shape: `g_settings_new(schema_id)` **aborts the
process** (`g_error()`) if the schema isn't found — fine for an installed
app with its schema as a hard dependency, wrong for this app's normal dev
loop (CLAUDE.md: launch straight from `builddir`, no `meson install`
first), where the schema is routinely *not* compiled/installed yet. Every
constructor call therefore goes through `g_settings_schema_source_lookup()`
first (returns `nullptr` instead of aborting), confirmed by a standalone
repro to not crash against a nonexistent `GSETTINGS_SCHEMA_DIR`; when it
comes back null, every getter falls back to its hardcoded default and
every setter is a silent no-op (matches CLAUDE.md's frei0r-degrade
precedent: detect and degrade at runtime, don't crash). `data/meson.build`
compiles the schema into `builddir/data/gschemas.compiled` for this
uninstalled case — point `GSETTINGS_SCHEMA_DIR` there, or use
`meson devenv -C builddir` (which sets it automatically), to exercise real
persistence during development; a real `meson install` also works, via the
system schema search path.

Testing this without polluting the real system dconf database (CLAUDE.md:
verification must never touch real user state) uses `GSETTINGS_BACKEND=memory`
— a real, documented GIO env var selecting GIO's in-process memory
backend — confirmed via a standalone repro (set a value through it, then
read the same key back through the normal `dconf`/`gsettings` CLI: the
real system value was provably untouched). `tests/app/test_settings.cpp`
runs with both env vars set (schema found, real in-memory persistence);
`tests/app/test_settings_missing_schema.cpp` runs with `GSETTINGS_SCHEMA_DIR`
*and* `XDG_DATA_DIRS` pointed at empty scratch directories (the system
schema source is derived entirely from `XDG_DATA_DIRS`, so both need
overriding to actually simulate "nothing installed") to cover the degrade
path — confirmed via the same kind of standalone repro that
`g_settings_schema_source_lookup()` genuinely returns null rather than
crashing in that state, before relying on it in the test.
