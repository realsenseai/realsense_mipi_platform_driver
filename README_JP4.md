# Intel® RealSense™ camera driver for GMSL* interface on NVIDIA® Jetson AGX Xavier™ JetPack 4.6.1

# D457 MIPI on NVIDIA® Jetson AGX Xavier™
The Intel® RealSense™ MIPI platform driver enables the user to control and stream RealSense 3D MIPI cameras.

The system shall include:
* NVIDIA® Jetson™ platform (Currently Supported JetPack versions are: 4.6.1)
* RealSense™ De-Serialize board (https://store.intelrealsense.com/buy-intel-realsense-des457.html)
* RS MIPI camera (e.g. https://store.intelrealsense.com/buy-intel-realsense-depth-camera-d457.html)

> Note: This MIPI reference driver is based on RealSense™ de-serialize board. For other de-serialize boards, modification might be needed.

### Links
- Intel® RealSense™ camera driver for GMSL* interface [Front Page](./README.md)
- Jetson AGX Orin™ board setup - AGX Orin™ [JetPack 6.x](./README_JP6.md) setup guide
- Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 5.x.2](./README_JP5.md) setup guide
- Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 4.6.1](./README_JP4.md) setup guide
- Build Tools manual page [Build Manual page](./README_tools.md)
- Driver API manual page [Driver API page](./README_driver.md)


## NVIDIA® Jetson AGX Xavier™ board setup for cross compile on x86-64

