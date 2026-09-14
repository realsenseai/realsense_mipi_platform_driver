#!/bin/bash

# Display help message
if [ "$#" -lt 1 ] || [ "$1" == "-h" ] || [ "$1" == "--help" ]; then
    echo "Usage: $0 <JETPACK_VERSION> [BOOT_FOLDER] [DELAY_SECONDS]"
      echo ""
      echo "Update the kernel modules and boot files on the local device for a specific JetPack version."
      echo ""
      echo "Arguments:"
      echo "  JETPACK_VERSION   JetPack version (e.g., 5.0.2, 5.1.2, 6.0, 6.1, 6.2, 6.2.1, 7.0, 7.1, 7.2)"
      echo "  BOOT_FOLDER       Folder name under /boot to copy Image (default: dev)"
      echo "  DELAY_SECONDS     Delay before reboot in seconds (default: 0)"
      echo ""
      echo "Example:"
      echo "  $0 6.2 foo"
      exit 0
fi

JETPACK_VERSION="$1"
FOLDER="${2:-dev}"
DELAY="${3:-0}"

# Validate DELAY is a non-negative integer
if ! [[ "${DELAY}" =~ ^[0-9]+$ ]]; then
    echo "Error: DELAY_SECONDS must be a non-negative integer"
    exit 1
fi

if [ ! -d /boot/${FOLDER} ]; then
    sudo mkdir /boot/${FOLDER}
fi

# Only extract and use rootfs.tar.gz for 5.0.2 and 5.1.2
if [ "${JETPACK_VERSION}" = "5.0.2" ] || [ "${JETPACK_VERSION}" = "5.1.2" ]; then
    if [ -f rootfs.tar.gz ]; then
        tar xf rootfs.tar.gz
    fi
fi

# JetPack 5.x installs only a few .ko into /lib/modules/$(uname -r), so the new
# Image must match the running kernel; a mismatched-JetPack Image boots without
# modules and can latch the bootloader into recovery. Override: SKIP_KERNEL_CHECK=1.
if [ "${JETPACK_VERSION}" = "5.0.2" ] || [ "${JETPACK_VERSION}" = "5.1.2" ]; then
    NEW_IMAGE=$(ls boot/Image Image 2>/dev/null | head -n1)
    if [ -n "${NEW_IMAGE}" ] && [ "${SKIP_KERNEL_CHECK:-0}" != "1" ]; then
        NEW_KVER=$(grep -a -m1 -oE 'Linux version [0-9][^ ]+' "${NEW_IMAGE}" | awk '{print $3}')
        if [ "${NEW_KVER}" != "$(uname -r)" ]; then
            echo "Error: new Image kernel (${NEW_KVER:-unknown}) != running kernel ($(uname -r)); wrong JetPack build. Set SKIP_KERNEL_CHECK=1 to override."
            exit 1
        fi
    fi
fi

echo "Copying kernel files for JetPack ${JETPACK_VERSION}..."
if [ "${JETPACK_VERSION}" = "5.0.2" ] || [ "${JETPACK_VERSION}" = "5.1.2" ]; then
    echo "sudo cp tegra194-p2888-0001-p2822-0000.dtb /boot/${FOLDER}/"
          sudo cp tegra194-p2888-0001-p2822-0000.dtb /boot/${FOLDER}/
    echo "sudo cp d4xx.ko /lib/modules/$(uname -r)/updates/"
          sudo cp d4xx.ko /lib/modules/$(uname -r)/updates/
    if [ -f max96712.ko ]; then
        echo "sudo cp max96712.ko /lib/modules/$(uname -r)/updates/"
              sudo cp max96712.ko /lib/modules/$(uname -r)/updates/
    else
        echo "Note: max96712.ko not found, using existing module from BSP"
    fi
    echo "sudo cp uvcvideo.ko /lib/modules/$(uname -r)/updates/"
          sudo cp uvcvideo.ko /lib/modules/$(uname -r)/updates/
    echo "sudo cp videobuf-core.ko /lib/modules/$(uname -r)/updates/"
          sudo cp videobuf-core.ko /lib/modules/$(uname -r)/updates/
    echo "sudo cp videobuf-vmalloc.ko /lib/modules/$(uname -r)/updates/"
          sudo cp videobuf-vmalloc.ko /lib/modules/$(uname -r)/updates/
