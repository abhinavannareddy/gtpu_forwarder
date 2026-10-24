"""Integration tests: start the real gtpu-fwd binary and send it packets."""
import os
import socket
import struct
import subprocess
import time

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "gtpu-fwd")


def gpdu(teid, payload=b"hello", flags=0x30, mtype=0xFF, length=None):
    length = len(payload) if length is None else length
    return struct.pack("!BBHI", flags, mtype, length, teid) + payload


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


@pytest.fixture()
def fwd(tmp_path):
    sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sink.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sink.bind(("127.0.0.1", 0))
    sink.settimeout(1.0)
    sink_port = sink.getsockname()[1]
    routes = tmp_path / "routes.conf"
    routes.write_text(f"# test routes\n100 200 127.0.0.1:{sink_port}\n101 201 127.0.0.1:{sink_port}\n")
    port = free_port()
    proc = subprocess.Popen([BIN, "-r", str(routes), "-b", "127.0.0.1", "-p", str(port)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(0.3)
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.settimeout(1.0)
    yield proc, ("127.0.0.1", port), client, sink
    if proc.poll() is None:
        proc.terminate()
        proc.wait(timeout=5)


def finish(proc):
    proc.terminate()
    out, _ = proc.communicate(timeout=5)
    return out


def test_forwards_and_rewrites_teid(fwd):
    proc, addr, client, sink = fwd
    client.sendto(gpdu(100, b"payload-1"), addr)
    data = sink.recv(2048)
    assert data == gpdu(200, b"payload-1")
    out = finish(proc)
    assert "forwarded=1" in out


def test_second_route(fwd):
    proc, addr, client, sink = fwd
    client.sendto(gpdu(101, b"x"), addr)
    assert sink.recv(2048) == gpdu(201, b"x")
    finish(proc)


def test_drops_invalid_and_unknown(fwd):
    proc, addr, client, sink = fwd
    client.sendto(gpdu(999), addr)                              # no route
    client.sendto(gpdu(100, flags=0x50), addr)                  # version 2
    client.sendto(gpdu(100, length=99), addr)                   # bad length
    client.sendto(b"\x30\xff", addr)                            # too short
    client.sendto(gpdu(100, mtype=26), addr)                    # unsupported type
    with pytest.raises(socket.timeout):
        sink.recv(2048)
    time.sleep(0.2)
    out = finish(proc)
    assert "forwarded=0" in out
    assert "drop_no_route=1" in out
    assert "drop_bad_version=1" in out
    assert "drop_length_mismatch=1" in out
    assert "drop_too_short=1" in out
    assert "drop_unsupported_type=1" in out


def test_echo_response(fwd):
    proc, addr, client, sink = fwd
    req = struct.pack("!BBHI", 0x32, 1, 4, 0) + struct.pack("!HBB", 0xBEEF, 0, 0)
    client.sendto(req, addr)
    resp = client.recv(2048)
    assert resp[1] == 2
    assert resp[8:10] == b"\xbe\xef"
    out = finish(proc)
    assert "echo_replied=1" in out


def test_survives_burst_and_keeps_order_counts(fwd):
    proc, addr, client, sink = fwd
    sink.settimeout(2.0)
    got = 0
    # Send in bursts of 50 and drain the sink after each, so the test sink's
    # (kernel-capped) receive buffer cannot overflow and make the test flaky.
    for start in range(0, 500, 50):
        for i in range(start, start + 50):
            client.sendto(gpdu(100, struct.pack("!I", i)), addr)
        try:
            for _ in range(50):
                sink.recv(2048)
                got += 1
        except socket.timeout:
            break
    out = finish(proc)
    assert got == 500
    assert "rx=500" in out and "forwarded=500" in out


def test_bad_route_file_exits_nonzero(tmp_path):
    bad = tmp_path / "bad.conf"
    bad.write_text("not a route\n")
    r = subprocess.run([BIN, "-r", str(bad), "-p", str(free_port())],
                       capture_output=True, text=True, timeout=5)
    assert r.returncode != 0
    assert "bad route line" in r.stderr
