"""Post-v1 external measurement support; no application code or user data deletion."""
import base64
from contextlib import contextmanager, closing
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import struct
import subprocess
import sys
import threading
import time
import uuid

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
import cluster_support as cluster
import benchmark as accepted_benchmark

require = cluster.require
now = time.monotonic_ns
HEADER = struct.Struct("!IHHQI")
MAX_FRAME = 4194304


def save(path, value):
    cluster.atomic_json(Path(path), value)


def fresh_root(path):
    path = Path(path).absolute()
    require(not path.exists() and not path.is_symlink(), "use a new, nonexistent artifact root")
    path.mkdir(parents=True, mode=0o700)
    require(subprocess.check_output(["findmnt", "-T", str(path), "-n", "-o", "FSTYPE"], text=True).strip() == "ext4",
            "formal data roots must be ext4-backed")
    save(path / "measurement-owner.json", dict(uid=os.geteuid(), token=str(uuid.uuid4())))
    return path


def environment(bin_dir, root):
    bin_dir = Path(bin_dir).resolve(strict=True)
    cache = (bin_dir / "CMakeCache.txt").read_text()
    require("CMAKE_BUILD_TYPE:STRING=Release" in cache and "TS_SANITIZER:STRING=none" in cache,
            "use actual non-sanitized Release build-tree binaries")
    value = accepted_benchmark.environment(root)
    value.update(build_type="Release", python=sys.version.split()[0],
                 clock=time.get_clock_info("monotonic").implementation,
                 clock_resolution_seconds=time.get_clock_info("monotonic").resolution,
                 source_commit=subprocess.check_output(["git", "-C", str(SCRIPTS.parent), "rev-parse", "HEAD"], text=True).strip(),
                 mount=subprocess.check_output(["findmnt", "-T", str(root), "-n", "-o", "TARGET,FSTYPE,OPTIONS"], text=True).strip(),
                 binaries={name: hashlib.sha256((bin_dir / name).read_bytes()).hexdigest() for name in ("controller", "worker", "taskctl")},
                 compiler_cache=[line for line in cache.splitlines() if line.startswith("CMAKE_CXX_COMPILER:")],
                 harness_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(Path(__file__).parent.glob("*.py"))},
                 compile_flags=(bin_dir / "CMakeFiles/controller.dir/flags.make").read_text())
    return value


@contextmanager
def defer_interrupts(raise_after=True):
    # Defer Python exceptions during spawn+registry publication, without changing
    # the signal mask inherited by the application's exec. Main thread only.
    pending = []
    signals = (signal.SIGINT, signal.SIGTERM)
    previous = {sig: signal.getsignal(sig) for sig in signals}
    try:
        for sig in signals:
            signal.signal(sig, lambda number, _: pending.append(number))
        yield
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        if pending and raise_after:
            raise KeyboardInterrupt("interrupted during process bookkeeping")


def install_interrupt_handler():
    def interrupted(*_):
        raise KeyboardInterrupt("measurement interrupted")
    signal.signal(signal.SIGTERM, interrupted)


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise EOFError("connection closed during frame")
        data.extend(part)
    return bytes(data)


class Client:
    """One independent persistent TCP client; one in-flight correlated request."""
    def __init__(self, port):
        start = now()
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.sock.settimeout(10)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.connection = dict(start_ns=start, connected_ns=now(), local_port=self.sock.getsockname()[1])
        self.request_id = 0

    def close(self):
        self.sock.close()

    def request(self, kind, payload):
        start = now()
        self.request_id += 1
        data = json.dumps(payload, separators=(",", ":")).encode()
        require(len(data) <= MAX_FRAME, "oversized client payload")
        self.sock.sendall(HEADER.pack(0x54534348, 1, kind, self.request_id, len(data)) + data)
        sent = now()
        magic, version, response, request, length = HEADER.unpack(exact(self.sock, HEADER.size))
        require(magic == 0x54534348 and version == 1 and length <= MAX_FRAME, "invalid response framing")
        body = exact(self.sock, length)
        received = now()
        value = json.loads(body)
        require(request == self.request_id, "response correlation mismatch")
        if response == 1:
            raise RuntimeError("explicit protocol error: " + json.dumps(value))
        require(response == kind + 1, "wrong response type")
        return value, dict(request_start_ns=start, request_sent_ns=sent, response_received_ns=received, response_ns=now())

    def get(self, task):
        return self.request(0x0103, dict(task_id=task))

    def workers(self):
        return self.request(0x010D, {})

    def logs(self, task, stream):
        offset, end, chunks = 0, None, []
        while True:
            reply, _ = self.request(0x0107, dict(task_id=task, attempt_no=1, stream=stream,
                                                offset=offset, limit_bytes=262144, snapshot_end=end))
            if end is None:
                end = reply["snapshot_end"]
            data = base64.b64decode(reply["data_base64"], validate=True)
            require(reply["task_id"] == task and reply["stream"] == stream and reply["attempt_no"] == 1 and
                    reply["offset"] == offset and reply["snapshot_end"] == end and
                    reply["next_offset"] == offset + len(data) and reply["logs_complete"], "invalid complete snapshot")
            chunks.append(data)
            offset = reply["next_offset"]
            if reply["eof_snapshot"]:
                require(offset == end, "snapshot ended early")
                return b"".join(chunks)
            require(data, "snapshot did not advance")


