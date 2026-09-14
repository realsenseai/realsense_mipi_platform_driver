---
name: soak-watch
description: Watch a long-running hardware soak / repro loop on a rig WITHOUT polling it turn-by-turn. Stages a cycle loop that emits heartbeat + terminal markers, then runs a single background watcher that escalates the moment something actionable happens — done, failed, device-gone, OR stuck (stalled progress / dead process / unreachable rig / hard deadline). The watcher wakes the session only on a terminal event, so a multi-hour soak costs ~zero conversation turns and a HUNG soak still surfaces instead of sitting blind. Use when a repro check or validation soak runs for many minutes to hours (e.g. a 5000-cycle preset loop, an endurance run, a bisect leg), or when the user asks to "watch the soak", "run this for N cycles", "let it run overnight", or "tell me when it's done or stuck".
---

# Skill: soak-watch (escalate-on-event, don't poll-per-turn)

## Why this exists

Babysitting a multi-hour rig soak turn-by-turn is the single most expensive thing a long
session does: every "is it done yet?" turn re-reads the full context (cache-read tokens), so
a few hundred status pokes over a night can cost more than the actual fix work. But you can't
just fire-and-forget either — if the loop **hangs** (process deadlocks, log stops advancing,
SSH dies) a fire-and-forget notifier that only watches for "complete" sits blind until its
timeout.

This skill resolves the tension by moving **liveness detection into a single background
watcher**. The watcher polls the rig cheaply (SSH, no LLM turns) and exits — waking the
session exactly once — the moment something actionable happens. "Stuck" is a first-class wake
condition, so you find out about a wedge as fast as you'd find out about success, without
paying for a full-context turn every minute.

> Rule of thumb: an SSH probe is nearly free; a **main-session turn** is what costs cache-read
> tokens. So probe often, escalate rarely — only on a real event.

## The two pieces

| Script | Runs on | Role |
|---|---|---|
| `soak_loop.sh` | **the rig** (under `nohup`/`setsid`) | runs the per-cycle command N times; emits `PROGRESS iter=N` heartbeats and a terminal `SOAK_COMPLETE` / `SOAK_FAILED`; writes its PID to `$LOG.pid`. |
| `soak_probe.sh` | **the rig** (one shot per poll) | one cheap snapshot: tail of the log + loop-PID alive? + USB-device count. All the fragile SSH quoting lives here. |
| `soak_watch.sh` | **the dev box** (background) | polls via `soak_probe.sh`, tracks progress, and exits with ONE verdict line on the first terminal/stuck/timeout condition. |

The loop and watcher are decoupled by a **marker contract** — the watcher only knows the soak
through the literal tokens the loop writes. Any loop that emits those tokens can be watched;
`soak_loop.sh` is just the convenient default.

## Marker contract (what the watcher greps for)

```
SOAK_START   tag=.. cycles=.. every=.. pid=..   # once, at start
PROGRESS     iter=N ts=..                        # heartbeat, every $SOAK_PROGRESS_EVERY cycles
SOAK_FAILED  iter=N rc=.. ts=..                  # a cycle failed  -> loop exits 1
SOAK_COMPLETE iter=N ts=..                       # all cycles passed -> loop exits 0
```

Plus, out of band: the loop's PID in `$LOG.pid` (liveness), and `lsusb -d 8086:` on the rig
(device-gone). If you wire a different loop, just make it emit `PROGRESS iter=N` periodically
and one terminal marker, and write its PID to `$LOG.pid`.

## Verdicts (the wake-up signal)

The watcher prints `SOAK_WATCH_VERDICT=<X>` and exits. Exactly one fires:

