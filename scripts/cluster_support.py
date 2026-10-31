"""Operator/test helpers. No shell, process-name kill, data deletion or adoption."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time
import uuid


def require(condition, message="verification failed"):
    if not condition:
        raise RuntimeError(message)


def eventually(predicate, seconds=15):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        result = predicate()
        if result:
            return result
        time.sleep(0.02)  # external operator tooling, never a production EventLoop
    raise RuntimeError("bounded readiness/completion deadline expired")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def proc_identity(pid):
    try:
        # comm is parenthesized and can contain spaces or ')'.
        stat = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        if stat[0] == "Z":
            return None
        return dict(start_ticks=stat[19], uid=Path(f"/proc/{pid}").stat().st_uid,
                    executable=str(Path(f"/proc/{pid}/exe").resolve(strict=True)))
    except (FileNotFoundError, ProcessLookupError):
        return None


def owned(record):
    expected = [os.fsencode(record["identity"]["executable"]), b"--config", os.fsencode(record["config"])]
    # /proc identity and argv are separate snapshots. A newly exec'd service (or
    # one exiting between reads) need not expose a stable argv on the first read.
    # Never trust a mismatch: use at most 50 checks, 10ms apart when mismatched.
    for attempt in range(50):
        now = proc_identity(record["pid"])
        if now is None:
            return False
        if now != record["identity"] or now["uid"] != os.geteuid():
            raise RuntimeError("PID identity/ownership changed; refusing to signal it")
        try:
            argv = Path(f'/proc/{record["pid"]}/cmdline').read_bytes().rstrip(b"\0").split(b"\0")
        except (FileNotFoundError, ProcessLookupError):
            continue  # the next identity read must confirm exit or the same owner
        if argv == expected:
            return True
        if attempt != 49:
            time.sleep(0.01)
    raise RuntimeError("owned process argv/config could not be verified; refusing to signal it")


def cli(state, *args, binary=False, allow_error=False):
    command = [str(Path(state["bin_dir"]) / "taskctl"), "--host", "127.0.0.1", "--port", str(state["client_port"])]
    if not binary:
        command.append("--json")
    result = subprocess.run(command + list(map(str, args)), capture_output=True, timeout=20)
    if result.returncode and not allow_error:
        raise RuntimeError(f"taskctl exited {result.returncode}: {result.stderr.decode(errors='replace')}")
    if binary:
        return result.stdout
    return [json.loads(line) for line in result.stdout.splitlines()]


def atomic_json(path, value):
    temp = path.with_name(path.name + ".new")
    with temp.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")
    temp.replace(path)


def stop_records(records):
    # Validate the complete registry before stopping even a correctly identified
    # sibling. A stale/tampered entry must not produce a partial shutdown.
    for record in records:
        owned(record)
    # Workers first, then Controller; never SIGKILL an unreaped workload owner.
    for record in reversed(records):
        if not owned(record):
            try:
                os.waitpid(record["pid"], os.WNOHANG)
            except ChildProcessError:
                pass
            continue
        children_path = Path(f'/proc/{record["pid"]}/task/{record["pid"]}/children')
        children = children_path.read_text().split() if children_path.exists() else []
        os.kill(record["pid"], signal.SIGTERM)
        eventually(lambda: not owned(record), seconds=35)
        try:
            os.waitpid(record["pid"], os.WNOHANG)
        except ChildProcessError:
            pass
        for child in children:
            eventually(lambda p=child: not Path(f"/proc/{p}").exists(), seconds=5)


def start(bin_dir, root, workers=2, client_port=0, worker_port=0):
    if workers < 1 or workers > 8:
        raise ValueError("workers must be 1..8 independent processes")
    if not 0 <= client_port <= 65535 or not 0 <= worker_port <= 65535:
        raise ValueError("ports must be 0 (automatic) or 1..65535")
    root = Path(root).absolute()
    if root.is_symlink():
        raise RuntimeError("local root must not be a symlink")
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    if root.stat().st_uid != os.geteuid() or root.stat().st_mode & 0o022:
        raise RuntimeError("local root must be owned and not group/world writable")
    marker = root / ".local-cluster-owner.json"
    state_file = root / "cluster.json"
    if state_file.exists():
        raise RuntimeError("cluster registry exists; inspect/status/stop it before restarting")
    if not marker.exists():
        if any(root.iterdir()):
            raise RuntimeError("refusing a nonempty directory not initialized by this tool")
        atomic_json(marker, {"owner": os.geteuid(), "instance": str(uuid.uuid4())})
    if json.loads(marker.read_text())["owner"] != os.geteuid():
        raise RuntimeError("local root marker belongs to another owner")
    client_port = client_port or free_port()
    worker_port = worker_port or free_port()
    if client_port == worker_port:
        raise RuntimeError("listeners need distinct ports; retry with unused ports")
    state = dict(bin_dir=str(Path(bin_dir).resolve(strict=True)), root=str(root),
                 client_port=client_port, worker_port=worker_port, processes=[])
    for name in ("controller", "worker", "taskctl"):
        if not os.access(Path(state["bin_dir"]) / name, os.X_OK):
            raise RuntimeError("missing installed executable: " + name)
    config = dict(client_listen=dict(address="127.0.0.1", port=client_port),
                  worker_listen=dict(address="127.0.0.1", port=worker_port), data_dir=str(root / "data"),
                  heartbeat=dict(interval_ms=3000, offline_timeout_ms=10000),
                  dispatch=dict(ack_timeout_ms=5000, start_timeout_ms=10000),
                  protocol=dict(max_payload_bytes=4194304),
                  queues=dict(persistence_commands=4096, log_bytes=67108864, log_commands=4096,
                    log_control_reserve=128, initial_session_log_window_bytes=1048576, initial_session_log_window_chunks=64),
                  termination_grace_ms=200, logging=dict(level="info", destination="stderr"))
    (root / "controller.json").write_text(json.dumps(config))
    try:
        for index in range(workers + 1):
            name = "controller" if index == 0 else "worker"
            config_path = root / ("controller.json" if index == 0 else f"worker-{index}.json")
            if index:
                config_path.write_text(json.dumps(dict(worker_id=f"local-worker-{index}",
                    controller=config["worker_listen"], capacity=dict(cpu_slots=2, memory_mb=512),
                    max_concurrent_tasks=2, logging=dict(level="info", destination="stderr"))))
            log_path = root / (config_path.stem + ".system.log")
            log_offset = log_path.stat().st_size if log_path.exists() else 0
            with log_path.open("ab") as log:
                process = subprocess.Popen([str(Path(state["bin_dir"]) / name), "--config", str(config_path)],
                                           stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
            identity = proc_identity(process.pid)
            if identity is None:
                process.wait(timeout=3)
                raise RuntimeError("process failed during startup: " + str(log_path))
            state["processes"].append(dict(pid=process.pid, identity=identity, role=name, config=str(config_path)))
            atomic_json(state_file, state)
            if index == 0:
                def ready():
                    if not owned(state["processes"][0]):
                        raise RuntimeError("Controller failed; see " + str(log_path))
                    return b"READY client=" in log_path.read_bytes()[log_offset:]
                eventually(ready)
        def registered():
            if not all(owned(p) for p in state["processes"]):
                raise RuntimeError("owned service exited before registration")
            values = cli(state, "workers")[0]["workers"]
            return len(values) == workers and all(w["state"] == "HEALTHY" for w in values)
        eventually(registered)
        return state
    except BaseException:
        stop_records(state["processes"])
        if state_file.exists():
            state_file.unlink()
        raise


def load(root):
    root = Path(root).absolute()
    if root.is_symlink() or (root / "cluster.json").is_symlink():
        raise RuntimeError("refusing symlink registry")
    marker = json.loads((root / ".local-cluster-owner.json").read_text())
    state = json.loads((root / "cluster.json").read_text())
    if marker["owner"] != os.geteuid() or state["root"] != str(root):
        raise RuntimeError("registry root/owner mismatch")
    if any(Path(p["config"]).parent != root or p["role"] not in ("controller", "worker") for p in state["processes"]):
        raise RuntimeError("registry process is outside the owned configuration root")
    return state


def stop(root):
    state = load(root)
    stop_records(state["processes"])
    (Path(root).absolute() / "cluster.json").unlink()


def terminal(state, task, wanted="SUCCEEDED"):
    def observed():
        view = cli(state, "get", task)[0]
        if view["task"]["state"] not in ("PENDING", "ACTIVE"):
            if view["task"]["state"] != wanted:
                raise RuntimeError("unexpected terminal distribution: " + json.dumps(view))
            attempts = view["attempts"]
            if attempts and attempts[-1]["finalization_pending"]:
                raise RuntimeError("terminal task has pending finalization")
            return view
        return None
    return eventually(observed, seconds=30)
