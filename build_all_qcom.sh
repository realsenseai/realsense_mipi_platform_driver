#!/bin/bash
# Build the RealSense out-of-tree modules for Ubuntu and package them as DKMS
# .debs (RSDEV-15171).
#
#   ./build_all_qcom.sh [--clean] [--no-deb] [series]
#
# The host cross-build is a COMPILE GATE. The .debs carry source, and the board
# rebuilds them through DKMS against its own linux-headers-qcom, which is what
# makes the modules ABI-correct for whatever kernel Canonical ships that week.

set -e

CLEAN=0
MAKE_DEB=1

while [[ "$1" == --* ]]; do
    case "$1" in
        --clean)   CLEAN=1; shift ;;
        --no-deb)  MAKE_DEB=0; shift ;;
        -h|--help)
            echo "build_all_qcom.sh [--clean] [--no-deb] [series]"
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

[[ -d "${MODULES_SRC}/d4xx" ]] || { echo "Not staged. Run ./apply_patches_qcom.sh first"; exit 2; }

CROSS_COMPILE=$(qcom_cross_compile "${DEVDIR}")
if [[ -n "${CROSS_COMPILE}" ]]; then
    export CROSS_COMPILE
    echo "Cross compiling with ${CROSS_COMPILE}"
else
    echo "Native aarch64 build"
fi

[[ $CLEAN == 1 ]] && { echo "Cleaning ${OUT}"; rm -rf "${OUT}"; }
mkdir -p "${OUT}"

# --- kernel headers for the compile gate ------------------------------------
if [[ ! -f "${OUT}/.config" ]]; then
    echo "=== Configuring kernel tree (${QCOM_DEFCONFIG}) ==="
    make -C "${KERNEL_SRC}" O="${OUT}" "${QCOM_DEFCONFIG}"
fi
make -C "${KERNEL_SRC}" O="${OUT}" -j"${NPROC}" modules_prepare

# modules_prepare emits no Module.symvers, so a first build cannot resolve
# kernel symbols. Compile anyway and say the check was skipped.
MODPOST_WARN=""
if [[ ! -f "${OUT}/Module.symvers" ]]; then
    echo -e "\e[33mNo Module.symvers - building without symbol resolution.\e[0m"
    MODPOST_WARN="KBUILD_MODPOST_WARN=1"
fi

build_tree() {
    local name="$1"
    echo "=== Building ${name} ==="
    make -C "${OUT}" M="${MODULES_SRC}/${name}" -j"${NPROC}" ${MODPOST_WARN} modules
}

build_tree d4xx
build_tree camss

# --- DKMS packaging ----------------------------------------------------------
VERSION=$(sed -n 's/^MODULE_VERSION("\(.*\)");/\1/p' "${DEVDIR}/kernel/realsense/d4xx_main.c" | head -1)
VERSION="${VERSION:-0.0.0}"

stage_dkms() {
    local pkg="$1" src="$2"; shift 2
    local root="${OUT}/dkms/${pkg}-dkms_${VERSION}"
    local usr="${root}/usr/src/${pkg}-${VERSION}"

    rm -rf "${root}"
    mkdir -p "${usr}" "${root}/DEBIAN"

    # Source, with the symlinks resolved - the .deb must be self-contained.
    cp -rL "${src}/." "${usr}/"
    find "${usr}" -name '*.o' -o -name '*.ko' -o -name '.*.cmd' -o -name '*.mod*' \
        -o -name 'modules.order' -o -name 'Module.symvers' | xargs -r rm -f

    {
        echo "PACKAGE_NAME=\"${pkg}\""
        echo "PACKAGE_VERSION=\"${VERSION}\""
        local i=0
        for m in "$@"; do
            echo "BUILT_MODULE_NAME[$i]=\"${m}\""
            echo "DEST_MODULE_LOCATION[$i]=\"/updates/dkms\""
            i=$((i+1))
        done
        echo 'AUTOINSTALL="yes"'
    } > "${usr}/dkms.conf"

    cat > "${root}/DEBIAN/control" <<EOF
Package: ${pkg}-dkms
Version: ${VERSION}
Architecture: all
Maintainer: RealSense <support@realsenseai.com>
Depends: dkms (>= 2.1.0.0)
Section: kernel
Priority: optional
Description: ${pkg} kernel module (DKMS)
 Out-of-tree RealSense module for Qualcomm QCS6490 / RB3 Gen 2, built on the
 target against linux-headers-qcom.
EOF
    cat > "${root}/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
dkms add -m ${pkg} -v ${VERSION} || true
dkms build -m ${pkg} -v ${VERSION}
dkms install -m ${pkg} -v ${VERSION} --force
EOF
    cat > "${root}/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
dkms remove -m ${pkg} -v ${VERSION} --all || true
EOF
    chmod 755 "${root}/DEBIAN/postinst" "${root}/DEBIAN/prerm"

    if [[ $MAKE_DEB == 1 ]] && command -v dpkg-deb > /dev/null; then
        dpkg-deb --build --root-owner-group "${root}" \
            "${OUT}/dkms/${pkg}-dkms_${VERSION}_all.deb" > /dev/null
    fi
}

echo "=== Packaging DKMS sources (${VERSION}) ==="
stage_dkms realsense-d4xx  "${MODULES_SRC}/d4xx"  d4xx max9295 max9296
stage_dkms realsense-camss "${MODULES_SRC}/camss" qcom-camss

echo
echo -e "\e[32mBuild complete.\e[0m Modules:"
find "${MODULES_SRC}" -name '*.ko' -printf '  %f\n' | sort
if [[ $MAKE_DEB == 1 ]]; then
    echo "Packages:"
    find "${OUT}/dkms" -name '*.deb' -printf '  %f\n' 2>/dev/null | sort
fi
echo "Output tree: ${OUT}"
