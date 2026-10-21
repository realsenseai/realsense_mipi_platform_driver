#!/bin/bash
# build_overlay_dtbo.sh <overlay.dts> <out.dtbo> [sources_dir]
# Standalone Orin (JP6) overlay build: cpp with the nv-public includes, then dtc -@.
# Same output as `make dtbs` for these overlays; needs a sources_<ver> tree from setup_workspace.sh.
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: $0 <overlay.dts> <out.dtbo> [sources_dir]" >&2; exit 2; }
in=$1 out=$2
src=${3:-$(ls -d "$(git rev-parse --show-toplevel)"/sources_6.2* 2>/dev/null | head -1)}
inc=$src/hardware/nvidia/t23x/nv-public/include
[ -d "$inc" ] || { echo "no nv-public include dir under '$src' (pass sources_dir)" >&2; exit 2; }
pp=$(mktemp --suffix=.dts)
trap 'rm -f "$pp"' EXIT
cpp -nostdinc -undef -x assembler-with-cpp -D__DTS__ \
    -I "$inc/kernel" -I "$inc/platforms" -I "$inc/nvidia-oot" "$in" -o "$pp"
dtc -q -@ -I dts -O dtb -o "$out" "$pp"
ls -l "$out"
