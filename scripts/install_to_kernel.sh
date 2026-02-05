#!/bin/bash

# Display help message
if [ "$#" -lt 1 ] || [ "$1" == "-h" ] || [ "$1" == "--help" ]; then
      echo "Usage: $0 <JETPACK_VERSION> [BOOT_FOLDER]"
      echo ""
      echo "Update the kernel modules and boot files on the local device for a specific JetPack version."
      echo ""
      echo "Arguments:"
      echo "  JETPACK_VERSION   JetPack version (e.g., 5.0.2, 5.1.2, 6.0, 6.1, 6.2, 6.2.1)"
      echo "  BOOT_FOLDER       Folder name under /boot to copy Image (default: dev)"
      echo ""
      echo "Example:"
      echo "  $0 6.2 foo"
      exit 0
fi

JETPACK_VERSION="$1"
FOLDER="${2:-dev}"

if [ ! -d /boot/${FOLDER} ]; then
    sudo mkdir /boot/${FOLDER}
fi

# Only extract and use rootfs.tar.gz for 5.0.2 and 5.1.2
if [ "${JETPACK_VERSION}" = "5.0.2" ] || [ "${JETPACK_VERSION}" = "5.1.2" ]; then
    tar xf rootfs.tar.gz
fi

echo "Copying kernel files for JetPack ${JETPACK_VERSION}..."
if [ "${JETPACK_VERSION}" = "5.0.2" ]; then
    echo "sudo cp tegra194-p2888-0001-p2822-0000.dtb /boot/${FOLDER}/"
          sudo cp tegra194-p2888-0001-p2822-0000.dtb /boot/${FOLDER}/
    echo "sudo cp d4xx.ko /lib/modules/$(uname -r)/updates/"
          sudo cp d4xx.ko /lib/modules/$(uname -r)/updates/
    echo "sudo cp max96712.ko /lib/modules/$(uname -r)/updates/"
          sudo cp max96712.ko /lib/modules/$(uname -r)/updates/
    echo "sudo cp uvcvideo.ko /lib/modules/$(uname -r)/updates/"
          sudo cp uvcvideo.ko /lib/modules/$(uname -r)/updates/
    echo "sudo cp videobuf-core.ko /lib/modules/$(uname -r)/updates/"
          sudo cp videobuf-core.ko /lib/modules/$(uname -r)/updates/
    echo "sudo cp videobuf-vmalloc.ko /lib/modules/$(uname -r)/updates/"
          sudo cp videobuf-vmalloc.ko /lib/modules/$(uname -r)/updates/
elif [ "${JETPACK_VERSION}" = "6.0" ] || [ "${JETPACK_VERSION}" = "6.1" ] || [ "${JETPACK_VERSION}" = "6.2" ] || [ "${JETPACK_VERSION}" = "6.2.1" ]; then
    tar xf rootfs.tar.gz
    echo "sudo cp -r lib/modules/$(uname -r) /lib/modules/."
          sudo cp -r lib/modules/$(uname -r) /lib/modules/.
    echo "sudo cp boot/tegra234-camera-d4xx-overlay*.dtbo /boot/."
          sudo cp boot/tegra234-camera-d4xx-overlay*.dtbo /boot/.
    echo "sudo cp boot/dtb/tegra234-p3737-0000+p3701-0005-nv.dtb /boot/dtb/."
          sudo cp boot/dtb/tegra234-p3737-0000+p3701-0005-nv.dtb /boot/dtb/.
fi

echo "sudo cp boot/Image /boot/${FOLDER}/."
      sudo cp boot/Image /boot/${FOLDER}/.
echo "sudo depmod"
      sudo depmod
echo "done - rebooting"
      sudo reboot
