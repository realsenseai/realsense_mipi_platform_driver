---
name: workspace-setup
description: Set up a fresh development workspace for the RealSense MIPI platform driver on a Linux VM. Use when the user has cloned the repo and needs to prepare for building — installing dependencies, downloading NVIDIA sources, applying patches, building, and configuring git. Triggers on requests mentioning setup workspace, prepare environment, first time setup, new workspace, get started, fresh clone, initialize workspace, or onboarding.
---

# Workspace Setup

Prepare a freshly cloned repo for development on a Linux VM (cross-compilation for Jetson).

## Workflow Overview

1. Determine JetPack version (guide user if needed)
2. Install build dependencies
3. Configure git identity
4. Run `setup_workspace.sh` (download NVIDIA sources + toolchain)
5. Run `apply_patches.sh` (apply D4XX driver patches)
6. Run `build_all.sh` (build kernel, DTBs, modules)
7. Verify build output

## Step 1: Determine JetPack Version

If the user hasn't specified a version, help them choose using this mapping:

| Jetson Platform | Supported JetPack Versions |
|----------------|---------------------------|
| AGX Xavier | 4.6.1, 5.0.2, 5.1.2 |
| AGX Orin | 6.0, 6.1, 6.2, 6.2.1 |

Ask which Jetson model they have, then recommend the latest version for that platform:
- **Xavier** → `5.1.2`
- **Orin** → `6.2.1`

Valid versions: `4.6.1`, `5.0.2`, `5.1.2`, `6.0`, `6.1`, `6.2`, `6.2.1`

## Step 2: Install Build Dependencies

```bash
sudo apt install -y build-essential bc wget flex bison curl libssl-dev xxd
```

Verify all installed successfully before proceeding.

## Step 3: Configure Git Identity

Required before `apply_patches.sh` (it commits patches to NVIDIA sub-repos).

```bash
git config user.name "User Name"
git config user.email "user@example.com"
```

Ask the user for their name and email. Set these in the repo root — `apply_patches.sh` propagates them to sub-repos automatically.

## Step 4: Download NVIDIA Sources and Toolchain

```bash
./setup_workspace.sh $VERSION
```

**Important:** This script is interactive — it displays the NVIDIA license and waits for a keypress. Warn the user they need to interact with the terminal.

What it does:
- Downloads the cross-compilation toolchain to `l4t-gcc/$VERSION/`
- Clones NVIDIA kernel sources to `sources_$VERSION/` (or extracts from `~/nvidia_sources_cache/` if a cached tarball exists)
- For JP 6.x: copies Makefile for out-of-tree module build

This step takes significant time (large git clones). If the user has a cached tarball at `~/nvidia_sources_cache/backup_sources_$VERSION.tar.gz`, it will be used instead (much faster).

## Step 5: Apply Patches

```bash
./apply_patches.sh $VERSION
```

For JP 5.0.2 only, there is an optional camera configuration flag:
- `--one-cam` (default) — single camera GMSL board
- `--dual-cam` — dual camera GMSL board

To reset patches (e.g., before re-applying):
```bash
./apply_patches.sh reset $VERSION
```

## Step 6: Build

```bash
./build_all.sh $VERSION
```

Build output goes to `images/$VERSION/`. This step takes significant time.

Optional flags:
- `--clean` — remove previous build artifacts first
- `--dev-dbg` — enable `CONFIG_DYNAMIC_DEBUG` in kernel

## Step 7: Verify Build

Check that build output was produced:

**JP 6.x (Orin):**
```bash
ls images/$VERSION/rootfs/boot/dtb/tegra234-*.dtb
ls images/$VERSION/rootfs/boot/tegra234-camera-d4xx-overlay*.dtbo
ls images/$VERSION/rootfs/lib/modules/*/extra/
```

**JP 4.x/5.x (Xavier):**
```bash
ls images/$VERSION/arch/arm64/boot/Image
ls images/$VERSION/modules/
```

If all outputs exist, the workspace is ready for development.

## Troubleshooting

- **Patches fail to apply**: Run `./apply_patches.sh reset $VERSION` then retry.
- **Missing git identity**: Ensure `git config user.name` and `git config user.email` are set (Step 3).
- **setup_workspace.sh fails on curl/wget**: Verify build dependencies are installed (Step 2).
- **Build fails with cross-compiler errors**: Ensure `setup_workspace.sh` completed successfully and `l4t-gcc/$VERSION/bin/` exists.
- **`BUILD_NUMBER` warning**: If this env var is set (common in CI), it changes the kernel vermagic string. Unset it for local builds: `unset BUILD_NUMBER`.
