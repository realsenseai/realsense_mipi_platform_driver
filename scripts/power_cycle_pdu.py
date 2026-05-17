#!/usr/bin/env python3
"""
Power-cycle a Raritan PX3 outlet by agent name using JSON-RPC.
"""
import json
import ssl
import base64
import urllib.request as u
import sys
import time

def parse_args():
    if len(sys.argv) != 6:
        print(
            "Usage: power_cycle_pdu.py <host> <user> <pass> <node_name> <delay_sec>",
            file=sys.stderr,
        )
        sys.exit(2)

    host = sys.argv[1]
    user = sys.argv[2]
    pw = sys.argv[3]
    node_name = sys.argv[4]

    try:
        delay_sec = int(sys.argv[5])
    except ValueError:
        print("delay_sec must be an integer", file=sys.stderr)
        sys.exit(2)

    if delay_sec < 0:
        print("delay_sec must be >= 0", file=sys.stderr)
        sys.exit(2)

    short_name = node_name.split(".", 1)[0]
    return host, user, pw, node_name, short_name, delay_sec


host, user, pw, agent_name, agent_short_name, delay_sec = parse_args()

print(f"Raritan host: {host}")
print(f"Agent name: {agent_name}")
print(f"Agent short name: {agent_short_name}")
print(f"Delay before power-cycle: {delay_sec}s")

# Wait before cycling
time.sleep(delay_sec)

ctx = ssl._create_unverified_context()
auth = "Basic " + base64.b64encode(f"{user}:{pw}".encode()).decode()

def rpc(path, method, params=None):
    d = {"jsonrpc": "2.0", "method": method, "id": 1}
    if params is not None: d["params"] = params
    req = u.Request(f"https://{host}{path}", data=json.dumps(d).encode(),
                    headers={"Authorization": auth, "Content-Type": "application/json"})
    resp = json.loads(u.urlopen(req, context=ctx, timeout=20).read().decode())
    if resp.get("error"):
        raise RuntimeError(f"JSON-RPC error: {resp['error']}")
    return resp

try:
    outs = rpc("/model/pdu/0", "getOutlets")["result"]["_ret_"]
    candidates = {agent_name, agent_short_name}

    idx = None
    seen_names = []
    for outlet in outs:
        outlet_name = rpc(outlet["rid"], "getSettings")["result"]["_ret_"].get("name")
        if outlet_name:
            seen_names.append(outlet_name)
        if outlet_name in candidates:
            idx = int(outlet["rid"].rsplit(".", 1)[1])
            break

    if idx is None:
        raise RuntimeError(
            f"No outlet found for {agent_name} (short {agent_short_name}). "
            f"Available names: {seen_names}"
        )

    resp = rpc(f"/model/pdu/0/outlet/{idx}", "cyclePowerState")
    print(f"Power-cycle result: {resp['result']}")
except Exception as e:
    print(f"Error: {e}", file=sys.stderr)
    sys.exit(1)