Please follow the [instruction](https://docs.nvidia.com/sdk-manager/install-with-sdkm-jetson/index.html) to flash JetPack to the Jetson AGX Xavier™ with NVIDIA® SDK Manager or other methods NVIDIA provides. Make sure the board is ready to use.


## Build kernel, dtb and D457 driver - cross compile x86-64

<details>
<summary>JetPack 4.6.1 manual build</summary>

### Download Jetson Linux source code tarball from 
- [JetPack 4.6.1 BSP sources](https://developer.nvidia.com/embedded/l4t/r32_release_v7.1/sources/t186/public_sources.tbz2)
- [JetPack 4.6.1 Toolchain](http://releases.linaro.org/components/toolchain/binaries/7.3-2018.05/aarch64-linux-gnu/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu.tar.xz)

### JetPack 4.6.1 manual workspace setup
```
mkdir -p l4t-gcc/4.6.1
cd ./l4t-gcc/4.6.1
wget http://releases.linaro.org/components/toolchain/binaries/7.3-2018.05/aarch64-linux-gnu/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu.tar.xz
tar xf gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu.tar.xz --strip-components 1
cd ../..
wget https://developer.nvidia.com/embedded/l4t/r32_release_v7.1/sources/t186/public_sources.tbz2
tar xjf public_sources.tbz2
cd Linux_for_Tegra/source/public
tar xjf kernel_src.tbz2
cd ../../..
```

### Apply D457 patches and build the kernel image, dtb and D457 driver.

```
# install dependencies
sudo apt install build-essential bc flex bison

# apply patches
./apply_patches_ext.sh 4.6.1 ./Linux_for_Tegra/source/public

# remove BUILD_NUMBER env dependency kernel vermagic
sed -i s/'UTS_RELEASE=\$(KERNELRELEASE)-ab\$(BUILD_NUMBER)'/'UTS_RELEASE=\$(KERNELRELEASE)'/g ./Linux_for_Tegra/source/public/kernel/kernel-4.9/Makefile
sed -i 's/the-space :=/E =/g' ./Linux_for_Tegra/source/public/kernel/kernel-4.9/scripts/Kbuild.include
sed -i 's/the-space += /the-space = \$E \$E/g' ./Linux_for_Tegra/source/public/kernel/kernel-4.9/scripts/Kbuild.include

# build kernel, dtb and D457 driver
./build_all.sh 4.6.1 ./Linux_for_Tegra/source/public
# Debian build
# ./build_all_deb.sh --no-dbg-pkg 4.6.1 ./Linux_for_Tegra/source/public
```
</details>

---

The developers can set up the source code with NVIDIA's Jetson git repositories by using the provided scripts.
- Prepare build workspace for cross-compile on host
- Apply necessary patches
- Build workspace
- Deploy build results on target Jetson

```
# prepare workspace
./setup_workspace.sh 4.6.1

# apply patches
./apply_patches.sh apply 4.6.1

# install dependencies
sudo apt install build-essential bc flex bison

# build kernel, dtb and D457 driver

# method 2: build kernel and modules only
./build_all.sh 4.6.1

```

Debian build:
```
./build_all_deb.sh [--no-dbg-pkg] 4.6.1 
```

Debian packages will be generated in `images` folder.

## Install kernel, device-tree and D457 driver to Jetson AGX Xavier

1. Install the kernel and modules

1.2 If building with `build_all.sh`

The necessary files are:

- kernel image `images/4.6.1/arch/arm64/boot/Image`
- dtb `images/4.6.1/arch/arm64/boot/dts/tegra194-p2888-0001-p2822-0000.dtb`
- D457 driver `images/4.6.1/drivers/media/i2c/d4xx.ko`
- UVC Video driver `images/4.6.1/drivers/media/usb/uvc/uvcvideo.ko`
- V4L2 Core Video driver `images/4.6.1/drivers/media/v4l2-core/videobuf-core.ko`
- V4L2 VMalloc Video driver `images/4.6.1/drivers/media/v4l2-core/videobuf-vmalloc.ko`

Copy them to the right places on Jetson target:
```
sudo cp Image /boot
sudo cp tegra194-p2888-0001-p2822-0000.dtb /boot/dtb
sudo cp d4xx.ko /lib/modules/$(uname -r)/kernel/drivers/media/i2c/
sudo cp uvcvideo.ko /lib/modules/$(uname -r)/kernel/drivers/media/usb/uvc/
sudo cp videobuf-core.ko /lib/modules/$(uname -r)/kernel/drivers/media/v4l2-core/
sudo cp videobuf-vmalloc.ko /lib/modules/$(uname -r)/kernel/drivers/media/v4l2-core/
sudo depmod
```

2. Edit `/boot/extlinux/extlinux.conf` primary boot option's LINUX/FDT lines to use built kernel image and dtb file:

```
LINUX /boot/Image
FDT /boot/dtb/tegra194-p2888-0001-p2822-0000.dtb
```

3. Make D457 I2C module autoload at boot time: `echo "d4xx" | sudo tee /etc/modules-load.d/d4xx.conf`

After rebooting Jetson, the D457 driver should work.

### Verify driver
- Driver API manual page [Driver API page](./README_driver.md)

## Available directives on max9295/max9296 register setting

- Dump registers
```
cat /sys/bus/i2c/drivers/max9295/30-0040/register_dump
cat /sys/bus/i2c/drivers/max9296/30-0048/register_dump
```

- Dump setting version

```
cat /sys/module/max9295/parameters/max9295_setting_verison
cat /sys/module/max9296/parameters/max9296_setting_verison
```

- Disable updating setting dynamically (updating setting manually by running script).
  **0** means disable updating setting dynamically, while **1** means enable updating setting dynamically.

```
echo 0 | sudo tee /sys/module/max9295/parameters/max9295_dynamic_update
echo 0 | sudo tee /sys/module/max9296/parameters/max9296_dynamic_update
```

- Refresh max9295/max9295 register values, this is used for forcely set serdes setting when necessary

```
echo 1 | sudo tee /sys/bus/i2c/drivers/max9295/30-0040/refresh_setting
echo 1 | sudo tee /sys/bus/i2c/drivers/max9296/30-0048/refresh_setting
```
**NOTE**
- It's recommended to save the original kernel image as backup boot option in `/boot/extlinux/extlinux.conf`.
