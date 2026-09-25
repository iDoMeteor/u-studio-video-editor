#!/bin/sh
# ADR-017: POSIX/Linux-only headers and /proc stay inside src/platform/.
# The files doc 14's migration list still names are allowed until they move.
# usage: platform_boundary_check.sh <source root> <stamp to touch>
set -e
# The stamp path is relative to the build directory: resolve it before cd.
case "$2" in /*) stamp="$2" ;; *) stamp="$PWD/$2" ;; esac
cd "$1"
include='^[[:space:]]*#[[:space:]]*include[[:space:]]*<(unistd\.h|fcntl\.h|dlfcn\.h|signal\.h|csignal|glib-unix\.h|sys/)'
proc='"/proc/'
# Not yet migrated (doc 14, "Migration list"): strike a line when its file moves.
allowed='^src/(core/xml/writer\.cpp|app/main\.cpp|app/settings\.cpp|app/snap_env\.cpp|app/portal_path\.cpp|engine/factory_policy\.cpp|engine/engine\.cpp|dropins/registry\.cpp):'
found=$(grep -rnE -e "$include" -e "$proc" src --include='*.cpp' --include='*.h' | grep -v '^src/platform/' | grep -vE "$allowed" || true)
if [ -n "$found" ]; then
    echo "$found"
    echo "POSIX/Linux-only code outside src/platform/ (ADR-017): add a platform:: function instead."
    exit 1
fi
touch "$stamp"
