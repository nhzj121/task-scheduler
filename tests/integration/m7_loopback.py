"""M7 real-process acceptance. Standard-library test harness, not production RPC.

Production taskctl is used for user workflows. Raw TCP peers below deliberately
exercise malicious/fragmented input and inspect actual protocol ordering. SQLite
readers are independent connections; fault executable uses the real M5 SQL hook.
"""
import argparse
import base64
from contextlib import closing
import json
import os
from pathlib import Path
import select
import signal
import socket
import sqlite3
import struct
import subprocess
import tempfile
import time
import unittest
import uuid

BIN = None


def eventually(predicate, seconds=6):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(0.01)  # external harness only, never an application EventLoop
    raise AssertionError("bounded condition deadline")


def uid():
    return str(uuid.uuid4())


def frame(kind, payload, request=0):
    data = json.dumps(payload, separators=(",", ":")).encode()
    return struct.pack("!IHHQI", 0x54534348, 1, kind, request, len(data)) + data


def exact(sock, count):
    data = bytearray()
    while len(data) < count:
        piece = sock.recv(count - len(data))
        if not piece:
            raise EOFError("TCP peer closed")
        data.extend(piece)
    return bytes(data)


def receive(sock):
    magic, version, kind, request, size = struct.unpack("!IHHQI", exact(sock, 20))
    assert magic == 0x54534348 and version == 1 and size <= 4194304
    return kind, request, json.loads(exact(sock, size))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Peer:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.sock.settimeout(4)

    def close(self):
        self.sock.close()

    def request(self, kind, payload, request=1):
        self.sock.sendall(frame(kind, payload, request))
        response = receive(self.sock)
        assert response[1] == request, response
        return response

    def register(self, worker="raw-worker", session=None, cpu=2):
        self.worker = worker
        self.session = session or uid()
        return self.request(0x0201, dict(worker_id=worker, session_id=self.session,
            capacity=dict(cpu_slots=cpu, memory_mb=100), max_concurrent_tasks=cpu,
            worker_version="m7-test"))

    def heartbeat(self, sequence, session=None):
        self.sock.sendall(frame(0x0203, dict(worker_id=self.worker, session_id=session or self.session,
            sequence=sequence, running_attempt_ids=[])))


