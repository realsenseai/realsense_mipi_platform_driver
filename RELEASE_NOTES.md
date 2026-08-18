# Release Notes

## R58.5 (v1.0.6.3) - Pre-release

### Changes Since R58.4

#### SerDes Enhancements
- **MAX96717**: Code review fixes and improvements
- **MAX96712**: Updated 4-lane rate to 1300Mbps (from 1100Mbps) and HKR lane rate to 1200Mbps (from 1000Mbps)

#### D585 Platform Support
- Added external sync support on Fangzhu-12-16CH platform

#### D5xx Camera Improvements
- Set RGB gain range to 16-248 for improved color control
- Updated compatible string for MAX96712 overlays to use D5xx designation

#### Driver Stability
- **[Jetson][VI5]**: Fixed recovery teardown resource lifetime issue
  - Hold explicit references to VI5 capture kthreads until teardown completion
  - Prevents kthread_stop() from dereferencing released task_struct during recovery
  - Corrected failed capture-setup cleanup to properly free per-port coherent request buffers
  - Fixes invalid and repeated frees when recovery failure races with STREAMOFF
  - Applied across all supported JetPack kernel source families

### Commits
- f72a75d: Update max96717.c
- 94f3e88: RSDEV-13332 D585 External sync on Fangzhu-12-16CH
- 7ec01e3: max96712 update 4 lane rate to 1300Mbps
- de396fa: [D5xx] Set RGB gain range to 16-248
- 453a6af: Use D5xx compatible string for MAX96712 overlays
- e73fcb2: [Jetson][VI5] fix recovery teardown resource lifetime

---

## R58.4 (v1.0.5.20) - Latest Release
Released: 2026-07-30

## R58.3 (v1.0.4.9)
Released: 2026-06-28

## R58.2 (v1.0.3.18)
Released: 2026-06-04

## R58.1 (v1.0.2.34)
Released: 2026-05-07
