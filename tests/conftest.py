"""Shared fixtures: a real gtpu-fwd process with its control socket enabled."""
import os
import socket
import subprocess
import sys
import time

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "gtpu-fwd")
sys.path.insert(0, os.path.join(ROOT, "dashboard"))


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Fwd:
    def __init__(self, proc, data_addr, ctl_addr, sink):
        self.proc, self.data_addr, self.ctl_addr, self.sink = proc, data_addr, ctl_addr, sink


@pytest.fixture()
def fwd_ctl(tmp_path):
    sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sink.bind(("127.0.0.1", 0))
    sink.settimeout(1.0)
    sink_port = sink.getsockname()[1]
    routes = tmp_path / "routes.conf"
    routes.write_text(f"100 200 127.0.0.1:{sink_port}\n101 201 127.0.0.1:{sink_port}\n")
    data_port, ctl_port = free_port(), free_port()
    proc = subprocess.Popen([BIN, "-r", str(routes), "-b", "127.0.0.1", "-p", str(data_port),
                             "-c", str(ctl_port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(0.3)
    assert proc.poll() is None, proc.stderr.read()
    yield Fwd(proc, ("127.0.0.1", data_port), ("127.0.0.1", ctl_port), sink)
    if proc.poll() is None:
        proc.terminate()
        proc.wait(timeout=5)
    sink.close()
