"""Admission and wait acceptance: real cluster plus bounded raw Controller peers."""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import threading
import time
import unittest
import m7_loopback as m7
from m7_loopback import Harness, eventually, frame, receive, uid
from m8_loopback import M8Loopback


def task_view(task, state, history=False):
    now = "2026-11-04T00:00:00.000000Z"
    attempt = uid()
    view = dict(task=dict(task_id=task, state=state, executable="/bin/true", args=[], env={}, cwd=None,
        resources=dict(cpu_slots=1, memory_mb=0), timeout_ms=None, cancel_requested_at=None,
        created_at=now, updated_at=now, latest_attempt_id=attempt if history else None), attempts=[])
    if history:
        view["attempts"] = [dict(attempt_id=attempt, task_id=task, attempt_no=1, state="FAILED",
            worker_id="history", session_id=uid(), assigned_at=now, started_at=now, finished_at=now,
            exit_code=1, term_signal=None, reason="old failure", stdout_bytes=0, stderr_bytes=0,
            logs_complete=True, finalization_pending=False)]
    return view


class RawController:
    def __init__(self, handler):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0)); self.listener.listen(1); self.listener.settimeout(5)
        self.port = self.listener.getsockname()[1]
        self.handler = handler; self.errors = []; self.requests = []
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        try:
            with self.listener.accept()[0] as peer:
                peer.settimeout(5)
                self.handler(self, peer)
        except Exception as error:
            self.errors.append(error)
        finally:
            self.listener.close()

    def get(self, peer):
        value = receive(peer); self.requests.append(value); return value

    def close(self):
        self.thread.join(6)
        if self.thread.is_alive():
            self.listener.close()
            raise AssertionError("raw Controller thread did not finish")
        if self.errors:
            raise AssertionError(self.errors)


