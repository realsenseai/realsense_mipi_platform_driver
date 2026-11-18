# RealSense™ camera driver for GMSL* interface

# D457 MIPI on NVIDIA® Jetson AGX Xavier™ and AGX Orin™ build scripts manual pages

### Links
- RealSense™ camera driver for GMSL* interface [Front Page](./README.md)
- NVIDIA® Jetson AGX Orin™ board setup - AGX Orin™ [JetPack 6.x](./README_JP6.md) setup guide
- NVIDIA® Jetson AGX Xavier™ board setup - AGX Xavier™ [JetPack 5.x.2](./README_JP5.md) setup guide
- Build Tools manual page [Build Manual page](./README_tools.md)
- Driver API manual page [Driver API page](./README_driver.md)

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
- JetPack 5.x.2: `sources_5.x`
- JetPack 6.x: `sources_6.x`
```
./setup_workspace.sh [--sources-archive <file>] [JetPack_version]
```
Example setup build workspace for JetPack 5.0.2:
```
./setup_workspace.sh 5.0.2
```
`--sources-archive <file>` takes the sources from a local archive instead of cloning them. It accepts NVIDIA's `public_sources.tbz2` for that JetPack, or a `backup_sources_<JetPack>.tar.gz` git-tree cache. For a `public_sources.tbz2` it unpacks the nested kernel (and on 6.x/7.x the OOT-module and display) bundles and makes each patched directory its own git repo, tagged with the L4T version. This is how Jenkins builds, and it refuses to run if `sources_<JetPack>` already exists:
```
./setup_workspace.sh --sources-archive public_sources.tbz2 6.2
```
Without the option, a git-tree cache at `/home/nvidia_sources_cache/backup_sources_<JetPack>.tar.gz` is used when present.

# apply_patches.sh
Apply D457 patches for kernel image, dtb and D457 driver.

```
./apply_patches.sh [--one-cam | --dual-cam] [JetPack_version]
```
Reset D457 patches (and any other changes) for kernel image, dtb and D457 driver.
```
./apply_patches.sh reset [JetPack_version]
```

Note: The `--one-cam` and `--dual-cam` option applies only for JetPack 5.0.2,
compatible with RealSense™ deserializer.
- By setting the `--one-cam` option it builds DT with only camera on GMSL link A (default).

- By setting the `--dual-cam` option it builds DT with dual cameras on GMSL link A and B.
- The default is to single camera configuration for JetPack 5.0.2.

Every patched directory must be its own git repo, owned by the user running the script. Before changing anything it exits with:
- `2`: the sources are missing, or a patched directory is not a git tree of its own (remove `sources_<JP>` if it exists, then run `setup_workspace.sh`, with `--sources-archive` for a tarball);
- `3`: git refused the repo, usually because another user owns it (for example after `sudo setup_workspace.sh`). Git's own message is printed with it.

# build_all.sh
Build kernel and modules only
```
./build_all.sh [JetPack_version] [JetPack_source_dir]
```
Example: build JetPack 5.0.2 workspace created by `setup_workspace.sh`:
```
./build_all.sh 5.0.2
```
`JetPack_source_dir` is only for a tree that already exists and was patched some other way (for example, an old release tag that still ships its own patch script):
```
./build_all.sh 6.0 ./Linux_for_tegra/source
```
The following directories will be created:
- JetPack 5.x.2: `images/5.x`
- JetPack 6.x: `images/6.x`

# build_all_deb.sh
Build kernel Debian packages for JetPack 5.0.2
```
./build_all_deb.sh [--no-dbg-pkg] [JetPack_version] [JetPack_source_dir]
```

Debian packages will be generated in `images` folder.

Example:

- Debian package `linux-image-5.10.104-d457_5.10.104-d457-1_arm64.deb`
- Install with `sudo dpkg -i linux-image-5.10.104-d457_5.10.104-d457-1_arm64.deb`
- The header, libc-dev, dbg and firmware packages are optional.


**NOTE**

- Each JetPack version's kernel may be different, the user needs to change the kernel version in file names and paths accordingly.
- It's recommended to save the original kernel image as backup boot option in `/boot/extlinux/extlinux.conf`.
