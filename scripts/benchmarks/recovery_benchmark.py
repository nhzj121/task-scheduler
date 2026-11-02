#!/usr/bin/env python3
"""Client-observed failure/recovery timings; LOST is not physical death or commit time."""
import argparse
from contextlib import closing
import json
import os
from pathlib import Path
import signal
import time

import measurement_support as m
from fault_proxy import FaultProxy


def group_absent(pid):
    if Path(f"/proc/{pid}").exists():
        return False
    try:
        os.killpg(pid, 0)
        return False
    except ProcessLookupError:
        return True


def sample(cluster, proxy, worker, args, scenario, record):
    with closing(cluster.client()) as client:
        initial = cluster.healthy(1)[0]
        record["old_session_id"] = initial["session_id"]
        record["initial_worker"] = initial
        task, submit = client.request(0x0101, dict(executable="/bin/sleep", args=["60"],
            env={}, resources=dict(cpu_slots=1, memory_mb=16)))
        task_id = task["task_id"]
        record.update(task_id=task_id, submit=submit, initial_worker_process=worker)
        running = m.cluster.eventually(lambda: (value if (value := client.get(task_id)[0])["attempts"] and
            value["attempts"][0]["state"] == "RUNNING" else None), seconds=10)
        m.require(len(running["attempts"]) == 1 and running["attempts"][0]["session_id"] == record["old_session_id"], "wrong initial Attempt")
        record["initial_task"] = running
        record["old_attempt_id"] = running["attempts"][0]["attempt_id"]
        child_path = Path(f'/proc/{worker["pid"]}/task/{worker["pid"]}/children')
        children = m.cluster.eventually(lambda: child_path.read_text().split())
        m.require(len(children) == 1, "expected one owned direct workload leader")
        leader = int(children[0])
        identity = m.cluster.proc_identity(leader)
        stat = Path(f"/proc/{leader}/stat").read_text().rsplit(")", 1)[1].split()
        m.require(identity and identity["uid"] == os.geteuid() and int(stat[1]) == worker["pid"] and int(stat[2]) == leader,
                  "workload parent/group identity mismatch")
        record["old_workload"] = dict(pid=leader, pgid=leader, identity=identity)
        m.cluster.eventually(lambda: proxy.snapshot()["connections"][0]["heartbeats_forwarded"] >= 2)
        before = client.workers()[0]["workers"][0]
        m.require(before["reserved"] == dict(cpu_slots=1, memory_mb=16) and before["running_attempts"] == 1, "missing old reservation")
        record["worker_before_fault"] = before
        if scenario == "sigterm":
            start = m.now()
            cluster.signal(worker, signal.SIGTERM)
            record["injection"] = dict(fault_start_ns=start, fault_end_ns=m.now(), method="verified Worker SIGTERM")
        else:
            record["injection"] = proxy.inject(scenario)
        record["polls"] = []
        deadline = m.now() + int(args.timeout * 1e9)
        while m.now() < deadline:
            view, task_timing = client.get(task_id)
            workers, worker_timing = client.workers()
            current = workers["workers"][0]
            record["polls"].append(dict(task=task_timing, worker=worker_timing, task_state=view["task"]["state"],
                attempt_state=view["attempts"][0]["state"], worker_state=current["state"],
                session_id=current["session_id"], reserved=current["reserved"], running_attempts=current["running_attempts"]))
            if view["task"]["state"] == "FAILED" and view["attempts"][0]["state"] == "LOST" and "lost_observed_ns" not in record:
                record["lost_observed_ns"] = task_timing["response_ns"]
                record["lost_task"] = view
                record["lost_sqlite"] = m.db_rows(cluster)
            if current["reserved"] == dict(cpu_slots=0, memory_mb=0) and current["running_attempts"] == 0:
                record.setdefault("release_observed_ns", worker_timing["response_ns"])
            if group_absent(leader):
                record.setdefault("old_group_absent_observed_ns", m.now())
            old_process = cluster.handles[worker["pid"]]
            if scenario == "sigterm" and old_process.poll() is not None:
                record.setdefault("worker_exit_observed_ns", m.now())
                m.require(old_process.returncode == 0, "graceful Worker exit failed")
                if all(k in record for k in ("lost_observed_ns", "release_observed_ns", "old_group_absent_observed_ns")) and "restart_start_ns" not in record:
                    record["restart_start_ns"] = m.now()
                    restarted = cluster.start_worker("recovery-worker", proxy.endpoint)
                    record["restart_spawn_completed_ns"] = m.now()
                    record["new_worker_process"] = restarted
            if current["state"] == "HEALTHY" and current["session_id"] != record["old_session_id"]:
                record.setdefault("healthy_observed_ns", worker_timing["response_ns"])
                record["new_session_id"] = current["session_id"]
                record["new_worker"] = current
            if all(k in record for k in ("lost_observed_ns", "release_observed_ns", "old_group_absent_observed_ns", "healthy_observed_ns")):
                break
            time.sleep(args.poll_ms / 1000)
        m.require(all(k in record for k in ("lost_observed_ns", "release_observed_ns", "old_group_absent_observed_ns", "healthy_observed_ns")),
                  "incomplete fault/recovery observation")
        final, record["final_query"] = client.get(task_id)
        m.require(len(final["attempts"]) == 1 and final["attempts"][0]["attempt_id"] == record["old_attempt_id"] and
            final["attempts"][0]["state"] == "LOST" and not final["attempts"][0]["logs_complete"] and
            final["task"]["state"] == "FAILED", "loss/adoption/retry/log contract failed")
        record["final_task"] = final
        record["final_sqlite"] = m.db_rows(cluster)
        m.require(record["final_sqlite"]["tasks"][0]["state"] == "FAILED" and
            record["final_sqlite"]["attempts"][0]["state"] == "LOST" and
            record["final_sqlite"]["attempts"][0]["logs_complete"] == 0, "second reader disagrees with terminal observation")
        m.require(m.idle_workers([record["new_worker"]], 1), "new session retained reservation")
        for active in cluster.records:
            if active["role"] == "worker" and cluster.handles[active["pid"]].poll() is None:
                m.require(m.cluster.owned(active), "new session has no verified local process")
                m.require(Path(f'/proc/{active["pid"]}/task/{active["pid"]}/children').read_text().strip() == "", "old child adopted or unreaped")
        record["proxy_at_recovery"] = proxy.snapshot()
        registrations = [e["payload"]["session_id"] for c in record["proxy_at_recovery"]["connections"]
            for e in c["events"] if e["kind"] == "frame" and e["type"] == 0x0201]
        m.require(registrations[0] == record["old_session_id"] and record["new_session_id"] in registrations[1:],
                  "fresh session not observed on real Worker wire")
        m.require(record["final_sqlite"]["attempts"][0]["session_id"] == record["old_session_id"], "old Attempt binding changed")
        if scenario != "sigterm":
            m.require(m.cluster.owned(worker), "reconnection unexpectedly replaced the local Worker process")
        if scenario == "heartbeat":
            old = record["proxy_at_recovery"]["connections"][0]
            m.require(old["heartbeats_dropped"] > 0 and not old["deliberately_closed"], "heartbeat case did not keep TCP open")
            first_close = next(e for e in old["events"] if e["kind"] == "close")
            m.require(first_close["reason"] == "controller_eof", "heartbeat was not detected by Controller")
        start = record["injection"]["fault_start_ns"]
        record["timings_s"] = {key: (record[key]-start)/1e9 for key in
            ("lost_observed_ns", "release_observed_ns", "old_group_absent_observed_ns", "healthy_observed_ns")}
        if "restart_start_ns" in record:
            record["timings_s"]["restart_to_healthy"] = (record["healthy_observed_ns"]-record["restart_start_ns"])/1e9


