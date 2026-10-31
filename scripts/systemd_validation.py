#!/usr/bin/env python3
"""Opt-in privileged TEMPORARY service validation; never a production installer."""
import argparse
import grp
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import subprocess
import sys
import time
import uuid
import cluster_support as cluster


def run(args):
    cluster.require(os.geteuid() == 0 and args.allow_temporary_system_services,
                    "root and explicit --allow-temporary-system-services required; obtain owner permission first")
    subprocess.run(["systemctl", "is-system-running"], check=True, capture_output=True)
    artifacts = args.artifacts.absolute()
    artifacts.mkdir(mode=0o755, parents=True, exist_ok=False)
    prefix = "ts-m10-" + uuid.uuid4().hex[:10]
    runtime = Path("/run") / prefix
    state_parent = Path("/var/lib") / prefix
    runtime.mkdir(mode=0o755); state_parent.mkdir(mode=0o755)
    for root in (runtime, state_parent): (root / ".test-owner").write_text(prefix)
    accounts, unit_paths = [], []
    cuser, wuser = prefix + "c", prefix + "w"
    controller = prefix + "-controller.service"
    template = prefix + "-worker@.service"
    standalone = prefix + "-worker.service"
    workers = [prefix + "-worker@a.service", prefix + "-worker@b.service"]
    units = [controller, *workers, standalone]
    report = dict(prefix=prefix, status="RUNNING", commands=[], observations={}, cleanup_errors=[])
    def save():
        (artifacts / "systemd.json").write_text(json.dumps(report, indent=2) + "\n")
    def command(argv, allow_failure=False):
        begin = time.monotonic()
        result = subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=65)
        report["commands"].append(dict(argv=list(map(str, argv)), code=result.returncode, seconds=time.monotonic() - begin,
                                      stdout=result.stdout, stderr=result.stderr))
        save()
        if result.returncode and not allow_failure:
            raise RuntimeError(f"{argv[0]} failed: {result.stderr}")
        return result
    def pid(unit):
        return int(command(["systemctl", "show", unit, "-p", "MainPID", "--value"]).stdout.strip())
    def active(unit):
        return command(["systemctl", "is-active", unit], True).returncode == 0
    def session_map(state):
        return {w["worker_id"]: w["session_id"] for w in cluster.cli(state, "workers")[0]["workers"] if w["state"] == "HEALTHY"}
    error = None
    try:
        for account in (cuser, wuser):
            try:
                pwd.getpwnam(account)
                raise RuntimeError("unexpected existing test account")
            except KeyError: pass
            command(["useradd", "--system", "--user-group", "--no-create-home", "--shell", "/usr/sbin/nologin", account])
            info = pwd.getpwnam(account); accounts.append((account, info.pw_uid, info.pw_gid))
        (runtime / "bin").mkdir(mode=0o755)
        for executable in ("controller", "worker", "taskctl"):
            destination = runtime / "bin" / executable
            shutil.copy2(args.bin_dir / executable, destination); destination.chmod(0o755)
        cp, wp = cluster.free_port(), cluster.free_port()
        cluster.require(cp != wp, "test port allocation collision")
        config = dict(client_listen=dict(address="127.0.0.1", port=cp), worker_listen=dict(address="127.0.0.1", port=wp),
                      data_dir=str(state_parent / "controller"), termination_grace_ms=200,
                      logging=dict(level="info", destination="journald"))
        (runtime / "controller.json").write_text(json.dumps(config)); (runtime / "controller.json").chmod(0o644)
        for instance in ("a", "b"):
            path = runtime / f"worker-{instance}.json"
            path.write_text(json.dumps(dict(worker_id="system-worker-" + instance, controller=config["worker_listen"],
                capacity=dict(cpu_slots=2, memory_mb=512), max_concurrent_tasks=2,
                logging=dict(level="info", destination="journald"))))
            path.chmod(0o644)
        single_config = runtime / "worker.json"
        single_config.write_text(json.dumps(dict(worker_id="system-worker-single", controller=config["worker_listen"],
            capacity=dict(cpu_slots=2, memory_mb=512), max_concurrent_tasks=2,
            logging=dict(level="info", destination="journald"))))
        single_config.chmod(0o644)
        for source, name, account in (("task-controller.service", controller, cuser),
                ("task-worker@.service", template, wuser), ("task-worker.service", standalone, wuser)):
            text = (args.units_dir / source).read_text()
            text = re.sub(r"^User=.*$", "User=" + account, text, flags=re.M)
            text = re.sub(r"^Group=.*$", "Group=" + account, text, flags=re.M)
            if account == cuser:
                text = re.sub(r"^StateDirectory=.*$", "StateDirectory=" + prefix + "/controller", text, flags=re.M)
                invocation = f'ExecStart="{runtime}/bin/controller" --config "{runtime}/controller.json"'
            else:
                config_name = "worker.json" if name == standalone else "worker-%i.json"
                invocation = f'ExecStart="{runtime}/bin/worker" --config "{runtime}/{config_name}"'
            text = re.sub(r"^ExecStart=.*$", lambda _: invocation, text, flags=re.M)
            path = Path("/run/systemd/system") / name
            with path.open("x") as stream: stream.write(text)
            unit_paths.append(path); (artifacts / name).write_text(text)
        command(["systemd-analyze", "verify", *unit_paths])
        command(["systemctl", "daemon-reload"])
        command(["systemctl", "start", controller])
        cluster.eventually(lambda: active(controller))
        state = dict(bin_dir=str(runtime / "bin"), client_port=cp)
        def ready():
            try: return cluster.cli(state, "workers")[0]["workers"] == []
            except RuntimeError: return False
        cluster.eventually(ready)
        command(["systemctl", "start", *workers])
        cluster.eventually(lambda: len(session_map(state)) == 2)
        report["observations"]["initial_sessions"] = session_map(state)
        command(["systemctl", "start", standalone])
        cluster.eventually(lambda: "system-worker-single" in session_map(state))
        single_pid = pid(standalone)
        cluster.require(cluster.proc_identity(single_pid)["uid"] == pwd.getpwnam(wuser).pw_uid != 0)
        single_session = session_map(state)["system-worker-single"]
        command(["systemctl", "restart", standalone])
        cluster.eventually(lambda: "system-worker-single" in session_map(state) and
            session_map(state)["system-worker-single"] != single_session)
        report["observations"]["standalone_worker"] = dict(uid=pwd.getpwnam(wuser).pw_uid,
            initial_session=single_session, restarted_session=session_map(state)["system-worker-single"])
        command(["systemctl", "stop", standalone])
        cluster.eventually(lambda: len(session_map(state)) == 2)
        for unit, expected_user in ((controller, cuser), *((unit, wuser) for unit in workers)):
            owner = pid(unit); info = cluster.proc_identity(owner)
            cluster.require(info and info["uid"] == pwd.getpwnam(expected_user).pw_uid and info["uid"] != 0)
            report["observations"][unit] = dict(pid=owner, uid=info["uid"], fds=len(list(Path(f"/proc/{owner}/fd").iterdir())),
                threads=len(list(Path(f"/proc/{owner}/task").iterdir())))
        task = cluster.cli(state, "submit", "/bin/true")[0]["task_id"]
        cluster.require(cluster.terminal(state, task)["attempts"][0]["logs_complete"])
        cluster.require(cluster.cli(state, "logs", task, "--both", binary=True) == b"")
        pending = cluster.cli(state, "submit", "--cpu", "100", "/bin/true")[0]["task_id"]
        long = cluster.cli(state, "submit", "/bin/sleep", "30")[0]["task_id"]
        def running():
            view = cluster.cli(state, "get", long)[0]
            return view if view["attempts"] and view["attempts"][-1]["state"] == "RUNNING" else None
        view = cluster.eventually(running)
        instance = view["attempts"][-1]["worker_id"][-1]
        selected = prefix + "-worker@" + instance + ".service"
        owner = pid(selected)
        children = Path(f"/proc/{owner}/task/{owner}/children").read_text().split()
        old_sessions = session_map(state)
        command(["systemctl", "stop", selected]); cluster.eventually(lambda: not active(selected))
        for child in children: cluster.eventually(lambda p=child: not Path(f"/proc/{p}").exists())
        cluster.require(cluster.terminal(state, long, "FAILED")["attempts"][-1]["state"] == "LOST")
        command(["systemctl", "start", selected])
        cluster.eventually(lambda: len(session_map(state)) == 2 and session_map(state)["system-worker-" + instance] != old_sessions["system-worker-" + instance])
        # Restart Controller while another real task runs; all healthy sessions
        # must change and old task state must not be adopted.
        second = cluster.cli(state, "submit", "/bin/sleep", "30")[0]["task_id"]
        cluster.eventually(lambda: cluster.cli(state, "get", second)[0]["attempts"] and
            cluster.cli(state, "get", second)[0]["attempts"][-1]["state"] == "RUNNING")
        old_sessions = session_map(state)
        command(["systemctl", "restart", controller])
        def recovered():
            try:
                current = session_map(state)
                return len(current) == 2 and all(current[k] != old_sessions[k] for k in current)
            except RuntimeError: return False
        cluster.eventually(recovered)
        cluster.require(cluster.terminal(state, second, "FAILED")["attempts"][-1]["state"] == "LOST")
        cluster.require(cluster.cli(state, "get", pending)[0]["task"]["state"] == "PENDING")
        cluster.require(cluster.cli(state, "get", task)[0]["task"]["state"] == "SUCCEEDED")
        prior_b = session_map(state)["system-worker-b"]
        command(["systemctl", "restart", workers[1]])
        cluster.eventually(lambda: len(session_map(state)) == 2 and session_map(state)["system-worker-b"] != prior_b)
        directory = state_parent / "controller"
        cluster.require(directory.stat().st_uid == pwd.getpwnam(cuser).pw_uid and directory.stat().st_mode & 0o777 == 0o700)
        cluster.require((directory / "metadata.sqlite3").stat().st_mode & 0o077 == 0)
        report["observations"].update(recovered_sessions=session_map(state), worker_stop_children_reaped=children,
            recovered_running_task=second, retained_pending=pending, retained_terminal=task)
        for path in runtime.glob("*.json"): shutil.copy2(path, artifacts / path.name)
        report["status"] = "PASS"
    except BaseException as caught:
        error = caught; report["status"] = "FAILED"; report["error"] = str(caught)
    finally:
        for unit in reversed(units):
            try:
                result = command(["systemctl", "stop", unit], True)
                cluster.require(result.returncode == 0, "temporary service stop failed")
                command(["systemctl", "reset-failed", unit], True)
            except Exception as caught: report["cleanup_errors"].append(str(caught))
        for unit in units:
            try:
                result = command(["journalctl", "-u", unit, "--no-pager", "-o", "short-unix"], True)
                (artifacts / (unit + ".journal.log")).write_text(result.stdout)
            except Exception as caught: report["cleanup_errors"].append(str(caught))
        for path in unit_paths:
            try: path.unlink()
            except OSError as caught: report["cleanup_errors"].append(str(caught))
        command(["systemctl", "daemon-reload"], True)
        for account, uid, gid in reversed(accounts):
            try:
                cluster.require(pwd.getpwnam(account).pw_uid == uid, "test account identity changed")
                command(["userdel", account])
                try:
                    group = grp.getgrnam(account)
                    cluster.require(group.gr_gid == gid, "test group identity changed")
                    command(["groupdel", account])
                except KeyError: pass
            except Exception as caught: report["cleanup_errors"].append(str(caught))
        if not report["cleanup_errors"]:
            for root, parent in ((runtime, Path("/run")), (state_parent, Path("/var/lib"))):
                cluster.require(root.parent == parent and root.name == prefix and not root.is_symlink() and
                                (root / ".test-owner").read_text() == prefix, "test cleanup path ownership guard")
                shutil.rmtree(root)
        if report["cleanup_errors"]: report["status"] = "FAILED_CLEANUP"
        save()
    if error: raise RuntimeError(f"systemd validation failed; artifacts {artifacts}: {error}")
    cluster.require(report["status"] == "PASS", "cleanup did not complete safely")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--allow-temporary-system-services", action="store_true")
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--units-dir", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    try:
        result = run(parser.parse_args())
        print(json.dumps(dict(status=result["status"], prefix=result["prefix"])))
    except (Exception, KeyboardInterrupt) as error:
        print(error, file=sys.stderr); raise SystemExit(1)
