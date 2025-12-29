#!/bin/bash
set -e

# add jenkins user to sudo group
# add the following line to /etc/sudoers for jenkins user, here nvidia
# nvidia ALL=(root) NOPASSWD: /sbin/reboot, /sbin/install.tegra.artifacts.sh

RELEASE=$(ls lib/modules)
cp -R lib/modules/* /lib/modules
[[ -f /boot/Image ]] && rm /boot/Image
[[ -f /boot/initrd.img ]] && rm /boot/initrd.img
cp boot/Image /boot/Image-$RELEASE
ln -s /boot/Image-$RELEASE /boot/Image
update-initramfs -uk $RELEASE
ln -s /boot/initrd.img-$RELEASE /boot/initrd.img
cp boot/*.dtbo /boot/

