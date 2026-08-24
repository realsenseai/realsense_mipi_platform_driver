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

Always reset patches before re-applying. **`reset` must come before `$VERSION`, not after** —
`apply_patches.sh`'s own argument loop stops parsing at the first token it doesn't recognize as
a flag, and `$VERSION` (e.g. `6.2`) isn't one, so `./apply_patches.sh $VERSION reset` never
reaches `reset` at all and silently runs a plain `apply` instead. Also pipe `yes` so the
"changes will be hard reset, continue?" prompt doesn't stall non-interactively:
```bash
yes | ./apply_patches.sh reset $VERSION
yes | ./apply_patches.sh $VERSION
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

**This build takes several minutes (kernel Image + OOT modules + DTBs) — run it with your
tool's own backgrounding, plain, with no output redirection.** Do not wrap it in your own
`nohup ... &`: if your tool call already backgrounds the command for you, adding your own `&`
on top makes the wrapper return the moment the child is launched, so the tool thinks the
command finished instantly while the real build keeps running as an untracked orphan — no
completion signal will ever fire for it.

### Step 3 (optional): Overlay-only rebuild

When only a device-tree overlay changed (new/edited `tegra234-camera-d4xx-overlay*.dts`) and the kernel/modules are already built, skip the full `build_all.sh` and rebuild just the DTBs — far faster, and it does NOT recompile `d4xx.c` (safe when the build tree sits on an unrelated WIP branch).

Prerequisite: the workspace was built at least once (`out` symlink + kernel image present in `sources_$VERSION/`).

1. If the overlay is **new**, link it into the tree and add it to the enumeration (mirrors what `apply_patches.sh` does):
   ```bash
   ov=sources_$VERSION/hardware/nvidia/t23x/nv-public/overlay
   ln -f hardware/realsense/<new-overlay>.dts "$ov/"
   grep -q "<new-overlay>.dtbo" "$ov/Makefile" || \
     sed -i "/<sibling-overlay>.dtbo/i dtbo-y += <new-overlay>.dtbo" "$ov/Makefile"
   ```
   Also add the same `dtbo-y +=` line to `hardware/nvidia/t23x/nv-public/6.x/0001-overlay-*.patch` so a clean `apply_patches.sh` keeps building it (bump the hunk `@@` count + diffstat).

2. Build only the DTBs (same env `build_all.sh` uses):
   ```bash
   . scripts/setup-common $VERSION
   export DEVDIR=$(pwd) TEGRA_KERNEL_OUT="$DEVDIR/images/$VERSION" ARCH=arm64
   export CROSS_COMPILE=$DEVDIR/l4t-gcc/6.x/bin/aarch64-buildroot-linux-gnu-
   export KERNEL_HEADERS="$DEVDIR/sources_$VERSION/kernel/kernel-jammy-src"
   cd sources_$VERSION && make dtbs
   ```
   Output: `sources_$VERSION/nvidia-oot/device-tree/platform/generic-dts/dtbs/<overlay>.dtbo`

3. Validate before deploy:
   ```bash
   dtc -I dtb -O dts <overlay>.dtbo | grep -E "overlay-name|num-channels|max9295"
   ```

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
