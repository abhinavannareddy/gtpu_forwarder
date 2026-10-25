"""Workers, strict inner-IP mode, Error Indication, IPv6 and control-channel authentication."""
import socket
import struct
import subprocess
import threading
import time

import pytest

from conftest import BIN, have_ipv6
from ctl_client import CtlClient, CtlError


def gpdu(teid, payload=b"hi"):
    return struct.pack("!BBHI", 0x30, 0xFF, len(payload), teid) + payload


def ipv4(total=40, good_sum=True):
    h = bytearray(20)
    h[0], h[2], h[3], h[8], h[9] = 0x45, total >> 8, total & 255, 64, 17
    h[12:16], h[16:20] = bytes([10, 0, 0, 1]), bytes([10, 0, 0, 2])
    if good_sum:
        s = sum((h[i] << 8) | h[i + 1] for i in range(0, 20, 2))
        while s >> 16:
            s = (s & 0xFFFF) + (s >> 16)
        h[10], h[11] = (~s >> 8) & 255, ~s & 255
    return bytes(h) + bytes(total - 20)


def wait_stats(c, pred, timeout=3.0):
    end = time.time() + timeout
    while True:
        st = c.stats()
        if pred(st) or time.time() > end:
            return st
        time.sleep(0.02)


# ------------------------------------------------------------------ workers
def test_workers_share_load_and_keep_counts_exact(launch):
    f = launch(["-w", "4"])
    c = CtlClient(*f.ctl_addr)
    f.sink.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    received = []

    def drain():
        f.sink.settimeout(0.5)
        try:
            while True:
                received.append(f.sink.recv(2048))
        except socket.timeout:
            pass

    senders = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(16)]   # 16 flows, 16 source ports
    for s in senders:
        s.bind(("127.0.0.1", 0))
    t = threading.Thread(target=drain)
    t.start()
    total = 0
    for rnd in range(20):
        for s in senders:
            s.sendto(gpdu(100, struct.pack("!I", rnd)), f.data_addr)
            total += 1
        time.sleep(0.005)
    t.join()
    st = wait_stats(c, lambda x: x["rx"] >= total)
    assert st["workers"] == 4
    assert st["rx"] == total and st["forwarded"] == total == len(received)
    assert {r["in_teid"]: r["pkts"] for r in c.routes()}[100] == total      # atomics: no lost updates


def test_route_changes_while_workers_are_busy(launch):
    f = launch(["-w", "4"], routes="100 200 127.0.0.1:SINK\n")
    c = CtlClient(*f.ctl_addr)
    sink_port = f.sink.getsockname()[1]
    stop = threading.Event()

    def blast():
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        while not stop.is_set():
            s.sendto(gpdu(100), f.data_addr)
            s.sendto(gpdu(500 + (int(time.time() * 1000) % 50)), f.data_addr)

    threads = [threading.Thread(target=blast) for _ in range(4)]
    [t.start() for t in threads]
    try:
        for i in range(100):                                  # churn the table under load
            assert c.add_route(500 + i % 50, 1, f"127.0.0.1:{sink_port}").get("ok") in (True, False)
            c.del_route(500 + i % 50)
    finally:
        stop.set()
        [t.join() for t in threads]
    assert f.proc.poll() is None and c.stats()["ok"]


def test_worker_option_validation():
    for bad in ["0", "65", "x", "-1"]:
        r = subprocess.run([BIN, "-r", "/dev/null", "-w", bad], capture_output=True, text=True, timeout=5)
        assert r.returncode == 2, bad


def test_clean_shutdown_with_workers(launch):
    f = launch(["-w", "8"])
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(gpdu(100), f.data_addr)
    f.sink.recv(2048)
    out = f.stop()
    assert "forwarded=1" in out and f.proc.returncode == 0


# ------------------------------------------------------------- strict inner
def test_strict_inner_ip_mode(launch):
    f = launch(["-i"])
    c = CtlClient(*f.ctl_addr)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(gpdu(100, ipv4()), f.data_addr)                      # valid IPv4
    s.sendto(gpdu(100, ipv4(good_sum=False)), f.data_addr)        # bad checksum
    s.sendto(gpdu(100, b"not an ip packet at all......"), f.data_addr)
    s.sendto(gpdu(100, ipv4(total=60)[:40]), f.data_addr)         # total length beyond payload
    f.sink.recv(2048)
    st = wait_stats(c, lambda x: x["rx"] >= 4)
    assert st["forwarded"] == 1 and st["drops"]["bad_inner_ip"] == 3 and st["strict_inner"] is True
    assert c.inspect(gpdu(100, ipv4()).hex())["action"] == "forward"
    assert c.inspect(gpdu(100, ipv4(good_sum=False)).hex())["reason"] == "bad_inner_ip"


def test_default_mode_does_not_inspect_inner_packet(launch):
    f = launch()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(gpdu(100, b"opaque"), f.data_addr)
    assert f.sink.recv(2048) == gpdu(200, b"opaque")


# ---------------------------------------------------------- error indication
def test_error_indication_for_unknown_teid(launch):
    f = launch(["-e"])
    c = CtlClient(*f.ctl_addr)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    s.sendto(gpdu(0xCAFE), f.data_addr)
    resp = s.recv(2048)
    assert resp[0] == 0x32 and resp[1] == 26 and resp[4:8] == bytes(4)        # TS 29.281 7.3.1
    assert resp[12] == 16 and struct.unpack("!I", resp[13:17])[0] == 0xCAFE   # TEID Data I
    assert resp[17] == 133 and resp[20:24] == bytes([127, 0, 0, 1])           # our address as peer IE
    st = c.stats()
    assert st["drop_no_route"] == 1 and st["error_indications"] == 1 and st["error_indication"] is True
    s.sendto(gpdu(100), f.data_addr)                                         # known TEID: no indication
    f.sink.recv(2048)
    with pytest.raises(socket.timeout):
        s.recv(2048)


