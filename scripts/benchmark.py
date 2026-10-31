#!/usr/bin/env python3
"""Bounded durable-terminal benchmark, not submission-ACK throughput."""
import argparse
import json
import os
from pathlib import Path
import platform
import statistics
import signal
import sqlite3
import subprocess
import sys
import time
import cluster_support as cluster


def resources(state):
    values = []
    for owner in state["processes"]:
        cluster.require(cluster.owned(owner), "service exited during benchmark")
        p = Path(f'/proc/{owner["pid"]}')
        stat = (p / "stat").read_text().rsplit(")", 1)[1].split()
        status = dict(line.split(":", 1) for line in (p / "status").read_text().splitlines() if ":" in line)
        values.append(dict(role=owner["role"], cpu_ticks=int(stat[11]) + int(stat[12]),
            peak_rss_kib=int(status["VmHWM"].split()[0]), fds=len(list((p / "fd").iterdir())),
            threads=int(status["Threads"])))
    return values


def environment(root):
    cpu = Path("/proc/cpuinfo").read_text()
    model = next((line.split(":", 1)[1].strip() for line in cpu.splitlines() if line.startswith("model name")), "unknown")
    return dict(kernel=platform.uname()._replace(node="<hostname omitted>")._asdict(), cpu_model=model,
        logical_cpus=os.cpu_count(), mem_total=Path("/proc/meminfo").read_text().splitlines()[0],
        filesystem=subprocess.check_output(["findmnt", "-T", str(root), "-n", "-o", "FSTYPE,OPTIONS"], text=True).strip(),
        compiler=subprocess.check_output(["c++", "--version"], text=True).splitlines()[0],
        cmake=subprocess.check_output(["cmake", "--version"], text=True).splitlines()[0],
        sqlite=subprocess.check_output(["pkg-config", "--modversion", "sqlite3"], text=True).strip(),
        clock_ticks=os.sysconf("SC_CLK_TCK"))


def percentile(values, fraction):
    return sorted(values)[max(0, min(len(values) - 1, int((len(values) - 1) * fraction)))]


def measured(state, workload, tasks, size, warmup, workload_path):
    def submit():
        command = ["/bin/true"] if workload == "empty" else [sys.executable, str(workload_path), str(size)]
        return cluster.cli(state, "submit", *command)[0]["task_id"]
    for _ in range(warmup):
        cluster.terminal(state, submit())
    before = resources(state)
    begin = time.perf_counter()
    samples = []
    for _ in range(tasks):
        issued = time.perf_counter()
        task = submit()
        samples.append(dict(task_id=task, issued=issued - begin, submit_seconds=time.perf_counter() - issued))
    for sample in samples:
        view = cluster.terminal(state, sample["task_id"])
        attempt = view["attempts"][-1]
        cluster.require(len(view["attempts"]) == 1 and attempt["logs_complete"], "missing durable Attempt/log barrier")
        cluster.require(attempt["state"] == "SUCCEEDED" and not attempt["finalization_pending"])
        sample["terminal_observed_seconds"] = time.perf_counter() - begin
        sample["end_to_end_seconds"] = sample["terminal_observed_seconds"] - sample["issued"]
        sample["stdout_bytes"], sample["stderr_bytes"] = attempt["stdout_bytes"], attempt["stderr_bytes"]
    terminal_elapsed = time.perf_counter() - begin
    expected_size = 0 if workload == "empty" else size
    pattern = bytes(i % 251 for i in range(expected_size))
    error_pattern = bytes(i ^ 0xa5 for i in pattern)
    for sample in samples:
        cluster.require((sample["stdout_bytes"], sample["stderr_bytes"]) == (expected_size, expected_size), "tail counters mismatch")
        for stream, expected in (("--stdout", pattern), ("--stderr", error_pattern)):
            raw = cluster.cli(state, "logs", sample["task_id"], "--attempt", "1", stream, binary=True)
            cluster.require(raw == expected, "downloaded log mismatch")
        data = Path(state["root"]) / "data"
        with sqlite3.connect(f"file:{data / 'metadata.sqlite3'}?mode=ro", uri=True) as db:
            row = db.execute("SELECT stdout_path,stderr_path,stdout_bytes,stderr_bytes,logs_complete FROM attempts WHERE task_id=?",
                             (sample["task_id"],)).fetchone()
        cluster.require(row and tuple(row[2:]) == (expected_size, expected_size, 1), "persisted tail mismatch")
        for relative, expected in zip(row[:2], (pattern, error_pattern)):
            path = data / relative
            cluster.require(path.resolve().is_relative_to(data.resolve()) and not path.is_symlink(), "unexpected log path")
            cluster.require(path.read_bytes() == expected, "stored file differs from downloaded tail")
    workers_after = cluster.cli(state, "workers")[0]["workers"]
    cluster.require(len(workers_after) == len(state["processes"]) - 1, "missing Worker after measurement")
    cluster.require(all(w["state"] == "HEALTHY" and w["running_attempts"] == 0 and
        w["reserved"] == dict(cpu_slots=0, memory_mb=0) for w in workers_after),
        "terminal batch retained a reservation or lost a Worker")
    after = resources(state)
    return dict(workload=workload, tasks=tasks, successes=len(samples), failures=0, bytes_per_stream=expected_size,
        warmup=warmup, elapsed_terminal_seconds=terminal_elapsed, elapsed_including_verification_seconds=time.perf_counter() - begin,
        terminal_tasks_per_second=tasks / terminal_elapsed, samples=samples, workers_after=workers_after,
        resources_before=before, resources_after=after,
        cpu_ticks_during_measurement=[end["cpu_ticks"] - start["cpu_ticks"] for start, end in zip(before, after)])


