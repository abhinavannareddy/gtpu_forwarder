#!/usr/bin/env python3
"""GTP-U Control Room - web dashboard for gtpu-fwd (standard library only).

Talks to the forwarder over its control socket, samples counters twice a
second, and serves a live UI plus a small JSON API.

    python3 dashboard/server.py --demo            # start forwarder, sinks, traffic
    python3 dashboard/server.py --ctl 127.0.0.1:9100 --fwd 127.0.0.1:2152
"""
import argparse
import collections
import json
import mimetypes
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ctl_client import CtlClient, CtlError          # noqa: E402
from traffic import TrafficGenerator                # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
STATIC = os.path.join(HERE, "static")
ROOT = os.path.dirname(HERE)
SAMPLE_INTERVAL = 0.5
HISTORY_LEN = 240                                   # two minutes of samples
MAX_BODY = 64 * 1024

DEMO_ROUTES = [
    (1000, 2000, 9000, "Stockholm gNB"),
    (1001, 2001, 9001, "Gothenburg gNB"),
    (1002, 2002, 9002, "Malmo gNB"),
    (1003, 2003, 9003, "Karlskrona gNB"),
]


class Sinks:
    """UDP listeners that stand in for next hops and count what arrives."""

    def __init__(self):
        self.counts = {}
        self._socks = []
        self._lock = threading.Lock()

    def listen(self, port, host="127.0.0.1"):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        s.bind((host, port))
        s.settimeout(0.3)
        self.counts[port] = 0
        self._socks.append(s)
        threading.Thread(target=self._loop, args=(s, port), daemon=True).start()

    def _loop(self, sock, port):
        while True:
            try:
                sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            with self._lock:
                self.counts[port] += 1

    def snapshot(self):
        with self._lock:
            return dict(self.counts)


class State:
    """Shared sampler state: latest counters plus a rolling history of rates."""

    def __init__(self, ctl, fwd_addr):
        self.ctl = ctl
        self.fwd_addr = fwd_addr
        self.history = collections.deque(maxlen=HISTORY_LEN)
        self.latest = {"online": False}
        self.routes_cache = []
        self.labels = {}                            # in_teid -> site name (demo mode)
        self.sinks = Sinks()
        self.traffic = TrafficGenerator(fwd_addr, self.known_teids)
        self.lock = threading.Lock()
        self._prev = None
        self._stop = threading.Event()

    def known_teids(self):
        return [r["in_teid"] for r in self.routes_cache]

    def start(self):
        threading.Thread(target=self._sample_loop, daemon=True, name="sampler").start()

    def stop(self):
        self._stop.set()
        self.traffic.stop()

    def _sample_loop(self):
        n = 0
        while not self._stop.is_set():
            self.sample(refresh_routes=(n % 4 == 0))
            n += 1
            self._stop.wait(SAMPLE_INTERVAL)

    def sample(self, refresh_routes=False):
        now = time.time()
        try:
            s = self.ctl.stats()
            if not s.get("ok"):
                raise CtlError(s.get("error", "stats failed"))
            if refresh_routes:
                self.routes_cache = self.ctl.routes()
        except CtlError as exc:
            with self.lock:
                self.latest = {"online": False, "error": str(exc), "t": now}
                self._prev = None
            return
        dropped = max(0, s["rx"] - s["forwarded"] - s["echo_replied"])
        snap = dict(s, online=True, t=now, dropped=dropped,
                    sinks=self.sinks.snapshot(), traffic=self.traffic.status())
        rates = dict(pps_rx=0.0, pps_fwd=0.0, pps_drop=0.0, bps=0.0)
        if self._prev and s["uptime_s"] > self._prev["uptime_s"]:
            dt = s["uptime_s"] - self._prev["uptime_s"]
            rates = dict(
                pps_rx=(s["rx"] - self._prev["rx"]) / dt,
                pps_fwd=(s["forwarded"] - self._prev["forwarded"]) / dt,
                pps_drop=(dropped - self._prev["dropped"]) / dt,
                bps=(s["bytes_forwarded"] - self._prev["bytes_forwarded"]) * 8 / dt)
        rates = {k: max(0.0, round(v, 1)) for k, v in rates.items()}
        snap.update(rates)
        self._prev = {"uptime_s": s["uptime_s"], "rx": s["rx"], "forwarded": s["forwarded"],
                      "dropped": dropped, "bytes_forwarded": s["bytes_forwarded"]}
        with self.lock:
            self.latest = snap
            self.history.append({"t": now, **rates, "rx": s["rx"], "forwarded": s["forwarded"],
                                 "dropped": dropped})

    def snapshot(self):
        with self.lock:
            return dict(self.latest)

    def history_list(self):
        with self.lock:
            return list(self.history)


