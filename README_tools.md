# Intel® RealSense™ camera driver for GMSL* interface

# D457 MIPI on Jetson AGX Xavier and Orin build scripts manual pages

- Jetson AGX Xavier board setup - AGX Xavier [JetPack 5.0.2](./README.md) setup guide

- Jetson AGX Orin board setup - AGX Orin [JetPack 6.0](./README_JP6.md) setup guide

# Build dependencies
```
sudo apt install -y build-essential bc wget flex bison curl libssl-dev xxd
```

# setup_workspace.sh
The developers can set up the source code with NVIDIA's Jetson git repositories by using the provided setup script:

Using setup script, recommended for developers.
If JetPack version is not given, default version, 5.0.2, will be chosen.

This script prepare all necessary tools and sources to build JetPack BSP.

The following directories will be created:
- JetPack 4.6.1: `sources_4.6.1`
- JetPack 5.0.2: `sources_5.0.2`
- JetPack 5.1.2: `sources_5.1.2`
- JetPack 6.0: `sources_6.0`
```
./setup_workspace.sh [JetPack_version]
```
Example setup build workspace for JetPack 5.0.2:
```
./setup_workspace.sh 5.0.2
```
# apply_patches.sh
Apply D457 patches for kernel image, dtb and D457 driver.

```
./apply_patches.sh [--one-cam | --dual-cam] apply [JetPack_version]
```
Reset D457 patches for kernel image, dtb and D457 driver.
```
./apply_patches.sh reset [JetPack_version]
```

Note: The `--one-cam` and `--dual-cam` option applies only for JetPack 5.0.2,
compatible with adapter: https://store.intelrealsense.com/buy-intel-realsense-des457.html.
- By setting the `--one-cam` option it builds DT with only camera on GMSL link A (default).

- By setting the `--dual-cam` option it builds DT with dual cameras on GMSL link A and B.
- The default is to single camera configuration for JetPack 5.0.2.

# apply_patches_ext.sh
When setup_workspace.sh was not used, using sources direct download method (example: for CI build)

Apply D457 patches for external build the kernel image, dtb and D457 driver.
```
./apply_patches_ext.sh [--one-cam | --dual-cam] ./Linux_for_tegra/source/public [JetPack_version]
```
Example: for JP5.0.2, source path is ./Linux_for_tegra/source :
```
./apply_patches_ext.sh ./Linux_for_tegra/source 5.0.2
```

# build_all.sh
build kernel and modules only
```
./build_all.sh [JetPack_version] [JetPack_source_dir]
```
Example: build JetPack 5.0.2 workspace created by `setup_workspace.sh`:
```
./build_all.sh 5.0.2
```
Example: build JetPack 5.0.2 workspace created by direct source download
```
./build_all.sh 6.0 ./Linux_for_tegra/source
```
The following directories will be created:
- JetPack 4.6.1: `images/4.6.1`
- JetPack 5.0.2: `images/5.0.2`
- JetPack 5.1.2: `images/5.1.2`
- JetPack 6.0: `images/6.0`

# build_all_deb.sh
Build kernel Debian packages for JetPack 5.0.2 or 4.6.1
```
./build_all_deb.sh [--no-dbg-pkg] [JetPack_version] [JetPack_source_dir]
```

Debian packages will be generated in `images` folder.

Example:

- Debian package `linux-image-5.10.104-d457_5.10.104-d457-1_arm64.deb`
- Install with `sudo dpkg -i linux-image-5.10.104-d457_5.10.104-d457-1_arm64.deb`
- The header, libc-dev, dbg and firmware packages are optional.


**NOTE**

- Each JetPack version's kernel may be different, the user needs to change the kernel version in file names and paths accordingly, for example for JetPack 4.6.1 the version is `4.9.253-d457` or `4.9.253-tegra`, depending on the build method applied.
- For JetPack 4.6.1, the dtb file is not included in the deb package. User needs to manually copy `images/4.6.1/arch/arm64/boot/dts/tegra194-p2888-0001-p2822-0000.dtb` file to board and edit `extlinux.conf` to point to it.
- It's recommended to save the original kernel image as backup boot option in `/boot/extlinux/extlinux.conf`.