def run(args):
    m.require(1 <= args.repeats <= 20 and 1 <= args.poll_ms <= 100 and 5 <= args.timeout <= 60 and
              20 <= args.heartbeat_ms < args.offline_ms <= 10000, "measurement bounds exceeded")
    m.require(len(set(args.scenarios)) == len(args.scenarios), "duplicate scenario")
    root = m.fresh_root(args.root)
    parameters = dict(scenarios=args.scenarios, repeats=args.repeats, poll_ms=args.poll_ms, timeout=args.timeout,
                      heartbeat_ms=args.heartbeat_ms, offline_ms=args.offline_ms)
    output = dict(kind="recovery", mode=args.mode, status="RUNNING", environment=m.environment(args.bin_dir, root), parameters=parameters, runs=[])
    if args.mode == "formal":
        m.require(args.repeats == 20 and args.pilot_evidence, "formal recovery requires 20 repeats per scenario")
        m.pilot_gate(args.pilot_evidence, "recovery", {k:v for k,v in parameters.items() if k != "repeats"}, output["environment"]["binaries"])
    m.save(root / "results.json", output)
    for scenario in args.scenarios:
        for repetition in range(args.repeats):
            record = dict(scenario=scenario, repetition=repetition, status="RUNNING")
            output["runs"].append(record)
            cluster, proxy = None, None
            try:
                cluster = m.ManagedCluster(args.bin_dir, root / f"{scenario}-{repetition}",
                    dict(interval_ms=args.heartbeat_ms, offline_timeout_ms=args.offline_ms))
                record["artifact_root"] = str(cluster.root)
                cluster.start_controller()
                proxy = FaultProxy(cluster.config["worker_listen"]["port"])
                worker = cluster.start_worker("recovery-worker", proxy.endpoint)
                record["controller_config"], record["worker_configs"] = cluster.config, cluster.worker_configs
                sample(cluster, proxy, worker, args, scenario, record)
                record["status"] = "PASS"
            except (Exception, KeyboardInterrupt) as error:
                record.update(status="FAILED", error=str(error))
                if isinstance(error, KeyboardInterrupt):
                    raise
            finally:
                if cluster:
                    cleanup_errors = []
                    for owner in reversed(cluster.records[1:]):
                        try:
                            cluster.stop_one(owner)
                        except Exception as error:
                            cleanup_errors.append(str(error))
                    if proxy:
                        try:
                            proxy.close()
                        except Exception as error:
                            cleanup_errors.append(str(error))
                    try:
                        cluster.close()
                    except Exception as error:
                        cleanup_errors.append(str(error))
                    if cleanup_errors:
                        record.update(status="FAILED_CLEANUP", cleanup_errors=cleanup_errors)
                    record["processes"] = cluster.records
                    if proxy:
                        record["proxy_final"] = proxy.snapshot()
                    m.save(cluster.root / "run.json", record)
                complete = len(output["runs"]) == len(args.scenarios) * args.repeats
                output["status"] = ("PASS" if complete else "RUNNING") if all(r["status"] == "PASS" for r in output["runs"]) else "FAILED"
                m.save(root / "results.json", output)
    print(json.dumps(dict(status=output["status"], runs=len(output["runs"]), root=str(root))))
    return 0 if output["status"] == "PASS" else 1


if __name__ == "__main__":
    os.umask(0o077)
    m.install_interrupt_handler()
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--bin-dir", type=Path, required=True)
    p.add_argument("--root", type=Path, required=True)
    p.add_argument("--mode", choices=("pilot", "formal"), default="pilot")
    p.add_argument("--pilot-evidence", type=Path)
    p.add_argument("--repeats", type=int, default=2)
    p.add_argument("--scenarios", nargs="+", choices=("disconnect","sigterm","heartbeat"), default=["disconnect","sigterm","heartbeat"])
    p.add_argument("--heartbeat-ms", type=int, default=100)
    p.add_argument("--offline-ms", type=int, default=1500)
    p.add_argument("--poll-ms", type=int, default=10)
    p.add_argument("--timeout", type=float, default=15)
    raise SystemExit(run(p.parse_args()))
