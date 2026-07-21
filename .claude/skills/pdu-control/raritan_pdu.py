#!/usr/bin/env python3
"""
Raritan PX3 PDU control via JSON-RPC.

Subcommands:
  list                     - print all outlets with name and power state
  status  <node>           - print state of one outlet by name
  on      <node>           - power on an outlet
  off     <node>           - power off an outlet
  cycle   <node>           - power-cycle an outlet (off -> on)

node: outlet label as configured on the PDU (e.g. "jetson-orin-01").
      Matched against both the full node name and the short name
      (first component before the first dot).

Usage:
  raritan_pdu.py <host> <user> <pass> <subcommand> [<node>] [--delay <sec>]

Exit codes: 0 ok, 1 error, 2 usage error.
"""
import json
import ssl
import base64
import urllib.request as u
import sys
import time
import argparse


def parse_args():
    p = argparse.ArgumentParser(add_help=False)
    p.add_argument("host")
    p.add_argument("user")
    p.add_argument("pw")
    p.add_argument("subcmd", choices=["list", "status", "on", "off", "cycle"])
    p.add_argument("node", nargs="?", default=None)
    p.add_argument("--delay", type=int, default=0,
                   help="Seconds to wait before executing (default 0)")
    args = p.parse_args()

    if args.subcmd != "list" and args.node is None:
        print(f"Error: subcommand '{args.subcmd}' requires a <node> argument",
              file=sys.stderr)
        sys.exit(2)
    if args.delay < 0:
        print("Error: --delay must be >= 0", file=sys.stderr)
        sys.exit(2)

    return args


def make_rpc(host, auth):
    ctx = ssl._create_unverified_context()

    def rpc(path, method, params=None):
        d = {"jsonrpc": "2.0", "method": method, "id": 1}
        if params is not None:
            d["params"] = params
        req = u.Request(
            f"https://{host}{path}",
            data=json.dumps(d).encode(),
            headers={"Authorization": auth, "Content-Type": "application/json"},
        )
        resp = json.loads(u.urlopen(req, context=ctx, timeout=20).read().decode())
        if resp.get("error"):
            raise RuntimeError(f"JSON-RPC error on {method}: {resp['error']}")
        return resp

    return rpc


POWER_STATES = {0: "off", 1: "on", 2: "cycling"}


def get_outlets(rpc):
    """Return list of dicts: {idx, rid, name, state}."""
    outs = rpc("/model/pdu/0", "getOutlets")["result"]["_ret_"]
    result = []
    for outlet in outs:
        rid = outlet["rid"]
        idx = int(rid.rsplit(".", 1)[1])
        settings = rpc(rid, "getSettings")["result"]["_ret_"]
        name = settings.get("name", "")
        state_code = rpc(rid, "getState")["result"]["_ret_"].get("powerState", -1)
        state = POWER_STATES.get(state_code, f"unknown({state_code})")
        result.append({"idx": idx, "rid": rid, "name": name, "state": state})
    return result


def find_outlet(outlets, node):
    short = node.split(".", 1)[0]
    candidates = {node, short}
    for o in outlets:
        if o["name"] in candidates:
            return o
    names = [o["name"] for o in outlets]
    raise RuntimeError(
        f"No outlet found for '{node}' (short '{short}'). "
        f"Available: {names}"
    )


def cmd_list(rpc):
    outlets = get_outlets(rpc)
    w = max((len(o["name"]) for o in outlets), default=4)
    print(f"{'#':<4}  {'Name':<{w}}  State")
    print(f"{'—'*4}  {'—'*w}  {'—'*8}")
    for o in outlets:
        print(f"{o['idx']:<4}  {o['name']:<{w}}  {o['state']}")


def cmd_status(rpc, node):
    outlets = get_outlets(rpc)
    o = find_outlet(outlets, node)
    print(f"Outlet {o['idx']}  name={o['name']}  state={o['state']}")


def cmd_on(rpc, node):
    outlets = get_outlets(rpc)
    o = find_outlet(outlets, node)
    rpc(o["rid"], "switchOn")
    print(f"Outlet {o['idx']} ({o['name']}): powered ON")


def cmd_off(rpc, node):
    outlets = get_outlets(rpc)
    o = find_outlet(outlets, node)
    rpc(o["rid"], "switchOff")
    print(f"Outlet {o['idx']} ({o['name']}): powered OFF")


def cmd_cycle(rpc, node):
    outlets = get_outlets(rpc)
    o = find_outlet(outlets, node)
    rpc(o["rid"], "cyclePowerState")
    print(f"Outlet {o['idx']} ({o['name']}): power-cycled")


def main():
    args = parse_args()

    if args.delay > 0:
        print(f"Waiting {args.delay}s before executing...")
        time.sleep(args.delay)

    auth = "Basic " + base64.b64encode(f"{args.user}:{args.pw}".encode()).decode()
    rpc = make_rpc(args.host, auth)

    dispatch = {
        "list":   lambda: cmd_list(rpc),
        "status": lambda: cmd_status(rpc, args.node),
        "on":     lambda: cmd_on(rpc, args.node),
        "off":    lambda: cmd_off(rpc, args.node),
        "cycle":  lambda: cmd_cycle(rpc, args.node),
    }

    try:
        dispatch[args.subcmd]()
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