class ManagedCluster:
    def __init__(self, bin_dir, root, heartbeat=None):
        self.bin = Path(bin_dir).resolve(strict=True)
        self.root = fresh_root(root)
        self.records, self.handles, self.cleanup_errors = [], {}, []
        client_port, worker_port = cluster.free_port(), cluster.free_port()
        while client_port == worker_port:
            worker_port = cluster.free_port()
        self.config = dict(client_listen=dict(address="127.0.0.1", port=client_port),
            worker_listen=dict(address="127.0.0.1", port=worker_port), data_dir=str(self.root / "data"),
            heartbeat=heartbeat or dict(interval_ms=3000, offline_timeout_ms=10000),
            dispatch=dict(ack_timeout_ms=5000, start_timeout_ms=10000), termination_grace_ms=200,
            protocol=dict(max_payload_bytes=4194304),
            queues=dict(persistence_commands=4096, log_bytes=67108864, log_commands=4096, log_control_reserve=128,
                initial_session_log_window_bytes=1048576, initial_session_log_window_chunks=64),
            logging=dict(level="info", destination="stderr"))
        save(self.root / "controller.json", self.config)
        self.worker_configs = []

    def checkpoint(self):
        save(self.root / "processes.json", dict(processes=self.records, cleanup_errors=self.cleanup_errors))

    def spawn(self, name, config):
        with defer_interrupts():
            log = self.root / f"{name}-{len(self.records)}.system.log"
            with log.open("xb") as handle:
                process = subprocess.Popen([str(self.bin / name), "--config", str(config)], stdin=subprocess.DEVNULL,
                    stdout=handle, stderr=handle, start_new_session=True)
            # Keep the Popen owner before any fallible inspection/checkpoint.
            self.handles[process.pid] = process
            record = dict(pid=process.pid, role=name, config=str(config), identity=None, log=str(log), exit_code=None)
            self.records.append(record)
            self.checkpoint()
            record["identity"] = cluster.eventually(lambda: cluster.proc_identity(process.pid) or
                (process.poll() is not None and require(False, "application exited during startup")), seconds=3)
            require(record["identity"]["executable"] == str((self.bin / name).resolve()), "unexpected executable")
            self.checkpoint()
        return record

    def start_controller(self):
        record = self.spawn("controller", self.root / "controller.json")
        def ready():
            require(cluster.owned(record), "Controller exited before READY")
            return "READY client=" in Path(record["log"]).read_text()
        cluster.eventually(ready)
        return record

    def start_worker(self, worker_id, endpoint=None):
        path = self.root / (worker_id + ".json")
        if not path.exists():
            value = dict(worker_id=worker_id, controller=endpoint or self.config["worker_listen"],
                capacity=dict(cpu_slots=2, memory_mb=512), max_concurrent_tasks=2,
                reconnect=dict(initial_backoff_ms=100, max_backoff_ms=500), logging=dict(level="info", destination="stderr"))
            save(path, value)
            self.worker_configs.append(value)
        return self.spawn("worker", path)

    def client(self):
        return Client(self.config["client_listen"]["port"])

    def healthy(self, count):
        with closing(self.client()) as client:
            return cluster.eventually(lambda: (value if len(value := client.workers()[0]["workers"]) == count and
                all(w["state"] == "HEALTHY" for w in value) else None), seconds=15)

    def signal(self, record, sig):
        require(record["identity"] and cluster.owned(record), "cannot signal an unverified owner")
        os.kill(record["pid"], sig)

    def stop_one(self, record):
        process = self.handles[record["pid"]]
        children = []
        if process.poll() is None:
            child_path = Path(f'/proc/{record["pid"]}/task/{record["pid"]}/children')
            children = child_path.read_text().split()
            record["children_before_stop"] = children
            self.signal(record, signal.SIGTERM)
        record["exit_code"] = process.wait(timeout=35)
        require(record["exit_code"] == 0, "application exited nonzero: " + str(record))
        for child in children:
            require(not Path(f"/proc/{child}").exists(), "unreaped direct workload child")
            try:
                os.killpg(int(child), 0)
            except ProcessLookupError:
                continue
            raise RuntimeError("surviving workload process group")
        try:
            os.killpg(record["pid"], 0)
        except ProcessLookupError:
            return
        raise RuntimeError("surviving application process group")

    def close(self):
        with defer_interrupts(raise_after=False):
            # Verify every live record before sending any signal to a sibling.
            try:
                for record in self.records:
                    if self.handles[record["pid"]].poll() is None:
                        require(record["identity"] and cluster.owned(record), "unverified live process retained")
            except Exception as error:
                self.cleanup_errors.append(str(error)); self.checkpoint(); raise
            for record in reversed(self.records):
                try:
                    self.stop_one(record)
                except Exception as error:
                    self.cleanup_errors.append(str(error))
            self.checkpoint()
            require(not self.cleanup_errors, "cleanup failed: " + repr(self.cleanup_errors))