def run(args):
    os.umask(0o077)
    cluster.require(1 <= args.tasks <= 200 and 1 <= args.repeats <= 20 and 0 <= args.bytes <= 1048576 and 0 <= args.warmup <= 20,
                    "benchmark bounds: 1..200 tasks, 1..20 repeats, 0..1MiB per stream")
    cluster.require(not args.output.exists(), "refusing to overwrite benchmark output")
    root = args.root.absolute()
    root.mkdir(mode=0o700, parents=True, exist_ok=False)
    output = dict(environment=environment(root), build_type=args.build_type,
        measurement="sequential taskctl processes included; monotonic request issuance to first observed durable terminal; downloads validated afterward",
        topology="one host, loopback, independent Workers share physical resources; advertised accounting is not enforcement",
        status="RUNNING", runs=[], summaries=[])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    def save():
        cluster.atomic_json(args.output, output)
    save()
    workload_path = Path(__file__).resolve().parent / "workloads" / "binary_output.py"
    if not workload_path.exists():  # installed tools are deliberately flat
        workload_path = Path(__file__).resolve().parent / "binary_output.py"
    for workers in args.workers:
        for workload in ("empty", "binary"):
            group = []
            for repetition in range(args.repeats):
                state = None
                try:
                    state = cluster.start(args.bin_dir, root / f"w{workers}-{workload}-{repetition}", workers)
                    record = measured(state, workload, args.tasks, args.bytes, args.warmup, workload_path)
                    record.update(workers=workers, repetition=repetition, worker_capacity=dict(cpu_slots=2, memory_mb=512),
                        max_concurrent_tasks=2, controller_config=json.loads((Path(state["root"]) / "controller.json").read_text()))
                    record["controller_config"]["data_dir"] = "<isolated Linux-local data directory>"
                    group.append(record); output["runs"].append(record); save()
                except BaseException as error:
                    output["status"] = "FAILED"
                    output["runs"].append(dict(workers=workers, workload=workload, repetition=repetition,
                        phase="measurement" if state else "startup", error=str(error)))
                    save(); raise
                finally:
                    if state:
                        try:
                            cluster.stop(state["root"])
                        except BaseException as error:
                            output.update(status="FAILED_CLEANUP", cleanup_error=str(error))
                            save(); raise
            rates = [r["terminal_tasks_per_second"] for r in group]
            latency = [s["end_to_end_seconds"] for r in group for s in r["samples"]]
            output["summaries"].append(dict(workers=workers, workload=workload, throughput_median=statistics.median(rates),
                throughput_min=min(rates), throughput_max=max(rates), observed_latency_p50=percentile(latency, .5),
                observed_latency_p95=percentile(latency, .95)))
            save()
    output["status"] = "PASS"; save()
    return output


def main():
    def interrupted(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--bin-dir", type=Path, required=True)
    p.add_argument("--root", type=Path, required=True, help="new Linux-local artifact directory; never deleted")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--workers", type=int, nargs="+", default=[1, 2])
    p.add_argument("--tasks", type=int, default=20)
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--warmup", type=int, default=2)
    p.add_argument("--bytes", type=int, default=32768)
    p.add_argument("--build-type", default="Debug", help="record actual supplied build configuration")
    args = p.parse_args()
    result = run(args)
    print(json.dumps(result["summaries"], indent=2))


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        print(f"benchmark: {error}", file=sys.stderr)
        raise SystemExit(1)
