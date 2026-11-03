#!/bin/bash
# Apply the RealSense patches to a Qualcomm workspace (RSDEV-15171).
#
#   ./apply_patches_qcom.sh [reset] [kernel_version]
#
# `reset` must come before the version, matching apply_patches.sh.
#
# Three things happen on apply:
#   1. qcom/<ver>/*.patch goes onto the Qualcomm kernel (UAPI formats today).
#   2. nvidia-oot/6.0/000{2,3} goes onto the SerDes donor tree, so max9295,
#      max9296 and max96712 come from the same carriers every other platform
#      uses. There is no Qualcomm fork of those drivers.
#   3. The module build directory is populated with symlinks to the driver
#      sources, so editing kernel/realsense/*.c reaches the next build.

set -e

ACTION="apply"
while [[ $# -gt 0 ]]; do
    if [[ "$1" == reset ]]; then
        ACTION="reset"
    elif [[ "$1" == "-h" ]]; then
        echo "Usage: $0 [reset] [kernel_version]"
        echo -e 'reset\t: hard reset (git) the Qualcomm kernel and SerDes donor to their pinned base'
        echo -e '-h\t: show this help'
        exit 0
    else break
    fi
    shift
done

. scripts/setup-common-qcom "$1"

DEVDIR=$(cd "$(dirname "$0")" && pwd)

if [[ ! -d "${BUILD_SRCS}" ]]; then
    echo "Sources folder not found. Run ./setup_workspace_qcom.sh first"
    exit 2
fi

# Hard reset a sub-repo to the tag setup_workspace_qcom.sh placed on its base
# commit. Non-interactive: unlike the Jetson flow these trees hold no work of
# ours, everything is regenerated below.
reset_tree() {
    local tree="$1"
    [[ -d "${tree}/.git" ]] || return 0
    echo -n "$(ls -d "${tree}"): "
    git -C "${tree}" reset --hard rs-base
}

reset_tree "${QCOM_KERNEL_SRC}"
reset_tree "${QCOM_NVOOT_SRC}"

if [[ "$ACTION" == "reset" ]]; then
    rm -rf "${QCOM_MODULES_SRC}"
    echo "Reset complete"
    exit 0
fi

# --- 1. kernel patches -------------------------------------------------------
QCOM_PATCH_DIR="${DEVDIR}/qcom/${QCOM_INPUT_VERSION}"
if compgen -G "${QCOM_PATCH_DIR}/*.patch" > /dev/null; then
    echo -e "\e[33m${QCOM_PATCH_DIR}\e[0m"
    ls -w1 "${QCOM_PATCH_DIR}"/*.patch | xargs -n1 basename
    git -C "${QCOM_KERNEL_SRC}" apply "${QCOM_PATCH_DIR}"/*.patch
else
    echo "No kernel patches in ${QCOM_PATCH_DIR}"
fi

# --- 2. SerDes patches onto the donor ---------------------------------------
# 0002 carries max9295 + max9296, the pair the vision mezzanine has. The donor
# tag is chosen in setup-common-qcom to match the carrier's base.
echo -e "\e[33m${DEVDIR}/${NVOOT_PATCH_DIR} (SerDes)\e[0m"
for p in "${DEVDIR}/${NVOOT_PATCH_DIR}"/0002-max9295-max9296-SerDes-support-for-D4xx.patch; do
    echo "  $(basename "$p")"
    git -C "${QCOM_NVOOT_SRC}" apply "$p"
done

# --- 3. module build directory ----------------------------------------------
rm -rf "${QCOM_MODULES_SRC}"
mkdir -p "${QCOM_MODULES_SRC}/include/media"

ln -sf "${DEVDIR}/qcom/modules/Kbuild" "${QCOM_MODULES_SRC}/Kbuild"
ln -sf "${DEVDIR}/qcom/compat-include" "${QCOM_MODULES_SRC}/compat-include"

# D4XX driver - the same files every platform builds.
for f in d4xx_main.c d4xx_desc.c d4xx.h d4xx_desc.h; do
    ln -sf "${DEVDIR}/kernel/realsense/${f}" "${QCOM_MODULES_SRC}/${f}"
done

# SerDes sources derived from the donor in step 2.
NVOOT_ABS=$(cd "${QCOM_NVOOT_SRC}" && pwd)
for f in max9295.c max9296.c; do
    ln -sf "${NVOOT_ABS}/drivers/media/i2c/${f}" "${QCOM_MODULES_SRC}/${f}"
done
for f in max9295.h max9296.h gmsl-link.h; do
    ln -sf "${NVOOT_ABS}/include/media/${f}" "${QCOM_MODULES_SRC}/include/media/${f}"
done

echo
echo -e "\e[32mPatches applied successfully.\e[0m Next: ./build_all_qcom.sh ${QCOM_INPUT_VERSION}"
