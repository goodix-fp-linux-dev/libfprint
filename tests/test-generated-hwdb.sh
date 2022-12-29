#!/usr/bin/env bash
set -e

if [ ! -x "$UDEV_HWDB" ]; then
    echo "E: UDEV_HWDB (${UDEV_HWDB}) unset or not executable."
    exit 1
fi

if [ "$UDEV_HWDB_CHECK_CONTENTS" == 1 ]; then
    generated_rules=$(mktemp "${TMPDIR:-/tmp}/libfprint-XXXXXX.hwdb")
else
    generated_rules=/dev/null
fi

$UDEV_HWDB > "$generated_rules"
if [ $? != 0 ]; then
    echo "E: UDEV_HWDB (${UDEV_HWDB}) failed to run without error."
    exit 1
fi

if [ "$UDEV_HWDB_CHECK_CONTENTS" != 1 ]; then
    exit 77
fi

if ! diff -u "$MESON_SOURCE_ROOT/data/autosuspend.hwdb" "$generated_rules"; then
    echo "E: Autosuspend file needs to be re-generated!"
    echo "   ninja -C $MESON_BUILD_ROOT sync-udev-hwdb"
    exit 1
fi

rm "$generated_rules"
