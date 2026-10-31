"""Opt-in TSan execution of unchanged real-application acceptance cases."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import sqlite3
import subprocess
import time
import unittest

import m7_loopback as m7
from m8_loopback import M8Loopback
from m9_loopback import M9Loopback


CASES = (
    (M8Loopback, "test_empty_real_barrier_terminal_release_and_restart"),
    (M8Loopback, "test_large_binary_multiple_replenishments_and_paged_cli_are_exact"),
    (M9Loopback, "test_retry_can_succeed_on_another_worker_preserving_exact_old_log_history"),
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def runtime_thread_overhead(build, artifacts, fixed_layout):
    cache = (build / "CMakeCache.txt").read_text()
    compiler = re.search(r"^CMAKE_CXX_COMPILER:(?:FILEPATH|STRING)=(.*)$", cache, re.M).group(1)
    source = Path(__file__).resolve().parents[1] / "helpers/tsan_thread_probe.cpp"
    native = artifacts / "native-thread-probe"
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Wconversion",
               "-Wsign-conversion", "-Wshadow", "-Werror", "-pthread", str(source), "-o", str(native)]
    compilation = subprocess.run(command, capture_output=True, text=True)
    require(compilation.returncode == 0, compilation.stderr)
    observations = dict(native_compile=dict(argv=command, exit_code=compilation.returncode), probes=[])
    for label, executable in (("native", native), ("tsan", build / "ts_tsan_thread_probe")):
        argv = (["setarch", os.uname().machine, "-R"] if fixed_layout else []) + [str(executable)]
        with (artifacts / (label + "-thread-probe.log")).open("wb") as log:
            process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
            try:
                require(select.select([process.stdout], [], [], 5)[0], "thread probe did not enter main")
                require(process.stdout.readline().strip() == b"THREAD_PROBE_MAIN_ENTERED", "thread probe failed")
                counts = []
                for _ in range(5):
                    counts.append(len(list(Path(f"/proc/{process.pid}/task").iterdir())))
                    time.sleep(0.05)  # external diagnostic probe, not an owner EventLoop
                process.stdin.write(b"R"); process.stdin.flush()
                code = process.wait(timeout=5)
                require(code == 0, "thread probe reported a failure")
                observations["probes"].append(dict(kind=label, argv=argv, exit_code=code, thread_counts=counts))
            finally:
                process.stdin.close(); process.stdout.close()
                if process.poll() is None:
                    process.terminate(); process.wait(timeout=5)
    (artifacts / "thread-calibration.json").write_text(json.dumps(observations, indent=2) + "\n")
    require(observations["probes"][0]["thread_counts"] == [2] * 5, "native probe has unexpected threads")
    require(observations["probes"][1]["thread_counts"] == [3] * 5, "TSan overhead not established as exactly one thread")
    observations["runtime_threads"] = 1
    return observations


class RetainedDirectory:
    def __init__(self, original, destination, observation):
        self.original, self.destination, self.observation = original, destination, observation
        self.name = original.name

    def cleanup(self):
        try:
            # Harness calls this after stopping/reaping owners and closing logs.
            shutil.copytree(self.name, self.destination)
            db = self.destination / "data/metadata.sqlite3"
            if db.exists():
                with sqlite3.connect(f"file:{db}?mode=ro", uri=True) as reader:
                    self.observation["tasks"] = reader.execute(
                        "SELECT task_id,state FROM tasks ORDER BY created_at,task_id").fetchall()
                    self.observation["attempts"] = reader.execute(
                        "SELECT attempt_id,worker_id,state,stdout_bytes,stderr_bytes,logs_complete "
                        "FROM attempts ORDER BY task_id,attempt_no").fetchall()
        finally:
            self.original.cleanup()  # only the original mkdtemp-owned test leaf


class ObservedApplications:
    def setUp(self):
        super().setUp()
        self.temp = RetainedDirectory(self.temp, self.artifacts, self.observation)

    def tearDown(self):
        self.observation["threads_before_cleanup"] = []
        for process in self.processes:
            try:
                threads = {p.name: (p / "comm").read_text().strip()
                           for p in Path(f"/proc/{process.pid}/task").iterdir()}
                self.observation["threads_before_cleanup"].append(dict(pid=process.pid, threads=threads))
            except (FileNotFoundError, ProcessLookupError):
                pass
        super().tearDown()

    def spawn(self, name, *args, **kwargs):
        process = super().spawn(name, *args, **kwargs)
        self.observation["processes"].append(dict(pid=process.pid, name=name,
            argv=[str(m7.BIN / name), *map(str, args)], log=process.log_path.name))
        return process

    def observe(self, process, name):
        expected = (m7.BIN / name).resolve(strict=True)
        actual = Path(f"/proc/{process.pid}/exe").resolve(strict=True)
        require(actual == expected, "application executable differs from instrumented binary")
        maps = Path(f"/proc/{process.pid}/maps").read_text()
        modules = sorted({line.split()[-1] for line in maps.splitlines() if "libtsan.so" in line})
        require(modules, "actual application has no mapped TSan runtime")
        personality = int(Path(f"/proc/{process.pid}/personality").read_text().strip(), 16)
        if self.fixed_layout:
            require(personality & 0x40000, "setarch -R did not reach the application")
        self.observation["live_applications"].append(dict(pid=process.pid, name=name,
            executable=str(actual), tsan_modules=modules, personality=hex(personality),
            threads=len(list(Path(f"/proc/{process.pid}/task").iterdir()))))

    def controller(self, *args, **kwargs):
        process = super().controller(*args, **kwargs)
        self.observe(process, "controller")
        return process

    def worker(self, *args, **kwargs):
        process = super().worker(*args, **kwargs)
        self.observe(process, "worker")
        return process

    def submit(self, *args):
        task = super().submit(*args)
        self.observation["submissions"].append(dict(task_id=task, argv=list(map(str, args))))
        return task


def run(args):
    os.umask(0o077)
    m7.BIN = args.bin.resolve(strict=True)
    args.artifacts.mkdir(parents=True, exist_ok=False)
    cache = (m7.BIN / "CMakeCache.txt").read_text()
    require("TS_SANITIZER:STRING=thread" in cache, "TSan build required")
    evidence = dict(status="RUNNING", bin=str(m7.BIN), fixed_layout=args.fixed_address_layout,
                    options=dict(TSAN_OPTIONS=os.environ.get("TSAN_OPTIONS", "")), binaries={}, cases=[])
    def save():
        (args.artifacts / "applications.json").write_text(json.dumps(evidence, indent=2) + "\n")
    save()
    try:
        evidence["thread_calibration"] = runtime_thread_overhead(m7.BIN, args.artifacts, args.fixed_address_layout)
    except Exception as error:
        evidence.update(status="FAILED_CALIBRATION", error=str(error))
        save()
        raise
    for name in ("controller", "worker", "taskctl", "ts_log_workload", "ts_runtime_test_helper"):
        path = m7.BIN / name
        symbols = subprocess.check_output(["nm", "-u", str(path)], text=True)
        require("__tsan_func_entry" in symbols and "__tsan_read" in symbols,
                "binary lacks project TSan instrumentation: " + name)
        flags = (m7.BIN / "CMakeFiles" / (name + ".dir") / "flags.make").read_text()
        require("-fsanitize=thread" in flags and "-Werror" in flags, "missing strict instrumented flags")
        evidence["binaries"][name] = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            tsan_symbols=[line.strip() for line in symbols.splitlines() if "__tsan_" in line], flags=flags)
    for base, method in CASES:
        observation = dict(case=base.__name__ + "." + method, processes=[], live_applications=[], submissions=[])
        evidence["cases"].append(observation)
        selected = type("Instrumented" + base.__name__, (ObservedApplications, base), {})
        test = selected(method)
        test.expected_controller_threads = 3 + evidence["thread_calibration"]["runtime_threads"]
        test.observation, test.fixed_layout = observation, args.fixed_address_layout
        test.artifacts = args.artifacts / observation["case"]
        begin = time.monotonic()
        with (args.artifacts / (observation["case"] + ".log")).open("w") as log:
            outcome = unittest.TextTestRunner(stream=log, verbosity=2).run(unittest.TestSuite([test]))
        observation.update(seconds=time.monotonic() - begin, successful=outcome.wasSuccessful(),
            tests_run=outcome.testsRun, skipped=[reason for _, reason in outcome.skipped],
            failures=outcome.failures, errors=outcome.errors)
        # unittest failure entries contain TestCase objects; retain text, not live owners.
        for key in ("failures", "errors"):
            observation[key] = [text for _, text in observation[key]]
        for record, process in zip(observation["processes"], getattr(test, "processes", [])):
            record["exit_code"] = process.returncode
        diagnostics = []
        logs = sorted(test.artifacts.rglob("*.log")) + [args.artifacts / (observation["case"] + ".log")]
        for path in logs:
            text = path.read_text(errors="replace")
            if re.search(r"(?:WARNING|FATAL|SUMMARY): ThreadSanitizer", text):
                diagnostics.append(dict(file=str(path), text=text))
        observation["sanitizer_diagnostics"] = diagnostics
        names = {record["name"] for record in observation["live_applications"]}
        passed = outcome.wasSuccessful() and outcome.testsRun == 1 and not outcome.skipped and not diagnostics and {"controller", "worker"} <= names and all(
            p["exit_code"] == 0 for p in observation["processes"])
        observation["status"] = "PASS" if passed else "FAILED"
        if not passed:
            # Only explicit mapping diagnostics establish this known runtime limit.
            mapping = any("FATAL: ThreadSanitizer: unexpected memory mapping" in d["text"] for d in diagnostics)
            warning = any(re.search(r"(?:WARNING|SUMMARY): ThreadSanitizer", d["text"]) for d in diagnostics)
            evidence["status"] = "ENVIRONMENT_LIMITED" if mapping and not warning else "FAILED"
            save()
            return 2 if evidence["status"] == "ENVIRONMENT_LIMITED" else 1
        save()
    evidence["status"] = "PASS"
    save()
    print(json.dumps(dict(status="PASS", cases=len(CASES), artifacts=str(args.artifacts))))
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--fixed-address-layout", action="store_true")
    raise SystemExit(run(parser.parse_args()))
