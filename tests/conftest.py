"""Shared fixtures: a real gtpu-fwd process with its control socket enabled."""
import os
import socket
import subprocess
import sys
import time

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("GTPU_BIN") or os.path.join(ROOT, "gtpu-fwd")   # GTPU_BIN: e.g. the TSan build
sys.path.insert(0, os.path.join(ROOT, "dashboard"))


def free_port(family=socket.AF_INET):
    s = socket.socket(family, socket.SOCK_DGRAM)
    s.bind(("::1" if family == socket.AF_INET6 else "127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def have_ipv6():
    try:
        s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        s.bind(("::1", 0))
        s.close()
        return True
    except OSError:
        return False


class Fwd:
    def __init__(self, proc, data_addr, ctl_addr, sink):
        self.proc, self.data_addr, self.ctl_addr, self.sink = proc, data_addr, ctl_addr, sink

    def stop(self):
        """Terminate and return the final counters line printed on stdout."""
        self.proc.terminate()
        out, _ = self.proc.communicate(timeout=5)
        return out


@pytest.fixture()
def launch(tmp_path):
    """Factory: launch(args=[], bind="127.0.0.1", routes=None, env=None) -> Fwd. Cleans up afterwards."""
    started = []

    def _launch(args=(), bind="127.0.0.1", routes=None, env=None):
        fam = socket.AF_INET6 if ":" in bind else socket.AF_INET
        sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sink.bind(("127.0.0.1", 0))
        sink.settimeout(1.0)
        sink_port = sink.getsockname()[1]
        if routes is None:
            routes = f"100 200 127.0.0.1:{sink_port}\n101 201 127.0.0.1:{sink_port}\n"
        else:
            routes = routes.replace("SINK", str(sink_port))
        rfile = tmp_path / f"routes{len(started)}.conf"
        rfile.write_text(routes)
        data_port, ctl_port = free_port(fam), free_port()
        proc = subprocess.Popen([BIN, "-r", str(rfile), "-b", bind, "-p", str(data_port), "-c", str(ctl_port),
                                 *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                env={**os.environ, **(env or {})})
        time.sleep(0.3)
        assert proc.poll() is None, proc.stderr.read()
        f = Fwd(proc, (bind, data_port), ("127.0.0.1", ctl_port), sink)
        started.append(f)
        return f

    yield _launch
    for f in started:
        err = ""
        if f.proc.poll() is None:
            f.proc.terminate()
            _, err = f.proc.communicate(timeout=10)
        f.sink.close()
        assert "ThreadSanitizer" not in err and "AddressSanitizer" not in err, err


@pytest.fixture()
def fwd_ctl(launch):
    return launch()