| Verdict | Exit | Means | What you do on wake |
|---|---|---|---|
| `DONE` | 0 | `SOAK_COMPLETE` seen | soak passed all cycles — report the count, move on |
| `FAILED` | 1 | `SOAK_FAILED` seen | a cycle hit the symptom — pull the log around that iter |
| `DEVICE_GONE` | 1 | no `8086:` device on USB | camera dropped off the bus (HC death / wedge) — for most repros this **is** the reproduction; recover the rig (e.g. PCI rebind / hub power-cycle) |
| `STUCK` | 3 | no progress for `STALL_POLLS` polls, PID alive | loop hung mid-soak — investigate the live process (don't just wait) |
| `CRASHED` | 2 | loop PID gone, no terminal marker | loop died silently — check for an exception/OOM near the log tail |
| `UNREACHABLE` | 4 | SSH failed `SSH_FAILS_MAX`× | rig/network down — confirm the rig is up before assuming the soak failed |
| `TIMEOUT` | 5 | deadline reached | soak outran the cap — decide extend vs abort |

`DONE` / `FAILED` / `DEVICE_GONE` are the soak's own outcome. `STUCK` / `CRASHED` /
`UNREACHABLE` / `TIMEOUT` are the **liveness escalations** — the answer to "how will I know
it's not stuck?". Any of them wakes you exactly once.

## How to run it

### 1. Stage the scripts on the rig (once per rig)
Copy `soak_loop.sh` + `soak_probe.sh` into `REMOTE_DIR` (default `~/soak`). On a rig where
`scp` is flaky (e.g. rslnxrvp2's newer sftp), pipe base64 over ssh stdin — see `hub-control`'s
"STAGE_VIA = base64" note.

### 2. Launch the loop detached on the rig
The per-cycle command is **one customer-faithful iteration** that exits non-zero only on the
symptom. Launch it so it survives the SSH session:
```bash
ssh <rig> "SOAK_CYCLES=5000 SOAK_PROGRESS_EVERY=100 SOAK_LOG=/tmp/<KEY>.log \
  setsid nohup bash ~/soak/soak_loop.sh python3 /tmp/one_cycle.py >/dev/null 2>&1 &"
```
Verify it's singular before watching (a second loop appended to the same log corrupts the
iter sequence): `ssh <rig> "pgrep -af soak_loop.sh"` should show exactly one.

### 3. Start the watcher in the BACKGROUND from the dev box
Invoke `soak_watch.sh` via the **Bash tool with `run_in_background: true`**. This is the whole
point — the harness re-invokes the session only when the watcher exits, so you spend no turns
waiting:
```bash
RIG=<user@host> LOG=/tmp/<KEY>.log SSH_KEY=~/.ssh/<key> \
  PROBE=~/soak/soak_probe.sh POLL=60 \
  bash .claude/skills/soak-watch/soak_watch.sh
```
Then **stop narrating and do other work** (or end the turn). When the watcher exits you'll be
notified with the verdict line; act on it per the table above. Do **not** also poll the rig
yourself in the meantime — that re-introduces exactly the cost this skill removes.

> Picking `POLL`: keep it < 300 s so a foreground watch stays in the prompt cache, but since
> the watcher runs in the background the value mostly trades SSH chatter against detection
> latency. 60 s is fine for a ~3 s/cycle loop (a stall shows within ~5 min at `STALL_POLLS=5`).
> Match `STALL_POLLS` to the cadence: a stall threshold should be many times the normal
> per-cycle time so a slow-but-progressing loop never trips it.

## Tuning the stall watchdog

`STUCK` fires when `iter=` hasn't advanced for `STALL_POLLS` consecutive polls. Set it from the
known cadence, not a round number:
- **Fast loop** (cycles in seconds): the iter should advance most polls; `STALL_POLLS=5` at
  `POLL=60` = ~5 min of no progress = unambiguously hung.
- **Slow / coarse loop** (a cycle takes minutes, or `PROGRESS_EVERY` is large): raise
  `STALL_POLLS` so one slow legitimate cycle can't trip it, or rely on `CRASHED`
  (dead-PID) + `DEADLINE_POLLS` instead and disable the stall check by setting `STALL_POLLS`
  very high.
- **No incremental progress at all** (e.g. a single long build, not a cycle loop): there's no
  `iter` to watch — lean on `CRASHED` (PID gone) + `DEADLINE_POLLS`. The stall check is for
  loops that *should* be ticking.

## Failure modes & gotchas

| Symptom | Cause | Handling |
|---|---|---|
| Watcher fires `STUCK` but the soak is fine | `STALL_POLLS` too tight for the cadence, or `PROGRESS_EVERY` so large that heartbeats are rarer than the stall window | raise `STALL_POLLS` or lower `PROGRESS_EVERY` so a heartbeat lands within the window. |
| `CRASHED` immediately | pidfile not written yet (watcher started before the loop) or wrong `LOG`/`PIDFILE` | start the loop first, confirm `$LOG.pid` exists, then start the watcher. |
| Two loops, garbled iter sequence | a previous detached loop was left running (orphaned `nohup`) | `pgrep -af soak_loop.sh`, kill stale ones **by PID** (not `pkill -f` — it self-matches the SSH command, exit 255; use the `grep "loop[.]sh"` `[.]` trick if you must pattern-match). |
| `UNREACHABLE` but rig is up | shared-rig xHCI death took the controller (and SSH-over-nothing is fine but the camera bus is gone) vs. a real network blip | check the rig directly; `DEVICE_GONE` is the camera-bus signal, `UNREACHABLE` is the host/SSH signal — they're different. |
| Watcher never exits | `DEADLINE_POLLS` set huge and nothing terminal happened | that's the backstop's job; lower the deadline, or the loop isn't emitting markers (check the contract). |
| `scp` hangs staging to the rig | newer sftp-based scp vs the rig's sshd | stage via base64-over-ssh-stdin (see `hub-control`). |

## Integration with the fix workflow

The long soaks in this repo run inside:
- **`ds5-validate`** (Phase 5 of `fix-and-validate`) — the A/B repro check, especially with
  `--repeats <n>` or a high-cycle `--repro-test`.
- **`ds5-bug-reproduction`** (Phase 4) — the customer-faithful repro loop.
- **`ds5-bug-reproduction`** (Phase 6) — bisect legs, each a full soak.

When a repro check / validation leg is a **long loop (more than a few minutes)**, those skills
should express it as a `soak_loop.sh` run + a backgrounded `soak_watch.sh`, rather than running
the loop in the foreground and polling it. A short, single-shot `run-fw-tests` nodeid does
**not** need this — run it inline. This skill is for the multi-minute-to-multi-hour case.

## Related skills
- [`ds5-validate`](../ds5-validate/SKILL.md) — A/B validation; wrap a long repro leg with this watcher.
- [`ds5-bug-reproduction`](../ds5-bug-reproduction/SKILL.md) — the repro loop and bisect legs that this watcher monitors.
- [`fix-and-validate`](../fix-and-validate/SKILL.md) — Phase 5 delegates to `ds5-validate`; long soaks there use this skill.
- [`run-fw-tests`](../run-fw-tests/SKILL.md) — the per-cycle / repro check the loop actually runs.
- [`hub-control`](../hub-control/SKILL.md) — recover a `DEVICE_GONE` rig (hub/port power-cycle); also the base64-staging pattern for flaky `scp`.
- [`ssh-setup`](../ssh-setup/SKILL.md) — per-rig SSH key/hostkey registry the watcher's `SSH_KEY` resolves from.
