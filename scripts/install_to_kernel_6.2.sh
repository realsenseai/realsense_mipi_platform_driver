#!/bin/bash

# Display help message
if [ "$#" -eq 0 ] || [ "$1" == "-h" ] || [ "$1" == "--help" ]; then
    echo "Usage: $0 [BOOT_FOLDER]"
    echo ""
    echo "Update the kernel modules and boot files on the local device."
    echo ""
    echo "Arguments:"
    echo "  BOOT_FOLDER   Folder name under /boot to copy Image (default: dev)"
    echo ""
    echo "Example:"
    echo "  $0 foo"
    exit 0
fi

# Set default folder name
FOLDER="${1:-dev}"

if [ ! -d /boot/${FOLDER} ]; then
      sudo mkdir /boot/${FOLDER}
fi

tar xf rootfs.tar.gz
echo "sudo cp -r lib/modules/5.15.148-tegra /lib/modules/."
      sudo cp -r lib/modules/5.15.148-tegra /lib/modules/.
echo "sudo cp boot/tegra234-camera-d4xx-overlay*.dtbo /boot/."
      sudo cp boot/tegra234-camera-d4xx-overlay*.dtbo /boot/.
echo "sudo cp boot/dtb/tegra234-p3737-0000+p3701-0005-nv.dtb /boot/dtb/."
      sudo cp boot/dtb/tegra234-p3737-0000+p3701-0005-nv.dtb /boot/dtb/.
echo "sudo cp boot/Image /boot/${FOLDER}/."
      sudo cp boot/Image /boot/${FOLDER}/.
echo "sudo depmod"
      sudo depmod
echo "done - rebooting"
      sudo reboot
