---
name: mipi-bandwidth-check
description: Measure the real MIPI/CSI link bandwidth of a multi-stream RS400 config on a Jetson rig using exact per-channel SOF/EOF timing (ftrace camera_common:tegra_channel_capture_frame), then compute the peak aggregate rate and coexistence headroom. Confirms how concurrent streams interleave (e.g. D401 dual-RGB: RAW = full sensor scan, depth = nested REC-crop window) instead of guessing overlap from byte counts. Use when asked to check MIPI/CSI/GMSL bandwidth, verify stream coexistence headroom, measure SOF/EOF or readout (RO) timing, confirm depth-vs-RAW interleaving, or decide whether a shorter HTS/RO still fits the link.
---

# Skill: MIPI/CSI bandwidth check via per-channel SOF/EOF

> **Command Execution Standards** (CLAUDE.md): every remote command carries a `[HH:MM:SS]`
> timestamp + machine name + timeout as Claude's **text** output. SSH/scp from Windows must use
> `-o GSSAPIAuthentication=no -o BatchMode=yes -n -o StrictHostKeyChecking=accept-new`.
> Confirm the rig (host/user/OS) at the start. This skill is **read-only** (stream + ftrace) — no flashing.

## Why this exists

Bandwidth analysis from byte counts alone forces a guess about **overlap**: do concurrent streams
sum (peak = Σ rates) or time-multiplex (peak = max)? That guess swings the answer by ~2×. The only
way to settle it is to measure when each stream is actually on the wire. The Tegra VI logs a
**hardware SOF and EOF timestamp per captured frame** per channel — exact to ~ns. From those you get
each stream's readout span, how the streams nest in time, and the true peak aggregate rate.

## The model it confirmed (D401 dual-RGB, reference data)

Inputs are the same stereo sensors; **outputs differ**. RAW (EP3/EP4) is the full 808-line sensor
scan; depth (EP2) is only the rectification/crop window. Measured on fw-orin-3, FW 5.17.3.153,
depth 1280×720 + 2× RAW 1288×808 pBAA @30 fps (1612 B/line on the wire) (period 33.31 ms):

```
frame:  |<----------------- RAW scan 24.0 ms ----------------->|        (V-blank 9.3ms)
        0                                                      24.0
            |<------------- depth 21.1 ms ------------->|
            2.11ms                                    23.3ms
        ^^^ RAW-only head 2.1ms           RAW-only tail 0.73ms ^^
        |---------------- overlap (all 3) = 196 MB/s ----------|
```

| stream | span | offset vs RAW | rate |
|---|---|---|---|
| RAW ×2 | 24.0 ms | sync'd to <10 µs of each other | 54.2 MB/s each → 108.5 pair |
| depth | 21.1 ms | SOF +2.11 ms, EOF −0.73 ms (nested) | 87.4 MB/s |

**Peak = depth + 2×RAW over the overlap = 196 MB/s** (measured, clean: 3 discards). Line time =
24.0 ms / 808 ≈ 29.7 µs = sensor HTS/PCLK.

### Bandwidth formulas (validated by the above)
```
line_time   = raw_span / raw_active_lines            (= HTS / PCLK)
raw_rate    = raw_bytes / raw_span                   per RAW stream;  ∝ 1/sensor-HTS
depth_rate  = depth_bytes / depth_span               (Z16). MEASURED: depth_span ≈ const ~20ms
              (REC-window readout, only weakly res-dependent), so depth_rate ∝ output PIXEL COUNT (W×H).
PEAK        = Σ(streams overlapping in time) of their rates       (sum ONLY over the overlap window)

Because every span scales with line_time (= HTS/PCLK), the whole overlap peak scales linearly with
1/HTS:  PEAK(HTS) = PEAK@0x930 × (0x930 / HTS).  So the min sensor HTS that still fits a ceiling C is
HTS_min = 0x930 × (measured PEAK@0x930) / C. (Validate by re-measuring at the shorter HTS — the VI
ceiling is only bounded to ~196–215 MB/s, not pinned.)
```
Two-term split for D401: `PEAK = 2·raw_rate + depth_rate`. The **RAW pair is dominant** and set by
the **sensor HTS** (the readout span); the depth term is minor and pixel-count-driven (∝ W×H;
depth_span ≈ const ~20ms). The coexistence ceiling is the **Tegra VI capture-buffer drain rate** —
it must sustain the overlap peak. Reference: 196 MB/s overlap = clean; small-res v151 failures were
the RAW pair crammed into a 12.9 ms span (~201 MB/s pair alone), i.e. a short sensor-HTS, not the
depth resolution.

