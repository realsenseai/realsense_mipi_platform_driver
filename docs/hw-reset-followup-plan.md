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

1. **Add tracking fields to `struct ds5`** (`kernel/realsense/d4xx.c`):
   - `int consecutive_reset_failures` — incremented when `ds5_hw_reset_with_recovery()` returns an error, reset to 0 on success
   - `unsigned long last_reset_jiffies` — timestamp of the last reset attempt
   - `int total_resets` — lifetime counter for diagnostics (never reset)

2. **Add circuit-breaker constants** near the existing `DS5_HW_RESET_*` defines:
   - `DS5_HW_RESET_MAX_CONSECUTIVE_FAILURES` = 3 — after 3 consecutive failed resets, refuse further resets
   - `DS5_HW_RESET_COOLDOWN_MS` = 5000 — minimum interval between resets (5 seconds)
   - `DS5_HW_RESET_BREAKER_RESET_MS` = 60000 — if no reset for 60s, clear the failure counter (auto-recovery)

3. **Add gating logic at top of `ds5_hw_reset_with_recovery()`**:
   - Check `consecutive_reset_failures >= MAX_CONSECUTIVE_FAILURES`:
     - If the breaker timeout (`DS5_HW_RESET_BREAKER_RESET_MS`) has elapsed since `last_reset_jiffies`, auto-clear the counter and allow the reset (self-healing)
     - Otherwise, log `dev_err` "circuit breaker tripped — %d consecutive failures, refusing HW reset. Will auto-reset after %d seconds of inactivity" and return `-EBUSY`
   - Check cooldown: if `jiffies - last_reset_jiffies < msecs_to_jiffies(COOLDOWN_MS)`, log `dev_warn` "HW reset throttled — last reset was %d ms ago" and return `-EAGAIN`

4. **Update success/failure tracking at bottom of function**:
   - On success (return 0): set `state->consecutive_reset_failures = 0`, update `state->last_reset_jiffies = jiffies`, increment `state->total_resets`
   - On failure: increment `state->consecutive_reset_failures`, update `state->last_reset_jiffies = jiffies`, increment `state->total_resets`

5. **Expose diagnostic counters via existing V4L2 log** — add the counters to the final `dev_info` message at the end of `ds5_hw_reset_with_recovery()` so they appear in dmesg.

6. **Skip circuit-breaker for probe path** — the `ds5_probe()` call should bypass the circuit-breaker since it's a one-time initialization, not a userspace retry loop. Add a `bool force` parameter to `ds5_hw_reset_with_recovery()`, or handle it by checking `last_reset_jiffies == 0` (uninitialized).

---

## Item 2: Phase 1 Dual-Camera Safety (Low Priority)

### Analysis Conclusion

The D401 dual-camera log errors (bus 12 I2C failures 12ms after SERDES re-init) occurred with the **old driver code** that did full SERDES re-init including deserializer `reset_oneshot`. Our new Phase 1 calls only `max9295_init_settings()`, which writes exclusively to serializer-local registers (`PIPE_EN`=0x2, `START_PIPE`=0x311, `MIPI_RX1`=0x331, `CSI_PORT_SEL`). This **cannot** disrupt sibling cameras on a different serializer.

### Remaining Risk

Theoretical — if the GMSL link is unstable, a serializer register write could briefly glitch the shared GMSL bus during the I2C transaction. This is extremely unlikely with MAX9295/MAX9296 but worth monitoring.

### Steps

7. **Add post-Phase-1 sibling health check** in `ds5_hw_reset_serdes_recovery()` — after the Phase 1 success path (`return 0`), before returning:
   - Iterate `serdes_inited[]` for true siblings (same `dser_dev`, different `ser_dev`)
   - For each streaming sibling, do a quick I2C read (`ds5_read(sib, DS5_FW_VERSION, &tmp)`)
   - If any fail: log `dev_warn` "Phase 1 serializer re-init may have disrupted sibling %s" (informational only — do NOT fail the reset)
   - This is a **diagnostic-only** check that gives us data to decide if future mitigation is needed

8. **Add a brief stabilization delay** — increase the post-Phase-1 `msleep(100)` to `msleep(150)` to give the GMSL link a bit more time to re-lock after serializer reconfiguration, before verifying. This is conservative and costs only 50ms.

---

## Verification Plan

- **Circuit-breaker**: Trigger 5+ rapid HW resets on a camera with a broken GMSL link. Verify that after 3 failures, the driver logs "circuit breaker tripped" and returns `-EBUSY`. Wait 60s, verify reset is accepted again.
- **Cooldown**: Send two HW resets <5s apart. Verify the second one returns `-EAGAIN` with a throttle warning.
- **Probe bypass**: Verify `modprobe d4xx` still performs initial HW reset successfully without tripping the circuit-breaker.
- **Phase 1 safety**: On a dual-camera Orin setup, HW-reset camera A while camera B is streaming. Check dmesg for "may have disrupted sibling" warning. Confirm camera B's stream is unaffected.
- **Regression**: Run `python3 run_ci.py` from `test/` on a D457 to verify no existing tests break.

## Design Decisions

- **Circuit-breaker is per-instance, not per-camera**: Each `struct ds5` has its own counter. Since userspace sends HW reset to one instance (typically Depth at `9-001a`), this naturally tracks per-camera. Peer instances aren't reset independently.
- **Return -EBUSY for tripped breaker, -EAGAIN for cooldown**: Distinct error codes let userspace distinguish "permanently failed" from "try again later".
- **Auto-clear after 60s inactivity**: Avoids permanent lockout. If the camera recovers externally (e.g., power cycle), the driver will accept resets again.
- **Phase 1 dual-camera is diagnostic only**: No blocking behavior added — just logging. If field data shows disruption, we can add a sibling-streaming gate in a future commit.
