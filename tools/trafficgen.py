#!/usr/bin/env python3
"""GTP-U traffic generator and measurement client for gtpu-fwd.

Sends G-PDU packets to the forwarder. The forwarder's route for the chosen
TEID must point back to this tool's receive port, so send and receive times
come from the same clock and latency can be measured exactly.

Example:
    ./gtpu-fwd -r tests/bench_routes.conf -p 2152 &
    python3 tools/trafficgen.py --fwd 127.0.0.1:2152 --listen 9000 \
        --teid 1000 --expect-teid 2000 --packets 200000 --size 128
"""
import argparse
import socket
import statistics
import struct
import threading
import time

HDR = struct.Struct("!BBHI")      # flags, type, length, TEID
STAMP = struct.Struct("!QI")      # send time (ns), sequence number


def build_gpdu(teid, seq, size, t_ns):
    payload = STAMP.pack(t_ns, seq)
    payload += bytes(max(0, size - len(payload)))
    return HDR.pack(0x30, 0xFF, len(payload), teid) + payload


def percentile(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    k = min(len(sorted_vals) - 1, int(round(p / 100.0 * (len(sorted_vals) - 1))))
    return sorted_vals[k]


def run(args):
    host, port = args.fwd.split(":")
    fwd = (host, int(port))

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
    rx.bind(("127.0.0.1", args.listen))
    rx.settimeout(0.5)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 * 1024 * 1024)

    latencies = []
    bad_teid = [0]
    done = threading.Event()

    def receiver():
        while not (done.is_set() and last_rx_idle[0] > 3):
            try:
                data = rx.recv(4096)
            except socket.timeout:
                if done.is_set():
                    last_rx_idle[0] += 1
                continue
            now = time.perf_counter_ns()
            last_rx_idle[0] = 0
            _, _, _, teid = HDR.unpack_from(data)
            if args.expect_teid is not None and teid != args.expect_teid:
                bad_teid[0] += 1
                continue
            t_ns, _ = STAMP.unpack_from(data, HDR.size)
            latencies.append(now - t_ns)

    last_rx_idle = [0]
    th = threading.Thread(target=receiver, daemon=True)
    th.start()

    interval = 1.0 / args.rate if args.rate > 0 else 0.0
    start = time.perf_counter()
    next_t = start
    for seq in range(args.packets):
        if interval:
            next_t += interval
            while time.perf_counter() < next_t:
                pass
        tx.sendto(build_gpdu(args.teid, seq, args.size, time.perf_counter_ns()), fwd)
    send_secs = time.perf_counter() - start
    done.set()
    th.join(timeout=10)
    total_secs = max(1e-9, send_secs)

    got = len(latencies)
    lat_us = sorted(x / 1000.0 for x in latencies)
    print(f"sent            : {args.packets}")
    print(f"received        : {got}  (loss {100.0 * (args.packets - got) / args.packets:.2f}%)")
    print(f"wrong TEID      : {bad_teid[0]}")
    print(f"send duration   : {send_secs:.3f} s")
    print(f"throughput      : {got / total_secs:,.0f} packets/s received")
    if lat_us:
        print(f"latency (us)    : p50={percentile(lat_us, 50):.1f}  p99={percentile(lat_us, 99):.1f}  "
              f"max={lat_us[-1]:.1f}  mean={statistics.fmean(lat_us):.1f}")
    return got


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fwd", default="127.0.0.1:2152", help="forwarder ip:port")
    ap.add_argument("--listen", type=int, default=9000, help="local port the route points back to")
    ap.add_argument("--teid", type=int, default=1000, help="TEID to send")
    ap.add_argument("--expect-teid", type=int, default=2000, help="TEID expected after rewrite")
    ap.add_argument("--packets", type=int, default=100000)
    ap.add_argument("--size", type=int, default=128, help="GTP-U payload bytes")
    ap.add_argument("--rate", type=int, default=0, help="target packets/s (0 = as fast as possible)")
    run(ap.parse_args())


if __name__ == "__main__":
    main()