def snapshot(records):
    observed = []
    for owner in records:
        require(cluster.owned(owner), "resource owner disappeared or changed")
        path = Path(f'/proc/{owner["pid"]}')
        stat = (path / "stat").read_text().rsplit(")", 1)[1].split()
        values = dict(line.split(":", 1) for line in (path / "status").read_text().splitlines() if ":" in line)
        observed.append(dict(pid=owner["pid"], role=owner["role"], cpu_ticks=int(stat[11])+int(stat[12]),
            rss_kib=int(values["VmRSS"].split()[0]), hwm_kib=int(values["VmHWM"].split()[0]),
            fds=len(list((path / "fd").iterdir())), threads=int(values["Threads"])))
    return dict(at_ns=now(), owners=observed)


def db_rows(cluster_instance):
    path = cluster_instance.root / "data/metadata.sqlite3"
    with closing(sqlite3.connect(f"file:{path}?mode=ro", uri=True)) as reader:
        reader.row_factory = sqlite3.Row
        tasks = [dict(row) for row in reader.execute("SELECT * FROM tasks ORDER BY task_id")]
        attempts = [dict(row) for row in reader.execute("SELECT * FROM attempts ORDER BY task_id,attempt_no")]
    return dict(observed_ns=now(), tasks=tasks, attempts=attempts)


def idle_workers(value, expected):
    return len(value) == expected and all(w["state"] == "HEALTHY" and w["running_attempts"] == 0 and
        w["reserved"] == dict(cpu_slots=0, memory_mb=0) for w in value)


def no_worker_children(cluster_instance):
    for record in cluster_instance.records:
        if record["role"] == "worker" and cluster_instance.handles[record["pid"]].poll() is None:
            require(cluster.owned(record), "unverified Worker at cleanup check")
            require(Path(f'/proc/{record["pid"]}/task/{record["pid"]}/children').read_text().strip() == "",
                    "unexpected live/unreaped child after terminal visibility")


def quantiles(values):
    return dict(p50=accepted_benchmark.percentile(values, .5), p95=accepted_benchmark.percentile(values, .95),
                minimum=min(values), maximum=max(values)) if values else None


def pilot_gate(path, mode, expected, binary_hashes):
    pilot = json.loads(Path(path).read_text())
    require(pilot["mode"] == "pilot" and pilot["status"] == "PASS" and pilot["kind"] == mode,
            "formal runs require a successful matching pilot")
    require(pilot["environment"]["binaries"] == binary_hashes, "pilot used different binaries")
    require(pilot["environment"]["harness_sha256"] == {
        p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(Path(__file__).parent.glob("*.py"))},
        "harness changed after pilot; rerun a pilot")
    parameters = pilot["parameters"]
    expected_runs = parameters["repeats"] * (len(parameters["workers"]) * len(parameters["workloads"])
        if mode == "concurrent" else len(parameters["scenarios"]))
    require(len(pilot["runs"]) == expected_runs and all(r["status"] == "PASS" for r in pilot["runs"]), "incomplete pilot")
    for key, value in expected.items():
        require(pilot["parameters"][key] == value, "pilot parameter mismatch: " + key)
