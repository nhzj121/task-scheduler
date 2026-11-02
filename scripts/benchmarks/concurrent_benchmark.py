#!/usr/bin/env python3
"""Bounded independent-client measurement of accepted v1, never scheduler instrumentation."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
import json
import os
from pathlib import Path
import queue
import statistics
import sys
import threading
import time

import measurement_support as m


def task_payload(workload, size):
    if workload == "true":
        executable, args = "/bin/true", []
    elif workload == "sleep":
        executable, args = "/bin/sleep", ["0.2"]
    else:
        executable, args = sys.executable, [str(m.SCRIPTS / "workloads/binary_output.py"), str(size)]
    return dict(executable=executable, args=args, env={}, resources=dict(cpu_slots=1, memory_mb=0))


def verify_logs(cluster, client, samples, size):
    rows = m.db_rows(cluster)
    attempts = {a["task_id"]: a for a in rows["attempts"]}
    pattern = bytes(i % 251 for i in range(size))
    for sample in samples:
        if sample.get("terminal", {}).get("task", {}).get("state") != "SUCCEEDED":
            continue
        task = sample["task_id"]
        attempt = attempts[task]
        m.require(attempt["state"] == "SUCCEEDED" and attempt["logs_complete"] == 1 and
                  attempt["stdout_bytes"] == attempt["stderr_bytes"] == size, "SQLite final tail mismatch")
        for stream, expected in (("stdout", pattern), ("stderr", bytes(x ^ 0xa5 for x in pattern))):
            actual = client.logs(task, stream)
            path = cluster.root / "data" / attempt[stream + "_path"]
            m.require(path.resolve().is_relative_to((cluster.root / "data").resolve()) and not path.is_symlink(),
                      "log path escaped measurement root")
            m.require(actual == path.read_bytes() == expected, "downloaded/stored log differs")
        sample["logs_verified"] = True
    return rows


def measured(cluster, args, workload, record):
    payload = task_payload(workload, args.bytes)
    record["payload"] = payload
    clients, observers = [], []
    try:
        clients = [cluster.client() for _ in range(args.clients)]
        observers = [cluster.client() for _ in range(args.observers)]
        record["connections"] = dict(submit=[c.connection for c in clients], observe=[c.connection for c in observers])
        samples = [dict(index=i, polls=[]) for i in range(args.tasks)]
        record["samples"] = samples
        pending = queue.Queue(maxsize=args.tasks)
        for sample in samples:
            pending.put(sample)
        lock, begin_event, sample_stop, abort = threading.Lock(), threading.Event(), threading.Event(), threading.Event()
        errors = record["errors"]

        def submit(client, number):
            begin_event.wait()
            while True:
                if abort.is_set():
                    return
                try:
                    sample = pending.get_nowait()
                except queue.Empty:
                    return
                try:
                    reply, timing = client.request(0x0101, payload)
                    m.require(reply["state"] == "PENDING", "submission response must describe durable PENDING")
                    with lock:
                        sample.update(client=number, submit=timing, task_id=reply["task_id"], submit_response=reply)
                except Exception as error:
                    with lock:
                        sample.update(submit_error=str(error), submission_status="UNKNOWN_OR_REJECTED")
                        errors.append(dict(phase="submit", index=sample["index"], error=str(error), at_ns=m.now()))
                finally:
                    pending.task_done()

        def observe(client, number):
            begin_event.wait()
            owned = samples[number::args.observers]
            while True:
                if abort.is_set():
                    return
                with lock:
                    remaining = [s for s in owned if not s.get("terminal") and not s.get("submit_error")]
                    ready = [s for s in remaining if "task_id" in s]
                if not remaining:
                    return
                if m.now() >= record["deadline_ns"]:
                    with lock:
                        for sample in remaining:
                            sample["incomplete"] = "no terminal observation before deadline"
                        errors.append(dict(phase="observe", error="batch deadline", at_ns=m.now()))
                    return
                for sample in ready:
                    try:
                        view, timing = client.get(sample["task_id"])
                        state = view["task"]["state"]
                        with lock:
                            sample["polls"].append(dict(**timing, task_state=state,
                                attempt_states=[a["state"] for a in view["attempts"]]))
                            if state not in ("PENDING", "ACTIVE"):
                                sample["terminal"] = view
                                sample["terminal_observed_ns"] = timing["response_ns"]
                                if state != "SUCCEEDED" or len(view["attempts"]) != 1 or not view["attempts"][0]["logs_complete"] or view["attempts"][0]["finalization_pending"]:
                                    errors.append(dict(phase="terminal", index=sample["index"], state=state))
                    except Exception as error:
                        with lock:
                            errors.append(dict(phase="query", index=sample["index"], error=str(error), at_ns=m.now()))
                            for unfinished in owned:
                                if "terminal" not in unfinished:
                                    unfinished["incomplete"] = "observer RPC failed"
                        return
                time.sleep(args.poll_ms / 1000)

        def sample_resources():
            begin_event.wait()
            while not sample_stop.is_set():
                try:
                    value = m.snapshot(cluster.records)
                    with lock:
                        record["resource_samples"].append(value)
                except Exception as error:
                    with lock:
                        errors.append(dict(phase="resources", error=str(error)))
                    return
                sample_stop.wait(args.resource_ms / 1000)

        record["resources_before"] = m.snapshot(cluster.records)
        with ThreadPoolExecutor(max_workers=args.clients + args.observers + 1) as pool:
            sampler = None
            try:
                futures = [pool.submit(submit, c, i) for i, c in enumerate(clients)]
                futures += [pool.submit(observe, c, i) for i, c in enumerate(observers)]
                sampler = pool.submit(sample_resources)
                record["batch_start_ns"] = m.now()
                record["deadline_ns"] = record["batch_start_ns"] + int(args.timeout * 1e9)
                for sample in samples:
                    sample["queued_ns"] = record["batch_start_ns"]
                begin_event.set()
                while not all(f.done() for f in futures):
                    time.sleep(.05)
                for future in futures:
                    future.result()
            except BaseException:
                abort.set()
                begin_event.set()
                raise
            finally:
                sample_stop.set()
                begin_event.set()
                if sampler is not None:
                    sampler.result()
        end = max((s.get("terminal_observed_ns", m.now()) for s in samples), default=m.now())
        record["batch_observation_end_ns"] = end
        record["elapsed_seconds"] = (end - record["batch_start_ns"]) / 1e9
        record["resources_after_observation"] = m.snapshot(cluster.records)
    finally:
        for client in clients + observers:
            client.close()
    # Validation is outside the throughput time interval. Failure is retained.
    with closing(cluster.client()) as client:
        rows = verify_logs(cluster, client, record["samples"], args.bytes if workload == "binary" else 0)
        record["database"] = rows
        record["workers_after"] = client.workers()[0]["workers"]
        m.require(m.idle_workers(record["workers_after"], record["workers"]), "unreleased reservations or unhealthy Worker")
    record["outcomes"] = dict(Counter(s.get("terminal", {}).get("task", {}).get("state", "INCOMPLETE") for s in samples))
    m.no_worker_children(cluster)
    record["verified_successes"] = sum(bool(s.get("logs_verified")) for s in samples)
    record["successful_terminal_tasks_s"] = record["verified_successes"] / record["elapsed_seconds"]
    record["submit_latency_s"] = m.quantiles([(s["submit"]["response_ns"]-s["submit"]["request_start_ns"])/1e9 for s in samples if "submit" in s])
    record["observed_completion_latency_s"] = m.quantiles([(s["terminal_observed_ns"]-s["submit"]["request_start_ns"])/1e9 for s in samples if "terminal_observed_ns" in s])
    m.require(len({s.get("task_id") for s in samples}) == args.tasks and len(rows["tasks"]) == len(rows["attempts"]) == args.tasks,
              "missing/duplicate task or Attempt; full DB evidence retained")
    m.require(not errors and record["verified_successes"] == args.tasks, "failed or incomplete task sample")


def run(args):
    m.require(1 <= args.tasks <= 1000 and 1 <= args.repeats <= 10 and 1 <= args.clients <= 32 and
              1 <= args.observers <= 8 and 0 <= args.bytes <= 1048576 and 1 <= args.poll_ms <= 100 and
              10 <= args.resource_ms <= 1000 and 10 <= args.timeout <= 300, "measurement bounds exceeded")
    m.require(set(args.workers) <= {1, 2, 4} and len(set(args.workers)) == len(args.workers), "Workers must be unique 1/2/4")
    m.require(len(set(args.workloads)) == len(args.workloads), "duplicate workload")
    root = m.fresh_root(args.root)
    parameters = dict(workers=args.workers, workloads=args.workloads, clients=args.clients, observers=args.observers,
        bytes=args.bytes, tasks=args.tasks, repeats=args.repeats, poll_ms=args.poll_ms, resource_ms=args.resource_ms, timeout=args.timeout)
    output = dict(kind="concurrent", mode=args.mode, status="RUNNING", environment=m.environment(args.bin_dir, root), parameters=parameters, runs=[])
    if args.mode == "formal":
        m.require(args.tasks == 120 and args.repeats == 5 and args.pilot_evidence, "formal contract is 120 tasks x 5 repeats")
        m.pilot_gate(args.pilot_evidence, "concurrent", {k:v for k,v in parameters.items() if k not in ("tasks", "repeats")}, output["environment"]["binaries"])
    else:
        m.require(args.tasks == 20, "pilot contract is 20 tasks per scenario")
    m.save(root / "results.json", output)
    for workers in args.workers:
        for workload in args.workloads:
            for repetition in range(args.repeats):
                record = dict(workers=workers, workload=workload, repetition=repetition, status="RUNNING", errors=[], resource_samples=[])
                output["runs"].append(record)
                cluster = None
                try:
                    cluster = m.ManagedCluster(args.bin_dir, root / f"w{workers}-{workload}-{repetition}")
                    record["artifact_root"] = str(cluster.root)
                    cluster.start_controller()
                    for i in range(workers):
                        cluster.start_worker(f"bench-worker-{i+1}")
                    record["initial_workers"] = cluster.healthy(workers)
                    record["controller_config"], record["worker_configs"] = cluster.config, cluster.worker_configs
                    measured(cluster, args, workload, record)
                    record["status"] = "PASS"
                except (Exception, KeyboardInterrupt) as error:
                    record.update(status="FAILED", error=str(error))
                    if isinstance(error, KeyboardInterrupt):
                        raise
                finally:
                    if cluster:
                        try:
                            cluster.close()
                        except Exception as error:
                            record.update(status="FAILED_CLEANUP", cleanup_error=str(error))
                        record["processes"] = cluster.records
                        m.save(cluster.root / "run.json", record)
                    complete = len(output["runs"]) == len(args.workers) * len(args.workloads) * args.repeats
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
    p.add_argument("--workers", type=int, nargs="+", default=[1,2,4])
    p.add_argument("--workloads", choices=("true", "sleep", "binary"), nargs="+", default=["true","sleep","binary"])
    p.add_argument("--tasks", type=int, default=20)
    p.add_argument("--repeats", type=int, default=1)
    p.add_argument("--clients", type=int, default=8)
    p.add_argument("--observers", type=int, default=2)
    p.add_argument("--bytes", type=int, default=32768)
    p.add_argument("--poll-ms", type=int, default=10)
    p.add_argument("--resource-ms", type=int, default=50)
    p.add_argument("--timeout", type=float, default=120)
    raise SystemExit(run(p.parse_args()))
