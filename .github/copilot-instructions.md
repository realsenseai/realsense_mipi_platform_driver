# Copilot Instructions for realsense_mipi_platform_driver

## Project Overview
This repository provides the RealSense™ camera driver for GMSL/MIPI interfaces, targeting NVIDIA® Jetson platforms (AGX Xavier, AGX Orin) and supporting multiple JetPack versions (6.x, 5.x, 4.6.1). It enables control and streaming of RealSense™ 3D MIPI cameras via custom kernel drivers, device trees, and build scripts.

## Architecture & Key Components
- **hardware/**: Device tree overlays and platform-specific configuration files for NVIDIA and RealSense hardware.
- **kernel/**: Kernel patches and sources for different JetPack and kernel versions.
- **nvidia-oot/**: Out-of-tree NVIDIA kernel modules and related patches.
- **scripts/**: Build, patch, and utility scripts for workspace setup, compilation, and firmware management.
- **test/**: Test scripts and metadata validation tools.
- **utilities/**: Example applications, parsers, and stream utilities for RealSense cameras.

## Developer Workflows
### 1. Workspace Setup & Build
- Use `setup_workspace.sh [JetPack_version]` to prepare sources and toolchains.
- Apply kernel and driver patches with `apply_patches.sh [--one-cam|--dual-cam] apply [JetPack_version]`.
- For CI/external builds, use `apply_patches_ext.sh`.
- Build all components with `build_all.sh [--dev-dbg] [JetPack_version]`.
- Example:
  ```sh
  ./setup_workspace.sh 6.2
  ./apply_patches.sh 6.2
  ./build_all.sh 6.2
  ```
- For debug logging, add `--dev-dbg` to build scripts.

### 2. Testing & Validation
- Use `test/` scripts for firmware and metadata validation.
- After installation, verify driver and video devices:
  ```sh
  sudo dmesg | grep d4xx
  ls -l /dev/video*
  ```
- Use V4L2 utilities for device checks: `sudo apt install v4l-utils`

### 3. Device Tree & Kernel Integration
- Device tree overlays are in `hardware/realsense/` and `hardware/nvidia/platform/`.
- Kernel patches are versioned under `kernel/` and must match JetPack version.
- Out-of-tree modules are managed in `nvidia-oot/`.

## Project-Specific Conventions
- JetPack version is a required argument for most scripts and build steps.
- Patch scripts support single/dual camera configuration for JetPack 5.x (`--one-cam`, `--dual-cam`).
- Kernel versioning and file paths must be updated per JetPack release.
- Artifacts (modules, images, dtb, etc.) are archived under `images/[JetPack_version]/`.

## Integration Points & External Dependencies
- Relies on NVIDIA Jetson Linux BSP sources and toolchains (see README_JP6.md, README_JP5.md, README_JP4.md).
- Uses RealSense de-serialize hardware and Leopard Imaging adapter boards.
- Artifactory integration for CI/CD and artifact uploads (see Jenkins pipeline scripts).

## Example: CI Pipeline Highlights

## CI Pipeline: GitHub Actions & Jenkins

### GitHub Actions Workflows
- Located in `.github/workflows/`, these YAML files automate build and release for each JetPack version:
  - `build-jp6.yml`, `build-jp6.2.yml`, `build-jp6.1.yml`, `build-jp512.yml`, `build-jp502.yml`: Build/test for JetPack 6.x/5.x/4.x on push/PR.
  - `release-jp6.yml`: Publishes release artifacts for JetPack 6.x.
- Each workflow:
  - Checks out code
  - Runs setup (`setup_workspace.sh [version]`), applies patches, builds (`build_all.sh [version]`)
  - Archives build outputs (modules, dtb, rootfs, etc.)
  - Uploads artifacts (via `actions/upload-artifact` or `softprops/action-gh-release`)
- Example build steps:
  ```yaml
  - name: setup workspace
    run: yes | ./setup_workspace.sh 6.2
  - name: apply patches
    run: ./apply_patches.sh 6.2
  - name: build
    run: ./build_all.sh 6.2
  ```
- Artifacts are found in `images/[JetPack_version]/` and uploaded for CI/release consumption.

### Jenkins Pipeline
- `LRS_Jetson_mipi_usb_driver_jp6.groovy` (legacy/optional): Automates checkout, patching, build, artifact archiving, and upload to Artifactory.
- Environment variables and parameters control build options, JetPack version, and artifact destinations.
- Artifacts are uploaded to Artifactory and notification emails are sent on build completion/failure.

## References
- See `README.md` for overall project intro and hardware requirements.
- See `README_JP6.md`, `README_JP5.md`, `README_JP4.md` for JetPack-specific build instructions.
- See `README_tools.md` for build script usage and options.
- See `README_driver.md` for driver API and validation steps.

---
**Feedback Requested:**
If any section is unclear or missing key details, please specify which workflows, conventions, or integration points need further documentation for AI agents.
