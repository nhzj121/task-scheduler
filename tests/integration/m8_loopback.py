"""Actual M8 binaries, Linux workload, TCP, SQLite and ext4-backed /tmp logs."""
import argparse
import base64
import os
import select
import signal
import socket
import time
from pathlib import Path
import subprocess
import sqlite3
from contextlib import closing
import unittest
import m7_loopback as m7
from m7_loopback import Harness, eventually, receive, frame, uid


def expected(size, mask=0):
    pattern = bytes((i ^ mask for i in range(251)))
    return (pattern * ((size + 250) // 251))[:size]


class M8Loopback(Harness):
    def setUp(self):
        super().setUp()
        self.config["queues"] = dict(log_bytes=131072, log_commands=24, log_control_reserve=4,
            initial_session_log_window_bytes=65536, initial_session_log_window_chunks=8)
        self.config_path = self.write_json("controller.json", self.config)

    def db(self, sql, args=()):
        with closing(sqlite3.connect(Path(self.config["data_dir"]) / "metadata.sqlite3", timeout=0.1)) as db:
            return db.execute(sql, args).fetchall()

    def fault_controller(self, mode):
        notify_read, notify_write = os.pipe()
        release_read, release_write = os.pipe()
        self.gate_read, self.gate_write = notify_read, release_write
        self.proc = self.spawn("ts_m8_fault_controller", self.config_path, mode, notify_write, release_read,
                               pass_fds=(notify_write, release_read))
        os.close(notify_write)
        os.close(release_read)
        eventually(lambda: "READY client=" in self.proc.log_path.read_text() or
                   (self.proc.poll() is not None and self.fail(self.proc.log_path.read_text())))

    def drop_gate(self):
        os.close(self.gate_read)
        os.close(self.gate_write)
        del self.gate_write

    def acknowledge(self, peer, run, request):
        binding = dict(worker_id=peer.worker, session_id=peer.session, attempt_id=run["attempt_id"])
        peer.sock.sendall(frame(0x0205, dict(**binding, accepted=True, error_code=None, message=None), request) +
                          frame(0x0206, dict(**binding, task_id=run["task_id"], pid=12345)))

    def complete(self, task, state="SUCCEEDED"):
        eventually(lambda: self.get(task)["task"]["state"] == state, seconds=15)
        view = self.get(task)
        self.assertEqual(view["task"]["state"], state)
        return view["attempts"][-1]

    def raw_logs(self, task, *options):
        result = subprocess.run([str(m7.BIN / "taskctl"), "--port", str(self.client_port), "logs", task, *options],
                                capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_empty_real_barrier_terminal_release_and_restart(self):
        self.controller()
        worker = self.worker("worker-a")
        task = self.submit("/bin/true")
        attempt = self.complete(task)
        self.assertTrue(attempt["logs_complete"])
        self.assertFalse(attempt["finalization_pending"])
        paths = self.db("SELECT stdout_path,stderr_path FROM attempts WHERE task_id=?", (task,))[0]
        for path in paths:
            self.assertEqual((self.root / "data" / path).read_bytes(), b"")
        self.assertEqual(self.raw_logs(task, "--both"), b"")
        self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 0)
        second = self.submit("/bin/true")
        self.assertTrue(self.complete(second)["logs_complete"])
        self.stop(worker)
        self.stop(self.proc)
        self.controller()
        self.assertEqual(self.get(task)["task"]["state"], "SUCCEEDED")
        self.assertEqual(self.raw_logs(task, "--stderr"), b"")

    def test_large_binary_multiple_replenishments_and_paged_cli_are_exact(self):
        self.controller()
        self.worker("worker-a")
        baseline_fds = len(list(Path(f"/proc/{self.proc.pid}/fd").iterdir()))
        out_size, err_size = 4 * 1024 * 1024 + 139, 3 * 1024 * 1024 + 251
        task = self.submit(str(m7.BIN / "ts_log_workload"), "binary", str(out_size), str(err_size))
        attempt = self.complete(task)
        self.assertTrue(attempt["logs_complete"])
        self.assertEqual((attempt["stdout_bytes"], attempt["stderr_bytes"]), (out_size, err_size))
        out, err = expected(out_size), expected(err_size, 0xa5)
        paths = self.db("SELECT stdout_path,stderr_path FROM attempts WHERE task_id=?", (task,))[0]
        self.assertEqual((self.root / "data" / paths[0]).read_bytes(), out)
        self.assertEqual((self.root / "data" / paths[1]).read_bytes(), err)
        self.assertEqual(self.raw_logs(task, "--limit-bytes", "17777"), out)
        self.assertEqual(self.raw_logs(task, "--stderr", "--limit-bytes", "65536"), err)
        self.assertEqual(self.raw_logs(task, "--both"), out + err)
        self.assertEqual(len(list(Path(f"/proc/{self.proc.pid}/task").iterdir())), self.expected_controller_threads)
        # One tasks/ directory fd may be retained; per-Attempt files and client
        # sockets must close rather than accumulating with snapshots/chunks.
        eventually(lambda: len(list(Path(f"/proc/{self.proc.pid}/fd").iterdir())) <= baseline_fds + 1)

    def test_active_snapshot_is_fixed_while_later_append_completes(self):
        gate = self.root / "workload.fifo"
        os.mkfifo(gate, 0o600)
        self.controller()
        self.worker("worker-a")
        size = 50000
        task = self.submit(str(m7.BIN / "ts_log_workload"), "staged", str(size), str(gate))
        peer = self.peer()
        def first_page():
            response = peer.request(0x0107, dict(task_id=task, attempt_no=None, stream="stdout", offset=0,
                                               limit_bytes=17000, snapshot_end=None), 8)
            if response[0] == 0x0108 and response[2]["snapshot_end"] == size:
                return response[2]
            return None
        first = eventually(first_page)
        self.assertFalse(first["logs_complete"])
        self.assertEqual(base64.b64decode(first["data_base64"]), expected(size)[:17000])
        def release():
            try:
                fd = os.open(gate, os.O_WRONLY | os.O_NONBLOCK)
                os.write(fd, b"R")
                os.close(fd)
                return True
            except OSError:
                return False
        eventually(release)
        self.complete(task)
        tail = peer.request(0x0107, dict(task_id=task, attempt_no=1, stream="stdout", offset=17000,
                                       limit_bytes=262144, snapshot_end=size), 9)[2]
        self.assertEqual(tail["snapshot_end"], size)
        self.assertEqual(base64.b64decode(tail["data_base64"]), expected(size)[17000:])
        self.assertTrue(tail["eof_snapshot"])
        self.assertEqual(self.raw_logs(task), expected(size) * 2)

    def test_every_file_directory_and_sql_gate_keeps_terminal_and_reservation_hidden(self):
        for mode in ("hold-file-out", "hold-file-err", "hold-dir-task", "hold-dir-tasks", "hold-dir-root", "terminal-commit"):
            with self.subTest(mode=mode):
                self.fault_controller(mode)
                worker = self.worker("worker-a")
                task = self.submit("/bin/true")
                self.held()
                self.assertEqual(self.db("SELECT state FROM tasks WHERE task_id=?", (task,)), [("ACTIVE",)])
                self.assertEqual(self.db("SELECT logs_complete FROM attempts WHERE task_id=?", (task,)), [(0,)])
                self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 1)
                self.assertEqual(self.workers()[0]["state"], "HEALTHY")
                self.release()
                self.assertTrue(self.complete(task)["logs_complete"])
                self.stop(worker)
                self.stop(self.proc)
                self.drop_gate()
                # Fresh namespace per mode so every directory level is dirty.
                self.config["data_dir"] = str(self.root / mode)
                self.config_path = self.write_json("controller.json", self.config)

    def test_sync_and_open_failures_preserve_observed_outcome_as_log_persistence_error(self):
        for mode in ("fail-file-out", "fail-file-err", "fail-dir-task", "fail-dir-tasks", "fail-dir-root", "fail-create"):
            with self.subTest(mode=mode):
                self.fault_controller(mode)
                worker = self.worker("worker-a")
                task = self.submit("/bin/true")
                attempt = self.complete(task, "FAILED")
                self.assertFalse(attempt["logs_complete"])
                self.assertEqual(attempt["reason"], "LOG_PERSISTENCE_ERROR")
                facts = self.db("SELECT observed_outcome_json FROM attempts WHERE task_id=?", (task,))[0][0]
                self.assertIn('"exit_code":0', facts)
                self.stop(worker, expected=None)
                self.stop(self.proc)
                self.drop_gate()
                self.config["data_dir"] = str(self.root / mode)
                self.config_path = self.write_json("controller.json", self.config)

    def test_write_error_before_finish_is_lost_not_a_fabricated_result(self):
        self.fault_controller("fail-write")
        worker = self.worker("worker-a")
        task = self.submit("/usr/bin/yes")
        attempt = self.complete(task, "FAILED")
        self.assertEqual(attempt["state"], "LOST")
        self.assertFalse(attempt["logs_complete"])
        self.stop(worker, expected=None)

    def test_completion_gate_prevents_replenishment_until_success_is_delivered(self):
        self.fault_controller("hold-notify")
        worker = self.peer(True)
        self.assertTrue(worker.register()[2]["accepted"])
        task = self.submit("/bin/true")
        _, request, run = receive(worker.sock)
        self.acknowledge(worker, run, request)
        binding = dict(worker_id=worker.worker, session_id=worker.session, task_id=task, attempt_id=run["attempt_id"])
        worker.sock.sendall(frame(0x0207, dict(**binding, stream="stdout", sequence=0, raw_size=1, data_base64="eA==")))
        self.held()
        self.assertFalse(select.select([worker.sock], [], [], 0.2)[0], "must not replenish on receipt/write alone")
        worker.heartbeat(1)
        self.assertEqual(self.workers()[0]["state"], "HEALTHY")
        self.release()
        kind, request, grant = receive(worker.sock)
        self.assertEqual((kind, request, grant), (0x0208, 0, dict(grant_bytes=1, grant_chunks=1)))
        worker.sock.sendall(frame(0x0209, dict(**binding, outcome="EXITED", exit_code=0, term_signal=None, reason=None,
            stdout_next_sequence=1, stderr_next_sequence=0, stdout_bytes=1, stderr_bytes=0)))
        self.assertTrue(self.complete(task)["logs_complete"])
        self.assertEqual(self.raw_logs(task), b"x")

    def test_crash_at_namespace_data_and_metadata_boundaries_never_infers_success(self):
        for mode in ("after-create", "hold-file-out", "after-file-out", "hold-file-err", "after-file-err",
                     "hold-dir-task", "after-dir-task", "hold-dir-tasks", "after-dir-tasks", "hold-dir-root", "after-dir-root",
                     "terminal-commit", "terminal-after"):
            with self.subTest(mode=mode):
                self.fault_controller(mode)
                worker = self.worker("worker-a")
                task = self.submit("/bin/true")
                self.held()
                self.proc.kill()
                self.proc.wait(timeout=3)
                self.stop(worker, expected=None)
                self.drop_gate()
                self.controller()
                attempt = self.get(task)["attempts"][0]
                if mode == "terminal-after":
                    self.assertEqual(attempt["state"], "SUCCEEDED")
                    self.assertTrue(attempt["logs_complete"])
                    self.assertEqual(self.raw_logs(task), b"")
                else:
                    self.assertEqual(attempt["state"], "LOST")
                    self.assertFalse(attempt["logs_complete"])
                self.stop(self.proc)
                self.config["data_dir"] = str(self.root / mode)
                self.config_path = self.write_json("controller.json", self.config)

    def test_partial_tcp_sends_keep_both_streams_before_finish(self):
        self.controller()
        config = dict(worker_id="short-worker", controller=dict(address="127.0.0.1", port=self.worker_port),
                      capacity=dict(cpu_slots=2, memory_mb=100), max_concurrent_tasks=2)
        worker = self.spawn("ts_m8_partial_worker", self.write_json("short.json", config))
        eventually(lambda: "REGISTERED" in worker.log_path.read_text())
        tasks = [self.submit(str(m7.BIN / "ts_log_workload"), "binary", "150031", "80003") for _ in range(2)]
        for task in tasks:
            self.assertTrue(self.complete(task)["logs_complete"])
            self.assertEqual(self.raw_logs(task, "--both"), expected(150031) + expected(80003, 0xa5))
        self.assertEqual(self.workers()[0]["state"], "HEALTHY")
        self.assertEqual(self.workers()[0]["running_attempts"], 0)

    def test_stalled_writer_backpressures_multiple_workers_without_hiding_heartbeats(self):
        self.fault_controller("hold-write")
        workers = [self.worker("worker-a"), self.worker("worker-b")]
        tasks = [self.submit(str(m7.BIN / "ts_log_workload"), "binary", "200003", "100007") for _ in range(2)]
        self.held()
        eventually(lambda: all(self.get(t)["attempts"] and self.get(t)["attempts"][0]["state"] == "RUNNING" for t in tasks))
        baseline = [len(list(Path(f"/proc/{w.pid}/fd").iterdir())) for w in workers]
        # More than offline timeout with both granted windows consumed. No socket
        # reads or SQLite owner is suspended while Writer is blocked on disk.
        time.sleep(1.8)
        self.assertTrue(all(w["state"] == "HEALTHY" and w["running_attempts"] == 1 for w in self.workers()))
        self.assertTrue(all(self.get(t)["task"]["state"] == "ACTIVE" for t in tasks))
        self.assertTrue(all(len(list(Path(f"/proc/{w.pid}/fd").iterdir())) <= before
                            for w, before in zip(workers, baseline)), "pending exec-status fds may close; no growth")
        self.assertEqual(len(list(Path(f"/proc/{self.proc.pid}/task").iterdir())), 3)
        self.release()
        for task in tasks:
            self.assertTrue(self.complete(task)["logs_complete"])
            self.assertEqual(self.raw_logs(task, "--both"), expected(200003) + expected(100007, 0xa5))

    def test_rejected_run_and_lost_and_missing_history_never_fabricate_logs(self):
        self.controller()
        worker = self.peer(True)
        self.assertTrue(worker.register(cpu=1)[2]["accepted"])
        task = self.submit("/bin/true")
        _, request, run = receive(worker.sock)
        binding = dict(worker_id=worker.worker, session_id=worker.session, attempt_id=run["attempt_id"])
        worker.sock.sendall(frame(0x0205, dict(**binding, accepted=False, error_code="LOCAL_CAPACITY", message="never launched"), request))
        self.assertTrue(self.complete(task, "FAILED")["logs_complete"])
        self.assertFalse((self.root / "data" / "tasks" / task).exists())
        self.assertEqual(self.raw_logs(task, "--both"), b"")
        lost = self.submit("/bin/true")
        self.assertEqual(receive(worker.sock)[0], 0x0204)
        worker.close()
        self.assertEqual(self.complete(lost, "FAILED")["state"], "LOST")
        page = self.cli("logs", lost)[0]
        self.assertFalse(page["logs_complete"])
        self.assertEqual(page["data_base64"], "")
        actual = self.worker("real-worker")
        normal = self.submit("/bin/true")
        self.complete(normal)
        path = self.db("SELECT stdout_path FROM attempts WHERE task_id=?", (normal,))[0][0]
        peer = self.peer()
        query = dict(task_id=normal, attempt_no=None, stream="stdout", offset=0, snapshot_end=None)
        (self.root / "data" / path).chmod(0o666)  # test corrupts filesystem safety
        self.assertEqual(peer.request(0x0107, query, 6)[2]["code"], "INTERNAL")
        (self.root / "data" / path).unlink()  # test corrupts completed history
        response = peer.request(0x0107, query, 7)
        self.assertEqual(response[2]["code"], "INTERNAL")
        self.assertTrue(self.get(normal)["attempts"][0]["logs_complete"], "read must not rewrite committed history")
        self.stop(actual)

    def test_default_clamped_paging_and_binary_payload_stay_below_frame_limit(self):
        self.controller()
        self.worker("worker-a")
        size = 3144960 + 119
        task = self.submit(str(m7.BIN / "ts_log_workload"), "binary", str(size), "0")
        self.complete(task)
        peer = self.peer()
        query = dict(task_id=task, attempt_no=None, stream="stdout", offset=0, snapshot_end=None)
        first = peer.request(0x0107, query, 3)[2]
        self.assertEqual(len(base64.b64decode(first["data_base64"])), 262144)
        kind, request, capped = peer.request(0x0107, dict(**query, limit_bytes=2**63), 4)
        self.assertEqual((kind, request), (0x0108, 4))
        self.assertEqual(len(base64.b64decode(capped["data_base64"])), 3144960)
        self.assertLessEqual(len(frame(kind, capped, request)) - 20, 4194304)
        self.assertFalse(capped["eof_snapshot"])
        self.assertEqual(self.raw_logs(task, "--limit-bytes", str(2**63)), expected(size))
        for bad in (0, -1, "100", 2**64):
            self.assertEqual(peer.request(0x0107, dict(**query, limit_bytes=bad), 5)[2]["code"], "INVALID_ARGUMENT")

    def test_disconnect_and_replacement_during_write_or_barrier_fence_late_success(self):
        for mode in ("hold-write", "hold-file-out"):
            with self.subTest(mode=mode):
                self.fault_controller(mode)
                worker = self.peer(True)
                self.assertTrue(worker.register(cpu=1)[2]["accepted"])
                task = self.submit("/bin/true")
                _, request, run = receive(worker.sock)
                self.acknowledge(worker, run, request)
                binding = dict(worker_id=worker.worker, session_id=worker.session, task_id=task, attempt_id=run["attempt_id"])
                worker.sock.sendall(frame(0x0207, dict(**binding, stream="stdout", sequence=0, raw_size=1, data_base64="eA==")) +
                    frame(0x0209, dict(**binding, outcome="EXITED", exit_code=0, term_signal=None, reason=None,
                        stdout_next_sequence=1, stderr_next_sequence=0, stdout_bytes=1, stderr_bytes=0)))
                self.held()
                worker.close()
                self.assertEqual(self.complete(task, "FAILED")["state"], "LOST")
                replacement = self.peer(True)
                self.assertTrue(replacement.register("raw-worker", cpu=1)[2]["accepted"])
                self.release()
                time.sleep(0.1)
                self.assertEqual(self.get(task)["attempts"][0]["state"], "LOST")
                self.assertFalse(self.get(task)["attempts"][0]["logs_complete"])
                self.assertEqual(self.workers()[0]["session_id"], replacement.session)
                self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 0)
                replacement.close()
                self.stop(self.proc)
                self.drop_gate()
                self.config["data_dir"] = str(self.root / mode)
                self.config_path = self.write_json("controller.json", self.config)

    def test_nonzero_and_signal_results_are_committed_only_with_complete_logs(self):
        self.controller()
        worker = self.worker("worker-a")
        task = self.submit("/bin/false")
        attempt = self.complete(task, "FAILED")
        self.assertTrue(attempt["logs_complete"])
        self.assertEqual(attempt["exit_code"], 1)
        task = self.submit("/bin/sleep", "30")
        eventually(lambda: self.get(task)["attempts"] and self.get(task)["attempts"][0]["state"] == "RUNNING")
        children = Path(f"/proc/{worker.pid}/task/{worker.pid}/children").read_text().split()
        self.assertEqual(len(children), 1)
        os.kill(int(children[0]), signal.SIGTERM)  # external test signal, not M9 cancellation
        attempt = self.complete(task, "FAILED")
        self.assertTrue(attempt["logs_complete"])
        self.assertEqual(attempt["term_signal"], signal.SIGTERM)

    def test_finish_count_mismatch_and_late_log_are_protocol_faults(self):
        for late in (False, True):
            with self.subTest(late=late):
                self.controller()
                worker = self.peer(True)
                self.assertTrue(worker.register()[2]["accepted"])
                task = self.submit("/bin/true")
                _, request, run = receive(worker.sock)
                self.acknowledge(worker, run, request)
                binding = dict(worker_id=worker.worker, session_id=worker.session, task_id=task, attempt_id=run["attempt_id"])
                finish = frame(0x0209, dict(**binding, outcome="EXITED", exit_code=0, term_signal=None, reason=None,
                    stdout_next_sequence=0, stderr_next_sequence=0, stdout_bytes=0 if late else 1, stderr_bytes=0))
                tail = frame(0x0207, dict(**binding, stream="stdout", sequence=0, raw_size=1, data_base64="eA==")) if late else b""
                worker.sock.sendall(finish + tail)
                self.assertEqual(self.complete(task, "FAILED")["state"], "LOST")
                self.assertFalse(self.get(task)["attempts"][0]["logs_complete"])
                self.stop(self.proc)

    def test_worker_pauses_independently_for_missing_bytes_or_slots_and_resumes_exactly(self):
        for initial_bytes, initial_slots in ((1, 10), (10, 1)):
            with self.subTest(bytes=initial_bytes, slots=initial_slots):
                listener = socket.socket()
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind(("127.0.0.1", self.worker_port))
                listener.listen()
                listener.settimeout(4)
                config = dict(worker_id="credit-worker", controller=dict(address="127.0.0.1", port=self.worker_port),
                              capacity=dict(cpu_slots=1, memory_mb=0), max_concurrent_tasks=1)
                worker = self.spawn("worker", "--config", self.write_json("credit-worker.json", config))
                sock, _ = listener.accept()
                try:
                    sock.settimeout(4)
                    _, request, registration = receive(sock)
                    session = registration["session_id"]
                    sock.sendall(frame(0x0202, dict(accepted=True, heartbeat_interval_ms=50, offline_timeout_ms=1500,
                        termination_grace_ms=50, initial_log_window_bytes=initial_bytes, initial_log_window_chunks=initial_slots,
                        controller_instance_id=uid()), request))
                    task, attempt = uid(), uid()
                    sock.sendall(frame(0x0204, dict(worker_id="credit-worker", session_id=session, task_id=task, attempt_id=attempt,
                        attempt_no=1, executable=str(m7.BIN / "ts_log_workload"), args=["binary", "20", "0"], env={}, cwd=None,
                        resources=dict(cpu_slots=1, memory_mb=0), timeout_ms=None), 2))
                    logs = []
                    while not logs:
                        kind, _, payload = receive(sock)
                        if kind == 0x0207:
                            logs.append(payload)
                    self.assertEqual(logs[0]["raw_size"], initial_bytes)
                    # Grant ONLY the dimension that was not exhausted: this must
                    # not enable a pipe read, log frame or premature FINISH.
                    wrong = dict(grant_bytes=0, grant_chunks=1) if initial_bytes == 1 else dict(grant_bytes=1, grant_chunks=0)
                    sock.sendall(frame(0x0208, wrong))
                    end = time.monotonic() + 0.2
                    heartbeats = 0
                    while time.monotonic() < end:
                        kind, _, _ = receive(sock)
                        self.assertEqual(kind, 0x0203)
                        heartbeats += 1
                    self.assertGreater(heartbeats, 0)
                    sock.sendall(frame(0x0208, dict(grant_bytes=100, grant_chunks=100)))
                    while True:
                        kind, _, payload = receive(sock)
                        if kind == 0x0207:
                            logs.append(payload)
                        if kind == 0x0209:
                            self.assertEqual(payload["stdout_bytes"], 20)
                            self.assertEqual(payload["stdout_next_sequence"], len(logs))
                            break
                    self.assertEqual([p["sequence"] for p in logs], list(range(len(logs))))
                    self.assertEqual(b"".join(base64.b64decode(p["data_base64"]) for p in logs), expected(20))
                    self.stop(worker)
                finally:
                    sock.close()
                    listener.close()

    def test_delayed_write_failure_after_finish_retains_outcome_and_never_syncs_success(self):
        self.fault_controller("fail-write-gated")
        worker = self.peer(True)
        self.assertTrue(worker.register()[2]["accepted"])
        task = self.submit("/bin/true")
        _, request, run = receive(worker.sock)
        self.acknowledge(worker, run, request)
        binding = dict(worker_id=worker.worker, session_id=worker.session, task_id=task, attempt_id=run["attempt_id"])
        worker.sock.sendall(frame(0x0207, dict(**binding, stream="stdout", sequence=0, raw_size=1, data_base64="eA==")) +
            frame(0x0209, dict(**binding, outcome="EXITED", exit_code=7, term_signal=None, reason=None,
                stdout_next_sequence=1, stderr_next_sequence=0, stdout_bytes=1, stderr_bytes=0)))
        self.held()
        eventually(lambda: self.get(task)["attempts"][0]["finalization_pending"])
        self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 1)
        self.release()
        attempt = self.complete(task, "FAILED")
        self.assertEqual(attempt["reason"], "LOG_PERSISTENCE_ERROR")
        self.assertFalse(attempt["logs_complete"])
        self.assertIn('"exit_code":7', self.db("SELECT observed_outcome_json FROM attempts WHERE task_id=?", (task,))[0][0])

    def test_completion_notification_failure_fail_stops_and_restart_is_conservative(self):
        self.fault_controller("fail-notify")
        worker = self.worker("worker-a")
        task = self.submit("/bin/true")
        self.assertEqual(self.proc.wait(timeout=5), 1)
        self.stop(worker, expected=None)
        self.assertEqual(self.db("SELECT logs_complete FROM attempts WHERE task_id=?", (task,)), [(0,)])
        self.drop_gate()
        self.controller()
        attempt = self.get(task)["attempts"][0]
        self.assertEqual(attempt["state"], "LOST")
        self.assertFalse(attempt["logs_complete"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--case", required=True)
    args = parser.parse_args()
    m7.BIN = args.bin
    outcome = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite([M8Loopback(args.case)]))
    raise SystemExit(0 if outcome.wasSuccessful() else 1)
