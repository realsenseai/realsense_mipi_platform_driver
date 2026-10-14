#!/usr/bin/env bash
# soak_loop.sh — run a per-cycle command N times on a rig, emitting the exact
# heartbeat + terminal markers that soak_watch.sh greps for. Stage this on the
# RIG (Linux) and run it under nohup/setsid so it survives the SSH session.
#
# Contract (the watcher depends on these literal tokens):
#   SOAK_START  tag=.. cycles=.. every=.. pid=..   (once, at start)
#   PROGRESS    iter=N ts=..                         (every $SOAK_PROGRESS_EVERY cycles)
#   SOAK_FAILED iter=N rc=.. ts=..                   (a cycle returned non-zero -> exit 1)
#   SOAK_COMPLETE iter=N ts=..                       (all cycles passed       -> exit 0)
# It also writes its own PID to "$SOAK_LOG.pid" so the watcher can prove liveness
# with `kill -0` instead of a fragile pgrep.
#
# Usage (on the rig):
#   SOAK_CYCLES=5000 SOAK_PROGRESS_EVERY=100 SOAK_LOG=/tmp/soak-run.log \
#     nohup bash soak_loop.sh python3 /tmp/one_cycle.py >/dev/null 2>&1 &
# where the per-cycle command is ONE customer-faithful iteration (open ->
# set_option -> stream -> close) and exits non-zero only on the symptom.
set -u

LOG=${SOAK_LOG:-/tmp/soak.log}
CYCLES=${SOAK_CYCLES:-5000}
EVERY=${SOAK_PROGRESS_EVERY:-100}
TAG=${SOAK_TAG:-soak}
PIDFILE="$LOG.pid"

CMD=("$@")
if [ ${#CMD[@]} -eq 0 ]; then
  echo "usage: SOAK_CYCLES=.. SOAK_LOG=.. soak_loop.sh <per-cycle-cmd...>" >&2
  exit 2
fi

echo "$$" > "$PIDFILE"
ts() { date -u +%FT%TZ; }
echo "SOAK_START tag=$TAG cycles=$CYCLES every=$EVERY pid=$$ ts=$(ts)" >> "$LOG"

i=0
while [ "$i" -lt "$CYCLES" ]; do
  i=$((i + 1))
  "${CMD[@]}" >> "$LOG" 2>&1
  rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "SOAK_FAILED iter=$i rc=$rc ts=$(ts)" >> "$LOG"
    exit 1
  fi
  if [ $((i % EVERY)) -eq 0 ]; then
    echo "PROGRESS iter=$i ts=$(ts)" >> "$LOG"
  fi
done

echo "SOAK_COMPLETE iter=$i ts=$(ts)" >> "$LOG"
exit 0
