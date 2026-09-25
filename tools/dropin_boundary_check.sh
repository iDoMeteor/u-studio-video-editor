#!/bin/sh
# Layer rules inside drop-ins (doc 02, doc 15, "Where the code lives"): a
# drop-in's core/ includes no GTK, GLib or MLT; its app/ no MLT or Pulse;
# and nothing in src/ includes from drop-ins/.
# usage: dropin_boundary_check.sh <source root> <stamp to touch>
set -e
# The stamp path is relative to the build directory: resolve it before cd.
case "$2" in /*) stamp="$2" ;; *) stamp="$PWD/$2" ;; esac
cd "$1"
include='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]'
for d in drop-ins/*/core; do
    [ -d "$d" ] || continue
    if grep -rnE "${include}(gtk|glib|gio|gobject|adwaita|mlt)" "$d"; then exit 1; fi
done
for d in drop-ins/*/app; do
    [ -d "$d" ] || continue
    if grep -rnE "${include}(mlt|pulse)" "$d"; then exit 1; fi
done
if grep -rnE "${include}[^>\"]*drop-ins/" src; then exit 1; fi
touch "$stamp"
