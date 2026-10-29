"""M9 real binaries, TCP, ext4 logs, independent SQLite readers and armed gates."""
import argparse
import base64
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import time
import unittest
import m7_loopback as m7
from m7_loopback import Harness, eventually, receive, frame, uid
from m8_loopback import M8Loopback


class M9Loopback(Harness):
    db = M8Loopback.db
    complete = M8Loopback.complete
    acknowledge = M8Loopback.acknowledge
    raw_logs = M8Loopback.raw_logs
    fault_controller = M8Loopback.fault_controller
    drop_gate = M8Loopback.drop_gate

    def setUp(self):
        super().setUp()
        self.orphans = []

    def tearDown(self):
        # SIGKILL Worker tests deliberately leave a workload orphan. This harness
        # explicitly owns its cleanup; SIGKILL is not a Worker cleanup guarantee.
        for pid in self.orphans:
            try: os.killpg(pid, signal.SIGKILL)
            except ProcessLookupError: pass
            try: os.waitpid(pid, 0)
            except ChildProcessError: pass
        super().tearDown()

    def gated(self, mode):
        nr, nw = os.pipe()
        rr, rw = os.pipe()
        self.gate_read, self.gate_write = nr, rw
        self.proc = self.spawn("ts_m9_fault_controller", self.config_path, mode, nw, rr, pass_fds=(nw, rr))
        os.close(nw)
        os.close(rr)
        eventually(lambda: "READY client=" in self.proc.log_path.read_text() or
                   (self.proc.poll() is not None and self.fail(self.proc.log_path.read_text())))

    def arm(self):
        os.write(self.gate_write, b"A")

    def running(self, task):
        return eventually(lambda: (v if v["attempts"] and v["attempts"][-1]["state"] == "RUNNING" else None)
                          if (v := self.get(task)) else None)

    def raw_run(self, worker):
        task = self.submit("/bin/true")
        kind, request, run = receive(worker.sock)
        self.assertEqual(kind, 0x0204)
        self.acknowledge(worker, run, request)
        self.running(task)
        return task, run

    def finished(self, peer, run, outcome="EXITED", code=0):
        peer.sock.sendall(frame(0x0209, dict(worker_id=peer.worker, session_id=peer.session,
            task_id=run["task_id"], attempt_id=run["attempt_id"], outcome=outcome, exit_code=code,
            term_signal=None, reason=None, stdout_bytes=0, stderr_bytes=0,
            stdout_next_sequence=0, stderr_next_sequence=0)))

    def cancel_ack(self, peer, command, request, **overrides):
        ack = dict(worker_id=peer.worker, session_id=peer.session, attempt_id=command["attempt_id"],
                   accepted=True, already_terminal=False, message=None)
        ack.update(overrides)
        peer.sock.sendall(frame(0x020B, ack, request))

    def test_pending_cancel_commit_gate_idempotence_and_missing_conflicts(self):
        self.gated("hold-task")
        task = self.submit("/bin/true")
        self.arm()
        a, b = self.peer(), self.peer()
        a.sock.sendall(frame(0x0109, dict(task_id=task), 2))
        self.held()
        b.sock.sendall(frame(0x0109, dict(task_id=task), 2))
        self.assertFalse(select.select([a.sock, b.sock], [], [], 0.08)[0])
        self.assertEqual(self.db("SELECT state,state_version FROM tasks WHERE task_id=?", (task,)), [("PENDING", 0)])
        self.assertEqual(self.db("SELECT * FROM attempts"), [])
        self.release()
        self.assertEqual(receive(a.sock)[2]["status"], "completed")
        self.assertEqual(receive(b.sock)[2]["status"], "already_cancelled")
        self.assertEqual(self.cli("cancel", task)[0]["status"], "already_cancelled")
        self.assertEqual(self.cli("retry", task, code=2)[0]["code"], "CONFLICT")
        self.assertEqual(self.cli("cancel", uid(), code=2)[0]["code"], "NOT_FOUND")
        self.assertEqual(self.db("SELECT state_version FROM tasks WHERE task_id=?", (task,)), [(1,)])

    def test_active_cancel_commit_gate_receipt_not_terminal_and_duplicate(self):
        self.gated("hold-task")
        worker = self.peer(True)
        self.assertTrue(worker.register()[2]["accepted"])
        task, run = self.raw_run(worker)
        self.arm()
        a, b = self.peer(), self.peer()
        a.sock.sendall(frame(0x0109, dict(task_id=task), 3))
        self.held()
        b.sock.sendall(frame(0x0109, dict(task_id=task), 4))
        self.assertFalse(select.select([a.sock, b.sock, worker.sock], [], [], 0.08)[0])
        self.assertEqual(self.db("SELECT cancel_requested_at FROM tasks WHERE task_id=?", (task,)), [(None,)])
        self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 1)
        self.release()
        for client in (a, b):
            response = receive(client.sock)[2]
            self.assertEqual((response["status"], response["task_state"]), ("accepted", "ACTIVE"))
            self.assertIsNotNone(response["cancel_requested_at"])
        kind, request, command = receive(worker.sock)
        self.assertEqual(kind, 0x020A)
        self.assertEqual(command, dict(worker_id=worker.worker, session_id=worker.session, task_id=task,
                                      attempt_id=run["attempt_id"], grace_ms=50))
        self.cancel_ack(worker, command, request, already_terminal=True)
        self.assertEqual(self.get(task)["task"]["state"], "ACTIVE")
        self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 1)
        self.assertEqual(self.cli("cancel", task)[0]["status"], "accepted")
        self.assertFalse(select.select([worker.sock], [], [], 0.08)[0], "duplicate must not send CANCEL twice")
        self.finished(worker, run)
        self.assertTrue(self.complete(task)["logs_complete"], "ACK must not suppress authoritative natural FINISH")
        self.assertEqual(self.cli("cancel", task, code=2)[0]["code"], "CONFLICT")

    def test_cancel_unknown_failed_commit_never_escapes_on_wire(self):
        for mode in ("busy-task", "full-task", "ioerr-task", "unknown-task"):
            with self.subTest(mode=mode):
                self.gated(mode)
                worker = self.peer(True)
                worker.register()
                task, _ = self.raw_run(worker)
                client = self.peer()
                self.arm()
                client.sock.sendall(frame(0x0109, dict(task_id=task), 8))
                self.held()
                self.assertFalse(select.select([worker.sock, client.sock], [], [], 0.04)[0])
                self.release()
                self.assertEqual(self.proc.wait(timeout=5), 1)
                self.assertEqual(worker.sock.recv(1), b"")
                self.assertEqual(client.sock.recv(1), b"")
                intent = self.db("SELECT cancel_requested_at FROM tasks WHERE task_id=?", (task,))[0][0]
                self.assertEqual(intent is not None, mode.startswith("unknown"))
                self.drop_gate()
                self.controller()
                self.assertEqual(self.get(task)["attempts"][0]["state"], "LOST")
                self.stop(self.proc)

    def test_no_dispatch_assigned_proven_empty_and_possible_bytes_cancel_intent(self):
        self.gated("one-request")
        worker = self.peer(True)
        worker.register(cpu=2)
        first = self.submit("/bin/true")
        self.assertEqual(receive(worker.sock)[0], 0x0204)  # intentionally keep RUN ACK correlation occupied
        second = self.submit("/bin/true")
        eventually(lambda: self.get(second)["task"]["state"] == "ACTIVE")
        result = self.cli("cancel", second)[0]
        self.assertEqual((result["status"], result["task_state"]), ("completed", "CANCELLED"))
        attempt = self.get(second)["attempts"][0]
        self.assertTrue(attempt["logs_complete"])
        self.assertEqual((attempt["stdout_bytes"], attempt["stderr_bytes"]), (0, 0))
        self.assertFalse(select.select([worker.sock], [], [], 0.05)[0], "neither RUN nor CANCEL may escape")
        self.assertEqual(self.cli("cancel", first)[0]["status"], "accepted")
        self.assertEqual(receive(worker.sock)[0], 0x020A)

    def test_retry_commit_preserves_history_logs_and_unique_attempt_number(self):
        self.gated("hold-task")
        worker = self.worker("worker-a")
        task = self.submit(str(m7.BIN / "ts_runtime_test_helper"), "emit")
        self.complete(task)
        self.assertEqual(self.cli("retry", task, code=2)[0]["code"], "CONFLICT")
        failed = self.submit(str(m7.BIN / "ts_runtime_test_helper"), "exit", "7")
        self.complete(failed, "FAILED")
        before = self.db("SELECT * FROM attempts WHERE task_id=?", (failed,))
        self.stop(worker)
        eventually(lambda: self.workers()[0]["state"] == "OFFLINE")
        self.arm()
        a, b = self.peer(), self.peer()
        a.sock.sendall(frame(0x010B, dict(task_id=failed), 2))
        self.held()
        b.sock.sendall(frame(0x010B, dict(task_id=failed), 2))
        self.assertFalse(select.select([a.sock, b.sock], [], [], 0.04)[0])
        self.assertEqual(self.db("SELECT state FROM tasks WHERE task_id=?", (failed,)), [("FAILED",)])
        self.release()
        self.assertEqual(receive(a.sock)[2], dict(task_id=failed, state="PENDING", next_attempt_no=2))
        self.assertEqual(receive(b.sock)[2]["code"], "CONFLICT")
        self.assertEqual(self.db("SELECT * FROM attempts WHERE task_id=?", (failed,)), before)
        self.worker("worker-a")
        self.complete(failed, "FAILED")
        rows = self.db("SELECT attempt_id,attempt_no,stdout_path,stderr_path FROM attempts WHERE task_id=? ORDER BY attempt_no", (failed,))
        self.assertEqual([row[1] for row in rows], [1, 2])
        self.assertNotEqual(rows[0][0], rows[1][0])
        self.assertNotEqual(rows[0][2:], rows[1][2:])
        self.assertEqual(self.db("SELECT * FROM attempts WHERE task_id=? AND attempt_no=1", (failed,)), before)
        self.assertEqual(self.raw_logs(failed, "--attempt", "1", "--both"), b"")
        self.assertEqual(self.raw_logs(task, "--both"), b"out\0\xff\nerr\0\xfe\n")

    def test_client_disconnect_and_reused_request_cannot_steal_cancel_retry_response(self):
        self.gated("hold-task")
        task = self.submit("/bin/true")
        self.arm()
        old = self.peer()
        old.sock.sendall(frame(0x0109, dict(task_id=task), 1))
        self.held()
        old.close()
        fresh = self.peer()
        self.assertEqual(fresh.request(0x010D, {}, 1)[0], 0x010E)
        self.release()
        eventually(lambda: self.get(task)["task"]["state"] == "CANCELLED")
        self.assertFalse(select.select([fresh.sock], [], [], 0.05)[0])

    def test_bad_cancel_ack_correlations_and_duplicates_invalidate_not_terminalize(self):
        self.controller()
        for mutation in ("request", "attempt", "session", "worker", "duplicate", "fields"):
            with self.subTest(mutation=mutation):
                peer = self.peer(True)
                peer.register("worker-" + mutation, cpu=1)
                task, run = self.raw_run(peer)
                self.cli("cancel", task)
                kind, request, command = receive(peer.sock)
                self.assertEqual(kind, 0x020A)
                changes = {}
                if mutation == "attempt": changes["attempt_id"] = uid()
                if mutation == "session": changes["session_id"] = uid()
                if mutation == "worker": changes["worker_id"] = "wrong-worker"
                if mutation == "fields": changes.update(accepted=False, already_terminal=True)
                if mutation == "request": request += 999
                self.cancel_ack(peer, command, request, **changes)
                if mutation == "duplicate": self.cancel_ack(peer, command, request)
                attempt = self.complete(task, "FAILED")
                self.assertEqual(attempt["state"], "LOST")
                self.assertFalse(attempt["logs_complete"])

    def test_real_cancel_timeout_and_noncooperative_group_cleanup(self):
        self.controller()
        worker = self.worker("worker-a")
        baseline = len(list(Path(f"/proc/{worker.pid}/fd").iterdir()))
        for mode in ("cooperative", "ignore-term"):
            task = self.submit("--timeout", "3", str(m7.BIN / "ts_runtime_test_helper"), mode)
            self.running(task)
            eventually(lambda: self.raw_logs(task) == b"ready\n")
            pid = int(Path(f"/proc/{worker.pid}/task/{worker.pid}/children").read_text().strip())
            self.assertEqual(self.cli("cancel", task)[0]["status"], "accepted")
            attempt = self.complete(task, "CANCELLED")
            self.assertEqual(attempt["state"], "CANCELLED")
            self.assertTrue(attempt["logs_complete"])
            self.assertEqual(self.raw_logs(task), b"ready\n")
            self.assertFalse(Path(f"/proc/{pid}").exists())
            with self.assertRaises(ProcessLookupError): os.killpg(pid, 0)
        timed = self.submit("--timeout", "1", str(m7.BIN / "ts_runtime_test_helper"), "ignore-term")
        attempt = self.complete(timed, "FAILED")
        self.assertEqual(attempt["state"], "TIMED_OUT")
        self.assertEqual(attempt["term_signal"], signal.SIGKILL)
        self.assertTrue(attempt["logs_complete"])
        eventually(lambda: len(list(Path(f"/proc/{worker.pid}/fd").iterdir())) == baseline)
        self.assertEqual(Path(f"/proc/{worker.pid}/task/{worker.pid}/children").read_text().strip(), "")
        self.assertEqual(len(list(Path(f"/proc/{worker.pid}/task").iterdir())), 1)

    def test_cancel_during_held_final_barrier_keeps_natural_outcome_and_reservation(self):
        self.fault_controller("hold-file-out")
        self.worker("worker-a")
        task = self.submit("/bin/true")
        self.held()
        self.assertEqual(self.cli("cancel", task)[0]["status"], "accepted")
        self.assertEqual(self.get(task)["task"]["state"], "ACTIVE")
        self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 1)
        self.release()
        self.assertTrue(self.complete(task)["logs_complete"])
        self.assertEqual(self.cli("cancel", task, code=2)[0]["code"], "CONFLICT")

    def test_controller_crash_running_cleanup_before_fresh_registration_and_recovery(self):
        self.controller()
        pending = self.submit("--cpu", "100", "/bin/true")
        worker = self.worker("worker-a")
        old_session = self.workers()[0]["session_id"]
        terminal = self.submit("/bin/true")
        self.complete(terminal)
        task = self.submit(str(m7.BIN / "ts_runtime_test_helper"), "ignore-term")
        self.running(task)
        pid = int(Path(f"/proc/{worker.pid}/task/{worker.pid}/children").read_text().strip())
        self.proc.kill()
        self.proc.wait(timeout=3)
        eventually(lambda: not Path(f"/proc/{pid}").exists())
        with self.assertRaises(ProcessLookupError): os.killpg(pid, 0)
        self.controller()
        eventually(lambda: self.workers() and self.workers()[0]["state"] == "HEALTHY")
        self.assertNotEqual(self.workers()[0]["session_id"], old_session)
        self.assertEqual(self.get(task)["attempts"][0]["state"], "LOST")
        self.assertFalse(self.get(task)["attempts"][0]["logs_complete"])
        self.assertEqual(self.get(pending)["task"]["state"], "PENDING")
        self.assertEqual(self.get(terminal)["task"]["state"], "SUCCEEDED")
        self.assertEqual(self.workers()[0]["running_attempts"], 0)
        self.assertEqual(worker.poll(), None)
        self.assertEqual(Path(f"/proc/{worker.pid}/task/{worker.pid}/children").read_text().strip(), "")

    def test_deadlines_and_lost_commit_hold_reservations_and_replacement_ack(self):
        self.gated("hold-loss")
        old = self.peer(True)
        old.register("replace-worker", cpu=1)
        task, _ = self.raw_run(old)
        self.arm()
        replacement = self.peer(True)
        replacement.sock.sendall(frame(0x0201, dict(worker_id=old.worker, session_id=uid(), capacity=dict(cpu_slots=1, memory_mb=100),
            max_concurrent_tasks=1, worker_version="m9"), 1))
        self.held()
        old.heartbeat(999)  # cannot revive it OR close it before COMMIT
        self.assertFalse(select.select([replacement.sock], [], [], 0.05)[0])
        self.assertEqual(self.db("SELECT state FROM attempts WHERE task_id=?", (task,)), [("RUNNING",)])
        self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 1)
        self.assertEqual(old.sock.recv(1, socket.MSG_DONTWAIT) if select.select([old.sock], [], [], 0)[0] else None, None)
        replacement.close()
        self.release()
        self.assertEqual(old.sock.recv(1), b"")
        self.assertEqual(self.complete(task, "FAILED")["state"], "LOST")
        eventually(lambda: self.workers()[0]["state"] == "OFFLINE")
        for phase in ("ack", "start", "heartbeat"):
            p = self.peer(True)
            p.register("deadline-" + phase, cpu=1)
            t = self.submit("/bin/true")
            _, request, run = receive(p.sock)
            if phase == "start":
                p.sock.sendall(frame(0x0205, dict(worker_id=p.worker, session_id=p.session, attempt_id=run["attempt_id"],
                    accepted=True, error_code=None, message=None), request))
            elif phase == "heartbeat": self.acknowledge(p, run, request)
            attempt = self.complete(t, "FAILED")
            self.assertEqual(attempt["state"], "LOST")
            self.assertFalse(attempt["logs_complete"])

    def wire_worker(self, credit=65536):
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", self.worker_port))
        listener.listen()
        listener.settimeout(5)
        self.addCleanup(listener.close)
        config = dict(worker_id="wire-worker", controller=dict(address="127.0.0.1", port=self.worker_port),
            capacity=dict(cpu_slots=2, memory_mb=100), max_concurrent_tasks=2,
            reconnect=dict(initial_backoff_ms=20, max_backoff_ms=80))
        worker = self.spawn("worker", "--config", self.write_json("wire.json", config))
        def accept(allowed=True):
            sock, _ = listener.accept()
            self.addCleanup(sock.close)
            sock.settimeout(5)
            _, request, registration = receive(sock)
            ack = dict(accepted=True, heartbeat_interval_ms=40, offline_timeout_ms=1500,
                termination_grace_ms=40, initial_log_window_bytes=credit, initial_log_window_chunks=64,
                controller_instance_id=uid()) if allowed else dict(accepted=False, error_code="REGISTRATION_REJECTED", message="test bounded capacity")
            sock.sendall(frame(0x0202, ack, request))
            return sock, registration["session_id"]
        sock, session = accept()
        return worker, sock, session, accept

    def worker_run_payload(self, session, mode="ignore-term", timeout=None, executable=None):
        return dict(worker_id="wire-worker", session_id=session, task_id=uid(), attempt_id=uid(), attempt_no=1,
            executable=executable or str(m7.BIN / "ts_runtime_test_helper"), args=[mode], env={}, cwd=None,
            resources=dict(cpu_slots=1, memory_mb=0), timeout_ms=timeout)

    def wire_until(self, sock, kind):
        messages = []
        while not messages or messages[-1][0] != kind:
            messages.append(receive(sock))
        return messages

    def test_zero_credit_cancel_duplicate_timeout_first_and_natural_first_wire(self):
        worker, sock, session, accept = self.wire_worker(credit=1)
        for mode, timeout, expected in (("ignore-term", None, "CANCELLED"), ("ignore-term", 30, "TIMED_OUT"), ("emit", None, "EXITED")):
            run = self.worker_run_payload(session, mode, timeout)
            sock.sendall(frame(0x0204, run, 2))
            events = self.wire_until(sock, 0x0207)  # consumes the only byte credit
            pid = next(e[2]["pid"] for e in events if e[0] == 0x0206)
            if expected != "CANCELLED": eventually(lambda: not Path(f"/proc/{pid}").exists())
            cancel = {k: run[k] for k in ("worker_id", "session_id", "task_id", "attempt_id")}
            cancel["grace_ms"] = 25
            sock.sendall(frame(0x020A, cancel, 3) + frame(0x020A, cancel, 4))
            first = self.wire_until(sock, 0x020B)[-1]
            second = self.wire_until(sock, 0x020B)[-1]
            self.assertEqual((first[1], second[1]), (3, 4))
            self.assertTrue(first[2]["accepted"])
            self.assertEqual(first[2]["already_terminal"], expected != "CANCELLED")
            # No FINISH can escape while final pipe bytes have no credit.
            incoming = self.wire_until(sock, 0x0203)
            self.assertFalse(any(e[0] == 0x0209 for e in incoming))
            sock.sendall(frame(0x0208, dict(grant_bytes=65536, grant_chunks=64)))
            tail = self.wire_until(sock, 0x0209)
            finish = tail[-1][2]
            self.assertEqual(finish["outcome"], expected)
            logs = [e[2] for e in events + tail if e[0] == 0x0207]
            out = b"".join(base64.b64decode(e["data_base64"]) for e in logs if e["stream"] == "stdout")
            self.assertEqual(out, b"out\0\xff\n" if mode == "emit" else b"ready\n")
            self.assertEqual(finish["stdout_bytes"], len(out))
            sock.sendall(frame(0x020A, cancel, 5))
            self.assertTrue(self.wire_until(sock, 0x020B)[-1][2]["already_terminal"])
            # Fresh session resets unused credit without inventing negative grants.
            if expected != "EXITED":
                sock.close()
                sock, session = accept()
        self.stop(worker)

    def test_running_disconnect_churn_cleans_groups_before_register_and_bounds_fds(self):
        worker, sock, session, accept = self.wire_worker()
        baseline = len(list(Path(f"/proc/{worker.pid}/fd").iterdir()))
        sessions = {session}
        for index in range(5):
            run = self.worker_run_payload(session)
            sock.sendall(frame(0x0204, run, index + 2))
            events = self.wire_until(sock, 0x0207)
            pid = next(e[2]["pid"] for e in events if e[0] == 0x0206)
            if index % 2:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            sock.close()
            sock, session = accept()
            self.assertNotIn(session, sessions)
            sessions.add(session)
            self.assertFalse(Path(f"/proc/{pid}").exists(), "REGISTER must follow reap")
            with self.assertRaises(ProcessLookupError): os.killpg(pid, 0)
            self.assertEqual(Path(f"/proc/{worker.pid}/task/{worker.pid}/children").read_text().strip(), "")
            self.assertEqual(len(list(Path(f"/proc/{worker.pid}/fd").iterdir())), baseline)
            self.assertEqual(len(list(Path(f"/proc/{worker.pid}/task").iterdir())), 1)
        self.stop(worker)

    def test_retry_can_succeed_on_another_worker_preserving_exact_old_log_history(self):
        self.controller()
        first = self.worker("worker-a")
        marker = self.root / "allow-success"
        task = self.submit(str(m7.BIN / "ts_runtime_test_helper"), "fail-until-file", str(marker))
        self.complete(task, "FAILED")
        before = self.db("SELECT * FROM attempts WHERE task_id=?", (task,))
        self.stop(first)
        eventually(lambda: self.workers()[0]["state"] == "OFFLINE")
        self.assertEqual(self.cli("retry", task)[0]["next_attempt_no"], 2)
        self.assertEqual(self.get(task)["task"]["state"], "PENDING")
        marker.touch()
        self.worker("worker-b")
        self.complete(task)
        self.assertEqual([a["attempt_no"] for a in self.get(task)["attempts"]], [1, 2])
        self.assertEqual(self.db("SELECT worker_id FROM attempts WHERE task_id=? ORDER BY attempt_no", (task,)), [("worker-a",), ("worker-b",)])
        self.assertEqual(self.db("SELECT * FROM attempts WHERE task_id=? AND attempt_no=1", (task,)), before)
        for attempt in (1, 2):
            self.assertEqual(self.raw_logs(task, "--attempt", str(attempt), "--both"), b"attempt-output\nattempt-diagnostic\n")
        self.stop(self.proc)
        self.controller()
        self.assertEqual(self.get(task)["task"]["state"], "SUCCEEDED")
        self.assertEqual(self.raw_logs(task, "--attempt", "1"), b"attempt-output\n")

    def test_disconnected_retry_and_failed_unknown_retry_commit_never_auto_schedule(self):
        for mode in ("hold-task", "ioerr-task", "unknown-task"):
            with self.subTest(mode=mode):
                self.gated(mode)
                worker = self.worker("retry-worker")
                task = self.submit("/missing-m9-exec")
                self.complete(task, "FAILED")
                self.stop(worker)
                eventually(lambda: self.workers()[0]["state"] == "OFFLINE")
                before = self.db("SELECT * FROM attempts WHERE task_id=?", (task,))
                self.arm()
                old = self.peer()
                old.sock.sendall(frame(0x010B, dict(task_id=task), 1))
                self.held()
                old.close()
                fresh = self.peer()
                self.assertEqual(fresh.request(0x010D, {}, 1)[0], 0x010E)
                self.release()
                expected = "FAILED" if mode == "ioerr-task" else "PENDING"
                if mode != "hold-task":
                    self.assertEqual(self.proc.wait(timeout=5), 1)
                    self.controller()
                eventually(lambda: self.get(task)["task"]["state"] == expected)
                self.assertEqual(self.db("SELECT * FROM attempts WHERE task_id=?", (task,)), before)
                if mode == "hold-task": self.assertFalse(select.select([fresh.sock], [], [], 0.04)[0])
                self.stop(self.proc)
                self.drop_gate()

    def test_crash_before_and_after_cancel_intent_commit_recovers_lost_never_replays(self):
        for mode in ("hold-task", "after-task"):
            with self.subTest(mode=mode):
                self.gated(mode)
                peer = self.peer(True)
                peer.register("crash-worker")
                task, _ = self.raw_run(peer)
                client = self.peer()
                self.arm()
                client.sock.sendall(frame(0x0109, dict(task_id=task), 2))
                self.held()
                self.assertFalse(select.select([client.sock, peer.sock], [], [], 0.05)[0])
                intent = self.db("SELECT cancel_requested_at FROM tasks WHERE task_id=?", (task,))[0][0]
                self.assertEqual(intent is not None, mode == "after-task")
                self.proc.kill()
                self.proc.wait(timeout=3)
                self.drop_gate()
                self.controller()
                view = self.get(task)
                self.assertEqual(view["task"]["state"], "FAILED")
                self.assertEqual(view["attempts"][0]["state"], "LOST")
                self.assertFalse(view["attempts"][0]["logs_complete"])
                self.assertEqual(self.workers(), [])
                self.stop(self.proc)

    def test_killed_worker_lost_has_no_automatic_retry_and_new_session_is_empty(self):
        # Adopt the intentionally orphaned direct workload in THIS TEST process so
        # cleanup can reap it even on hosts whose PID 1 does not reap promptly.
        import ctypes
        self.assertEqual(ctypes.CDLL(None).prctl(36, 1, 0, 0, 0), 0)  # PR_SET_CHILD_SUBREAPER
        self.controller()
        worker = self.worker("kill-worker")
        previous = self.workers()[0]["session_id"]
        task = self.submit(str(m7.BIN / "ts_runtime_test_helper"), "ignore-term")
        self.running(task)
        pid = int(Path(f"/proc/{worker.pid}/task/{worker.pid}/children").read_text().strip())
        self.orphans.append(pid)
        worker.kill()
        worker.wait(timeout=3)
        self.assertEqual(self.complete(task, "FAILED")["state"], "LOST")
        self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 0)
        self.worker("kill-worker")
        self.assertNotEqual(self.workers()[0]["session_id"], previous)
        self.assertEqual(len(self.get(task)["attempts"]), 1)
        self.assertEqual(self.workers()[0]["running_attempts"], 0)

    def test_real_multiworker_projected_choice_and_pending_registration_trigger(self):
        self.controller()
        pending = self.submit("/bin/true")
        self.assertEqual(self.get(pending)["task"]["state"], "PENDING")
        first = self.peer(True)
        first.register("worker-a", cpu=2)
        _, request, run = receive(first.sock)
        self.acknowledge(first, run, request)
        self.finished(first, run)
        self.complete(pending)
        second = self.peer(True)
        second.register("worker-b", cpu=4)
        task = self.submit("/bin/true")
        kind, _, run = receive(second.sock)
        self.assertEqual((kind, run["task_id"]), (0x0204, task))
        self.assertFalse(select.select([first.sock], [], [], 0.03)[0])
        # Projected b=(1+1)/4 ties a=(0+1)/2; lexical worker-a wins.
        next_task = self.submit("/bin/true")
        self.assertEqual(receive(first.sock)[2]["task_id"], next_task)

    def test_launch_setup_cancel_race_and_stale_worker_command_are_fenced(self):
        worker, sock, session, _ = self.wire_worker()
        run = self.worker_run_payload(session, executable="/missing-m9-exec")
        cancel = {k: run[k] for k in ("worker_id", "session_id", "task_id", "attempt_id")}
        cancel["grace_ms"] = 25
        # Coalescing keeps setup/cancel before the paced runtime poll.
        sock.sendall(frame(0x0204, run, 2) + frame(0x020A, cancel, 3))
        events = self.wire_until(sock, 0x0209)
        self.assertTrue(next(e[2]["accepted"] for e in events if e[0] == 0x020B))
        self.assertIn(events[-1][2]["outcome"], ("CANCELLED", "LAUNCH_FAILED"))
        self.assertFalse(any(e[0] == 0x0206 for e in events))
        run = self.worker_run_payload(session)
        sock.sendall(frame(0x0204, run, 4))
        events = self.wire_until(sock, 0x0207)
        pid = next(e[2]["pid"] for e in events if e[0] == 0x0206)
        cancel = {k: run[k] for k in ("worker_id", "session_id", "task_id", "attempt_id")}
        cancel.update(grace_ms=25, task_id=uid())
        sock.sendall(frame(0x020A, cancel, 5))
        self.assertEqual(worker.wait(timeout=5), 1)
        self.assertFalse(Path(f"/proc/{pid}").exists())
        self.assertIn("CANCEL Task mismatch", worker.log_path.read_text())

    def test_partial_cancel_send_is_lost_not_cancelled_and_late_valid_ack_is_receipt(self):
        self.gated("partial-cancel")
        peer = self.peer(True)
        peer.register("partial-worker")
        task, _ = self.raw_run(peer)
        self.assertEqual(self.cli("cancel", task)[0]["status"], "accepted")
        received = bytearray()
        while piece := peer.sock.recv(64): received.extend(piece)
        self.assertEqual(len(received), 7)
        self.assertEqual(self.complete(task, "FAILED")["state"], "LOST")
        self.assertIsNotNone(self.db("SELECT cancel_requested_at FROM tasks WHERE task_id=?", (task,))[0][0])
        self.stop(self.proc)
        self.drop_gate()
        self.controller()
        peer = self.peer(True)
        peer.register("late-worker")
        task, run = self.raw_run(peer)
        self.cli("cancel", task)
        _, request, command = receive(peer.sock)
        self.finished(peer, run)
        self.complete(task)
        self.cancel_ack(peer, command, request, already_terminal=True)
        peer.heartbeat(1)
        self.assertEqual(self.workers()[0]["state"], "HEALTHY")
        self.assertEqual(self.get(task)["task"]["state"], "SUCCEEDED")

    def test_stalled_log_path_cancel_control_two_workers_and_exact_cancel_tail(self):
        self.config["queues"] = dict(log_bytes=131072, log_commands=24, log_control_reserve=4,
            initial_session_log_window_bytes=65536, initial_session_log_window_chunks=8)
        self.config_path = self.write_json("controller.json", self.config)
        self.fault_controller("hold-write")
        a = self.worker("worker-a")
        b = self.worker("worker-b")
        task = self.submit(str(m7.BIN / "ts_runtime_test_helper"), "continuous")
        self.held()
        other = self.submit(str(m7.BIN / "ts_log_workload"), "binary", "50000", "50000")
        self.assertEqual(self.cli("cancel", task)[0]["status"], "accepted")
        self.assertEqual(self.get(task)["task"]["state"], "ACTIVE")
        self.assertEqual([w["state"] for w in self.workers()], ["HEALTHY", "HEALTHY"])
        self.assertEqual(len(list(Path(f"/proc/{self.proc.pid}/task").iterdir())), 3)
        self.release()
        attempt = self.complete(task, "CANCELLED")
        self.assertTrue(attempt["logs_complete"])
        raw = self.raw_logs(task)
        self.assertGreater(len(raw), 0)
        self.assertEqual(raw, b"Z" * len(raw))
        self.assertEqual(attempt["stdout_bytes"], len(raw))
        self.complete(other)
        from m8_loopback import expected
        self.assertEqual(self.raw_logs(other, "--both"), expected(50000) + expected(50000, 0xa5))
        for worker in (a, b):
            eventually(lambda: not Path(f"/proc/{worker.pid}/task/{worker.pid}/children").read_text().strip())

    def test_protocol_invalidation_keeps_socket_until_lost_commit_and_fences_late_packets(self):
        self.gated("hold-loss")
        peer = self.peer(True)
        peer.register()
        task, run = self.raw_run(peer)
        self.arm()
        peer.heartbeat(1, session=uid())  # wrong UUID, not physical EOF
        self.held()
        self.assertFalse(select.select([peer.sock], [], [], 0.05)[0])
        self.assertEqual(self.db("SELECT state FROM attempts WHERE task_id=?", (task,)), [("RUNNING",)])
        self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 1)
        self.finished(peer, run)  # reliable-looking old result must be quarantined
        peer.heartbeat(999)
        self.assertFalse(select.select([peer.sock], [], [], 0.05)[0])
        self.release()
        self.assertEqual(peer.sock.recv(1), b"")
        self.assertEqual(self.complete(task, "FAILED")["state"], "LOST")
        self.assertEqual(self.workers()[0]["state"], "OFFLINE")

    def test_cancelled_log_namespace_and_file_sync_failures_never_claim_complete(self):
        for mode in ("fail-create", "fail-file-out", "fail-file-err", "fail-dir-task", "fail-dir-tasks", "fail-dir-root"):
            with self.subTest(mode=mode):
                self.fault_controller(mode)
                worker = self.worker("worker-a")
                task = self.submit("/bin/sleep", "60")
                self.running(task)
                self.cli("cancel", task)
                attempt = self.complete(task, "FAILED")
                self.assertFalse(attempt["logs_complete"])
                self.assertEqual(attempt["reason"], "LOG_PERSISTENCE_ERROR")
                diagnostic = self.db("SELECT observed_outcome_json FROM attempts WHERE task_id=?", (task,))[0][0]
                self.assertEqual(json.loads(diagnostic)["outcome"], 3)  # ObservedOutcome::Cancelled (internal diagnostic)
                self.stop(worker)
                self.stop(self.proc)
                self.drop_gate()
                self.config["data_dir"] = str(self.root / mode)
                self.config_path = self.write_json("controller.json", self.config)

    def test_idle_reconnect_registration_rejection_backoff_resets_heartbeat_and_credit(self):
        worker, sock, session, accept = self.wire_worker()
        baseline = len(list(Path(f"/proc/{worker.pid}/fd").iterdir()))
        sessions = {session}
        for allowed, minimum in ((False, 0.01), (False, 0.025), (True, 0.06)):
            start = time.monotonic()
            sock.close()
            sock, session = accept(allowed)
            self.assertGreaterEqual(time.monotonic() - start, minimum)
            self.assertNotIn(session, sessions)
            sessions.add(session)
        heartbeat = self.wire_until(sock, 0x0203)[-1][2]
        self.assertEqual((heartbeat["sequence"], heartbeat["running_attempt_ids"]), (1, []))
        self.assertEqual(len(list(Path(f"/proc/{worker.pid}/fd").iterdir())), baseline)
        self.assertEqual(Path(f"/proc/{worker.pid}/task/{worker.pid}/children").read_text().strip(), "")
        self.stop(worker)

    def test_assignment_before_after_commit_crash_has_no_run_and_exact_recovery(self):
        for mode in ("hold-task", "after-task"):
            with self.subTest(mode=mode):
                self.config["data_dir"] = str(self.root / ("assignment-" + mode))
                self.config_path = self.write_json("controller.json", self.config)
                self.gated(mode)
                task = self.submit("/bin/true")
                self.arm()
                peer = self.peer(True)
                self.assertTrue(peer.register("assignment-worker")[2]["accepted"])
                self.held()
                self.assertFalse(select.select([peer.sock], [], [], 0.05)[0])
                expected = "PENDING" if mode == "hold-task" else "ACTIVE"
                self.assertEqual(self.db("SELECT state FROM tasks WHERE task_id=?", (task,)), [(expected,)])
                self.assertEqual(len(self.db("SELECT * FROM attempts WHERE task_id=?", (task,))), 0 if expected == "PENDING" else 1)
                self.proc.kill()
                self.proc.wait(timeout=3)
                self.drop_gate()
                self.controller()
                view = self.get(task)
                self.assertEqual(view["task"]["state"], "PENDING" if expected == "PENDING" else "FAILED")
                if expected == "PENDING": self.assertEqual(view["attempts"], [])
                else:
                    self.assertEqual(view["attempts"][0]["state"], "LOST")
                    self.assertFalse(view["attempts"][0]["logs_complete"])
                self.assertEqual(self.workers(), [])
                self.stop(self.proc)

    def test_cancel_terminal_before_after_commit_crash_does_not_infer_from_synced_files(self):
        for mode in ("terminal-commit", "terminal-after"):
            with self.subTest(mode=mode):
                self.fault_controller(mode)
                worker = self.worker("terminal-worker")
                task = self.submit(str(m7.BIN / "ts_runtime_test_helper"), "cooperative")
                self.running(task)
                eventually(lambda: self.raw_logs(task) == b"ready\n")
                self.assertEqual(self.cli("cancel", task)[0]["status"], "accepted")
                self.held()
                committed = mode == "terminal-after"
                self.assertEqual(self.db("SELECT state FROM tasks WHERE task_id=?", (task,)), [("CANCELLED" if committed else "ACTIVE",)])
                self.assertEqual(self.db("SELECT logs_complete FROM attempts WHERE task_id=?", (task,)), [(int(committed),)])
                self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 1, "completion has not released memory yet")
                self.assertEqual(Path(f"/proc/{worker.pid}/task/{worker.pid}/children").read_text().strip(), "")
                self.proc.kill()
                self.proc.wait(timeout=3)
                self.drop_gate()
                self.controller()
                view = self.get(task)
                self.assertEqual(view["task"]["state"], "CANCELLED" if committed else "FAILED")
                self.assertEqual(view["attempts"][0]["state"], "CANCELLED" if committed else "LOST")
                self.assertEqual(view["attempts"][0]["logs_complete"], committed)
                if committed: self.assertEqual(self.raw_logs(task), b"ready\n")
                eventually(lambda: self.workers() and self.workers()[0]["state"] == "HEALTHY")
                self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 0)
                self.stop(worker)
                self.stop(self.proc)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--case", required=True)
    args = parser.parse_args()
    m7.BIN = args.bin
    result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite([M9Loopback(args.case)]))
    raise SystemExit(0 if result.wasSuccessful() else 1)
