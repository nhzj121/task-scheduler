#!/usr/bin/env python3
"""Isolated installed-binary lifecycle. Keeps data/logs; never kills by name."""
import argparse
import json
import os
from pathlib import Path
import signal
import sys
import cluster_support as cluster


def demo(state):
    # Valid specifications wait for currently occupied slots; oversized requests
    # are now rejected before Task creation.
    workers = cluster.cli(state, "workers")[0]["workers"]
    blockers = [cluster.cli(state, "submit", "--cpu", str(w["capacity"]["cpu_slots"]),
                            "/bin/sleep", "30")[0]["task_id"] for w in workers]
    cluster.eventually(lambda: all(w["reserved"]["cpu_slots"] == w["capacity"]["cpu_slots"]
                                  for w in cluster.cli(state, "workers")[0]["workers"]))
    pending = cluster.cli(state, "submit", "--cpu", "1", "/bin/true")[0]["task_id"]
    cluster.require(cluster.cli(state, "get", pending)[0]["task"]["state"] == "PENDING")
    cluster.require(cluster.cli(state, "cancel", pending)[0]["status"] == "completed")
    for blocker in blockers:
        cluster.cli(state, "cancel", blocker)
        cluster.terminal(state, blocker, "CANCELLED")
    task = cluster.cli(state, "submit", "/bin/true")[0]["task_id"]
    view = cluster.terminal(state, task)
    cluster.require(view["attempts"][-1]["logs_complete"])
    cluster.require(cluster.cli(state, "logs", task, "--both", binary=True) == b"")
    active = cluster.cli(state, "submit", "/bin/sleep", "30")[0]["task_id"]
    cluster.eventually(lambda: cluster.cli(state, "get", active)[0]["task"]["state"] == "ACTIVE")
    cluster.require(cluster.cli(state, "cancel", active)[0]["status"] == "accepted")
    cluster.require(cluster.terminal(state, active, "CANCELLED")["attempts"][-1]["logs_complete"])
    timed = cluster.cli(state, "submit", "--timeout", "1", "/bin/sleep", "30")[0]["task_id"]
    cluster.require(cluster.terminal(state, timed, "FAILED")["attempts"][-1]["state"] == "TIMED_OUT")
    marker = Path(state["root"]) / "retry-precondition"
    if marker.exists():
        raise RuntimeError("demo requires a fresh retry-precondition path; use a new demo root")
    failed = cluster.cli(state, "submit", "/usr/bin/ls", str(marker))[0]["task_id"]
    old = cluster.terminal(state, failed, "FAILED")["attempts"][0]
    old_logs = cluster.cli(state, "logs", failed, "--attempt", "1", "--both", binary=True)
    marker.mkdir()
    cluster.require(cluster.cli(state, "retry", failed)[0]["next_attempt_no"] == 2)
    retried = cluster.terminal(state, failed)
    cluster.require(retried["attempts"][0] == old and len(retried["attempts"]) == 2)
    cluster.require(cluster.cli(state, "logs", failed, "--attempt", "1", "--both", binary=True) == old_logs)
    return dict(pending_cancel=pending, success=task, active_cancel=active, timeout=timed, retry=failed)


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("start", "status", "stop", "demo"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--bin-dir", type=Path)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--client-port", type=int, default=0)
    parser.add_argument("--worker-port", type=int, default=0)
    args = parser.parse_args()
    def interrupted(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    if args.command in ("start", "demo"):
        if not args.bin_dir:
            parser.error("start/demo requires --bin-dir")
        state = cluster.start(args.bin_dir, args.root, args.workers, args.client_port, args.worker_port)
        if args.command == "demo":
            try:
                print(json.dumps(demo(state)))
            finally:
                cluster.stop(args.root)
        else:
            print(json.dumps(state))
    elif args.command == "stop":
        cluster.stop(args.root)
        print("owned services stopped; data and diagnostics retained")
    else:
        state = cluster.load(args.root)
        if not all(cluster.owned(p) for p in state["processes"]):
            raise RuntimeError("registered process is no longer alive")
        print(json.dumps(cluster.cli(state, "workers")[0]))


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        print(f"local_cluster: {error}", file=sys.stderr)
        raise SystemExit(1)
