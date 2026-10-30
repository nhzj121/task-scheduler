"""M10 staged install and operator tooling: real binaries, no privileged tests."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import uuid

SOURCE = BUILD = BIN = None


class Deployment(unittest.TestCase):
    def setUp(self):
        os.umask(0o077)
        sys.path.insert(0, str(SOURCE / "scripts"))
        global cluster
        import cluster_support as cluster
        self.root = Path(tempfile.mkdtemp(prefix="ts-m10-"))
        self.roots = []
        self.records = []
        self.handles = []
        cache = (BUILD / "CMakeCache.txt").read_text()
        self.prefix = next(line.split("=", 1)[1] for line in cache.splitlines() if line.startswith("CMAKE_INSTALL_PREFIX:PATH="))
        self.stage = self.root / "stage with spaces"
        env = dict(os.environ, DESTDIR=str(self.stage))
        result = subprocess.run(["cmake", "--install", str(BUILD)], env=env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.installed = self.stage / self.prefix.lstrip("/")
        self.bin = self.installed / "bin"
        self.tools = self.installed / "share/task-scheduler/tools"

    def tearDown(self):
        errors = []
        for root in reversed(self.roots):
            try:
                if (root / "cluster.json").exists(): cluster.stop(root)
            except Exception as error: errors.append(str(error))
        try: cluster.stop_records(self.records)
        except Exception as error: errors.append(str(error))
        for handle in self.handles: handle.close()
        failed = errors or self._outcome.result.failures or self._outcome.result.errors
        if failed:
            print("M10 failure artifacts retained at", self.root, errors, file=sys.stderr)
        else:
            if self.root.parent != Path(tempfile.gettempdir()) or not self.root.name.startswith("ts-m10-"):
                raise RuntimeError("test artifact ownership guard")
            shutil.rmtree(self.root)
        self.assertFalse(errors, errors)

    def start(self, name="cluster", workers=2):
        root = self.root / name
        self.roots.append(root)
        return cluster.start(self.bin, root, workers)

    def tool(self, *args, code=0, optimized=False):
        command = [sys.executable] + (["-O"] if optimized else []) + [str(self.tools / "local_cluster.py"), *map(str, args)]
        result = subprocess.run(command, capture_output=True, timeout=45)
        self.assertEqual(result.returncode, code, (result.stdout, result.stderr))
        return result

    def test_staged_install_and_configurations(self):
        for name in ("controller", "worker", "taskctl"):
            self.assertTrue((self.bin / name).is_file())
            self.assertEqual(subprocess.run([str(self.bin / name), "--help"], capture_output=True).returncode, 0)
        self.assertFalse(any(p.name.startswith("ts_") for p in self.bin.iterdir()))
        examples = self.installed / "share/task-scheduler/examples"
        config = json.loads((examples / "controller.json").read_text())
        self.assertEqual(config["client_listen"]["address"], "127.0.0.1")
        self.assertEqual(config["worker_listen"]["address"], "127.0.0.1")
        config["data_dir"] = str(self.root / "example-data")
        config["client_listen"]["port"] = cluster.free_port()
        config["worker_listen"]["port"] = cluster.free_port()
        def spawn(name, value, file):
            path = self.root / file; path.write_text(json.dumps(value))
            handle = (self.root / (file + ".log")).open("wb"); self.handles.append(handle)
            process = subprocess.Popen([str(self.bin / name), "--config", str(path)], stdout=handle, stderr=handle)
            self.records.append(dict(pid=process.pid, identity=cluster.proc_identity(process.pid), role=name, config=str(path)))
        spawn("controller", config, "example-controller.json")
        cluster.eventually(lambda: b"READY client=" in (self.root / "example-controller.json.log").read_bytes())
        ids = []
        for file in ("worker-a.json", "worker-b.json"):
            value = json.loads((examples / file).read_text()); ids.append(value["worker_id"])
            value["controller"] = config["worker_listen"]; spawn("worker", value, file)
        state = dict(bin_dir=str(self.bin), client_port=config["client_listen"]["port"])
        cluster.eventually(lambda: len(cluster.cli(state, "workers")[0]["workers"]) == 2)
        self.assertEqual(len(set(ids)), 2)
        task = cluster.cli(state, "submit", "/bin/true")[0]["task_id"]
        self.assertTrue(cluster.terminal(state, task)["attempts"][0]["logs_complete"])
        units = list((self.installed / "lib/systemd/system").glob("*.service"))
        self.assertEqual(len(units), 3)
        self.assertTrue(all("UMask=0077" in p.read_text() and "Restart=on-failure" in p.read_text() for p in units))

    def test_local_lifecycle_and_durable_workflows(self):
        root = self.root / "demo with spaces"; self.roots.append(root)
        result = self.tool("demo", "--root", root, "--bin-dir", self.bin, optimized=True)
        self.assertIn("active_cancel", json.loads(result.stdout))
        self.assertFalse((root / "cluster.json").exists())
        self.assertTrue((root / "data/metadata.sqlite3").exists())
        state = cluster.start(self.bin, root)
        old = json.loads(result.stdout)["success"]
        self.assertEqual(cluster.cli(state, "get", old)[0]["task"]["state"], "SUCCEEDED")
        task = cluster.cli(state, "submit", sys.executable, str(self.tools / "binary_output.py"), "65537")[0]["task_id"]
        view = cluster.terminal(state, task)
        self.assertEqual((view["attempts"][0]["stdout_bytes"], view["attempts"][0]["stderr_bytes"]), (65537, 65537))
        pattern = bytes(i % 251 for i in range(65537))
        self.assertEqual(cluster.cli(state, "logs", task, "--both", binary=True), pattern + bytes(x ^ 0xa5 for x in pattern))

    def test_operational_rejections_and_private_diagnostics(self):
        state = self.start()
        config = json.loads((self.root / "cluster/controller.json").read_text())
        def rejected(text):
            path = self.root / "invalid.json"; path.write_text(text)
            result = subprocess.run([str(self.bin / "controller"), "--config", str(path)], capture_output=True, timeout=8)
            self.assertNotEqual(result.returncode, 0)
            return result.stderr
        self.assertIn(b"lock", rejected(json.dumps(config)).lower())
        for invalid in ("{", '{"data_dir":"a","data_dir":"b"}', json.dumps({**config, "unknown": 1})):
            rejected(invalid)
        for root in (self.root / "blocked", self.root / "world", self.root / "symlink"):
            if root.name == "blocked": root.mkdir(mode=0o500)
            elif root.name == "world": root.mkdir(); root.chmod(0o777)
            else: root.symlink_to(self.root / "cluster/data", target_is_directory=True)
            rejected(json.dumps({**config, "data_dir": str(root)}))
        bad_bind = {**config, "data_dir": str(self.root / "bind-data")}
        rejected(json.dumps(bad_bind))  # same occupied ports, a different unlocked data root
        result = subprocess.run([str(self.bin / "worker"), "--config", str(self.root / "absent.json")], capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        token = "m10-private-value-" + uuid.uuid4().hex
        task = cluster.cli(state, "submit", "--env", "M10_VALUE=" + token, "/usr/bin/printenv", "M10_VALUE")[0]["task_id"]
        cluster.terminal(state, task)
        self.assertEqual(cluster.cli(state, "logs", task, binary=True), token.encode() + b"\n")
        for path in (self.root / "cluster").glob("*.system.log"): self.assertNotIn(token, path.read_text())
        data = self.root / "cluster/data"
        self.assertEqual(data.stat().st_mode & 0o777, 0o700)
        self.assertEqual((data / "metadata.sqlite3").stat().st_mode & 0o077, 0)
        self.assertTrue(all((p.stat().st_mode & 0o077) == 0 for p in data.glob("tasks/*/*.log")))

    def test_script_ownership_spaces_and_reruns(self):
        root = self.root / "script root"; self.roots.append(root)
        self.tool("start", "--root", root, "--bin-dir", self.bin)
        state = cluster.load(root)
        record = state["processes"][0]
        actual_argv = Path(f'/proc/{record["pid"]}/cmdline').read_bytes()
        with mock.patch.object(Path, "read_bytes", side_effect=[b"", actual_argv]), \
                mock.patch.object(cluster.time, "sleep"):
            self.assertTrue(cluster.owned(record))
        with mock.patch.object(Path, "read_bytes", return_value=b"unexpected\0argv\0"), \
                mock.patch.object(cluster.time, "sleep"):
            with self.assertRaisesRegex(RuntimeError, "could not be verified"):
                cluster.owned(record)
        self.assertTrue(all(cluster.owned(p) for p in state["processes"]))
        self.tool("start", "--root", root, "--bin-dir", self.bin, code=1)
        self.tool("status", "--root", root)
        bad = json.loads(json.dumps(state)); bad["processes"][0]["identity"]["start_ticks"] = "0"
        (root / "cluster.json").write_text(json.dumps(bad))
        try:
            self.tool("stop", "--root", root, code=1)
            self.assertTrue(all(cluster.owned(p) for p in state["processes"]))
        finally:
            (root / "cluster.json").write_text(json.dumps(state))
        self.tool("stop", "--root", root)
        self.tool("start", "--root", root, "--bin-dir", self.bin)
        self.tool("stop", "--root", root)
        unrelated = self.root / "unrelated"; unrelated.mkdir(); (unrelated / "keep").write_text("keep")
        self.tool("start", "--root", unrelated, "--bin-dir", self.bin, code=1)
        self.assertEqual((unrelated / "keep").read_text(), "keep")
        interrupted_root = self.root / "interrupted demo"; self.roots.append(interrupted_root)
        process = subprocess.Popen([sys.executable, str(self.tools / "local_cluster.py"), "demo",
            "--root", str(interrupted_root), "--bin-dir", str(self.bin)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            def ready():
                if not (interrupted_root / "cluster.json").exists(): return None
                state = cluster.load(interrupted_root)
                if len(state["processes"]) != 3: return None
                return state if all(cluster.owned(p) for p in state["processes"]) else None
            interrupted_state = cluster.eventually(ready)
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=40)
            self.assertNotEqual(process.returncode, 0, (stdout, stderr))
            self.assertTrue(all(cluster.proc_identity(p["pid"]) is None for p in interrupted_state["processes"]))
            self.assertFalse((interrupted_root / "cluster.json").exists())
            self.assertTrue(interrupted_root.exists())
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                process.communicate(timeout=40)

    def test_benchmark_terminal_accounting(self):
        self.roots.extend(self.root / "benchmark" / f"w1-{shape}-0" for shape in ("empty", "binary"))
        result_path = self.root / "measurements.json"
        result = subprocess.run([sys.executable, str(self.tools / "benchmark.py"), "--bin-dir", str(self.bin),
            "--root", str(self.root / "benchmark"), "--output", str(result_path), "--tasks", "2", "--workers", "1",
            "--repeats", "1", "--warmup", "1", "--bytes", "257"], capture_output=True, timeout=40)
        self.assertEqual(result.returncode, 0, result.stderr)
        values = json.loads(result_path.read_text())
        self.assertEqual(values["status"], "PASS")
        self.assertEqual(len(values["runs"]), 2)
        for record in values["runs"]:
            self.assertEqual((record["tasks"], record["successes"], record["failures"]), (2, 2, 0))
            self.assertGreater(record["elapsed_terminal_seconds"], 0)
            self.assertTrue(all(s["terminal_observed_seconds"] >= s["issued"] + s["submit_seconds"] for s in record["samples"]))
        failed_output = self.root / "startup-failure.json"
        failed = subprocess.run([sys.executable, str(self.tools / "benchmark.py"),
            "--bin-dir", str(self.root / "missing-binaries"), "--root", str(self.root / "failed-benchmark"),
            "--output", str(failed_output), "--tasks", "1", "--workers", "1", "--repeats", "1"],
            capture_output=True, timeout=10)
        self.assertNotEqual(failed.returncode, 0)
        failure = json.loads(failed_output.read_text())
        self.assertEqual(failure["status"], "FAILED")
        self.assertEqual(failure["runs"][0]["phase"], "startup")
        self.assertNotIn("successes", failure["runs"][0])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--case", required=True)
    args = parser.parse_args()
    SOURCE, BUILD, BIN = args.source.resolve(), args.build.resolve(), args.bin.resolve()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite([Deployment(args.case)]))
    raise SystemExit(0 if result.wasSuccessful() else 1)
