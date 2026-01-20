#!/bin/bash

if [ ! -d /boot/dev ]; then
      sudo mkdir /boot/dev
fi

tar xf rootfs.tar.gz
echo "sudo cp -r lib/modules/5.15.148-tegra /lib/modules/."
      sudo cp -r lib/modules/5.15.148-tegra /lib/modules/.
echo "sudo cp boot/tegra234-camera-d4xx-overlay*.dtbo /boot/."
      sudo cp boot/tegra234-camera-d4xx-overlay*.dtbo /boot/.
echo "sudo cp boot/dtb/tegra234-p3737-0000+p3701-0005-nv.dtb /boot/dtb/."
      sudo cp boot/dtb/tegra234-p3737-0000+p3701-0005-nv.dtb /boot/dtb/.
echo "sudo cp boot/Image /boot/dev/."
      sudo cp boot/Image /boot/dev/.
echo "sudo depmod"
      sudo depmod
echo "done - rebooting"
      sudo reboot
