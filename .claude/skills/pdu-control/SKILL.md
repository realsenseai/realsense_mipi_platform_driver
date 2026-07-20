---
name: pdu-control
description: Control a Raritan PX3 PDU outlet by node/Jetson name. Supports list (enumerate outlets + power states), status (check one outlet), on, off, and cycle (power-cycle). Uses JSON-RPC over HTTPS. Reads PDU host/credentials from env vars (PDU_HOST, PDU_USER, PDU_PASS) or from the skill's config.ini. Node names are matched against PDU outlet labels by full name or short hostname. Use when the user asks to power-cycle a Jetson, reboot via PDU, turn an outlet on/off, check PDU outlet states, or otherwise control the lab PDU.
---

# Skill: Raritan PX3 PDU control

## Overview
Controls a **Raritan PX3** PDU over its JSON-RPC HTTPS API. Finds an outlet by matching the supplied node name (or its short hostname) against configured outlet labels on the PDU, then performs the requested action.

Composes naturally with other rig skills: `pdu-control cycle <node>` to hard-reboot a Jetson when SSH is unresponsive, then `deploy` / `v4l2-test` once it comes back.

## Parameters (from `<skill-dir>/config.ini` → `[pdu-control]`)

Config values can also be overridden by env vars (env takes precedence over config.ini):

| Config key | Env var    | Description |
|------------|-----------|-------------|
| `PDU_HOST` | `PDU_HOST` | Raritan PDU hostname or IP |
| `PDU_USER` | `PDU_USER` | PDU admin username |
| `PDU_PASS` | `PDU_PASS` | PDU admin password |
| `DELAY_SEC`| —          | Default delay before executing (default 0) |

Node-to-outlet label mappings are not required — names are resolved live by querying the PDU — but a `[nodes]` section in config.ini can document the mapping for reference.

## Subcommands / actions

| Action | What it does |
|--------|--------------|
| `list` | Print all outlets: index, label, current power state |
| `status <node>` | Print power state of one outlet by node name |
| `on <node>` | Power on the outlet |
| `off <node>` | Power off the outlet |
| `cycle <node>` | Power-cycle (PDU-native cycle — Raritan handles off→delay→on atomically) |

`<node>` is matched against the PDU outlet label using full name first, then the short hostname (first component before the first dot). If no outlet matches, the error message lists all available labels.

## Workflow

1. **Resolve credentials**: read `PDU_HOST / PDU_USER / PDU_PASS` from env; fall back to `<skill-dir>/config.ini` `[pdu-control]` section. If any are missing, ask the user.
2. **Run the script** directly from the dev box (no SSH needed — PDU is reachable over HTTPS):
   ```bash
   python3 <skill-dir>/raritan_pdu.py $PDU_HOST $PDU_USER $PDU_PASS <action> [<node>] [--delay N]
   ```
3. **Confirm result**: for `on`/`off`/`cycle`, follow up with a `status <node>` call ~3 s later to verify the outlet reached the expected state.
4. **After power-cycle**: if the user wants to restore SSH access to the Jetson, poll `ssh -o ConnectTimeout=5 <user>@<node> 'echo ok'` up to ~120 s.

## Command templates

### Resolve credentials (bash)
```bash
CFG="$SKILL_DIR/config.ini"
PDU_HOST="${PDU_HOST:-$(awk -F'=' '/^\[pdu-control\]/{f=1;next}/^\[/{f=0}f&&/^PDU_HOST/{gsub(/ /,"");print $2;exit}' "$CFG")}"
PDU_USER="${PDU_USER:-$(awk -F'=' '/^\[pdu-control\]/{f=1;next}/^\[/{f=0}f&&/^PDU_USER/{gsub(/ /,"");print $2;exit}' "$CFG")}"
PDU_PASS="${PDU_PASS:-$(awk -F'=' '/^\[pdu-control\]/{f=1;next}/^\[/{f=0}f&&/^PDU_PASS/{gsub(/ /,"");print $2;exit}' "$CFG")}"
PY="python3 $SKILL_DIR/raritan_pdu.py $PDU_HOST $PDU_USER $PDU_PASS"
```

### List all outlets
```bash
$PY list
```

### Status of one node
```bash
$PY status jetson-orin-01
```

### Power-cycle
```bash
$PY cycle jetson-orin-01
```

### Power-cycle with pre-delay (give OS time to flush logs)
```bash
$PY cycle jetson-orin-01 --delay 5
```

### On / Off
```bash
$PY on  jetson-orin-01
$PY off jetson-orin-01
```

### Wait for Jetson to come back after power-cycle
```bash
NODE=jetson-orin-01
echo "Waiting for $NODE to come back..."
for i in $(seq 1 40); do
    ssh -o BatchMode=yes -o ConnectTimeout=5 "user@$NODE" 'echo ok' 2>/dev/null && break
    sleep 3
done
```

## Config file — `<skill-dir>/config.ini`
```ini
[pdu-control]
PDU_HOST  = <pdu-host-ip>
PDU_USER  = <admin-user>
PDU_PASS  = <password>
DELAY_SEC = 0

[nodes]
; Run `/pdu-control list` to discover outlet labels, then document them here:
; jetson-orin-01 = <outlet label as shown on PDU>
```

## Usage notes
- **SSL cert**: The Raritan PX3 uses a self-signed cert. The script skips cert verification (`ssl._create_unverified_context()`).
- **Native cycle**: `cyclePowerState` is handled by PDU firmware (configurable off-delay, typically 1–3 s).
- **Node name mismatch**: if the outlet label on the PDU does not match the hostname, run `list` and note the label. You can use any string that matches the PDU label as the `<node>` argument.
- **No SSH required**: this skill runs entirely from the dev box over HTTPS.
- **Companion to deploy**: after `pdu-control cycle <jetson>`, wait for SSH to recover, then run `verify-deploy` or `v4l2-test` to confirm the system came up healthy.

## Related skills
- [`deploy`](../deploy/SKILL.md) — deploy kernel/driver to the Jetson; use pdu-control cycle if the Jetson is unresponsive first.
- [`v4l2-test`](../v4l2-test/SKILL.md) — run V4L2 tests; use pdu-control to reset the Jetson if needed.
- [`verify-deploy`](../verify-deploy/SKILL.md) — verify driver after deployment; run after a pdu-control cycle.
