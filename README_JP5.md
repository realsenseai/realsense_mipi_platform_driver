# Intel® RealSense™ camera driver for GMSL* interface on NVIDIA® Jetson AGX Xavier™ JetPack 5.x.2

# D457 MIPI on NVIDIA® Jetson AGX Xavier™
The Intel® RealSense™ MIPI platform driver enables the user to control and stream RealSense™ 3D MIPI cameras.

The system shall include:
* NVIDIA® Jetson™ platform (Currently Supported JetPack versions are: 5.1.2, 5.0.2)
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

## Build kernel, dtb and D457 driver on host - cross compile x86-64

<details>
<summary>JetPack manual build</summary>

Download Jetson Linux source code tarball from 
- [JetPack 5.1.2 BSP sources](https://developer.nvidia.com/downloads/embedded/l4t/r35_release_v4.1/sources/public_sources.tbz2)
- [JetPack 5.0.2 BSP sources](https://developer.nvidia.com/embedded/l4t/r35_release_v1.0/sources/public_sources.tbz2)

- [JetPack 5.x.2 Toolchain](https://developer.nvidia.com/embedded/jetson-linux/bootlin-toolchain-gcc-93)


## JetPack 5.1.2
```
mkdir -p l4t-gcc/5.x
cd ./l4t-gcc/5.x
wget https://developer.nvidia.com/embedded/jetson-linux/bootlin-toolchain-gcc-93 -O aarch64--glibc--stable-final.tar.gz
tar xf aarch64--glibc--stable-final.tar.gz
cd ../..
wget https://developer.nvidia.com/downloads/embedded/l4t/r35_release_v4.1/sources/public_sources.tbz2
tar xjf public_sources.tbz2
cd Linux_for_Tegra/source/public
tar xjf kernel_src.tbz2
```
## JetPack 5.0.2
```
mkdir -p l4t-gcc/5.x
cd ./l4t-gcc/5.x
wget https://developer.nvidia.com/embedded/jetson-linux/bootlin-toolchain-gcc-93 -O aarch64--glibc--stable-final.tar.gz
tar xf aarch64--glibc--stable-final.tar.gz --strip-components 1
cd ../..
wget https://developer.nvidia.com/embedded/l4t/r35_release_v1.0/sources/public_sources.tbz2
tar xjf public_sources.tbz2
cd Linux_for_Tegra/source/public
tar xjf kernel_src.tbz2
cd ../../..
```

## Apply D457 patches and build the kernel image, dtb and D457 driver.

```
# install dependencies
sudo apt install build-essential bc flex bison

# apply patches
./apply_patches_ext.sh 5.0.2 ./Linux_for_Tegra/source/public

# build kernel, dtb and D457 driver
./build_all.sh 5.0.2 ./Linux_for_Tegra/source/public
```
Note: dev_dbg() log support will not be enabled by default. If needed, run the `./build_all.sh` script with `--dev-dbg` option like below.
```
./build_all.sh --dev-dbg 5.0.2 ./Linux_for_Tegra/source/public
```

</details>

---

The developers can set up the source code with NVIDIA's Jetson git repositories by using the provided scripts.
- Prepare build workspace for cross-compile on host
- Apply necessary patches
- Build workspace
- Deploy build results on target Jetson

```
./setup_workspace.sh 5.0.2

./apply_patches.sh apply 5.0.2

./build_all.sh 5.0.2
```
Note: dev_dbg() log support will not be enabled by default. If needed, run the `./build_all.sh` script with `--dev-dbg` option like below.
```
./build_all.sh --dev-dbg 5.0.2
```

## Install kernel, device-tree and D457 driver to Jetson AGX Xavier

1. Install the kernel and modules

Building with `build_all.sh`

The necessary files are:

- kernel image `images/5.x/arch/arm64/boot/Image`
- dtb `images/5.x/arch/arm64/boot/dts/nvidia/tegra194-p2888-0001-p2822-0000.dtb`
- D457 driver `images/5.x/drivers/media/i2c/d4xx.ko`
- UVC Video driver `images/5.x/drivers/media/usb/uvc/uvcvideo.ko`
- V4L2 Core Video driver `images/5.x/drivers/media/v4l2-core/videobuf-core.ko`
- V4L2 VMalloc Video driver `images/5.x/drivers/media/v4l2-core/videobuf-vmalloc.ko`

Copy build results from Host to Jetson target `10.0.0.116` user `nvidia`
```
scp ./images/5.x/arch/arm64/boot/Image nvidia@10.0.0.116:~/
scp ./images/5.x/arch/arm64/boot/dts/nvidia/tegra194-p2888-0001-p2822-0000.dtb nvidia@10.0.0.116:~/
scp ./images/5.x/drivers/media/i2c/d4xx.ko nvidia@10.0.0.116:~/
scp ./images/5.x/drivers/media/usb/uvc/uvcvideo.ko nvidia@10.0.0.116:~/
scp ./images/5.x/drivers/media/v4l2-core/videobuf-core.ko nvidia@10.0.0.116:~/
scp ./images/5.x/drivers/media/v4l2-core/videobuf-vmalloc.ko nvidia@10.0.0.116:~/
```

Copy them to the right places on Jetson target:
```
sudo mkdir /boot/d457
sudo mkdir /lib/modules/$(uname -r)/updates
sudo cp Image /boot/d457/
sudo cp tegra194-p2888-0001-p2822-0000.dtb /boot/d457/
sudo cp d4xx.ko /lib/modules/$(uname -r)/updates/
sudo cp uvcvideo.ko /lib/modules/$(uname -r)/updates/
sudo cp videobuf-core.ko /lib/modules/$(uname -r)/updates/
sudo cp videobuf-vmalloc.ko /lib/modules/$(uname -r)/updates/
sudo depmod
```

2. Edit `/boot/extlinux/extlinux.conf` primary boot option's LINUX/FDT lines to use built kernel image and dtb file:

    ```
    LINUX /boot/d457/Image
    FDT /boot/d457/tegra194-p2888-0001-p2822-0000.dtb
    ```

```
$ cat /boot/extlinux/extlinux.conf
TIMEOUT 30
DEFAULT d457

MENU TITLE L4T boot options

LABEL primary
      MENU LABEL primary kernel
      LINUX /boot/Image
      INITRD /boot/initrd
      FDT /boot/dtb/tegra194-p2888-0001-p2822-0000.dtb
      APPEND ${cbootargs} quiet root=/dev/mmcblk0p1 rw rootwait rootfstype=ext4 console=ttyTCU0,115200n8 console=tty0 fbcon=map:0 net.ifnames=0 rootfstype=ext4

LABEL d457
      MENU LABEL d457 kernel
      LINUX /boot/d457/Image
      INITRD /boot/initrd
      FDT /boot/d457/tegra194-p2888-0001-p2822-0000.dtb
      APPEND ${cbootargs} root=/dev/mmcblk0p1 rw rootwait rootfstype=ext4 console=ttyTCU0,115200n8 console=tty0 fbcon=map:0 net.ifnames=0 rootfstype=ext4
```


3. Make D457 I2C module autoload at boot time:
    ```
    echo "d4xx" | sudo tee /etc/modules-load.d/d4xx.conf
    ```

After rebooting Jetson, the D457 driver should work.

**NOTE**

- It's recommended to save the original kernel image as backup boot option in `/boot/extlinux/extlinux.conf`.
