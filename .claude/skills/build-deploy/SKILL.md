---
name: build-deploy
description: Build and deploy the RealSense MIPI platform driver for NVIDIA Jetson. Use when the user wants to build the kernel/driver/DTBs for a specific JetPack version, deploy to a Jetson device, or troubleshoot build issues. Triggers on requests mentioning build, compile, deploy, flash, install kernel, or JetPack version numbers (4.6.1, 5.0.2, 5.1.2, 6.0, 6.1, 6.2, 6.2.1).
---

# Build & Deploy Skill

## Supported JetPack Versions

Valid versions: `4.6.1`, `5.0.2`, `5.1.2`, `6.0`, `6.1`, `6.2`, `6.2.1`

Always ask the user which JetPack version to target if not specified.

## Build Workflow

All commands run from the repository root. The `$VERSION` placeholder below refers to the JetPack version (e.g., `6.2`).

### Step 1: Apply patches (if workspace already set up)

Requires `git config user.name` and `git config user.email` to be set.

Always reset patches before re-applying:
```bash
./apply_patches.sh $VERSION reset
```

```bash
./apply_patches.sh $VERSION
```

### Step 2: Build

```bash
./build_all.sh $VERSION
```

Flags:
- `--clean` — remove previous build artifacts before building
- `--dev-dbg` — enable `CONFIG_DYNAMIC_DEBUG` and `CONFIG_DYNAMIC_DEBUG_CORE` in kernel

Output directory: `images/$VERSION/` (normalized: `images/6.x/` for JP 6.x, `images/5.x/` for JP 5.x).

For Debian packages:
```bash
./build_all_deb.sh [--no-dbg-pkg] $VERSION
```

### Step 3: Deploy to Jetson

To deploy use this bash command:

```bash
./scripts/deploy_kernel.sh $VERSION <TARGET_IP> [USERNAME] [REMOTE_PATH]
```

Defaults: USERNAME=`administrator`, REMOTE_PATH='git.USER.NAME'

The command must have all 3 arguments to perform the full deploy.
Ask the user to provide username and remote path if not provided.
Save in mempory for the next deploy command.

Deploy packs build artifacts into `kernel_mod/$VERSION/`, SCPs to the Jetson, runs the on-device install script, then reboots.

Without a TARGET argument, deploy only packages locally (no SCP/reboot).

reboot of the jetson will take about 2-5 minutes. After reboot, the new kernel/modules should be active.

## Build Architecture Details

### What gets built per JetPack generation

**JP 6.x (Orin):** Out-of-tree module build. Builds kernel image, NVIDIA OOT modules (nvidia-oot, nvgpu, etc.), device tree overlays (`tegra234-camera-d4xx-overlay*.dtbo`). Sources in `sources_6.x/`.

**JP 5.x / 4.6.1 (Xavier):** In-tree kernel build with `tegra_defconfig`. Builds kernel image, DTBs, and modules. Sources in `sources_5.x/` or `sources_4.6.1/`.

### Cross-compilation

Native builds on aarch64 skip toolchain setup. Cross-compilation toolchains are in `l4t-gcc/$VERSION/` (installed by `setup_workspace.sh`):
- JP 4.6.1: `aarch64-linux-gnu-`
- JP 5.x: `aarch64-buildroot-linux-gnu-`
- JP 6.x: `aarch64-buildroot-linux-gnu-`

### Key files copied during patch application

- `kernel/realsense/d4xx.c` → `sources_*/nvidia-oot/drivers/media/i2c/` (JP 6.x) or `sources_*/kernel/nvidia/drivers/media/i2c/` (JP 4/5)
- `hardware/realsense/tegra234-camera-d4xx-overlay*.dts` → overlay dir (JP 6.x)
- `hardware/realsense/tegra194-camera-d4xx-*.dtsi` → DT dir (JP 4/5)

### Step 4: Verify deployment

After deploy and reboot, SSH into the Jetson and run:

```bash
sudo dmesg | grep d4xx          # Check driver probe — expect "d4xx" probe messages with no errors
ls -l /dev/video*                # Should show 6 video devices per camera (video0–video5)
v4l2-ctl -d0 --stream-mmap      # Verify streaming works
```

If `dmesg` shows no d4xx messages or `/dev/video*` devices are missing, the driver did not load — check for patch/build version mismatch or missing DTB overlay.

## Common Issues

- **Patches fail to apply**: Run `./apply_patches.sh $VERSION reset` first, then re-apply.
- **Missing git identity**: Set `git config user.name` and `git config user.email` before `apply_patches.sh`.
- **Workspace not set up**: Run `./setup_workspace.sh $VERSION` first (downloads NVIDIA sources + toolchain).
- **BUILD_NUMBER set**: If `BUILD_NUMBER` env var is set (common in CI), it changes the kernel vermagic string.
