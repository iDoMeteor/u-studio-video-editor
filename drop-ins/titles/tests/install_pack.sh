#!/bin/sh
# u-studio-titles --install-pack with a generated pack (tools/make_test_pack.py):
# installs, refuses the same version again, and takes a newer one.
# usage: install_pack.sh <u-studio-titles> <make_test_pack.py> <scratch dir>
set -u
APP=$1 MAKE=$2 DIR=$3
rm -rf "$DIR" && mkdir -p "$DIR"
export XDG_DATA_HOME="$DIR/data" XDG_CONFIG_HOME="$DIR/config" XDG_STATE_HOME="$DIR/state" XDG_CACHE_HOME="$DIR/cache"
python3 "$MAKE" "$DIR/one.zip" --version 1.0.0 >/dev/null || exit 1
python3 "$MAKE" "$DIR/two.zip" --version 1.1.0 >/dev/null || exit 1
out=$("$APP" --install-pack "$DIR/one.zip" 2>/dev/null)
status=$?
case "$out" in
    *"can't open packs"*) echo "built without libarchive: skipped"; exit 77 ;;
esac
[ $status -eq 0 ] && echo "$out" | grep -q "^installed test/smoke-pack 1.0.0" || { echo "first ($status): $out"; exit 1; }
"$APP" --install-pack "$DIR/one.zip" >/dev/null 2>&1 && { echo "the same version installed twice"; exit 1; }
"$APP" --install-pack "$DIR/two.zip" 2>/dev/null | grep -q "^installed test/smoke-pack 1.1.0" || { echo "newer refused"; exit 1; }
[ -f "$DIR/data/ustudio/titles/templates/packs/test--smoke-pack/lower-third-two-lines/template.ustitle" ] || { echo "not in the library"; exit 1; }
echo "ok"