elif [ "${JETPACK_VERSION}" = "6.0" ] || [ "${JETPACK_VERSION}" = "6.1" ] || [ "${JETPACK_VERSION}" = "6.2" ] || [ "${JETPACK_VERSION}" = "6.2.1" ] || [ "${JETPACK_VERSION}" = "7.0" ] || [ "${JETPACK_VERSION}" = "7.1" ] || [ "${JETPACK_VERSION}" = "7.2" ]; then
    MODULES_DIR="lib/modules/$(uname -r)"
    # Accept any compression (rootfs.tar.gz, rootfs.tar.bz2, rootfs.tar.xz, ...);
    # tar autodetects the format from the file contents.
    ROOTFS_TARBALL=$(ls rootfs.tar.* 2>/dev/null | head -n1)
    echo "Extracting ${ROOTFS_TARBALL:-rootfs.tar.*}..."
    if [ -z "${ROOTFS_TARBALL}" ] || ! tar xf "${ROOTFS_TARBALL}"; then
        echo "Error: Failed to extract rootfs tarball (rootfs.tar.*); not modifying kernel modules."
        exit 1
    fi
    if [ ! -d "${MODULES_DIR}" ] || [ -z "$(ls -A "${MODULES_DIR}" 2>/dev/null)" ]; then
        echo "Error: Extracted modules directory '${MODULES_DIR}' is missing or empty; not modifying kernel modules."
        exit 1
    fi
    # JP7 overlays onto the existing module tree (keeps the BSP NVIDIA display
    # stack: nvidia.ko / nvidia-modeset.ko / nvidia-drm.ko, which the build no
    # longer produces). JP6 keeps the full replace.
    if [ "${JETPACK_VERSION}" != "7.0" ] && [ "${JETPACK_VERSION}" != "7.1" ] && [ "${JETPACK_VERSION}" != "7.2" ]; then
        echo "sudo rm -rf /${MODULES_DIR}"
        if ! sudo rm -rf /${MODULES_DIR}; then
            echo "Error: Failed to remove existing modules directory '${MODULES_DIR}', DON'T REBOOT"
            exit 1
        fi
    else
        echo "JP7: overlay install - keeping existing /${MODULES_DIR} (incl. BSP NVIDIA display modules)"
    fi
    echo "sudo cp -r ${MODULES_DIR} /lib/modules/."
    if ! sudo cp -r ${MODULES_DIR} /lib/modules/.; then
        echo "Error: Failed to copy modules to '/lib/modules/', DON'T REBOOT"
        exit 1
    fi
    if [ "${JETPACK_VERSION}" = "7.0" ] || [ "${JETPACK_VERSION}" = "7.1" ] || [ "${JETPACK_VERSION}" = "7.2" ]; then
        # Thor (tegra264) overlays; "|| true" so an Orin-only target without them does not fail.
        echo "sudo cp boot/tegra264-camera-d4xx-overlay*.dtbo /boot/."
              sudo cp boot/tegra264-camera-d4xx-overlay*.dtbo /boot/. 2>/dev/null || true
        # Orin (tegra234) overlays built for JP7.x on Orin (e.g. Fangzhu single-cam).
        echo "sudo cp boot/tegra234-camera-d4xx-overlay*.dtbo /boot/."
              sudo cp boot/tegra234-camera-d4xx-overlay*.dtbo /boot/. 2>/dev/null || true
    else
        echo "sudo cp boot/tegra234-camera-d4xx-overlay*.dtbo /boot/."
              sudo cp boot/tegra234-camera-d4xx-overlay*.dtbo /boot/.
        echo "sudo cp boot/dtb/tegra234-p3737-0000+p3701-0005-nv.dtb /boot/dtb/."
              sudo cp boot/dtb/tegra234-p3737-0000+p3701-0005-nv.dtb /boot/dtb/.
    fi
fi

if [ -f boot/Image ]; then
    echo "sudo cp boot/Image /boot/${FOLDER}/."
          sudo cp boot/Image /boot/${FOLDER}/.
elif [ -f Image ]; then
    echo "sudo cp Image /boot/${FOLDER}/."
          sudo cp Image /boot/${FOLDER}/.
else
    echo "Warning: Image not found, skipping kernel update"
fi
echo "sudo depmod"
      sudo depmod
echo "done - scheduling reboot in ${DELAY} seconds (backgrounded via sudo -n)"
sudo -n bash -c "nohup sh -c \"sleep ${DELAY}; /sbin/reboot\" >/dev/null 2>&1 &"
