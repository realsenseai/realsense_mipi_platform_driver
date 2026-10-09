#!/usr/bin/env bash
# Build only d4xx against an already prepared BSP; never installs/reloads it.
set -euo pipefail
if (( $# < 3 || $# > 4 )); then
    echo "Usage: $0 KERNEL_BUILD NVIDIA_OOT OUTPUT [EXTRA_SYMVERS]" >&2
    exit 2
fi
module_repo=$(cd "$(dirname "$0")/.." && pwd)
if [[ $(uname -m) != aarch64 && -z ${CROSS_COMPILE:-} ]]; then
    export CROSS_COMPILE="$module_repo/l4t-gcc/6.x/bin/aarch64-buildroot-linux-gnu-"
fi
kernel_build=$(realpath "$1")
nvidia_oot=$(realpath "$2")
mkdir -p "$3"
module_out=$(realpath "$3")
module_symvers=${4:-$nvidia_oot/Module.symvers}
[[ -f "$kernel_build/include/config/auto.conf" && -f "$kernel_build/Module.symvers" && -f "$module_symvers" ]]
cp "$module_repo/kernel/realsense/d4xx.c" "$module_repo/kernel/realsense/d585_dfu_v2_bench.h" "$module_out/"
cat > "$module_out/Makefile" <<'MAKEFILE'
obj-m += d4xx.o
ccflags-y += -I$(NVIDIA_OOT)/include
ccflags-y += -I$(NVIDIA_CONFTEST)
ccflags-y += -DCONFIG_VIDEO_D4XX_SERDES -DCONFIG_TEGRA_CAMERA_PLATFORM
ccflags-y += -DCONFIG_V4L2_ASYNC -DCONFIG_V4L2_FWNODE -DCONFIG_VIDEOBUF2_DMA_CONTIG
MAKEFILE
make -C "$kernel_build" ARCH=arm64 M="$module_out" NVIDIA_OOT="$nvidia_oot" \
    NVIDIA_CONFTEST="${NVIDIA_CONFTEST:-$nvidia_oot/../out/nvidia-conftest}" \
    KBUILD_EXTRA_SYMBOLS="$(realpath "$module_symvers")" modules
modinfo "$module_out/d4xx.ko" > "$module_out/modinfo.txt"
sha256sum "$module_out/d4xx.ko" > "$module_out/sha256.txt"
echo "Built only; verify all imported symbol CRCs against the running stack before loading."
