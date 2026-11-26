#!/bin/bash
# Snapshot the D4XX V4L2 surface so two driver builds can be diffed (RSDEV-15178).
#
#   ./capture_api.sh A        # on the baseline build
#   ./capture_api.sh B        # on the build under test, same DTB and FW
#   diff -r -x '*.raw' capture-A capture-B
#
# An empty diff means the build under test exposes the same nodes, formats,
# frame sizes, controls (type, range, step, default, menu, flags) and subdevs.
#
# Bench use only: listing controls reads each one, which consumes the camera's
# read-to-clear error_code latch.

set -e

[[ -n "$1" ]] || { echo "Usage: $0 <tag>"; exit 1; }
out="capture-$1"
rm -rf "$out"
mkdir -p "$out"

# Current values vary run to run (exposure, temperatures); everything else on
# a control line is part of the API and stays in.
strip_values() { sed -E 's/ value=[^ ]+//'; }

# Role symlinks are stable across boots; the /dev/videoN behind them follows
# probe order, so only the names are compared.
ls /dev/video-rs-* 2>/dev/null | sort > "$out/nodes.txt"

for n in /dev/video-rs-*; do
    [[ -e "$n" ]] || continue
    b=$(basename "$n")
    v4l2-ctl -d "$n" --list-formats-ext > "$out/$b.formats" 2>&1 || true
    v4l2-ctl -d "$n" --list-ctrls-menus 2>&1 | strip_values > "$out/$b.ctrls" || true
done

# Subdevs are keyed by entity name plus the device behind them, not by
# v4l-subdevN, for the same probe-order reason. A camera's four I2C instances
# give their sensors one name (the override_reg address), so the name alone
# would fold sixteen subdevs into four.
for s in /sys/class/video4linux/v4l-subdev*; do
    [[ -e "$s/name" ]] || continue
    name=$(tr ' /' '__' < "$s/name")
    inst=$(basename "$(readlink -f "$s/device")")
    key="subdev-${name}@${inst}"
    dev="/dev/$(basename "$s")"
    v4l2-ctl -d "$dev" --list-subdev-mbus-codes >> "$out/$key.mbus" 2>&1 || true
    v4l2-ctl -d "$dev" --list-ctrls-menus 2>&1 | strip_values >> "$out/$key.ctrls" || true
done

# Topology with the probe-order-dependent numbers masked; the untouched output
# is kept beside it for reading, and excluded from the diff by its suffix.
for m in /dev/media*; do
    [[ -e "$m" ]] || continue
    b=$(basename "$m")
    media-ctl -d "$m" -p > "$out/$b.raw" 2>&1 || true
    sed -E 's#/dev/video[0-9]+#/dev/videoN#g; s#/dev/v4l-subdev[0-9]+#/dev/v4l-subdevN#g;
            s/^- entity [0-9]+:/- entity:/' \
        "$out/$b.raw" > "$out/$b.topology"
done

echo "captured to $out ($(ls "$out" | wc -l) files)"
