#!/bin/bash
set -e

# SECURITY WARNING: The sudoers configuration below grants passwordless root access.
# Only use in isolated CI/CD environments with restricted network access.
# Never enable on production systems or systems with sensitive data.
# Consider using dedicated CI service accounts with audit logging.
#
# To enable for jenkins/CI user (NOT RECOMMENDED for production):
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

