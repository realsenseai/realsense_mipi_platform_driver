#!/usr/bin/env bash
# soak_watch.sh — DEV-BOX watcher for a long rig soak. Polls cheaply over SSH and
# exits the moment something ACTIONABLE happens, printing exactly one verdict line:
#
#   SOAK_WATCH_VERDICT=DONE|FAILED|DEVICE_GONE|STUCK|CRASHED|UNREACHABLE|TIMEOUT
#
# Run it with the Bash tool's run_in_background:true. The harness then re-invokes
# the session ONLY when this process exits — so the main loop spends ZERO turns
# polling a multi-hour soak. The verdict line is the wake-up signal.
#
# Liveness is the watcher's job, not yours: it escalates on a stall / dead PID /
# unreachable rig / hard deadline, so a HUNG soak wakes you the same as a finished
# one — you never sit blind on a wedge.
#
# Required env:
#   RIG=user@host           # the rig the soak runs on
#   LOG=/tmp/soak.log       # the soak log path ON THE RIG (matches soak_loop.sh SOAK_LOG)
# Optional env:
#   SSH_KEY=~/.ssh/<key>    # identity file (else rely on agent/default)
#   PIDFILE=$LOG.pid        # loop pidfile (soak_loop.sh writes it)
#   PROBE=~/soak/soak_probe.sh   # staged probe on the rig
#   POLL=60                 # seconds between polls (keep <300 to stay in cache)
#   STALL_POLLS=5           # consecutive no-progress polls -> STUCK (~POLL*5 s)
#   SSH_FAILS_MAX=3         # consecutive ssh failures -> UNREACHABLE
#   DEADLINE_POLLS=340      # hard cap (340*60 s ~= 5.6 h) -> TIMEOUT
#   USB_CHECK="lsusb -d 8086:"   # blank to disable the device-gone check
set -u

RIG=${RIG:?set RIG=user@host}
LOG=${LOG:?set LOG=/path/on/rig.log}
SSH_KEY=${SSH_KEY:-}
PIDFILE=${PIDFILE:-$LOG.pid}
PROBE=${PROBE:-~/soak/soak_probe.sh}
POLL=${POLL:-60}
STALL_POLLS=${STALL_POLLS:-5}
SSH_FAILS_MAX=${SSH_FAILS_MAX:-3}
DEADLINE_POLLS=${DEADLINE_POLLS:-340}
USB_CHECK=${USB_CHECK:-lsusb -d 8086:}

ssh_rig() {
  if [ -n "$SSH_KEY" ]; then
    ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=15 "$RIG" "$1"
  else
    ssh -o BatchMode=yes -o ConnectTimeout=15 "$RIG" "$1"
  fi
}

verdict() { # name, detail, exit-code
  echo "SOAK_WATCH_VERDICT=$1 detail=\"$2\" at=$(date -u +%FT%TZ) rig=$RIG log=$LOG"
  exit "${3:-0}"
}

prev_iter="-1"; stalls=0; sshfails=0; poll=0
while [ "$poll" -lt "$DEADLINE_POLLS" ]; do
  poll=$((poll + 1))

  if ! out=$(ssh_rig "bash '$PROBE' '$LOG' '$PIDFILE' '$USB_CHECK'"); then
    sshfails=$((sshfails + 1))
    [ "$sshfails" -ge "$SSH_FAILS_MAX" ] && verdict UNREACHABLE "ssh failed ${sshfails}x in a row" 4
    sleep "$POLL"; continue
  fi
  sshfails=0

  tailtext=${out%%---PID---*}
  rest=${out#*---PID---}
  alive=$(printf '%s' "${rest%%---USB---*}" | tr -d '[:space:]')
  usbn=$(printf '%s'  "${rest#*---USB---}"  | tr -d '[:space:]')

  # --- terminal markers from the loop (highest priority) ---
  if printf '%s' "$tailtext" | grep -q 'SOAK_COMPLETE'; then
    verdict DONE "$(printf '%s' "$tailtext" | grep 'SOAK_COMPLETE' | tail -1)" 0
  fi
  if printf '%s' "$tailtext" | grep -q 'SOAK_FAILED'; then
    verdict FAILED "$(printf '%s' "$tailtext" | grep 'SOAK_FAILED' | tail -1)" 1
  fi

  # --- camera dropped off USB (HC death / wedge): terminal for most repros ---
  if [ -n "$USB_CHECK" ] && [ "${usbn:-0}" = "0" ]; then
    verdict DEVICE_GONE "no device matched '$USB_CHECK' on the rig" 1
  fi

  # --- progress / stall watchdog ---
  cur_iter=$(printf '%s' "$tailtext" | grep -oE 'iter=[0-9]+' | tail -1 | cut -d= -f2)
  cur_iter=${cur_iter:-0}
  if [ "$cur_iter" = "$prev_iter" ]; then stalls=$((stalls + 1)); else stalls=0; prev_iter="$cur_iter"; fi

  if [ "$alive" = "DEAD" ]; then
    verdict CRASHED "loop PID gone with no terminal marker (last iter=$cur_iter)" 2
  fi
  if [ "$stalls" -ge "$STALL_POLLS" ]; then
    verdict STUCK "no progress for $stalls polls (~$((stalls * POLL))s) at iter=$cur_iter, PID still ALIVE" 3
  fi

  sleep "$POLL"
done

verdict TIMEOUT "deadline of $DEADLINE_POLLS polls reached (last iter=$prev_iter)" 5
