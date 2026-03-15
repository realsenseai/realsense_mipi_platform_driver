# HW Reset Follow-up Plan: Circuit-Breaker & Phase 1 Dual-Camera Safety

**Date:** 2026-03-02
**Branch:** `fix/hw-reset-gmsl-recovery`
**Commits:** ecbfa82 → 97b91b9 → 0e205e5
**JIRA:** RSDSO-21151, RSDSO-21257, RSDSO-21254

## Summary

Two follow-up items from the log analysis. **Item 1** (circuit-breaker) adds rate-limiting and consecutive-failure tracking to `ds5_hw_reset_with_recovery()` to break the 35-reset infinite loop seen in `reset_issue_dmesg.txt`. **Item 2** (Phase 1 dual-camera impact) is largely resolved by analysis — `max9295_init_settings()` only writes serializer-local registers and cannot disrupt siblings — but a lightweight safety check should be added.

---

## Item 1: Reset Circuit-Breaker (High Priority)

The `reset_issue_dmesg.txt` log shows 35 consecutive userspace-triggered HW resets (~30s apart) via `HWMC_RW` opcode 0x20, each triggering full SERDES recovery. After ~3 resets, I2C errors (err -121) appear and the device never recovers — yet the driver keeps accepting reset commands indefinitely.

### Steps

1. **Add tracking fields to `struct ds5_dev`** (`kernel/realsense/d4xx.c`):
   - `int consecutive_reset_failures` — incremented when `ds5_hw_reset_with_recovery()` returns an error, reset to 0 on success
   - `unsigned long last_reset_jiffies` — timestamp of the last reset attempt
   - `int total_resets` — lifetime counter for diagnostics (never reset)

2. **Add circuit-breaker constants** near the existing `DS5_HW_RESET_*` defines:
   - `DS5_HW_RESET_MAX_CONSECUTIVE_FAILURES` = 3 — after 3 consecutive failed resets, refuse further resets
   - `DS5_HW_RESET_COOLDOWN_MS` = 5000 — minimum interval between resets (5 seconds)
   - `DS5_HW_RESET_BREAKER_RESET_MS` = 60000 — if no reset for 60s, clear the failure counter (auto-recovery)

### Implementation Notes (updated for refactored driver)

- Use tracking fields in `struct ds5_dev`: `consecutive_reset_failures`, `last_reset_jiffies`, and `total_resets` (all per-camera, shared across sensor instances).
- Reference `ds5_inited[]` for sibling streaming checks (not `serdes_inited[]`).
- Circuit-breaker logic:
   - Check `ds5_dev->consecutive_reset_failures >= DS5_HW_RESET_MAX_CONSECUTIVE_FAILURES`.
      - If `DS5_HW_RESET_BREAKER_RESET_MS` has elapsed since `ds5_dev->last_reset_jiffies`, auto-clear the failure counter and allow the reset (self-healing).
      - Otherwise, log `dev_err` "circuit breaker tripped — %d consecutive failures, refusing HW reset. Will auto-reset after %d seconds of inactivity" and return `-EBUSY`.
   - Cooldown: if `jiffies - ds5_dev->last_reset_jiffies < msecs_to_jiffies(DS5_HW_RESET_COOLDOWN_MS)`, log `dev_warn` "HW reset throttled — last reset was %d ms ago" and return `-EAGAIN`.
- Update success/failure tracking at the end of `ds5_hw_reset_with_recovery()`:
   - On success: set `ds5_dev->consecutive_reset_failures = 0`, update `ds5_dev->last_reset_jiffies = jiffies`, increment `ds5_dev->total_resets`.
   - On failure: increment `ds5_dev->consecutive_reset_failures`, update `ds5_dev->last_reset_jiffies = jiffies`, increment `ds5_dev->total_resets`.
- Expose diagnostic counters via the final `dev_info` log in `ds5_hw_reset_with_recovery()` so they appear in dmesg.
- Skip circuit-breaker for probe path: ensure probe-time HW reset bypasses the breaker logic (e.g., by checking `last_reset_jiffies == 0` or using a `force` parameter).
- For dual-camera safety, after Phase 1 serializer re-init, iterate `ds5_inited[]` for sibling instances (same `ds5_dev`, different sensor). For each streaming sibling, perform a quick I2C read (`ds5_read(sib, DS5_FW_VERSION, &tmp)`). If any fail, log `dev_warn` "Phase 1 serializer re-init may have disrupted sibling %s" (diagnostic only, do not fail the reset).

8. **Add a brief stabilization delay** — increase the post-Phase-1 `msleep(100)` to `msleep(150)` to give the GMSL link a bit more time to re-lock after serializer reconfiguration, before verifying. This is conservative and costs only 50ms.

---

## Verification Plan

- **Circuit-breaker**: Trigger 5+ rapid HW resets on a camera with a broken GMSL link. Verify that after 3 failures, the driver logs "circuit breaker tripped" and returns `-EBUSY`. Wait 60s, verify reset is accepted again.
- **Cooldown**: Send two HW resets <5s apart. Verify the second one returns `-EAGAIN` with a throttle warning.
- **Probe bypass**: Verify `modprobe d4xx` still performs initial HW reset successfully without tripping the circuit-breaker.
- **Phase 1 safety**: On a dual-camera Orin setup, HW-reset camera A while camera B is streaming. Check dmesg for "may have disrupted sibling" warning. Confirm camera B's stream is unaffected.
- **Regression**: Run `python3 run_ci.py` from `test/` on a D457 to verify no existing tests break.

## Design Decisions

- **Circuit-breaker is per-camera, shared across instances**: The tracking fields live in `struct ds5_dev`, so all `struct ds5` instances for the same physical camera share a single counter and breaker state. If any instance (typically Depth at `9-001a`) trips the breaker, further HW reset requests from any instance for that camera are rejected until the breaker auto‑clears.
- **Return -EBUSY for tripped breaker, -EAGAIN for cooldown**: Distinct error codes let userspace distinguish "permanently failed" from "try again later".
- **Auto-clear after 60s inactivity**: Avoids permanent lockout. If the camera recovers externally (e.g., power cycle), the driver will accept resets again.
- **Phase 1 dual-camera is diagnostic only**: No blocking behavior added — just logging. If field data shows disruption, we can add a sibling-streaming gate in a future commit.