class Harness(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="task-scheduler-m7-")
        self.root = Path(self.temp.name)
        self.processes = []
        self.handles = []
        self.peers = []
        self.client_port, self.worker_port = free_port(), free_port()
        while self.client_port == self.worker_port:
            self.worker_port = free_port()
        self.config = dict(client_listen=dict(address="127.0.0.1", port=self.client_port),
            worker_listen=dict(address="127.0.0.1", port=self.worker_port), data_dir=str(self.root / "data"),
            heartbeat=dict(interval_ms=100, offline_timeout_ms=1500),
            dispatch=dict(ack_timeout_ms=1000, start_timeout_ms=1500), termination_grace_ms=50,
            logging=dict(level="info", destination="stderr"))
        self.config_path = self.write_json("controller.json", self.config)

    def write_json(self, name, data):
        path = self.root / name
        path.write_text(json.dumps(data))
        return str(path)

    def spawn(self, name, *args, pass_fds=()):
        log = self.root / (name + "-" + str(len(self.processes)) + ".log")
        handle = log.open("wb")
        self.handles.append(handle)
        proc = subprocess.Popen([str(BIN / name), *map(str, args)], stdout=handle, stderr=handle,
                                start_new_session=True, pass_fds=pass_fds)
        proc.log_path = log
        self.processes.append(proc)
        return proc

    def controller(self, mode=None):
        if mode:
            notify_read, notify_write = os.pipe()
            release_read, release_write = os.pipe()
            self.gate_read, self.gate_write = notify_read, release_write
            self.proc = self.spawn("ts_m7_fault_controller", self.config_path, mode, notify_write, release_read,
                                   pass_fds=(notify_write, release_read))
            os.close(notify_write)
            os.close(release_read)
        else:
            self.proc = self.spawn("controller", "--config", self.config_path)
        def ready():
            if self.proc.poll() is not None:
                raise AssertionError(self.proc.log_path.read_text())
            return "READY client=" in self.proc.log_path.read_text()
        eventually(ready)
        return self.proc

    def peer(self, worker=False):
        peer = Peer(self.worker_port if worker else self.client_port)
        self.peers.append(peer)
        return peer

    def cli(self, *args, code=0):
        proc = subprocess.run([str(BIN / "taskctl"), "--port", str(self.client_port), "--json", *args],
                              capture_output=True, text=True, timeout=8)
        self.assertEqual(proc.returncode, code, (proc.stdout, proc.stderr))
        return [json.loads(line) for line in proc.stdout.splitlines()]

    def workers(self):
        return self.cli("workers")[0]["workers"]

    def submit(self, *args):
        return self.cli("submit", *args)[0]["task_id"]

    def get(self, task):
        return self.cli("get", task)[0]

    def worker(self, worker, cpu=1):
        config = dict(worker_id=worker, controller=dict(address="127.0.0.1", port=self.worker_port),
                      capacity=dict(cpu_slots=cpu, memory_mb=100), max_concurrent_tasks=cpu,
                      logging=dict(level="info", destination="stderr"))
        path = self.write_json(worker + ".json", config)
        proc = self.spawn("worker", "--config", path)
        eventually(lambda: "REGISTERED worker=" in proc.log_path.read_text() or
                   (proc.poll() is not None and self.fail(proc.log_path.read_text())))
        return proc

    def stop(self, proc, expected=0):
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        result = proc.wait(timeout=8)
        if expected is not None:
            self.assertEqual(result, expected, proc.log_path.read_text())

    def db(self, sql, args=()):
        with closing(sqlite3.connect(self.root / "data" / "metadata.sqlite3", timeout=0.1)) as db:
            return db.execute(sql, args).fetchall()

    def held(self):
        self.assertTrue(select.select([self.gate_read], [], [], 5)[0], self.proc.log_path.read_text())
        self.assertEqual(os.read(self.gate_read, 1), b"G")

    def release(self):
        os.write(self.gate_write, b"R")

    def tearDown(self):
        # Release a test gate even if an assertion failed; never strand a SQL owner.
        if hasattr(self, "gate_write"):
            try:
                os.write(self.gate_write, b"R")
            except OSError:
                pass
            os.close(self.gate_write)
            os.close(self.gate_read)
        for peer in self.peers:
            peer.close()
        errors = []
        for proc in reversed(self.processes):
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=7)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=3)
                errors.append("forced cleanup: " + proc.log_path.read_text())
        for handle in self.handles:
            handle.close()
        # Session/process groups of these owners must be absent after wait/reap.
        for proc in self.processes:
            try:
                os.killpg(proc.pid, 0)
                errors.append("surviving owner process group: " + str(proc.pid))
            except ProcessLookupError:
                pass
        self.temp.cleanup()
        self.assertFalse(errors, errors)


