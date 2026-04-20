# RealSense™ camera driver for GMSL* interface

# D457 MIPI on NVIDIA® Jetson AGX Orin™ JetPack 6.x 
The RealSense™ MIPI platform driver enables the user to control and stream RealSense™ 3D MIPI cameras.
The system shall include:
* NVIDIA® Jetson™ platform Supported JetPack versions are:
    - [6.2.2 production release](https://developer.nvidia.com/embedded/jetpack-sdk-622)
    - [6.2.1 production release](https://developer.nvidia.com/embedded/jetpack-sdk-621)
    - [6.2 production release](https://developer.nvidia.com/embedded/jetpack-sdk-62)
    - [6.1 production release](https://developer.nvidia.com/embedded/jetpack-sdk-61)
    - [6.0 production release](https://developer.nvidia.com/embedded/jetpack-sdk-60)
* RealSense™ De-Serialize board
* Jetson AGX Orin™ Passive adapter board from [Leopard Imaging® LI-JTX1-SUB-ADPT](https://leopardimaging.com/product/accessories/adapters-carrier-boards/for-nvidia-jetson/li-jtx1-sub-adpt/)
* RS MIPI camera [D457](https://store.realsenseai.com/buy-intel-realsense-depth-camera-d457.html)

![orin_adapter](https://github.com/dmipx/realsense_mipi_platform_driver/assets/104717350/524e3eb6-6e6b-41cf-9562-9c0f920dd821)


> Note: This MIPI reference driver is based on RealSense™ de-serialize board. For other de-serialize boards, modification might be needed. 

### Links
- RealSense™ camera driver for GMSL* interface [Front Page](./README.md)
- NVIDIA® Jetson AGX Orin™ board setup - AGX Orin™ [JetPack 7.x](./README_JP7.md) setup guide
- NVIDIA® Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 5.x.2](./README_JP5.md) setup guide
- NVIDIA® Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 4.6.1](./README_JP4.md) setup guide
- Build Tools manual page [Build Manual page](./README_tools.md)
- Driver API manual page [Driver API page](./README_driver.md)

## NVIDIA® Jetson AGX Orin™ board setup

Please follow the [instruction](https://docs.nvidia.com/sdk-manager/install-with-sdkm-jetson/index.html) to flash JetPack to the NVIDIA® Jetson AGX Orin™ with NVIDIA® SDK Manager or other methods NVIDIA provides. Make sure the board is ready to use.

## Build environment prerequisites
```
sudo apt-get install -y build-essential bc wget flex bison curl libssl-dev xxd tar
```
## Build NVIDIA® kernel drivers, dtb and D457 driver

These are descriptiver steps. Bash commands to be issued follow:
1. Clone [realsense_mipi_platform_driver](https://github.com/realsenseai/realsense_mipi_platform_driver.git) repo.
2. Checkout the `dev` branch.
3. Set up build environment, ARM64 compiler, kernel sources and NVIDIA's Jetson git repositories by using the setup script.
4. Apply patches for kernel drivers, nvidia-oot module and tegra devicetree.
5. Build the project
6. Apply build results to the target (Jetson).
7. Configure the target.

Assuming building for 6.2.2. One can also build for 6.2.1, 6.2, 6.1, 6.0 just replace the parameter to ./setup_workspace.sh script.
Build version can be specified only once. It will be written to jetpack_version file and used for later steps.
You can display the current version running any script below with -h option. Effective version will be also shown while running any script.
```
git clone --branch dev --single-branch https://github.com/realsenseai/realsense_mipi_platform_driver.git
cd realsense_mipi_platform_driver
./setup_workspace.sh 6.2
./apply_patches.sh
./build_all.sh
```
Note: dev_dbg() log support will not be enabled by default. If needed, run the `./build_all.sh` script with `--dev-dbg` option like below.
```
./build_all.sh --dev-dbg
```
## Install kernel drivers, extra modules and device-tree to Jetson AGX Orin™

Following steps required:

1. Copy build artifacts:

If you build locally (native build on Jetson) use the following bash commands:
```
sudo cp -r images/$(cat jetpack_version)/rootfs/lib/modules/$(cat kernel_version) /lib/modules/
sudo cp -r images/$(cat jetpack_version)/rootfs/boot/dtb /boot/dev/
sudo cp -v images/$(cat jetpack_version)/rootfs/boot/vmlinu?-$(cat kernel_version) /boot/dev/
sudo ln -sfT /boot/dev/vmlinu?-$(cat kernel_version) /boot/dev/Image
```
In case of crossbuild on external host prepare a tarball to ssh-copy to Jetson target.
Example user 'nvidia' on Jetson with host name 'jetson.domain'
```
tar czf rootfs.tar.gz -C images/$(cat jetpack_version)/rootfs boot lib kernel_version
scp rootfs.tar.gz nvidia@jetson.domain:
```
Log in into Jetson target, extract the tarball and install extracted files:
```
rm -rf boot lib
tar xf rootfs.tar.gz
sudo cp -r lib/modules/$(cat kernel_version) /lib/modules/
sudo cp -r boot/dtb /boot/dev/
sudo cp -v boot/vmlinu?-$(cat kernel_version) /boot/dev/
sudo ln -sfT /boot/dev/vmlinu?-$(cat kernel_version) /boot/dev/Image
```
2.	Enable and run depmod scan for "extra" & "kernel" modules
```
# original file content: cat /etc/depmod.d/ubuntu.conf -- search updates ubuntu built-in
sudo sed -i 's/search updates/search extra updates kernel/g' /etc/depmod.d/ubuntu.conf
# update driver cache
sudo depmod
```
3.	Update initrd (regenerate kernel modules)

The kernel patches modify the I2C subsystem header (`i2c.h`), which changes the CRC of all exported I2C symbols. The boot initrd contains cached kernel modules that must be regenerated to match the new kernel, otherwise modules like `ucsi_ccg` will fail to load with "disagrees about version of symbol" errors.
```
sudo update-initramfs -u -k $(cat kernel_version)
sudo ln -sfT /boot/initrd.img-$(cat kernel_version) /boot/dev/initrd
```
4. Select the correct overlay for your HW:

    Currently supported overlays are -

    | Overlay file | Description |
    |---|---|
    | `tegra234-camera-d4xx-overlay.dtbo` | max9296 deserializer board |
    | `tegra234-camera-d4xx-overlay.calib.dtbo` | max9296 deserializer board w/o IR metadata (For calib) |
    | `tegra234-camera-d4xx-overlay-dual.dtbo` | max9296 deserializer board w/ two connected cameras |
    | `tegra234-camera-d4xx-overlay-dual.calib.dtbo` | max9296 deserializer board w/ two connected cameras w/o IR metadata (For calib) |
    | `tegra234-camera-d4xx-overlay-max96712-EVB.dtbo` | max96712 evaluation board |
    | `tegra234-camera-d4xx-overlay-max96712-EVB.calib.dtbo` | max96712 evaluation board w/o IR metadata (For calib) |
    | `tegra234-camera-d4xx-overlay-max96712-EVB-cams-0-1.dtbo` | max96712 evaluation board w/ two connected cameras |
    | `tegra234-camera-d4xx-overlay-fg12-16ch.dtbo` | Fangzhu fg12-16ch board with a single camera connected to cam0 |
    | `tegra234-camera-d4xx-overlay-fg12-16ch.calib.dtbo` | Fangzhu fg12-16ch board with a single camera connected to cam0 w/o IR metadata (For calib) |
    | `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0-1.dtbo` | Fangzhu fg12-16ch board with two cameras connected to cam0 & cam1 |
    | `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0-1-2-3.dtbo` | Fangzhu fg12-16ch board with four cameras connected to cam0,1,2 & 3 (all links of the first deserializer) |
    | `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0-4.dtbo` | Fangzhu fg12-16ch board with two cameras connected to cam0 & cam4 (one camera per deserializer) |
    | `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0-4.calib.dtbo` | Fangzhu fg12-16ch board with two cameras connected to cam0 & cam4 (one camera per deserializer) w/o IR metadata (For calib) |
    | `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0-4-8-12.dtbo` | Fangzhu fg12-16ch board with four cameras connected to cam0,4,8 & 12 (one camera per deserializer) |
    | `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0-4-8-12.calib.dtbo` | Fangzhu fg12-16ch board with four cameras connected to cam0,4,8 & 12 (one camera per deserializer) w/o IR metadata (For calib) |
    | `tegra234-camera-d4xx-overlay-fg12-16ch-PWR-only.dtbo` | Fangzhu fg12-16ch board ONLY POWER GPIOS (driver will not be probed) - for development use |
    | `tegra234-camera-d4xx-overlay-advantech.dtbo` | Advantech board with one camera connected to bottom right of the left port |
    | `tegra234-camera-d4xx-overlay-avermedia.dtbo` | AverMedia board with one camera connected to bottom right of the right port |
    | `tegra234-camera-d4xx-overlay-seeed.dtbo` | Seeed reComputer board with one camera connected to top right link |
    | `tegra234-camera-d4xx-overlay-seeed-cams-0-1.dtbo` | Seeed reComputer board with two cameras connected to top two links |
    | `tegra234-camera-d4xx-overlay-seeed-cams-0-1-2-3.dtbo` | Seeed reComputer board with four cameras connected |

5. Modify bootloader configuration:
 - Open /boot/extlinux/extlinux.conf for editing using sudo and your preferred editor
 - Copy existing primary section and rename the new section LABEL to "JetsonIO" or other meaningful name
 - Set the following entries in the new section:
    - MENU LABEL to a meaningful label (e.g "development kernel")
    - LINUX /boot/dev/Image
    - INITRD /boot/dev/initrd
    - FDT line pointing at the correct device tree 
       - For Orin devkit: /boot/dev/dtb/tegra234-p3737-0000+p3701-0000-nv.dtb (copied in step 2)
       - For Orin production board: /boot/dev/dtb/tegra234-p3737-0000+p3701-0005-nv.dtb (copied in step 2)
       - For Seeed Orin nano: /boot/dtb/kernel_tegra234-j401-p3768-0000+p3767-0004-recomputer-robo-gmsl.dtb (Requires Seeed's GMSL-enabled BSP - see step 2)
 - Add the "OVERLAYS" line pointing to the required overlay as chosen in step 5 (e.g /boot/dev/dtb/tegra234-camera-d4xx-overlay.dtbo)
 - Change the DEFAULT entry on top to select the new LABEL name as the default

The result should be (e.g for Orin devkit with max9296 overlay):

```
cat /boot/extlinux/extlinux.conf
----<CUT>----
LABEL JetsonIO
    MENU LABEL Custom Header Config: <CSI Jetson RealSense Camera D457>
    LINUX /boot/dev/Image
    INITRD /boot/dev/initrd
    APPEND ${cbootargs} root=<Long APPEND line copied from primary...>
    FDT /boot/dtb/tegra234-p3737-0000+p3701-0000-nv.dtb
    OVERLAYS /boot/dev/dtb/tegra234-camera-d4xx-overlay.dtbo
----<CUT>----
```
5.
Reboot cycling the power or using shell command
```
sudo reboot
```

### Verify driver loaded - on Jetson:
- Driver API manual page [Driver API page](./README_driver.md)

```
nvidia@ubuntu:~$ sudo dmesg | grep tegra-capture-vi
[    9.357521] platform 13e00000.host1x:nvcsi@15a00000: Fixing up cyclic dependency with tegra-capture-vi
[    9.419926] tegra-camrtc-capture-vi tegra-capture-vi: ep of_device is not enabled endpoint.
[    9.419932] tegra-camrtc-capture-vi tegra-capture-vi: ep of_device is not enabled endpoint.
[   10.001170] tegra-camrtc-capture-vi tegra-capture-vi: subdev DS5 mux 9-001a bound
[   10.025295] tegra-camrtc-capture-vi tegra-capture-vi: subdev DS5 mux 12-001a bound
[   10.040934] tegra-camrtc-capture-vi tegra-capture-vi: subdev DS5 mux 13-001a bound
[   10.056151] tegra-camrtc-capture-vi tegra-capture-vi: subdev DS5 mux 14-001a bound
[   10.288088] tegra-camrtc-capture-vi tegra-capture-vi: subdev 13e00000.host1x:nvcsi@15a00000- bound
[   10.324025] tegra-camrtc-capture-vi tegra-capture-vi: subdev 13e00000.host1x:nvcsi@15a00000- bound
[   10.324631] tegra-camrtc-capture-vi tegra-capture-vi: subdev 13e00000.host1x:nvcsi@15a00000- bound
[   10.325056] tegra-camrtc-capture-vi tegra-capture-vi: subdev 13e00000.host1x:nvcsi@15a00000- bound

nvidia@ubuntu:~$ sudo dmesg | grep d4xx
[    9.443608] d4xx 9-001a: Probing driver for D45x
[    9.983168] d4xx 9-001a: ds5_chrdev_init() class_create
[    9.989521] d4xx 9-001a: D4XX Sensor: DEPTH, firmware build: 5.15.1.0
[   10.007813] d4xx 12-001a: Probing driver for D45x
[   10.013899] d4xx 12-001a: D4XX Sensor: RGB, firmware build: 5.15.1.0
[   10.025787] d4xx 13-001a: Probing driver for D45x
[   10.029095] d4xx 13-001a: D4XX Sensor: Y8, firmware build: 5.15.1.0
[   10.041282] d4xx 14-001a: Probing driver for D45x
[   10.044759] d4xx 14-001a: D4XX Sensor: IMU, firmware build: 5.15.1.0

```

### Known issues
- Camera not recognized
Verify I2C MUX detected. If "probe failed" reported, replace extension board adapter (LI-JTX1-SUB-ADPT).
```
nvidia@ubuntu:~$ sudo dmesg | grep pca954x
[    3.933113] pca954x 2-0072: probe failed
```

- kernel does not recognize the I2C device
```
# Make sure which Jetson Carrier board is used:
#   p3701-0000 → Dev kit carrier board
#   p3701-0005 → Production carrier board or custom carrier
# if you have the *0005* board, replace the relevant dtb file in in the instructions above

Example: 
sudo cat /proc/device-tree/compatible

Output:
nvidia,p3701-0000
```
### Notes
- With the introduction of meta data support for depth IR starting from release r/1.0.1.27, calibration format streaming requires a separate DTB that disables meta data since the camera does not metadata in calibration mode.
- If calibration is needed, it's recommended to add `d457_calib` boot option as shown below.

```
LABEL primary
    MENU LABEL primary kernel
    LINUX /boot/Image
    INITRD /boot/initrd
    APPEND ${cbootargs} root=PARTUUID=634b7e44-aacc-4dd9-a769-3a664b83b159 rw rootwait rootfstype=ext4 mminit_loglevel=4 console=ttyTCU0,115200 console=ttyAMA0,115200 firmware_class.path=/etc/firmware fbcon=map:0 net.ifnames=0 nospectre_bhb video=efifb:off console=tty0 nv-auto-config

LABEL JetsonIO
    MENU LABEL Custom Header Config: <CSI Jetson RealSense Camera D457 dual>
    LINUX /boot/dev/vmlinux-<ver from previous step>
    FDT /boot/dtb/kernel_tegra234-p3737-0000+p3701-0000-nv.dtb
    INITRD /boot/initrd.img-<ver from previous step>
    APPEND ${cbootargs} root=PARTUUID=634b7e44-aacc-4dd9-a769-3a664b83b159 rw rootwait rootfstype=ext4 mminit_loglevel=4 console=ttyTCU0,115200 console=ttyAMA0,115200 firmware_class.path=/etc/firmware fbcon=map:0 net.ifnames=0 nospectre_bhb video=efifb:off console=tty0 nv-auto-config
    OVERLAYS /boot/dev/dtb/tegra234-camera-d4xx-overlay-dual.dtbo

LABEL JetsonIO_calib
    MENU LABEL Custom Header Config: <CSI Jetson RealSense Camera D457 dual - Calibration>
    LINUX /boot/dev/vmlinux-<ver from previous step>
    FDT /boot/dtb/kernel_tegra234-p3737-0000+p3701-0000-nv.dtb
    INITRD /boot/initrd.img-<ver from previous step>
    APPEND ${cbootargs} root=PARTUUID=634b7e44-aacc-4dd9-a769-3a664b83b159 rw rootwait rootfstype=ext4 mminit_loglevel=4 console=ttyTCU0,115200 console=ttyAMA0,115200 firmware_class.path=/etc/firmware fbcon=map:0 net.ifnames=0 nospectre_bhb video=efifb:off console=tty0 nv-auto-config
    OVERLAYS /boot/dev/dtb/tegra234-camera-d4xx-overlay-dual.calib.dtbo
```
<<<<<<< HEAD

### External Sync (fg12-16ch)

For multi-camera frame synchronization on the fg12-16ch board using TSC signal generators or an external signal source, see the [External Sync Guide](./docs/external-sync-fg12-16ch.md).
