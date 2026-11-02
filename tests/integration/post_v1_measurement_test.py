"""Standalone checks of the new external measurement harness, not production tests."""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import sys
import threading
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE / "scripts/benchmarks"))
import measurement_support as m
from fault_proxy import FaultProxy


def wire(kind, payload, request=1):
    body = json.dumps(payload).encode()
    return m.HEADER.pack(0x54534348, 1, kind, request, len(body)) + body


class MeasurementHarness(unittest.TestCase):
    def client_response(self, response, error=None):
        listener = socket.socket(); listener.bind(("127.0.0.1", 0)); listener.listen()
        errors = []
        def peer():
            try:
                with listener.accept()[0] as sock:
                    header = m.exact(sock, 20)
                    length = m.HEADER.unpack(header)[-1]
                    m.exact(sock, length)
                    for byte in response: sock.sendall(bytes([byte]))
            except Exception as problem: errors.append(repr(problem))
        thread = threading.Thread(target=peer); thread.start()
        client = m.Client(listener.getsockname()[1])
        try:
            if error:
                with self.assertRaisesRegex(RuntimeError, error): client.workers()
            else:
                value, times = client.workers()
                self.assertEqual(value, {"workers":[]})
                self.assertLessEqual(times["request_start_ns"], times["request_sent_ns"])
                self.assertLessEqual(times["request_sent_ns"], times["response_received_ns"])
                self.assertLessEqual(times["response_received_ns"], times["response_ns"])
        finally:
            client.close(); thread.join(timeout=5); listener.close()
        self.assertFalse(thread.is_alive()); self.assertFalse(errors)

    def test_fragmented_correlated_response(self):
        self.client_response(wire(0x010E, dict(workers=[])))

    def test_wrong_correlation_is_not_success(self):
        self.client_response(wire(0x010E, dict(workers=[]), request=99), "correlation")

    def test_oversized_header_rejected_before_payload(self):
        self.client_response(m.HEADER.pack(0x54534348,1,0x010E,1,m.MAX_FRAME+1), "framing")

    def test_explicit_server_error_is_not_success(self):
        self.client_response(wire(1,dict(code="INTERNAL",message="fixture error")), "explicit protocol error")

    def test_proxy_drops_only_heartbeat_without_closing_tcp(self):
        upstream_listener = socket.socket(); upstream_listener.bind(("127.0.0.1",0)); upstream_listener.listen()
        proxy = FaultProxy(upstream_listener.getsockname()[1])
        downstream = socket.create_connection(("127.0.0.1",proxy.endpoint["port"]), timeout=3)
        upstream, _ = upstream_listener.accept(); upstream.settimeout(3)
        heartbeat = wire(0x0203, dict(sequence=1), request=0)
        control = wire(0x0205, dict(accepted=True), request=7)
        try:
            downstream.sendall(heartbeat)
            self.assertEqual(m.exact(upstream,len(heartbeat)),heartbeat)
            proxy.inject("heartbeat")
            downstream.sendall(heartbeat+control)
            self.assertEqual(m.exact(upstream,len(control)),control)
            self.assertEqual(upstream.getsockopt(socket.SOL_SOCKET,socket.SO_ERROR),0)
            state=proxy.snapshot()["connections"][0]
            self.assertEqual(state["heartbeats_forwarded"],1)
            self.assertEqual(state["heartbeats_dropped"],1)
            self.assertFalse(state["deliberately_closed"])
        finally:
            downstream.close(); upstream.close(); upstream_listener.close(); proxy.close()

    def test_interrupt_between_spawn_and_publication_does_not_lose_owner(self):
        cluster = m.ManagedCluster(BIN, ROOT / "interrupt")
        real = m.subprocess.Popen
        def interrupted(*args, **kwargs):
            process = real(*args, **kwargs)
            os.kill(os.getpid(), signal.SIGINT)
            return process
        try:
            with mock.patch.object(m.subprocess,"Popen",side_effect=interrupted):
                with self.assertRaises(KeyboardInterrupt): cluster.start_controller()
            self.assertEqual(len(cluster.records),1)
            record=cluster.records[0]
            self.assertTrue(m.cluster.owned(record))
            m.cluster.eventually(lambda:"READY client=" in Path(record["log"]).read_text())
        finally:
            cluster.close()
        self.assertEqual(record["exit_code"],0)


if __name__ == "__main__":
    p=argparse.ArgumentParser()
    p.add_argument("--bin-dir",type=Path,required=True)
    p.add_argument("--root",type=Path,required=True)
    args=p.parse_args();BIN=args.bin_dir;ROOT=m.fresh_root(args.root)
    os.umask(0o077)
    outcome=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(MeasurementHarness))
    raise SystemExit(0 if outcome.wasSuccessful() else 1)