### Measured resolution sweep (fw-orin-3, v153, sensor HTS 0x930, 3-stream, all CLEAN)
| depth res | depth span | depth rate | RAW span ×2 | overlap PEAK | HTS_min @ceiling 196 |
|---|---|---|---|---|---|
| 1280×720 | 21.10 ms | 87.4 | 24.02 → 108.5 | **196 MB/s** | 0x930 (at limit) |
| 848×480  | 20.59 ms | 39.5 | 24.03 → 108.4 | **148 MB/s** | ~0x6F0 (~18 ms) |
| 640×360  | 20.22 ms | 22.8 | 24.03 → 108.4 | **131 MB/s** | ~0x627 (~16 ms) |
| 480×270  | 19.82 ms | 13.1 | 24.03 → 108.4 | **122 MB/s** | ~0x5B0 (~15 ms) |
RAW span constant (full sensor scan); peak dominated by RAW pair. Smaller depth res → lower peak →
more headroom → tolerates a shorter sensor HTS (reclaims fps). HTS_min = 0x930 × PEAK@0x930 / ceiling.

## When to use
- "Check the MIPI/CSI/GMSL bandwidth / headroom for this multi-stream config."
- "Does depth+RAW (or any N-stream combo) actually fit — and with how much margin?"
- "Confirm how the streams interleave / measure SOF/EOF / readout (RO) timing."
- "Would a shorter HTS/RO still fit?" → measure the overlap peak vs the VI ceiling.

## Method

### 1. Confirm rig + no conflicting session
Resolve host/key from `../ssh-setup/config.ini` (`[host:<RIG>]` → `KEY_PATH`, `TARGET_USER`).
Probe `:22`. Check `pgrep -a v4l2-ctl; pgrep -a rs-fw-update` is empty.

### 2. Capture (rig) — `capture_sofeof.sh`
Stages staggered so each VI channel becomes a **distinct PID** (one stream alone first → +1 → +1
all together). Strip CR after scp (avoid `$` in the remote string — PowerShell pre-expands it):
```powershell
$key='<KEY_PATH>'   # e.g. ~/.ssh/id_ed25519-<user>
scp -i $key -o GSSAPIAuthentication=no -o BatchMode=yes -o StrictHostKeyChecking=accept-new `
    "<skill-dir>\capture_sofeof.sh" <user>@<RIG>:/tmp/capture_sofeof.sh
ssh -n -i $key -o GSSAPIAuthentication=no -o BatchMode=yes -o StrictHostKeyChecking=accept-new `
    <user>@<RIG> "tr -d '\r' < /tmp/capture_sofeof.sh > /tmp/cap.sh; bash /tmp/cap.sh"
```
Override defaults via env: `DEPTH_DEV` (/dev/video0), `RAW_DEVS` ("/dev/video2 /dev/video4"),
`DEPTH_W`/`DEPTH_H` (1280/720), `RAW_W`/`RAW_H`/`RAW_FMT` (1288/808/pBAA — pass `1612x808`, the wire bytes per line, to `parse_sofeof.py`), `FPS` (30).
The summary must show **distinct PIDs** (one per channel) and low `short_frame_discards`.

### 3. Parse (host) — `parse_sofeof.py`
```powershell
scp ... <user>@<RIG>:/tmp/sofeof_phased.txt "<scratch>\sofeof_phased.txt"
python "<skill-dir>\parse_sofeof.py" "<scratch>\sofeof_phased.txt"
```
Prints per-channel span/period/firstSOF, classifies depth (earliest SOF) vs RAW, and the
cross-channel dSOF/dEOF vs the last-started anchor. **`+dSOF` = starts later; `−dEOF` = ends
earlier** ⇒ that stream is nested. Compute PEAK = Σ rates over the overlap (= the nested span).

> ⚠️ Parsing gotcha (baked into the parser): the `sof:`/`eof:` payload is `<secs>.<nanoseconds>`
> with **ns NOT zero-padded** — `1939.5533632` = 1939 s + 5,533,632 ns = 1939.005533632 s, *not*
> 1939.55 s. Parse secs and ns as separate integers (`secs + ns/1e9`); a naïve `float()` gives a
> ~25× wrong span.

### 4. Interpret
- depth span < RAW span and depth nested ⇒ overlap = depth span; peak = sum over it.
- Peak ≤ the VI sustainable rate (≈196 MB/s proven clean on this rig; fail onset ~200+ when the
  RAW pair span is crammed) ⇒ coexists. Above it ⇒ short frames / starvation.
- Two HTS-invariant floors still bite "marginal hosts": frame-average (payload×fps) and the
  instantaneous line-peak (= GMSL/CSI line rate, set by PCLK, unaffected by HTS).

## Notes / portability
- Tracepoint: `camera_common:tegra_channel_capture_frame` (NOT `tegra_capture`). tracefs at
  `/sys/kernel/debug/tracing` or `/sys/kernel/tracing`; needs passwordless `sudo`.
- Channel→stream mapping is by PID + SOF→EOF span (RAW = longest/full-scan; depth = shorter). The
  two RAW channels are indistinguishable from each other (identical) — fine, you only need depth-vs-RAW.
- All paths/hosts here are placeholders — resolve at runtime from `ssh-setup/config.ini`; never hardcode.
- To sweep resolutions, re-run with different `DEPTH_W`/`DEPTH_H` and compare overlap peaks.
