#!/usr/bin/env python3
"""Independent strict/sanitizer builds. Incompatible runtimes exit 2, never PASS."""
import argparse
import json
import os
import platform
from pathlib import Path
import subprocess
import sys
import time


def run(args):
    if args.fixed_address_layout and args.mode != "tsan":
        raise RuntimeError("--fixed-address-layout is only for an isolated TSan build")
    source, build = args.source.resolve(strict=True), args.build.absolute()
    if build == source or (args.mode != "debug" and (build / "BUILD_COMMANDS.md").exists()):
        raise RuntimeError("use an independent out-of-source instrumentation tree")
    if (build / "validation.json").exists() and json.loads((build / "validation.json").read_text())["mode"] != args.mode:
        raise RuntimeError("do not mix sanitizer modes in one build tree")
    build.mkdir(parents=True, exist_ok=True)
    mode = {"debug": "none", "asan": "address", "tsan": "thread"}[args.mode]
    env = os.environ.copy()
    if args.mode == "asan":
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1:abort_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    if args.mode == "tsan":
        env["TSAN_OPTIONS"] = "halt_on_error=1"
    evidence = dict(mode=args.mode, source=str(source), build=str(build), status="RUNNING", commands=[],
                    options={k: env[k] for k in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS") if k in env})
    def save():
        (build / "validation.json").write_text(json.dumps(evidence, indent=2) + "\n")
    def execute(command, name):
        begin = time.monotonic()
        path = build / (name + ".log")
        with path.open("w") as log:
            result = subprocess.run(list(map(str, command)), env=env, stdout=log, stderr=subprocess.STDOUT)
        evidence["commands"].append(dict(argv=list(map(str, command)), exit_code=result.returncode,
                                          seconds=time.monotonic() - begin, log=str(path)))
        save()
        return result.returncode, path.read_text(errors="replace")
    save()
    for command, name in [(["cmake", "-S", source, "-B", build, "-DCMAKE_BUILD_TYPE=Debug", "-DBUILD_TESTING=ON",
                           "-DTS_SANITIZER=" + mode, "-DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST"], "configure"),
                          (["cmake", "--build", build, "-j" + str(args.jobs)], "build")]:
        code, _ = execute(command, name)
        if code:
            evidence["status"] = "FAILED_BUILD"; save(); return 1
    prefix = ["setarch", platform.machine(), "-R"] if args.fixed_address_layout else []
    if prefix:
        evidence["runtime_workaround"] = "explicit process-local fixed address layout via setarch -R"
    code, text = execute(prefix + [build / "ts_sanitizer_probe"], "runtime-probe")
    if code and not prefix and args.mode == "tsan" and "PROBE_MAIN_ENTERED" not in text and "unexpected memory mapping" in text:
        # Per-process only: no sysctl/global ASLR/security setting is changed.
        prefix = ["setarch", platform.machine(), "-R"]
        evidence["runtime_workaround"] = "process-local fixed address layout via setarch -R"
        code, text = execute(prefix + [build / "ts_sanitizer_probe"], "runtime-probe-fixed-layout")
    if code:
        runtime_failure = "PROBE_MAIN_ENTERED" not in text and any(t in text for t in
            ("unexpected memory mapping", "AddressSanitizer:DEADLYSIGNAL", "CHECK failed", "personality"))
        evidence.update(status="ENVIRONMENT_LIMITED" if runtime_failure else "FAILED_PROBE",
                        attempted_probe=1, entered_main="PROBE_MAIN_ENTERED" in text, functional_tests_executed=0)
        save(); return 2 if runtime_failure else 1
    if args.mode == "tsan":
        suites = [
            ("ts_milestone1_tests", "BoundedQueue.*"),
            ("ts_milestone2_tests", "*Channel*:*Wakeup*"),
            ("ts_milestone5_tests", "StoreTest.CountReserve*:StoreTest.CompletionByte*:StoreTest.SlowSqlite*:StoreTest.Eventfd*:StorageStress.*:StorageChannel.Notification*"),
            ("ts_milestone8_tests", "LogWriter.*")]
        for executable, selection in suites:
            code, text = execute(prefix + [build / executable, "--gtest_filter=" + selection], executable)
            if code or "Running 0 tests" in text:
                evidence["status"] = "FAILED_TESTS"; save(); return 1
    else:
        code, _ = execute(["ctest", "--test-dir", build, "--output-on-failure"], "ctest")
        if code:
            evidence["status"] = "FAILED_TESTS"; save(); return 1
        if args.repeat:
            code, _ = execute(["ctest", "--test-dir", build, "-L", "m10", "--output-on-failure", "--repeat", "until-fail:20"], "repeat")
            if code:
                evidence["status"] = "FAILED_REPEAT"; save(); return 1
    evidence["status"] = "PASS"; save(); return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("debug", "asan", "tsan"), required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--repeat", action="store_true")
    parser.add_argument("--fixed-address-layout", action="store_true",
                        help="TSan only: run every instrumented process under setarch -R")
    try:
        raise SystemExit(run(parser.parse_args()))
    except (Exception, KeyboardInterrupt) as error:
        print(f"run_validation: {error}", file=sys.stderr)
        raise SystemExit(1)