class Handler(BaseHTTPRequestHandler):
    server_version = "gtpu-dashboard/1.0"
    state = None                                    # set by main()

    def log_message(self, fmt, *args):              # quiet: the stream polls constantly
        if os.environ.get("DASHBOARD_VERBOSE"):
            super().log_message(fmt, *args)

    # -- helpers ---------------------------------------------------------
    def _json(self, obj, status=HTTPStatus.OK):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _error(self, status, msg):
        self._json({"ok": False, "error": msg}, status)

    def _body(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = -1
        if n < 0 or n > MAX_BODY:
            raise ValueError("bad or oversized request body")
        raw = self.rfile.read(n) if n else b"{}"
        data = json.loads(raw or b"{}")
        if not isinstance(data, dict):
            raise ValueError("JSON object expected")
        return data

    def _ctl(self, fn):
        try:
            return fn()
        except CtlError as exc:
            self._error(HTTPStatus.BAD_GATEWAY, str(exc))
        except (ValueError, TypeError, KeyError) as exc:
            self._error(HTTPStatus.BAD_REQUEST, str(exc) or "bad request")
        return None

    # -- routing ---------------------------------------------------------
    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/api/stats":
            return self._json(self.state.snapshot())
        if path == "/api/history":
            return self._json({"interval": SAMPLE_INTERVAL, "samples": self.state.history_list()})
        if path == "/api/routes":
            res = self._ctl(self.state.ctl.routes)
            if res is not None:
                self._json({"ok": True, "routes": [dict(r, label=self.state.labels.get(r["in_teid"]))
                                                   for r in res]})
            return None
        if path == "/api/traffic":
            return self._json(self.state.traffic.status())
        if path == "/api/health":
            return self._json({"ok": True, "forwarder_online": bool(self.state.snapshot().get("online"))})
        if path == "/api/stream":
            return self._stream()
        return self._static(path)

    def do_POST(self):
        path = urlparse(self.path).path
        try:
            data = self._body()
        except (ValueError, UnicodeDecodeError) as exc:
            return self._error(HTTPStatus.BAD_REQUEST, str(exc))
        st = self.state
        if path == "/api/routes":
            res = self._ctl(lambda: st.ctl.add_route(int(data["in_teid"]), int(data["out_teid"]),
                                                     data["next_hop"]))
            if res is not None:
                st.sample(refresh_routes=True)
                self._json(res, HTTPStatus.OK if res.get("ok") else HTTPStatus.CONFLICT)
        elif path == "/api/inspect":
            res = self._ctl(lambda: st.ctl.inspect(str(data["hex"])))
            if res is not None:
                self._json(res)
        elif path == "/api/send":
            self._send_packet(data)
        elif path == "/api/traffic":
            try:
                st.traffic.start(int(data.get("pps", 100)), data.get("mix"))
            except (ValueError, TypeError) as exc:
                return self._error(HTTPStatus.BAD_REQUEST, str(exc))
            self._json(st.traffic.status())
        else:
            self._error(HTTPStatus.NOT_FOUND, "not found")

    def do_DELETE(self):
        path = urlparse(self.path).path
        st = self.state
        if path.startswith("/api/routes/"):
            try:
                teid = int(path.rsplit("/", 1)[1])
            except ValueError:
                return self._error(HTTPStatus.BAD_REQUEST, "TEID must be an integer")
            res = self._ctl(lambda: st.ctl.del_route(teid))
            if res is not None:
                st.sample(refresh_routes=True)
                self._json(res, HTTPStatus.OK if res.get("ok") else HTTPStatus.NOT_FOUND)
        elif path == "/api/traffic":
            st.traffic.stop()
            self._json(st.traffic.status())
        else:
            self._error(HTTPStatus.NOT_FOUND, "not found")

    # -- endpoints -------------------------------------------------------
    def _send_packet(self, data):
        try:
            raw = bytes.fromhex("".join(str(data["hex"]).split()))
            count = max(1, min(int(data.get("count", 1)), 1000))
        except (KeyError, ValueError, TypeError):
            return self._error(HTTPStatus.BAD_REQUEST, "hex must be a valid hex string")
        if not 0 < len(raw) <= 2048:
            return self._error(HTTPStatus.BAD_REQUEST, "packet must be 1..2048 bytes")
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(0.4)
        reply = None
        try:
            for _ in range(count):
                sock.sendto(raw, self.state.fwd_addr)
            try:
                reply = sock.recv(2048).hex()       # Echo Responses come straight back
            except socket.timeout:
                pass
        finally:
            sock.close()
        self._json({"ok": True, "sent": count, "reply_hex": reply})

    def _stream(self):
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        try:
            while True:
                msg = json.dumps(self.state.snapshot())
                self.wfile.write(f"data: {msg}\n\n".encode())
                self.wfile.flush()
                time.sleep(SAMPLE_INTERVAL)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def _static(self, path):
        rel = "index.html" if path in ("", "/") else path.lstrip("/")
        full = os.path.realpath(os.path.join(STATIC, rel))
        if os.path.commonpath([full, os.path.realpath(STATIC)]) != os.path.realpath(STATIC) \
                or not os.path.isfile(full):
            return self._error(HTTPStatus.NOT_FOUND, "not found")
        ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
        with open(full, "rb") as f:
            body = f.read()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", ctype + ("; charset=utf-8" if ctype.startswith("text/") or
                                                  ctype in ("application/javascript", "image/svg+xml") else ""))
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)