class AdmissionWait(Harness):
    db = M8Loopback.db
    complete = M8Loopback.complete
    raw_logs = M8Loopback.raw_logs
    fault_controller = M8Loopback.fault_controller

    def command(self, *args, port=None, timeout=8):
        return subprocess.run([str(m7.BIN / "taskctl"), "--port", str(port or self.client_port), *args],
                              capture_output=True, text=True, timeout=timeout)

    def wait_process(self, task, *args):
        proc = subprocess.Popen([str(m7.BIN / "taskctl"), "--port", str(self.client_port), "wait", task, *args],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: (proc.kill(), proc.communicate()) if proc.poll() is None else None)
        return proc

    def test_rejection_plain_json_zero_and_joint_capacity_leave_no_rows(self):
        self.controller()
        for args in [("submit", "/bin/true"), ("--json", "submit", "/bin/true")]:
            result = self.command(*args); self.assertEqual(result.returncode, 3)
            if args[0] == "--json":
                response = json.loads(result.stdout)
                self.assertEqual(response["code"], "INVALID_ARGUMENT")
                self.assertEqual(response["details"], dict(reason="NO_KNOWN_WORKERS",
                    requested_cpu_slots=1, requested_memory_mb=0, known_worker_count=0))
                self.assertFalse(response["retryable"])
            else:
                self.assertEqual(result.stdout, ""); self.assertIn("No known", result.stderr)
        a = self.peer(True)
        a.request(0x0201, dict(worker_id="cpu-node", session_id=uid(), capacity=dict(cpu_slots=8, memory_mb=50),
                             max_concurrent_tasks=8, worker_version="admission-test"), 1)
        b = self.peer(True); b.register("small-node", cpu=2)
        for _ in range(10):
            reply = self.cli("submit", "--cpu", "4", "--memory", "80", "/bin/true", code=3)[0]
            self.assertEqual(reply["details"]["reason"], "NO_KNOWN_WORKER_FITS")
            self.assertEqual(reply["details"]["known_worker_count"], 2)
        self.assertEqual(self.db("SELECT count(*) FROM tasks"), [(0,)])
        self.assertEqual(self.db("SELECT count(*) FROM attempts"), [(0,)])
        self.assertTrue(all(w["reserved"]["cpu_slots"] == 0 for w in self.workers()))

    def test_offline_capacity_accepts_and_restart_forgets_advertisements(self):
        self.controller()
        peer = self.peer(True); peer.register(cpu=2); peer.close()
        eventually(lambda: self.workers()[0]["state"] == "OFFLINE")
        task = self.submit("--cpu", "2", "--memory", "100", "/bin/true")
        self.assertEqual(self.get(task)["task"]["state"], "PENDING")
        self.assertEqual(self.get(task)["attempts"], [])
        self.stop(self.proc); self.controller()
        self.assertEqual(self.workers(), [])
        self.assertEqual(self.get(task)["task"]["state"], "PENDING")
        self.assertEqual(self.command("submit", "/bin/true").returncode, 3)
        self.worker("raw-worker", cpu=2)
        self.assertTrue(self.complete(task)["logs_complete"])

    def test_retry_rejection_preserves_every_persisted_field(self):
        self.controller(); worker = self.worker("worker-a", cpu=2)
        task = self.submit("--cpu", "2", "--memory", "50", "/bin/false"); self.complete(task, "FAILED")
        self.stop(worker); self.stop(self.proc); self.controller()
        before = self.db("SELECT * FROM tasks"), self.db("SELECT * FROM attempts")
        rejected = self.command("--json", "retry", task)
        self.assertEqual(rejected.returncode, 3)
        self.assertEqual(json.loads(rejected.stdout)["details"]["reason"], "NO_KNOWN_WORKERS")
        self.assertEqual((self.db("SELECT * FROM tasks"), self.db("SELECT * FROM attempts")), before)
        self.assertEqual(self.command("retry", uid()).returncode, 2)
        small = self.peer(True); small.register("too-small", cpu=1)
        rejected = self.command("--json", "retry", task)
        self.assertEqual(rejected.returncode, 3)
        self.assertEqual(json.loads(rejected.stdout)["details"]["reason"], "NO_KNOWN_WORKER_FITS")
        self.assertEqual((self.db("SELECT * FROM tasks"), self.db("SELECT * FROM attempts")), before)
        small.close()
        self.worker("worker-a", cpu=2)
        self.assertEqual(self.command("retry", task).returncode, 0)
        self.complete(task, "FAILED")
        self.assertEqual(self.db("SELECT count(*) FROM attempts WHERE task_id=?", (task,)), [(2,)])

    def test_busy_pending_wait_timeout_and_interrupt_never_cancel(self):
        self.controller(); self.worker("worker-a")
        blocker = self.submit("/bin/sleep", "30")
        eventually(lambda: self.get(blocker)["attempts"][-1]["state"] == "RUNNING")
        task = self.submit("/bin/true")
        self.assertEqual(self.get(task)["task"]["state"], "PENDING")
        start = time.monotonic()
        result = self.command("wait", task, "--timeout", "1")
        self.assertEqual(result.returncode, 124); self.assertEqual(result.stdout, "")
        self.assertLess(time.monotonic() - start, 2)
        self.assertIsNone(self.get(task)["task"]["cancel_requested_at"])
        proc = self.wait_process(task)
        time.sleep(0.1); proc.send_signal(signal.SIGINT); proc.communicate(timeout=3)
        self.assertEqual(proc.returncode, -signal.SIGINT)
        self.assertEqual(self.get(task)["task"]["state"], "PENDING")
        cancelled = self.submit("/bin/true"); self.cli("cancel", cancelled)
        result = self.command("wait", cancelled)
        self.assertEqual(result.returncode, 1); self.assertIn("task_state=CANCELLED", result.stdout)
        self.assertNotIn("latest_recorded_attempt_id", result.stdout)
        self.cli("cancel", blocker); self.complete(blocker, "CANCELLED")
        result = self.command("--json", "wait", task)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(len(result.stdout.splitlines()), 1)
        self.assertEqual(json.loads(result.stdout)["task"]["state"], "SUCCEEDED")

    def test_terminal_outcomes_text_json_and_existing_terminal(self):
        self.controller(); self.worker("worker-a")
        for args, state, attempt_state in [
                (("/bin/true",), "SUCCEEDED", "SUCCEEDED"),
                (("/bin/false",), "FAILED", "FAILED"),
                (("--timeout", "1", "/bin/sleep", "30"), "FAILED", "TIMED_OUT")]:
            task = self.submit(*args)
            result = self.command("wait", task, "--timeout", "5")
            self.assertEqual(result.returncode, 0 if state == "SUCCEEDED" else 1, result.stderr)
            self.assertIn("task_state=" + state, result.stdout)
            self.assertIn("latest_recorded_attempt_id=", result.stdout)
            self.assertIn("attempt_state=" + attempt_state, result.stdout)
            start = time.monotonic()
            again = self.command("--json", "wait", task)
            self.assertEqual(again.returncode, result.returncode)
            self.assertLess(time.monotonic() - start, 0.8)
            self.assertEqual(json.loads(again.stdout)["task"]["state"], state)
        active = self.submit("/bin/sleep", "30")
        eventually(lambda: self.get(active)["task"]["state"] == "ACTIVE")
        proc = self.wait_process(active, "--timeout", "5")
        self.cli("cancel", active)
        out, err = proc.communicate(timeout=6)
        self.assertEqual(proc.returncode, 1, err); self.assertIn("task_state=CANCELLED", out)

    def test_durability_gates_hold_wait_until_terminal_commit(self):
        for mode in ("hold-file-out", "terminal-commit"):
            self.fault_controller(mode); worker = self.worker("worker-a")
            task = self.submit("/bin/true"); self.held()
            proc = self.wait_process(task, "--timeout", "5")
            time.sleep(0.15); self.assertIsNone(proc.poll())
            self.assertEqual(self.db("SELECT state FROM tasks WHERE task_id=?", (task,)), [("ACTIVE",)])
            self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 1)
            self.release()
            out, err = proc.communicate(timeout=6)
            self.assertEqual(proc.returncode, 0, err); self.assertIn("logs_complete=true", out)
            self.stop(worker); self.stop(self.proc)
            os.close(self.gate_write); os.close(self.gate_read); del self.gate_write

    def test_lost_is_failed_with_incomplete_logs(self):
        self.controller()
        peer = self.peer(True); peer.register()
        task = self.submit("/bin/true")
        _, _, run = receive(peer.sock)
        proc = self.wait_process(task, "--timeout", "5")
        peer.close()
        out, err = proc.communicate(timeout=6)
        self.assertEqual(proc.returncode, 1, err)
        self.assertIn("attempt_state=LOST", out); self.assertIn("logs_complete=false", out)
        self.assertEqual(self.get(task)["attempts"][-1]["attempt_id"], run["attempt_id"])

    def test_wait_argument_validation_not_found_and_connection_error(self):
        for args in [("wait",), ("wait", "bad-id"), ("wait", uid(), "--timeout", "0"),
                     ("wait", uid(), "--timeout", "-1"), ("wait", uid(), "--timeout", "18446744073709551615"),
                     ("wait", uid(), "--timeout"), ("wait", uid(), "--other")]:
            self.assertEqual(self.command(*args).returncode, 2)
        self.controller()
        result = self.command("--json", "wait", uid())
        self.assertEqual(result.returncode, 2); self.assertEqual(json.loads(result.stdout)["code"], "NOT_FOUND")
        self.stop(self.proc)
        result = self.command("wait", uid(), "--timeout", "3")
        self.assertEqual(result.returncode, 2); self.assertEqual(result.stdout, "")

    def test_raw_pending_history_polling_and_response_identity(self):
        task = uid()
        def handler(server, peer):
            view = task_view(task, "PENDING", history=True)
            for state in ("PENDING", "CANCELLED"):
                kind, request, payload = server.get(peer)
                self.assertEqual(kind, 0x0103); self.assertEqual(payload["task_id"], task)
                view["task"]["state"] = state
                peer.sendall(frame(0x0104, view, request))
        server = RawController(handler)
        try:
            result = self.command("--json", "wait", task, port=server.port)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(result.stdout)["task"]["state"], "CANCELLED")
            self.assertEqual(len(server.requests), 2)
        finally: server.close()
        def wrong(server, peer):
            _, request, _ = server.get(peer)
            peer.sendall(frame(0x0104, task_view(uid(), "SUCCEEDED"), request))
        server = RawController(wrong)
        try:
            result = self.command("wait", task, port=server.port)
            self.assertEqual(result.returncode, 2); self.assertEqual(result.stdout, "")
        finally: server.close()

    def test_raw_stalled_query_deadline_close_and_error_reason_types(self):
        def stalled(server, peer):
            server.get(peer)
            peer.recv(1)  # client closes at its whole deadline
        server = RawController(stalled)
        try:
            start = time.monotonic()
            result = self.command("wait", uid(), "--timeout", "1", port=server.port)
            self.assertEqual(result.returncode, 124); self.assertLess(time.monotonic() - start, 2)
        finally: server.close()
        def closed(server, peer): server.get(peer)
        server = RawController(closed)
        try:
            self.assertEqual(self.command("wait", uid(), port=server.port).returncode, 2)
        finally: server.close()
        server = RawController(closed)
        try:
            result = self.command("submit", "/bin/true", port=server.port)
            self.assertEqual(result.returncode, 1); self.assertEqual(result.stdout, "")
            self.assertIn("unknown", result.stderr)
        finally: server.close()
        for reason, code in [(None, 2), (42, 2), ({}, 2), (True, 2), ("other", 2),
                             ("NO_KNOWN_WORKERS", 3), ("NO_KNOWN_WORKER_FITS", 3)]:
            def error(server, peer):
                _, request, _ = server.get(peer)
                peer.sendall(frame(1, dict(code="INVALID_ARGUMENT", message="diagnostic", retryable=False,
                                           details=dict(reason=reason)), request))
            server = RawController(error)
            try:
                result = self.command("--json", "submit", "/bin/true", port=server.port)
                self.assertEqual(result.returncode, code)
                self.assertEqual(json.loads(result.stdout)["details"]["reason"], reason)
            finally: server.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True); parser.add_argument("--case", required=True)
    args = parser.parse_args(); m7.BIN = args.bin
    result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite([AdmissionWait(args.case)]))
    raise SystemExit(0 if result.wasSuccessful() else 1)