class M7Loopback(Harness):
    def test_real_vertical_slice_two_workers_and_crash_recovery(self):
        self.controller()
        pending = self.submit("--cpu", "3", "/bin/true")
        a = self.worker("worker-a")
        b = self.worker("worker-b")
        self.assertEqual(len(self.workers()), 2)
        # M8 completes /bin/true durably. Keep real workloads running until the
        # crash so this retained M7 test still exercises ACTIVE -> LOST recovery.
        ids = [self.submit("/bin/sleep", "30") for _ in range(2)]
        for task in ids:
            eventually(lambda: self.get(task)["attempts"] and self.get(task)["attempts"][0]["state"] == "RUNNING")
            view = self.get(task)
            self.assertEqual(view["task"]["state"], "ACTIVE")
            self.assertEqual(view["attempts"][0]["state"], "RUNNING")
            self.assertFalse(view["attempts"][0]["logs_complete"])
            self.assertEqual(self.db("SELECT state,stdout_bytes,stderr_bytes,logs_complete FROM attempts WHERE task_id=?", (task,)),
                             [("RUNNING", 0, 0, 0)])
        workers = [self.get(task)["attempts"][0]["worker_id"] for task in ids]
        self.assertEqual(workers, ["worker-a", "worker-b"])
        self.assertTrue(all(w["running_attempts"] == 1 for w in self.workers()))
        # Crash, not graceful invalidation: actual M5 restart recovery must do LOST.
        self.proc.kill()
        self.proc.wait(timeout=3)
        self.stop(a, expected=None)
        self.stop(b, expected=None)
        self.controller()
        self.assertEqual(self.workers(), [])
        self.assertEqual(self.get(pending)["task"]["state"], "PENDING")
        for task in ids:
            view = self.get(task)
            self.assertEqual(view["task"]["state"], "FAILED")
            self.assertEqual(view["attempts"][0]["state"], "LOST")
            self.assertFalse(view["attempts"][0]["logs_complete"])
        self.stop(self.proc)

    def test_help_invalid_configuration_lock_and_clean_cycles(self):
        for name in ("controller", "worker", "taskctl"):
            result = subprocess.run([str(BIN / name), "--help"], cwd=self.root, capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 0)
        self.assertFalse((self.root / "data").exists())
        # Spaces are valid ASCII Worker IDs; previously this fixture exited only
        # because the Controller was absent. M9 reconnect makes that valid config
        # stay alive. Use an actually invalid domain capacity for this assertion.
        bad = self.write_json("bad.json", dict(worker_id="bad worker", capacity=dict(cpu_slots=0, memory_mb=0)))
        invalid = self.spawn("worker", "--config", bad)
        self.assertEqual(invalid.wait(timeout=3), 1)
        for _ in range(3):
            self.controller()
            second = self.spawn("controller", "--config", self.config_path)
            self.assertEqual(second.wait(timeout=3), 1)
            self.assertIn("lock", second.log_path.read_text().lower())
            self.assertEqual(self.workers(), [])
            self.stop(self.proc)
        for patch in (dict(data_dir="/"), dict(client_listen=dict(address="localhost", port=self.client_port)),
                      dict(client_listen=self.config["worker_listen"])):
            path = self.write_json("invalid.json", {**self.config, **patch})
            failed = self.spawn("controller", "--config", path)
            self.assertEqual(failed.wait(timeout=4), 1)

    def test_fragmentation_roles_request_correlation_and_fd_churn(self):
        self.controller()
        peer = self.peer()
        data = frame(0x010D, {}, 19)
        for byte in data:
            peer.sock.sendall(bytes([byte]))
        self.assertEqual(receive(peer.sock), (0x010E, 19, {"workers": []}))
        peer.sock.sendall(frame(0x010D, {}, 20) + frame(0x0105, {}, 21))
        replies = [receive(peer.sock), receive(peer.sock)]
        self.assertEqual({r[1] for r in replies}, {20, 21})
        unknown = peer.request(0x0103, dict(task_id=uid()), 22)
        self.assertEqual(unknown[2]["code"], "NOT_FOUND")
        for _ in range(40):
            peer = self.peer()
            peer.sock.sendall(frame(0x0101, dict(executable="/bin/true", args=[], resources=dict(cpu_slots=1, memory_mb=0)), 77))
            peer.close()  # asynchronous submit may commit after the originating fd is reused
            query = self.peer()
            self.assertEqual(query.request(0x010D, {}, 77)[0], 0x010E)
            query.close()
        for worker, data in ((False, frame(0x0201, {}, 1)), (True, frame(0x010D, {}, 1)),
                             (True, frame(0x0203, {}, 0)), (False, b"BAD!" + bytes(16)),
                             (False, struct.pack("!IHHQI", 0x54534348, 1, 0x010D, 1, 4194305))):
            bad = self.peer(worker)
            bad.sock.sendall(data)
            self.assertEqual(bad.sock.recv(1), b"")
        self.assertEqual(self.workers(), [])

    def test_durable_submit_gate_and_disconnected_client(self):
        self.controller("hold-submit")
        peer = self.peer()
        peer.sock.sendall(frame(0x0101, dict(executable="/bin/true", args=[]), 91))
        self.held()
        self.assertFalse(select.select([peer.sock], [], [], 0.15)[0])
        self.assertEqual(self.db("SELECT count(*) FROM tasks"), [(0,)])
        self.assertEqual(self.workers(), [])  # independent EventLoop request still serves
        peer.close()
        self.release()
        self.assertEqual(len(self.cli("list")[0]["tasks"]), 1, self.proc.log_path.read_text())
        self.assertEqual(self.db("SELECT count(*) FROM tasks"), [(1,)], str(list((self.root / "data").iterdir())))
        listing = self.cli("list")[0]
        self.assertEqual(len(listing["tasks"]), 1)
        self.assertEqual(listing["tasks"][0]["state"], "PENDING")

    def test_assignment_gate_reserves_before_run_and_reuses_capacity_after_loss(self):
        self.controller("hold-assign")
        worker = self.peer(True)
        self.assertTrue(worker.register(cpu=1)[2]["accepted"])
        first = self.submit("/bin/true")
        self.held()
        client = self.peer()
        client.sock.sendall(frame(0x0101, dict(executable="/bin/true", args=[]), 2))
        self.assertFalse(select.select([worker.sock], [], [], 0.15)[0])
        self.assertEqual(self.db("SELECT state FROM tasks WHERE task_id=?", (first,)), [("PENDING",)])
        self.assertEqual(self.db("SELECT count(*) FROM attempts"), [(0,)])
        self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 1)
        self.release()
        kind, request, run = receive(worker.sock)
        self.assertEqual(kind, 0x0204)
        self.assertEqual(self.db("SELECT state,session_id FROM attempts WHERE attempt_id=?", (run["attempt_id"],)),
                         [("ASSIGNED", worker.session)], (run, self.db("SELECT attempt_id,state,session_id FROM attempts"), self.proc.log_path.read_text()))
        second = receive(client.sock)[2]["task_id"]
        self.assertEqual(self.get(second)["task"]["state"], "PENDING")
        # Wrong request ID must never advance STARTING. Connection loss conservatively commits LOST.
        worker.sock.sendall(frame(0x0205, dict(worker_id=worker.worker, session_id=worker.session,
            attempt_id=run["attempt_id"], accepted=True, error_code=None, message=None), request + 1))
        self.assertEqual(worker.sock.recv(1), b"")
        eventually(lambda: self.get(first)["task"]["state"] == "FAILED")
        offline = self.workers()[0]
        self.assertEqual(offline["state"], "OFFLINE")
        self.assertIsNone(offline["session_id"])
        self.assertEqual(offline["reserved"]["cpu_slots"], 0)
        actual = self.worker("raw-worker")
        eventually(lambda: self.get(second)["task"]["state"] == "SUCCEEDED")
        self.assertTrue(self.get(second)["attempts"][0]["logs_complete"])
        self.stop(actual)

    def test_essential_failure_and_unknown_commit_fail_stop_without_false_effects(self):
        self.controller("fail-assign")
        worker = self.peer(True)
        self.assertTrue(worker.register()[2]["accepted"])
        self.submit("/bin/true")
        self.held()
        self.release()
        self.assertEqual(self.proc.wait(timeout=5), 1)
        self.assertEqual(worker.sock.recv(1), b"")
        self.assertEqual(self.db("SELECT count(*) FROM attempts"), [(0,)])
        os.close(self.gate_write)
        os.close(self.gate_read)
        del self.gate_write
        self.controller("unknown-submit")
        client = self.peer()
        client.sock.sendall(frame(0x0101, dict(executable="/bin/true", args=[]), 18))
        self.held()
        self.release()
        self.assertEqual(self.proc.wait(timeout=5), 1)
        self.assertEqual(client.sock.recv(1), b"")
        self.assertEqual(self.db("SELECT count(*) FROM tasks"), [(2,)])

    def test_pagination_history_restore_and_truthful_unsupported(self):
        self.controller()
        task = self.submit("/bin/true")
        self.stop(self.proc)
        # Seed *test fixture* historical committed rows, not production decisions.
        # Actual restart recovery/read APIs must page all of them without dropping history.
        now = "2026-01-02T03:04:05.000000Z"
        session = uid()
        with closing(sqlite3.connect(self.root / "data" / "metadata.sqlite3")) as db, db:
            for number in range(1, 206):
                attempt = uid()
                db.execute("INSERT INTO attempts(attempt_id,task_id,attempt_no,state,state_version,worker_id,session_id,assigned_at,finished_at,reason,stdout_path,stderr_path) VALUES(?,?,?,'LOST',1,'history-worker',?,?,?,'TEST_HISTORY','out','err')",
                           (attempt, task, number, session, now, now))
            db.execute("UPDATE tasks SET state='FAILED',state_version=1,latest_attempt_id=? WHERE task_id=?", (attempt, task))
            for _ in range(206):
                db.execute("INSERT INTO tasks VALUES(?,'PENDING',0,'/bin/true','[]','{}',NULL,1,0,NULL,NULL,NULL,?,?)", (uid(), now, now))
        self.controller()
        view = self.get(task)
        self.assertEqual(len(view["attempts"]), 205)
        self.assertEqual([a["attempt_no"] for a in view["attempts"]], list(range(1, 206)))
        pages = self.cli("list")
        self.assertEqual([len(p["tasks"]) for p in pages], [100, 100, 7])
        listed = [t for page in pages for t in page["tasks"]]
        self.assertEqual(len({t["task_id"] for t in listed}), 207)
        self.assertEqual([(t["created_at"], t["task_id"]) for t in listed],
                         sorted([(t["created_at"], t["task_id"]) for t in listed], reverse=True))
        peer = self.peer()
        for token in ("bad", "a" * 257):
            self.assertEqual(peer.request(0x0105, dict(page_token=token), 3)[2]["code"], "INVALID_ARGUMENT")
        # M9 enables cancel/retry. Preserve history/list coverage above and check
        # the newly authorized conflict/success behavior instead of UNSUPPORTED.
        pending = self.submit("--cpu", "100", "/bin/true")
        self.assertEqual(peer.request(0x0107, dict(task_id=pending, attempt_no=None, stream="stdout", offset=0,
                                                snapshot_end=None), 4)[2]["code"], "NOT_FOUND")
        self.assertEqual(self.cli("cancel", task, code=2)[0]["code"], "CONFLICT")
        self.assertEqual(self.cli("retry", task)[0]["next_attempt_no"], 206)
        self.assertEqual(len(self.get(task)["attempts"]), 205)

    def test_registration_replacement_heartbeat_and_staging_exhaustion(self):
        self.config["queues"] = dict(log_bytes=1024, log_commands=3, log_control_reserve=1,
            initial_session_log_window_bytes=1024, initial_session_log_window_chunks=2)
        self.config_path = self.write_json("controller.json", self.config)
        self.controller()
        first = self.peer(True)
        ack = first.register("worker-a")
        self.assertEqual(ack[0], 0x0202)
        self.assertEqual(ack[2]["initial_log_window_bytes"], 1024)
        first.heartbeat(1)
        extra = self.peer(True)
        rejected = extra.register("worker-b")
        self.assertEqual(set(rejected[2]), {"accepted", "error_code", "message"})
        self.assertFalse(rejected[2]["accepted"])
        # Duplicate sequence must not renew deadline, but neither is it a fake new lease.
        for _ in range(4):
            first.heartbeat(1)
            time.sleep(0.1)
        eventually(lambda: self.workers()[0]["state"] == "OFFLINE", seconds=3)
        self.assertIsNone(self.workers()[0]["session_id"])
        self.assertIsNotNone(self.workers()[0]["last_heartbeat_at"])
        replacement = self.peer(True)
        self.assertTrue(replacement.register("worker-a")[2]["accepted"])
        replacement.heartbeat(1, session=uid())
        self.assertEqual(replacement.sock.recv(1), b"")
        eventually(lambda: self.workers()[0]["state"] == "OFFLINE")

    def test_output_credit_backpressure_control_liveness_and_group_shutdown(self):
        self.config["queues"] = dict(log_bytes=8, log_commands=3, log_control_reserve=1,
            initial_session_log_window_bytes=8, initial_session_log_window_chunks=2)
        self.config_path = self.write_json("controller.json", self.config)
        self.controller()
        worker = self.worker("worker-a")
        task = self.submit("/usr/bin/yes")
        eventually(lambda: self.get(task)["attempts"] and self.get(task)["attempts"][0]["state"] == "RUNNING")
        time.sleep(1.8)  # > offline timeout: pipe pause must not hide heartbeat processing
        self.assertEqual(self.workers()[0]["state"], "HEALTHY")
        self.assertFalse(self.get(task)["attempts"][0]["finalization_pending"])
        children = Path(f"/proc/{worker.pid}/task/{worker.pid}/children").read_text().split()
        self.assertEqual(len(children), 1)
        pid = int(children[0])
        self.stop(worker)
        eventually(lambda: self.get(task)["task"]["state"] == "FAILED")
        self.assertFalse(Path(f"/proc/{pid}").exists(), "owned child must be reaped, not orphaned/zombie")
        with self.assertRaises(ProcessLookupError):
            os.killpg(pid, 0)

    def test_worker_wire_exec_failure_credit_and_finish_barrier(self):
        # Test-only Controller peer; real worker executable, M3 sockets and M4
        # child runtime. This complements, not replaces, the two-worker smoke.
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", self.worker_port))
        listener.listen()
        listener.settimeout(4)
        config = dict(worker_id="observed-worker", controller=dict(address="127.0.0.1", port=self.worker_port),
            capacity=dict(cpu_slots=2, memory_mb=100), max_concurrent_tasks=2)
        worker = self.spawn("worker", "--config", self.write_json("wire-worker.json", config))
        sock, _ = listener.accept()
        self.addCleanup(listener.close)
        self.addCleanup(sock.close)
        sock.settimeout(4)
        kind, request, registration = receive(sock)
        self.assertEqual((kind, request), (0x0201, 1))
        session = registration["session_id"]
        sock.sendall(frame(0x0202, dict(accepted=True, heartbeat_interval_ms=100, offline_timeout_ms=1500,
            termination_grace_ms=50, initial_log_window_bytes=1024, initial_log_window_chunks=64,
            controller_instance_id=uid()), request))
        eventually(lambda: "REGISTERED" in worker.log_path.read_text())
        baseline_fds = len(list(Path(f"/proc/{worker.pid}/fd").iterdir()))
        def run(executable, args, cwd=None, request=2):
            payload = dict(worker_id="observed-worker", session_id=session, task_id=uid(), attempt_id=uid(), attempt_no=1,
                executable=executable, args=args, env={}, cwd=cwd, resources=dict(cpu_slots=1, memory_mb=0), timeout_ms=None)
            sock.sendall(frame(0x0204, payload, request))
            observations = []
            while not observations or observations[-1][0] != 0x0209:
                incoming = receive(sock)
                if incoming[0] != 0x0203:
                    observations.append(incoming)
            self.assertEqual((observations[0][0], observations[0][1], observations[0][2]["accepted"]), (0x0205, request, True))
            self.assertEqual(observations[-1][2]["attempt_id"], payload["attempt_id"])
            return observations
        for index in range(6):
            events = run("/bin/true", [], request=index + 2)
            self.assertEqual([e[0] for e in events], [0x0205, 0x0206, 0x0209])
            self.assertGreater(events[1][2]["pid"], 0)
            with self.assertRaises(ProcessLookupError):
                os.kill(events[1][2]["pid"], 0)
            self.assertEqual(events[-1][2]["outcome"], "EXITED")
            self.assertEqual(events[-1][2]["exit_code"], 0)
            self.assertEqual(events[-1][2]["stdout_bytes"], 0)
        for index, (executable, cwd) in enumerate((("/bin/true", "/no-such-m7-cwd"), ("/no-such-m7-exec", None))):
            events = run(executable, [], cwd=cwd, request=20 + index)
            self.assertEqual([e[0] for e in events], [0x0205, 0x0209])
            self.assertEqual(events[-1][2]["outcome"], "LAUNCH_FAILED")
            self.assertIsNone(events[-1][2]["exit_code"])
            self.assertTrue(events[-1][2]["reason"])
        events = run("/bin/echo", ["byte-exact"], request=30)
        logs = [e[2] for e in events if e[0] == 0x0207]
        self.assertTrue(logs)
        self.assertEqual(b"".join(base64.b64decode(e["data_base64"]) for e in logs), b"byte-exact\n")
        self.assertEqual(events[-1][2]["stdout_bytes"], 11)
        self.assertEqual(events[-1][2]["stdout_next_sequence"], len(logs))
        self.assertEqual(events[1][0], 0x0206)
        eventually(lambda: not Path(f"/proc/{worker.pid}/task/{worker.pid}/children").read_text().strip())
        self.assertEqual(len(list(Path(f"/proc/{worker.pid}/fd").iterdir())), baseline_fds)
        self.stop(worker)

    def test_negative_missing_ack_and_session_replacement(self):
        self.controller()
        first = self.peer(True)
        self.assertTrue(first.register(cpu=1)[2]["accepted"])
        task = self.submit("/bin/true")
        kind, request, run = receive(first.sock)
        self.assertEqual(kind, 0x0204)
        first.sock.sendall(frame(0x0205, dict(worker_id=first.worker, session_id=first.session,
            attempt_id=run["attempt_id"], accepted=False, error_code="LOCAL_CAPACITY", message="test rejected before launch"), request))
        eventually(lambda: self.get(task)["task"]["state"] == "FAILED")
        view = self.get(task)["attempts"][0]
        self.assertEqual(view["state"], "FAILED")
        self.assertEqual(view["stdout_bytes"], 0)
        second_task = self.submit("/bin/true")
        self.assertEqual(receive(first.sock)[0], 0x0204)
        # No ACK: conservative timeout invalidation, not an execution guarantee.
        eventually(lambda: self.get(second_task)["task"]["state"] == "FAILED", seconds=4)
        self.assertEqual(self.get(second_task)["attempts"][0]["state"], "LOST")
        old = self.peer(True)
        self.assertTrue(old.register("replace-worker")[2]["accepted"])
        third_task = self.submit("/bin/true")
        self.assertEqual(receive(old.sock)[0], 0x0204)
        new = self.peer(True)
        self.assertTrue(new.register("replace-worker")[2]["accepted"])
        # Accepted replacement is only visible AFTER old-session LOST commit.
        self.assertEqual(self.db("SELECT state FROM attempts WHERE task_id=?", (third_task,)), [("LOST",)])
        self.assertEqual(old.sock.recv(1), b"")
        self.assertEqual([w for w in self.workers() if w["worker_id"] == "replace-worker"][0]["session_id"], new.session)
        # Instrumented wire peer: coalesce ACK/START/FINISH into ONE receive
        # callback. Completion eventfds cannot be dispatched recursively, so the
        # later observations necessarily arrive ahead of ACK completion handling.
        # The real OS execution path is proved separately, not fabricated here.
        coalesced_task = self.submit("/bin/true")
        kind, request, run = receive(new.sock)
        self.assertEqual(kind, 0x0204)
        binding = dict(worker_id=new.worker, session_id=new.session, attempt_id=run["attempt_id"])
        ack = dict(**binding, accepted=True, error_code=None, message=None)
        started = dict(**binding, task_id=run["task_id"], pid=12345)
        finished = dict(**binding, task_id=run["task_id"], outcome="EXITED", exit_code=0, term_signal=None,
                        reason=None, stdout_next_sequence=0, stderr_next_sequence=0, stdout_bytes=0, stderr_bytes=0)
        new.sock.sendall(frame(0x0205, ack, request) + frame(0x0206, started) + frame(0x0209, finished))
        eventually(lambda: self.get(coalesced_task)["task"]["state"] == "SUCCEEDED")
        self.assertEqual(self.db("SELECT state,state_version,logs_complete FROM attempts WHERE task_id=?", (coalesced_task,)),
                         [("SUCCEEDED", 3, 1)])

    def test_startup_read_failure_never_opens_ready_listeners(self):
        notify_read, notify_write = os.pipe()
        release_read, release_write = os.pipe()
        try:
            failed = self.spawn("ts_m7_fault_controller", self.config_path, "read-fail", notify_write, release_read,
                               pass_fds=(notify_write, release_read))
            self.assertEqual(failed.wait(timeout=4), 1)
            self.assertNotIn("READY client=", failed.log_path.read_text())
            for port in (self.client_port, self.worker_port):
                with self.assertRaises(ConnectionRefusedError):
                    socket.create_connection(("127.0.0.1", port), timeout=0.2)
        finally:
            for fd in (notify_read, notify_write, release_read, release_write):
                os.close(fd)

    def test_real_launch_failures_hold_starting_without_fabricated_running(self):
        self.controller()
        self.worker("worker-a", cpu=2)
        ids = [self.submit("--cwd", "/no-such-m7-directory", "/bin/true"), self.submit("/no-such-m7-executable")]
        for task in ids:
            eventually(lambda: self.get(task)["task"]["state"] == "FAILED")
            attempt = self.get(task)["attempts"][0]
            self.assertEqual(attempt["state"], "FAILED")
            self.assertIsNone(attempt["started_at"])
            self.assertTrue(attempt["logs_complete"])
            self.assertEqual(self.db("SELECT state,logs_complete,started_at FROM attempts WHERE task_id=?", (task,)),
                             [("FAILED", 1, None)])

    def test_connection_output_pressure_is_bounded_and_other_clients_survive(self):
        self.controller()
        slow = self.peer()
        slow.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        baseline = len(list(Path(f"/proc/{self.proc.pid}/fd").iterdir()))
        # Bounded batches without receiving. Ordinary control frames are small;
        # the transport/16-request admission cap may disconnect this peer, never
        # block all EventLoop work or leak it. M3 has exact queue saturation tests.
        batch = b"".join(frame(0x010D, {}, i) for i in range(1, 101))
        try:
            for _ in range(30):
                slow.sock.sendall(batch)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.assertEqual(self.workers(), [])
        slow.close()
        eventually(lambda: len(list(Path(f"/proc/{self.proc.pid}/fd").iterdir())) <= baseline)
        self.assertEqual(len(list(Path(f"/proc/{self.proc.pid}/task").iterdir())), 3)

    def test_partial_run_transport_failure_keeps_uncertainty_and_commits_lost(self):
        self.controller("partial-run")
        worker = self.peer(True)
        self.assertTrue(worker.register(cpu=1)[2]["accepted"])
        task = self.submit("/bin/true")
        self.assertEqual(exact(worker.sock, 7), bytes.fromhex("54534348000102"))
        self.assertEqual(worker.sock.recv(1), b"")
        eventually(lambda: self.get(task)["task"]["state"] == "FAILED")
        row = self.get(task)["attempts"][0]
        self.assertEqual(row["state"], "LOST")
        self.assertFalse(row["logs_complete"])
        self.assertIsNone(row["started_at"])
        self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 0)

    def test_retained_metadata_and_session_tombstones_have_lifetime_admission_bounds(self):
        self.controller("metadata-bound")
        task = self.submit("/bin/true")
        self.assertEqual(self.cli("submit", "/bin/true", code=2)[0]["code"], "TEMPORARILY_UNAVAILABLE")
        self.assertEqual(self.get(task)["task"]["state"], "PENDING")
        self.assertEqual(self.db("SELECT count(*) FROM tasks"), [(1,)])
        self.stop(self.proc)
        os.close(self.gate_write)
        os.close(self.gate_read)
        del self.gate_write
        self.controller("registration-bound")
        for _ in range(2):
            worker = self.peer(True)
            self.assertTrue(worker.register("bounded-worker")[2]["accepted"])
            worker.close()
            eventually(lambda: self.workers()[0]["state"] == "OFFLINE")
        third = self.peer(True)
        rejected = third.register("bounded-worker")
        self.assertFalse(rejected[2]["accepted"])
        self.assertIn("capacity", rejected[2]["message"])
        self.assertEqual(self.workers()[0]["state"], "OFFLINE")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--case", required=True)
    args = parser.parse_args()
    BIN = args.bin
    suite = unittest.TestSuite([M7Loopback(args.case)])
    outcome = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if outcome.wasSuccessful() else 1)