def hostport(s):
    host, _, port = s.rpartition(":")
    return host or "127.0.0.1", int(port)


def attach_sinks(state, host):
    """Listen on the demo next-hop ports so deliveries are counted and sites get names."""
    for in_t, _, port, name in DEMO_ROUTES:
        state.labels[in_t] = name
        state.sinks.listen(port, host)


def start_demo_forwarder(args, state):
    """Launch gtpu-fwd with demo routes, attach sinks, return the process."""
    binary = args.bin or os.path.join(ROOT, "gtpu-fwd")
    if not os.path.exists(binary):
        sys.exit(f"{binary} not found - run `make` first")
    fwd_host, fwd_port = hostport(args.fwd)
    ctl_host, ctl_port = hostport(args.ctl)
    routes = tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False)
    for in_t, out_t, port, _ in DEMO_ROUTES:
        routes.write(f"{in_t} {out_t} 127.0.0.1:{port}\n")
    routes.close()
    proc = subprocess.Popen([binary, "-r", routes.name, "-b", fwd_host, "-p", str(fwd_port),
                             "-c", str(ctl_port), "-C", ctl_host],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.3)
    if proc.poll() is not None:
        sys.exit("demo forwarder failed to start (port in use?)")
    return proc, routes.name


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="127.0.0.1:8765", help="dashboard HTTP address")
    ap.add_argument("--fwd", default="127.0.0.1:2152", help="forwarder GTP-U address")
    ap.add_argument("--ctl", default="127.0.0.1:9100", help="forwarder control socket address")
    ap.add_argument("--demo", action="store_true", help="start a forwarder with sinks and background traffic")
    ap.add_argument("--bin", help="path to gtpu-fwd (demo mode)")
    ap.add_argument("--sinks", metavar="HOST", help="also listen on the demo next-hop ports 9000-9003 at HOST")
    ap.add_argument("--traffic", type=int, metavar="PPS", help="start background traffic at this rate")
    args = ap.parse_args()

    ctl_host, ctl_port = hostport(args.ctl)
    state = State(CtlClient(ctl_host, ctl_port), hostport(args.fwd))
    Handler.state = state
    httpd = ThreadingHTTPServer(hostport(args.listen), Handler)   # bind first: fail before spawning
    httpd.daemon_threads = True
    proc = route_file = None
    if args.demo:
        proc, route_file = start_demo_forwarder(args, state)
    if args.demo or args.sinks:
        attach_sinks(state, args.sinks or "127.0.0.1")
    state.start()
    if args.demo or args.traffic:
        state.sample(refresh_routes=True)
        state.traffic.start(args.traffic or 300)

    print(f"GTP-U Control Room on http://{args.listen}  (forwarder ctl {args.ctl})", flush=True)

    def shutdown(*_):
        threading.Thread(target=httpd.shutdown, daemon=True).start()
    signal.signal(signal.SIGTERM, shutdown)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        state.stop()
        httpd.server_close()
        if proc:
            proc.terminate()
            proc.wait(timeout=5)
        if route_file:
            os.unlink(route_file)


if __name__ == "__main__":
    main()
