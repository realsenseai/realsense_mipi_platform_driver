---
name: build
description: Build the RealSense MIPI platform driver for NVIDIA Jetson. Use when the user wants to build the kernel/driver/DTBs for a specific JetPack version or troubleshoot build issues. Triggers on requests mentioning build, compile, make, or JetPack version numbers (4.6.1, 5.0.2, 5.1.2, 6.0, 6.1, 6.2, 6.2.1).
---

# Build Skill

## Supported JetPack Versions

Valid versions: `4.6.1`, `5.0.2`, `5.1.2`, `6.0`, `6.1`, `6.2`, `6.2.1`

Always ask the user which JetPack version to target if not specified.

## Build Workflow

All commands run from the repository root. The `$VERSION` placeholder below refers to the JetPack version (e.g., `6.2`).

### Step 1: Ask whether to apply patches

Ask the user whether they need to apply patches or just copy `d4xx.c` and build.

- **Apply patches** — Full reset and re-apply of all patches. Required after a fresh workspace setup, when kernel/DT patches changed, or when the user explicitly asks for it.
- **Copy d4xx.c only** — Quick path when only the d4xx driver source changed. Skips patch reset/apply and just copies the driver file to the build tree.

#### Option A: Full patch apply

Requires `git config user.name` and `git config user.email` to be set.

Always reset patches before re-applying:
```bash
./apply_patches.sh $VERSION reset
./apply_patches.sh $VERSION
```

#### Option B: Copy d4xx.c only (skip patches)

Copy the driver source directly to the build tree:

- **JP 6.x (Orin):**
  ```bash
  cp kernel/realsense/d4xx.c sources_$VERSION/nvidia-oot/drivers/media/i2c/d4xx.c
  ```
- **JP 4.x / 5.x (Xavier):**
  ```bash
  cp kernel/realsense/d4xx.c sources_$VERSION/kernel/nvidia/drivers/media/i2c/d4xx.c
  ```

Where `$VERSION` is the actual JetPack version (e.g., `6.2`, `5.1.2`), matching the `sources_*` directory name.

### Step 2: Build

```bash
./build_all.sh $VERSION
```

Flags:
- `--clean` — remove previous build artifacts before building
- `--dev-dbg` — enable `CONFIG_DYNAMIC_DEBUG` and `CONFIG_DYNAMIC_DEBUG_CORE` in kernel

Output directory: `images/$VERSION/` (normalized: `images/6.x/` for JP 6.x, `images/5.x/` for JP 5.x).

## Build Architecture Details

### What gets built per JetPack generation

**JP 6.x (Orin):** Out-of-tree module build. Builds kernel image, NVIDIA OOT modules (nvidia-oot, nvgpu, etc.), device tree overlays (`tegra234-camera-d4xx-overlay*.dtbo`). Sources in `sources_6.*/`. i.e for JP6.2 sources will be located at `sources_6.2/`

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

## Common Issues

- **Patches fail to apply**: Run `./apply_patches.sh $VERSION reset` first, then re-apply.
- **Missing git identity**: Set `git config user.name` and `git config user.email` before `apply_patches.sh`.
- **Workspace not set up**: Run `./setup_workspace.sh $VERSION` first (downloads NVIDIA sources + toolchain).
- **BUILD_NUMBER set**: If `BUILD_NUMBER` env var is set (common in CI), it changes the kernel vermagic string.
