"""End-to-end tests of the control socket against the real binary."""
import json
import socket
import struct
import subprocess
import time

from conftest import BIN, free_port
from ctl_client import CtlClient


def gpdu(teid, payload=b"hi"):
    return struct.pack("!BBHI", 0x30, 0xFF, len(payload), teid) + payload


def wait_stats(c, pred, timeout=2.0):
    """Control and data sockets are independent, so poll until the counters catch up."""
    end = time.time() + timeout
    while True:
        st = c.stats()
        if pred(st) or time.time() > end:
            return st
        time.sleep(0.02)


def client(f):
    return CtlClient(*f.ctl_addr, timeout=1.0)


def test_stats_count_traffic_and_bytes(fwd_ctl):
    f = fwd_ctl
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for _ in range(5):
        s.sendto(gpdu(100), f.data_addr)
    s.sendto(gpdu(999), f.data_addr)
    s.sendto(b"\x30", f.data_addr)
    for _ in range(5):
        f.sink.recv(2048)
    st = wait_stats(client(f), lambda x: x["rx"] >= 7)
    assert st["ok"] and st["rx"] == 7 and st["forwarded"] == 5
    assert st["drop_no_route"] == 1 and st["drops"]["too_short"] == 1
    assert st["bytes_forwarded"] == 5 * 10 and st["routes"] == 2 and st["uptime_s"] > 0


def test_routes_listing_has_per_route_counters(fwd_ctl):
    f = fwd_ctl
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for _ in range(3):
        s.sendto(gpdu(101), f.data_addr)
    for _ in range(3):
        f.sink.recv(2048)
    routes = {r["in_teid"]: r for r in client(f).routes()}
    assert routes[101]["pkts"] == 3 and routes[101]["bytes"] == 30
    assert routes[100]["pkts"] == 0 and routes[100]["out_teid"] == 200


def test_runtime_add_and_delete_change_forwarding(fwd_ctl):
    f = fwd_ctl
    c = client(f)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sink_port = f.sink.getsockname()[1]
    s.sendto(gpdu(500), f.data_addr)
    assert wait_stats(c, lambda x: x["drop_no_route"] >= 1)["drop_no_route"] == 1   # not routed yet
    assert c.add_route(500, 600, f"127.0.0.1:{sink_port}")["ok"]
    s.sendto(gpdu(500, b"new"), f.data_addr)
    assert f.sink.recv(2048) == gpdu(600, b"new")             # TEID rewritten via runtime route
    assert c.add_route(500, 1, f"127.0.0.1:{sink_port}")["ok"] is False
    assert c.del_route(500)["ok"]
    s.sendto(gpdu(500), f.data_addr)
    assert wait_stats(c, lambda x: x["drop_no_route"] >= 2)["drop_no_route"] == 2   # gone again
    assert c.del_route(500)["ok"] is False


def test_inspect_matches_real_forwarding_decision(fwd_ctl):
    c = client(fwd_ctl)
    r = c.inspect(gpdu(100).hex())
    assert r["action"] == "forward" and r["route"]["out_teid"] == 200
    assert c.inspect(gpdu(12345).hex())["reason"] == "no_route"
    assert c.inspect("50ff000000000064")["reason"] == "bad_version"
    assert c.inspect("3201000400000000beef0000")["action"] == "echo_response"
    assert c.inspect("xyz")["ok"] is False
    assert c.stats()["rx"] == 0                                # a dry run must not touch counters


def test_many_routes_paginate(fwd_ctl):
    c = client(fwd_ctl)
    port = fwd_ctl.sink.getsockname()[1]
    for i in range(300):
        assert c.add_route(10_000 + i, i, f"127.0.0.1:{port}")["ok"]
    assert len(c.routes()) == 302


def test_garbage_on_control_socket_is_harmless(fwd_ctl):
    f = fwd_ctl
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    for junk in [b"", b"\xff\xfe\x00", b"add", b"add 1", b"del x", b"A" * 7000, b"routes -5 abc",
                 b"inspect " + b"0" * 5000]:
        s.sendto(junk, f.ctl_addr)
        assert json.loads(s.recv(65535))["ok"] in (True, False)
    assert f.proc.poll() is None
    assert client(f).stats()["ok"]


def test_control_port_validation_and_bind_conflict(tmp_path):
    routes = tmp_path / "r.conf"
    routes.write_text("1 2 127.0.0.1:9\n")
    r = subprocess.run([BIN, "-r", str(routes), "-c", "0"], capture_output=True, text=True, timeout=5)
    assert r.returncode == 2
    r = subprocess.run([BIN, "-r", str(routes), "-p", "70000"], capture_output=True, text=True, timeout=5)
    assert r.returncode == 2
    busy = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    busy.bind(("127.0.0.1", 0))
    r = subprocess.run([BIN, "-r", str(routes), "-p", str(free_port()), "-c", str(busy.getsockname()[1]),
                        "-C", "127.0.0.1"], capture_output=True, text=True, timeout=5)
    assert r.returncode == 1 and "bind" in r.stderr