def test_error_indication_is_off_by_default_and_rate_limited(launch):
    f = launch()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.3)
    s.sendto(gpdu(0xCAFE), f.data_addr)
    with pytest.raises(socket.timeout):
        s.recv(2048)
    g = launch(["-e"])
    s.settimeout(2)
    for _ in range(1500):
        s.sendto(gpdu(0xBEEF), g.data_addr)
    c = CtlClient(*g.ctl_addr)
    st = wait_stats(c, lambda x: x["rx"] >= 1500)
    assert st["drop_no_route"] == 1500 and 0 < st["error_indications"] <= 2000   # bounded: 1000/s per worker


def test_error_indication_requires_specific_bind_address(tmp_path):
    r = tmp_path / "r.conf"
    r.write_text("1 2 127.0.0.1:9\n")
    p = subprocess.run([BIN, "-r", str(r), "-e"], capture_output=True, text=True, timeout=5)
    assert p.returncode == 2 and "specific local address" in p.stderr


# ------------------------------------------------------------------- ipv6
@pytest.mark.skipif(not have_ipv6(), reason="no IPv6 loopback")
def test_ipv6_listener_and_next_hops(launch):
    sink6 = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sink6.bind(("::1", 0))
    sink6.settimeout(1.0)
    p6 = sink6.getsockname()[1]
    f = launch(bind="::1", routes=f"100 200 127.0.0.1:SINK\n101 201 [::1]:{p6}\n")
    s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    s.sendto(gpdu(101, b"six"), f.data_addr)                   # v6 in, v6 out
    assert sink6.recv(2048) == gpdu(201, b"six")
    s.sendto(gpdu(100, b"mixed"), f.data_addr)                 # v6 in, v4 out
    assert f.sink.recv(2048) == gpdu(200, b"mixed")
    echo = struct.pack("!BBHI", 0x32, 1, 4, 0) + struct.pack("!HBB", 0x1234, 0, 0)
    s.sendto(echo, f.data_addr)                                # echo reply goes back over v6
    assert s.recv(2048)[1] == 2
    c = CtlClient(*f.ctl_addr)
    assert any(r["next_hop"] == f"[::1]:{p6}" for r in c.routes())
    assert c.add_route(300, 301, f"[::1]:{p6}")["ok"]
    assert c.add_route(302, 303, "[::1]")["ok"] is False


@pytest.mark.skipif(not have_ipv6(), reason="no IPv6 loopback")
def test_dual_stack_wildcard_accepts_ipv4_clients(launch):
    f = launch(bind="::")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(gpdu(100, b"v4"), ("127.0.0.1", f.data_addr[1]))
    assert f.sink.recv(2048) == gpdu(200, b"v4")


@pytest.mark.skipif(not have_ipv6(), reason="no IPv6 loopback")
def test_ipv4_listener_forwards_to_ipv6_hop(launch):
    sink6 = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sink6.bind(("::1", 0))
    sink6.settimeout(1.0)
    f = launch(routes=f"100 200 [::1]:{sink6.getsockname()[1]}" + chr(10))
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(gpdu(100, b"cross"), f.data_addr)
    assert sink6.recv(2048) == gpdu(200, b"cross")


# -------------------------------------------------------------------- auth
def test_token_authentication(launch):
    f = launch(["-t", "s3cret"])
    assert CtlClient(*f.ctl_addr, token="s3cret").stats()["ok"]
    with pytest.raises(CtlError, match="token"):
        CtlClient(*f.ctl_addr).stats()
    with pytest.raises(CtlError, match="token"):
        CtlClient(*f.ctl_addr, token="nope").add_route(1, 2, "127.0.0.1:9")
    raw = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    raw.settimeout(1.0)
    for req in [b"stats", b"s3cretstats", b"s3cre stats", b"", b"s3cret", b"S3CRET stats"]:
        raw.sendto(req, f.ctl_addr)
        assert b"unauthorized" in raw.recv(65535), req
    c = CtlClient(*f.ctl_addr, token="s3cret")
    assert c.add_route(900, 901, "127.0.0.1:9")["ok"]
    assert c.stats()["routes"] == 3                             # rejected requests changed nothing


def test_token_from_environment(launch):
    f = launch(env={"GTPU_CTL_TOKEN": "fromenv"})
    assert CtlClient(*f.ctl_addr, token="fromenv").stats()["ok"]
    with pytest.raises(CtlError):
        CtlClient(*f.ctl_addr).stats()


def test_warning_for_open_control_socket(tmp_path):
    r = tmp_path / "r.conf"
    r.write_text("1 2 127.0.0.1:9\n")
    p = subprocess.Popen([BIN, "-r", str(r), "-p", "0", "-c", "0"], stderr=subprocess.PIPE, text=True)
    assert p.wait(timeout=5) == 2
    s = socket.socket()
    # ports must be valid: use real free ports and a non-loopback control address
    from conftest import free_port
    p = subprocess.Popen([BIN, "-r", str(r), "-p", str(free_port()), "-c", str(free_port()), "-C", "0.0.0.0"],
                         stderr=subprocess.PIPE, text=True)
    time.sleep(0.4)
    p.terminate()
    _, err = p.communicate(timeout=5)
    assert "no token" in err
    s.close()


def test_version_flag():
    r = subprocess.run([BIN, "-V"], capture_output=True, text=True, timeout=5)
    assert r.returncode == 0 and r.stdout.startswith("gtpu-fwd 1.")
