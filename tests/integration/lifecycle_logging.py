"""Real application diagnostics; accepted M7/M8 ownership and fault-gate helpers."""
import argparse
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import subprocess
import unittest
import m7_loopback as m7
from m7_loopback import Harness, eventually
from m8_loopback import M8Loopback, expected


class LifecycleLogging(Harness):
    db = M8Loopback.db
    complete = M8Loopback.complete
    raw_logs = M8Loopback.raw_logs
    fault_controller = M8Loopback.fault_controller

    def events(self, proc, task=None):
        rows = []
        for line in proc.log_path.read_text().splitlines():
            match = re.search(r"\bevent=(TASK_ACCEPTED|TASK_ASSIGNED|ATTEMPT_RUNNING|TASK_TERMINAL|EXEC_STARTED|EXEC_FINISHED)\b", line)
            if match:
                row = dict(part.split("=", 1) for part in shlex.split(line[match.start():]))
                if task is None or row["task_id"] == task:
                    rows.append(row)
        return rows

    def terminal_event(self, task):
        return eventually(lambda: next((r for r in self.events(self.proc, task)
                                        if r["event"] == "TASK_TERMINAL"), None))

    def test_normal_correlated_pid_and_committed_versions(self):
        self.controller()
        worker = self.worker("worker-a")
        task = self.submit("/bin/sleep", "0.4")
        started = eventually(lambda: next((r for r in self.events(worker, task)
                                          if r["event"] == "EXEC_STARTED"), None))
        pid = int(started["pid"])
        self.assertEqual(Path(f"/proc/{pid}/exe").resolve(), Path("/usr/bin/sleep"))
        self.assertEqual(int(Path(f"/proc/{pid}/stat").read_text().split(") ")[1].split()[1]), worker.pid)
        attempt = self.complete(task)
        terminal = self.terminal_event(task)
        controller = self.events(self.proc, task)
        self.assertEqual([r["event"] for r in controller],
                         ["TASK_ACCEPTED", "TASK_ASSIGNED", "ATTEMPT_RUNNING", "TASK_TERMINAL"])
        self.assertEqual(controller[2]["task_state"], "ACTIVE")
        self.assertEqual(controller[2]["attempt_state"], "RUNNING")
        self.assertEqual(terminal["reservation"], "released")
        self.assertEqual(terminal["logs_complete"], "true")
        self.assertEqual(terminal["attempt_id"], attempt["attempt_id"])
        self.assertEqual(started["session_id"], controller[1]["session_id"])
        self.assertEqual(started["attempt_id"], controller[1]["attempt_id"])
        self.assertEqual(started["worker_id"], controller[1]["worker_id"])
        self.assertEqual(self.db("SELECT state_version FROM tasks WHERE task_id=?", (task,)),
                         [(int(terminal["task_version"]),)])
        self.assertEqual(self.db("SELECT state_version FROM attempts WHERE task_id=?", (task,)),
                         [(int(terminal["attempt_version"]),)])
        finished = eventually(lambda: next((r for r in self.events(worker, task)
                                           if r["event"] == "EXEC_FINISHED"), None))
        self.assertEqual(finished["pid"], started["pid"])
        self.assertEqual(finished["outcome"], "EXITED")
        self.assertEqual(finished["exit_code"], "0")
        self.assertFalse(Path(f"/proc/{pid}").exists())

    def test_terminal_waits_for_file_barrier_and_metadata_commit(self):
        for mode in ("hold-file-out", "terminal-commit"):
            with self.subTest(mode=mode):
                self.fault_controller(mode)
                worker = self.worker("worker-a")
                task = self.submit("/bin/true")
                self.held()
                self.assertFalse(any(r["event"] == "TASK_TERMINAL" for r in self.events(self.proc, task)))
                self.assertEqual(self.db("SELECT state FROM tasks WHERE task_id=?", (task,)), [("ACTIVE",)])
                self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 1)
                self.release()
                self.assertTrue(self.complete(task)["logs_complete"])
                self.assertEqual(self.terminal_event(task)["reservation"], "released")
                self.assertEqual(self.workers()[0]["reserved"]["cpu_slots"], 0)
                self.stop(worker); self.stop(self.proc)
                os.close(self.gate_write); os.close(self.gate_read); del self.gate_write

    def test_cancel_timeout_and_launch_failure_remain_distinct(self):
        self.controller()
        worker = self.worker("worker-a")
        cancel = self.submit("/bin/sleep", "30")
        eventually(lambda: any(r["event"] == "ATTEMPT_RUNNING" for r in self.events(self.proc, cancel)))
        self.assertEqual(self.cli("cancel", cancel)[0]["status"], "accepted")
        self.complete(cancel, "CANCELLED")
        timed = self.submit("--timeout", "1", "/bin/sleep", "30")
        self.assertEqual(self.complete(timed, "FAILED")["state"], "TIMED_OUT")
        failed = self.submit("/missing-lifecycle-executable")
        self.complete(failed, "FAILED")
        for task, outcome in ((cancel, "CANCELLED"), (timed, "TIMED_OUT"), (failed, "LAUNCH_FAILED")):
            row = eventually(lambda: next((r for r in self.events(worker, task)
                                          if r["event"] == "EXEC_FINISHED"), None))
            self.assertEqual(row["outcome"], outcome)
            self.assertNotIn("durable", row)
            self.assertEqual(self.terminal_event(task)["logs_complete"], "true")
        self.assertFalse(any(r["event"] == "EXEC_STARTED" for r in self.events(worker, failed)))

    def test_disconnect_cleanup_is_local_and_recovery_does_not_replay(self):
        self.controller()
        worker = self.worker("worker-a")
        task = self.submit("/bin/sleep", "30")
        started = eventually(lambda: next((r for r in self.events(worker, task)
                                          if r["event"] == "EXEC_STARTED"), None))
        old_session = started["session_id"]
        self.proc.kill(); self.proc.wait(timeout=3)
        finished = eventually(lambda: next((r for r in self.events(worker, task)
                                           if r["event"] == "EXEC_FINISHED"), None))
        self.assertEqual(finished["outcome"], "CANCELLED")
        self.assertEqual(finished["session_id"], old_session)
        eventually(lambda: not Path(f"/proc/{started['pid']}").exists())
        self.controller()
        self.assertFalse(self.complete(task, "FAILED")["logs_complete"])
        self.assertEqual(self.get(task)["attempts"][-1]["state"], "LOST")
        self.assertEqual(self.events(self.proc, task), [])
        eventually(lambda: any(w["state"] == "HEALTHY" and w["session_id"] != old_session for w in self.workers()))

    def test_binary_logs_exact_and_payloads_not_mirrored(self):
        self.controller()
        worker = self.worker("worker-a")
        task = self.submit(str(m7.BIN / "ts_log_workload"), "binary", "32768", "32768")
        self.assertTrue(self.complete(task)["logs_complete"])
        self.assertEqual(self.raw_logs(task, "--both"), expected(32768) + expected(32768, 0xa5))
        payload = "LIFECYCLE_PRIVATE_OUTPUT_9fd82"
        echo = self.submit("/bin/echo", payload)
        self.complete(echo)
        self.assertEqual(self.raw_logs(echo), (payload + "\n").encode())
        self.assertNotIn(payload, worker.log_path.read_text())
        self.assertNotIn(payload, self.proc.log_path.read_text())
        self.assertEqual(self.terminal_event(task)["logs_complete"], "true")

    def test_info_disabled_keeps_cli_and_workload_output_unchanged(self):
        self.config["logging"]["level"] = "off"
        self.config_path = self.write_json("controller.json", self.config)
        self.proc = self.spawn("controller", "--config", self.config_path)
        def listening():
            with socket.socket() as sock:
                return sock.connect_ex(("127.0.0.1", self.client_port)) == 0
        eventually(listening)
        config = dict(worker_id="worker-a", controller=dict(address="127.0.0.1", port=self.worker_port),
                      capacity=dict(cpu_slots=1, memory_mb=100), logging=dict(level="off", destination="stderr"))
        worker = self.spawn("worker", "--config", self.write_json("off-worker.json", config))
        eventually(lambda: any(w["state"] == "HEALTHY" for w in self.workers()))
        response = subprocess.run([str(m7.BIN / "taskctl"), "--port", str(self.client_port),
                                   "submit", "/bin/echo", "hello"], capture_output=True, text=True, timeout=8)
        self.assertEqual(response.returncode, 0)
        task = response.stdout.strip()
        self.assertRegex(task, r"^[0-9a-f-]{36}$")
        self.assertEqual(response.stderr, "")
        self.complete(task)
        self.assertEqual(self.raw_logs(task), b"hello\n")
        self.assertEqual(self.events(self.proc), [])
        self.assertEqual(self.events(worker), [])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--case", required=True)
    args = parser.parse_args()
    m7.BIN = args.bin
    outcome = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite([LifecycleLogging(args.case)]))
    raise SystemExit(0 if outcome.wasSuccessful() else 1)
