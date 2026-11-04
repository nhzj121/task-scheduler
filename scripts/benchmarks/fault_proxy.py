"""Bounded loopback-only measurement proxy: pass v1 frames, reset or drop old heartbeats."""
import json
import socket
import struct
import threading

import measurement_support as m


class Connection:
    def __init__(self, owner, number, downstream, upstream):
        self.owner, self.number = owner, number
        self.downstream, self.upstream = downstream, upstream
        self.closed = threading.Event()
        self.close_lock = threading.Lock()
        self.record = dict(number=number, accepted_ns=m.now(), heartbeats_forwarded=0, heartbeats_dropped=0,
                           events=[], deliberately_closed=False)

    def event(self, kind, **fields):
        with self.owner.lock:
            self.record["events"].append(dict(at_ns=m.now(), kind=kind, **fields))

    def close(self, reason, reset=False):
        with self.close_lock:
            if self.closed.is_set():
                return
            self.closed.set()
            self.event("close", reason=reason, reset=reset)
            for sock in (self.downstream, self.upstream):
                if reset:
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                sock.close()

    def forward(self, source, destination, from_worker):
        try:
            while not self.closed.is_set():
                header = m.exact(source, m.HEADER.size)
                magic, version, kind, request, length = m.HEADER.unpack(header)
                m.require(magic == 0x54534348 and version == 1 and length <= m.MAX_FRAME, "invalid proxy frame")
                data = m.exact(source, length)
                with self.owner.lock:
                    drop = from_worker and kind == 0x0203 and self.owner.drop_connection == self.number
                    if from_worker and kind == 0x0203:
                        if drop:
                            self.record["heartbeats_dropped"] += 1
                            self.record.setdefault("first_heartbeat_dropped_ns", m.now())
                        else:
                            self.record["heartbeats_forwarded"] += 1
                            self.record["last_heartbeat_forward_start_ns"] = m.now()
                if kind in (0x0201, 0x0202, 0x0206, 0x0207):
                    payload = json.loads(data)
                    self.event("frame", direction="worker_to_controller" if from_worker else "controller_to_worker",
                               type=kind, request_id=request, payload=payload)
                if not drop:
                    destination.sendall(header + data)
        except (EOFError, ConnectionResetError, BrokenPipeError) as error:
            self.event("eof_or_reset", direction="worker_to_controller" if from_worker else "controller_to_worker", error=str(error))
        except OSError as error:
            if not self.closed.is_set() and not self.owner.stopping.is_set():
                self.owner.errors.append(repr(error))
        except Exception as error:
            self.owner.errors.append(repr(error))
        finally:
            self.close("worker_eof" if from_worker else "controller_eof")


class FaultProxy:
    def __init__(self, controller_port):
        self.controller_port = controller_port
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(.1)
        self.endpoint = dict(address="127.0.0.1", port=self.listener.getsockname()[1])
        self.connections, self.threads, self.errors = [], [], []
        self.lock, self.stopping = threading.Lock(), threading.Event()
        self.drop_connection = None
        self.thread = threading.Thread(target=self.accept, name="measurement-proxy")
        self.thread.start()

    def accept(self):
        try:
            while not self.stopping.is_set():
                try:
                    downstream, _ = self.listener.accept()
                except socket.timeout:
                    continue
                try:
                    m.require(len(self.connections) < 5, "bounded proxy connection budget exceeded")
                    upstream = socket.create_connection(("127.0.0.1", self.controller_port), timeout=5)
                    upstream.settimeout(None)
                    downstream.settimeout(None)
                    for sock in (upstream, downstream):
                        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    connection = Connection(self, len(self.connections), downstream, upstream)
                    with self.lock:
                        self.connections.append(connection)
                    for source, destination, direction in ((downstream, upstream, True), (upstream, downstream, False)):
                        thread = threading.Thread(target=connection.forward, args=(source, destination, direction))
                        self.threads.append(thread)
                        thread.start()
                except Exception:
                    downstream.close()
                    raise
        except OSError as error:
            if not self.stopping.is_set():
                self.errors.append(repr(error))
        except Exception as error:
            self.errors.append(repr(error))

    def inject(self, scenario):
        with self.lock:
            m.require(len(self.connections) == 1 and not self.connections[0].closed.is_set(), "no current proxy connection")
            connection = self.connections[0]
            begin = m.now()
            if scenario == "heartbeat":
                self.drop_connection = connection.number
            elif scenario == "disconnect":
                connection.record["deliberately_closed"] = True
        if scenario == "disconnect":
            connection.close("injected_tcp_disconnect", reset=True)
        return dict(fault_start_ns=begin, fault_end_ns=m.now(), connection=connection.number,
                    method="drop only old-session HEARTBEAT frames" if scenario == "heartbeat" else "reset/close both ends of old TCP proxy connection")

    def snapshot(self):
        with self.lock:
            return json.loads(json.dumps(dict(connections=[c.record for c in self.connections], errors=self.errors)))

    def close(self):
        self.stopping.set()
        self.listener.close()
        self.thread.join(timeout=6)
        for connection in self.connections:
            connection.close("proxy_teardown")
        for thread in self.threads:
            thread.join(timeout=6)
        m.require(not self.thread.is_alive() and not any(t.is_alive() for t in self.threads), "proxy thread leak")
        m.require(not self.errors, "proxy error: " + repr(self.errors))
