#!/bin/bash
# Build the Qualcomm kernel, DTBs and the D4XX + SerDes modules (RSDEV-15171).
#
#   ./build_all_qcom.sh [--clean] [--modules-only] [kernel_version]
#
# Output goes to images/qcom-<ver>/.

set -e

CLEAN=0
MODULES_ONLY=0

while [[ "$1" == --* ]]; do
    case "$1" in
        --clean)        CLEAN=1; shift ;;
        --modules-only) MODULES_ONLY=1; shift ;;
        -h|--help)
            echo "build_all_qcom.sh [--clean] [--modules-only] [kernel_version]"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

export DEVDIR=$(cd "$(dirname "$0")" && pwd)
NPROC=$(nproc)

. scripts/setup-common-qcom "$1"

KERNEL_SRC="${DEVDIR}/${QCOM_KERNEL_SRC}"
MODULES_SRC="${DEVDIR}/${QCOM_MODULES_SRC}"
OUT="${DEVDIR}/images/qcom-${QCOM_INPUT_VERSION}"

if [[ ! -d "${MODULES_SRC}" ]]; then
    echo "Module sources not staged. Run ./apply_patches_qcom.sh first"
    exit 2
fi

CROSS_COMPILE=$(qcom_cross_compile "${DEVDIR}")
if [[ -n "${CROSS_COMPILE}" ]]; then
    export CROSS_COMPILE
    echo "Cross compiling with ${CROSS_COMPILE}"
else
    echo "Native aarch64 build"
fi

if [[ $CLEAN == 1 ]]; then
    echo "Cleaning build artifacts for qcom-${QCOM_INPUT_VERSION}..."
    rm -rf "${OUT}"
fi

mkdir -p "${OUT}"

# --- kernel ------------------------------------------------------------------
# modules_prepare is what an out-of-tree module strictly needs, but the RB3 runs
# our own kernel (the stock one has no RealSense UAPI formats), so the default
# build produces Image + modules + DTB too.
CONFIG_FRAGMENT="${DEVDIR}/qcom/${QCOM_INPUT_VERSION}/realsense.config"

# defconfig plus our fragment. The fragment is not optional: it turns off the
# in-tree staging max96712, whose module name and DT compatibles are both ours.
qcom_configure() {
    make -C "${KERNEL_SRC}" O="${OUT}" "${QCOM_DEFCONFIG}"
    if [[ -f "${CONFIG_FRAGMENT}" ]]; then
        echo "=== Merging ${CONFIG_FRAGMENT##*/} ==="
        ARCH=arm64 "${KERNEL_SRC}/scripts/kconfig/merge_config.sh" \
            -m -O "${OUT}" "${OUT}/.config" "${CONFIG_FRAGMENT}"
        make -C "${KERNEL_SRC}" O="${OUT}" olddefconfig
    fi
}

if [[ $MODULES_ONLY == 0 ]]; then
    echo "=== Building kernel (${QCOM_DEFCONFIG}) ==="
    qcom_configure
    make -C "${KERNEL_SRC}" O="${OUT}" -j"${NPROC}" Image modules "${QCOM_DTB}"

    export INSTALL_MOD_PATH="${OUT}/rootfs"
    make -C "${KERNEL_SRC}" O="${OUT}" modules_install

    mkdir -p "${OUT}/rootfs/boot/dtb"
    cp "${OUT}/arch/arm64/boot/Image" "${OUT}/rootfs/boot/"
    cp "${OUT}/arch/arm64/boot/dts/${QCOM_DTB}" "${OUT}/rootfs/boot/dtb/"
else
    echo "=== Preparing kernel headers only ==="
    [[ -f "${OUT}/.config" ]] || qcom_configure
    make -C "${KERNEL_SRC}" O="${OUT}" -j"${NPROC}" modules_prepare
fi

# --- D4XX + SerDes modules ---------------------------------------------------
# modules_prepare does not emit Module.symvers, so a --modules-only run before
# any full build cannot resolve kernel symbols. Compile anyway and say that the
# check was skipped, rather than failing a source-only iteration.
MODPOST_WARN=""
if [[ ! -f "${OUT}/Module.symvers" ]]; then
    echo -e "\e[33mNo Module.symvers yet - building without symbol resolution.\e[0m"
    echo -e "\e[33mRun a full ./build_all_qcom.sh once to check for undefined symbols.\e[0m"
    MODPOST_WARN="KBUILD_MODPOST_WARN=1"
fi

echo "=== Building D4XX + SerDes modules ==="
make -C "${OUT}" M="${MODULES_SRC}" -j"${NPROC}" ${MODPOST_WARN} modules

export INSTALL_MOD_PATH="${OUT}/rootfs"
mkdir -p "${INSTALL_MOD_PATH}"
make -C "${OUT}" M="${MODULES_SRC}" INSTALL_MOD_DIR=extra modules_install

echo
echo -e "\e[32mBuild complete.\e[0m Modules:"
find "${MODULES_SRC}" -name '*.ko' -printf '  %f\n' | sort
echo "Output tree: ${OUT}"
