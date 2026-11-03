#!/bin/bash
# Fetch everything the Qualcomm (QCS6490 / RB3 Gen 2) build needs (RSDEV-15171).
#
#   ./setup_workspace_qcom.sh [kernel_version]
#
# Produces sources_qcom_<ver>/{kernel,nv-oot} and a cross toolchain. Run
# ./apply_patches_qcom.sh next, then ./build_all_qcom.sh.

set -e

if [[ $1 == "-h" ]]; then
    echo "Usage: $0 [kernel_version]"
    . scripts/setup-common-qcom -h
    exit 0
fi

. scripts/setup-common-qcom "$1"

DEVDIR=$(cd "$(dirname "$0")" && pwd)

echo "Setup Qualcomm ${QCOM_INPUT_VERSION} to ${BUILD_SRCS}"

# --- toolchain ---------------------------------------------------------------
if [[ $(uname -m) == aarch64 ]]; then
    echo "Native aarch64 build - no cross toolchain needed"
elif [[ -n "$(qcom_cross_compile "${DEVDIR}")" ]]; then
    echo "Using existing toolchain: $(qcom_cross_compile "${DEVDIR}")"
else
    echo "Installing ARM GNU toolchain ${QCOM_TOOLCHAIN_VER} (aarch64-none-linux-gnu)"
    mkdir -p "${QCOM_GCC_DIR}"
    TARBALL="${QCOM_GCC_DIR}/arm-gnu-toolchain.tar.xz"
    wget --quiet --show-progress "${QCOM_TOOLCHAIN_URL}" -O "${TARBALL}"
    tar xf "${TARBALL}" -C "${QCOM_GCC_DIR}" --strip-components 1
    rm -f "${TARBALL}"
fi

mkdir -p "${BUILD_SRCS}"

# --- Qualcomm Linux kernel ---------------------------------------------------
# Shallow at the pin: we only ever build and patch this one revision, and a full
# history of the kernel is several GB.
if [[ ! -d "${QCOM_KERNEL_SRC}/.git" ]]; then
    echo "Cloning ${QCOM_KERNEL_REPO} (${QCOM_KERNEL_BRANCH} @ ${QCOM_KERNEL_PIN:0:12})"
    git init --quiet "${QCOM_KERNEL_SRC}"
    git -C "${QCOM_KERNEL_SRC}" remote add origin "${QCOM_KERNEL_REPO}"
    git -C "${QCOM_KERNEL_SRC}" fetch --depth 1 origin "${QCOM_KERNEL_PIN}"
    git -C "${QCOM_KERNEL_SRC}" checkout --quiet FETCH_HEAD
    # Named ref so apply_patches_qcom.sh reset has a stable target.
    git -C "${QCOM_KERNEL_SRC}" tag -f rs-base FETCH_HEAD > /dev/null
else
    echo "Kernel source already present: ${QCOM_KERNEL_SRC}"
fi

# --- SerDes donor tree -------------------------------------------------------
# max9295/max9296/max96712 exist in this repo only as patches against NVIDIA's
# sources. Clone the donor so apply_patches_qcom.sh can replay those same
# carriers instead of forking a Qualcomm copy that would silently drift.
if [[ ! -d "${QCOM_NVOOT_SRC}/.git" ]]; then
    echo "Cloning SerDes donor ${NVOOT_REPO} (${NVOOT_TAG})"
    if ! git clone --quiet --depth 1 --branch "${NVOOT_TAG}" \
            "${NVOOT_REPO}" "${QCOM_NVOOT_SRC}"; then
        echo -e "\e[31mFailed to clone the SerDes donor.\e[0m"
        echo "If you already have a JetPack workspace, you can point at it instead:"
        echo "  ln -s \$PWD/sources_6.2/nvidia-oot ${QCOM_NVOOT_SRC}"
        exit 1
    fi
    git -C "${QCOM_NVOOT_SRC}" tag -f rs-base HEAD > /dev/null
else
    echo "SerDes donor already present: ${QCOM_NVOOT_SRC}"
fi

echo
echo -e "\e[32mWorkspace ready.\e[0m Next: ./apply_patches_qcom.sh ${QCOM_INPUT_VERSION}"
