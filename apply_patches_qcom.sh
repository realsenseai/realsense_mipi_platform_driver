#!/bin/bash
# Stage the RealSense out-of-tree module trees for Ubuntu (RSDEV-15171).
#
#   ./apply_patches_qcom.sh [reset] [series]
#
# `reset` must come before the series, matching apply_patches.sh.
#
# Nothing is patched into the kernel. Three things happen on apply:
#   1. nvidia-oot/6.0/0002 goes onto the SerDes donor tree, so max9295 and
#      max9296 come from the same carrier every other platform uses.
#   2. modules/d4xx is populated with symlinks to the driver sources, so
#      editing kernel/realsense/*.c reaches the next build.
#   3. modules/camss is a copy of the kernel's CAMSS driver plus our patches
#      from qcom/<series>/camss/, built out-of-tree and shipped via DKMS.

set -e

ACTION="apply"
while [[ $# -gt 0 ]]; do
    if [[ "$1" == reset ]]; then
        ACTION="reset"
    elif [[ "$1" == "-h" ]]; then
        echo "Usage: $0 [reset] [series]"
        echo -e 'reset\t: hard reset (git) the SerDes donor and drop staged modules'
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

# Non-interactive: these trees hold no work of ours, everything is regenerated.
reset_tree() {
    local tree="$1"
    [[ -d "${tree}/.git" ]] || return 0
    echo -n "$(ls -d "${tree}"): "
    git -C "${tree}" reset --hard rs-base
}

reset_tree "${QCOM_NVOOT_SRC}"

if [[ "$ACTION" == "reset" ]]; then
    rm -rf "${QCOM_MODULES_SRC}"
    echo "Reset complete"
    exit 0
fi

# --- 1. SerDes patches onto the donor ---------------------------------------
# 0002 carries max9295 + max9296, the pair the vision mezzanine has.
echo -e "\e[33m${DEVDIR}/${NVOOT_PATCH_DIR} (SerDes)\e[0m"
SER_PATCH="${DEVDIR}/${NVOOT_PATCH_DIR}/0002-max9295-max9296-SerDes-support-for-D4xx.patch"
echo "  $(basename "${SER_PATCH}")"
git -C "${QCOM_NVOOT_SRC}" apply "${SER_PATCH}"

rm -rf "${QCOM_MODULES_SRC}"

# --- 2. d4xx + SerDes module tree -------------------------------------------
D4XX_DIR="${QCOM_MODULES_SRC}/d4xx"
mkdir -p "${D4XX_DIR}/include/media"

ln -sf "${DEVDIR}/qcom/modules/Kbuild" "${D4XX_DIR}/Kbuild"
ln -sf "${DEVDIR}/qcom/compat-include" "${D4XX_DIR}/compat-include"

for f in d4xx_main.c d4xx_desc.c d4xx.h d4xx_desc.h; do
    ln -sf "${DEVDIR}/kernel/realsense/${f}" "${D4XX_DIR}/${f}"
done

NVOOT_ABS=$(cd "${QCOM_NVOOT_SRC}" && pwd)
for f in max9295.c max9296.c; do
    ln -sf "${NVOOT_ABS}/drivers/media/i2c/${f}" "${D4XX_DIR}/${f}"
done
for f in max9295.h max9296.h gmsl-link.h; do
    ln -sf "${NVOOT_ABS}/include/media/${f}" "${D4XX_DIR}/include/media/${f}"
done

# --- 3. CAMSS fork ----------------------------------------------------------
# A copy, not symlinks: we patch these files. The kernel tree stays pristine.
CAMSS_SRC="${QCOM_KERNEL_SRC}/drivers/media/platform/qcom/camss"
CAMSS_DIR="${QCOM_MODULES_SRC}/camss"
mkdir -p "${CAMSS_DIR}"
cp "${CAMSS_SRC}"/*.c "${CAMSS_SRC}"/*.h "${CAMSS_DIR}/"
cp "${DEVDIR}/qcom/modules/camss.Kbuild" "${CAMSS_DIR}/Kbuild"
ln -sf "${DEVDIR}/qcom/compat-include" "${CAMSS_DIR}/compat-include"

# Our Kbuild hardcodes the object list; warn if the kernel's has moved.
KERNEL_OBJS=$(grep -oE '^\s+camss[a-z0-9-]*\.o' "${CAMSS_SRC}/Makefile" | tr -d ' \t' | sort)
OUR_OBJS=$(grep -oE '^\s+camss[a-z0-9-]*\.o' "${CAMSS_DIR}/Kbuild" | tr -d ' \t' | sort)
if [[ "${KERNEL_OBJS}" != "${OUR_OBJS}" ]]; then
    echo -e "\e[33mWarning: CAMSS object list differs from the kernel's Makefile.\e[0m"
    diff <(echo "${KERNEL_OBJS}") <(echo "${OUR_OBJS}") || true
    echo "Update qcom/modules/camss.Kbuild."
fi

CAMSS_PATCH_DIR="${DEVDIR}/qcom/${QCOM_INPUT_VERSION}/camss"
if compgen -G "${CAMSS_PATCH_DIR}/*.patch" > /dev/null; then
    echo -e "\e[33m${CAMSS_PATCH_DIR}\e[0m"
    for p in "${CAMSS_PATCH_DIR}"/*.patch; do
        echo "  $(basename "$p")"
        patch -s -p1 -d "${CAMSS_DIR}" --no-backup-if-mismatch < "$p"
    done
else
    echo "No CAMSS patches yet in ${CAMSS_PATCH_DIR} (stock driver)"
fi

echo
echo -e "\e[32mModules staged.\e[0m Next: ./build_all_qcom.sh ${QCOM_INPUT_VERSION}"
