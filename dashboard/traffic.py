"""Background GTP-U traffic generator used by the dashboard.

Produces a configurable mix of well-formed G-PDUs, packets for unknown TEIDs,
malformed packets and Echo Requests, so every counter in the forwarder moves.
"""
import random
import socket
import struct
import threading
import time

HDR = struct.Struct("!BBHI")


def gpdu(teid, payload=b"", flags=0x30, mtype=0xFF, length=None):
    length = len(payload) if length is None else length
    return HDR.pack(flags, mtype, length & 0xFFFF, teid & 0xFFFFFFFF) + payload


def echo_request(seq):
    return HDR.pack(0x32, 1, 4, 0) + struct.pack("!HBB", seq & 0xFFFF, 0, 0)


def malformed(rng):
    """One of several distinct ways to break a GTP-U packet."""
    kind = rng.choice(["version", "pt", "length", "short", "ext", "type"])
    body = bytes(rng.randrange(256) for _ in range(rng.randrange(8, 64)))
    teid = rng.randrange(1, 2 ** 32)
    if kind == "version":
        return gpdu(teid, body, flags=0x50)
    if kind == "pt":
        return gpdu(teid, body, flags=0x20)
    if kind == "length":
        return gpdu(teid, body, length=len(body) + rng.randrange(1, 40))
    if kind == "short":
        return gpdu(teid, b"")[: rng.randrange(0, 8)]
    if kind == "ext":                               # E flag, zero-length extension header
        payload = struct.pack("!HBB", 0, 0, 0x85) + b"\x00\x00\x00\x00"
        return gpdu(teid, payload, flags=0x34)
    return gpdu(teid, body, mtype=26)               # Error Indication: unsupported here


DEFAULT_MIX = {"valid": 80, "unknown": 8, "malformed": 8, "echo": 4}


class TrafficGenerator:
    def __init__(self, fwd_addr, get_teids, seed=None):
        self.fwd_addr = fwd_addr
        self.get_teids = get_teids              # callable returning known in-TEIDs
        self._rng = random.Random(seed)
        self._thread = None
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self.sent = 0
        self.pps = 0
        self.mix = dict(DEFAULT_MIX)
        self.started_at = None

    @property
    def running(self):
        return self._thread is not None and self._thread.is_alive()

    def status(self):
        with self._lock:
            return {"running": self.running, "pps": self.pps if self.running else 0,
                    "mix": dict(self.mix), "sent": self.sent,
                    "elapsed_s": round(time.time() - self.started_at, 1)
                    if self.running and self.started_at else 0}

    def start(self, pps, mix=None):
        pps = max(1, min(int(pps), 50000))
        weights = dict(DEFAULT_MIX)
        if mix:
            weights = {k: max(0.0, float(mix.get(k, 0))) for k in DEFAULT_MIX}
        if sum(weights.values()) <= 0:
            raise ValueError("mix must have a positive weight")
        with self._lock:
            self.pps, self.mix = pps, weights       # live-update when already running
            if self.running:
                return
            self._stop.clear()
            self.sent = 0
            self.started_at = time.time()
            self._thread = threading.Thread(target=self._run, daemon=True, name="trafficgen")
            self._thread.start()

    def stop(self):
        self._stop.set()
        t = self._thread
        if t:
            t.join(timeout=2)

    def _pick(self, seq):
        kinds, weights = zip(*self.mix.items())
        kind = self._rng.choices(kinds, weights)[0]
        teids = self.get_teids()
        if kind == "valid" and teids:
            size = self._rng.choice([64, 128, 256, 512, 1024])
            return gpdu(self._rng.choice(teids), bytes(size))
        if kind == "echo":
            return echo_request(seq)
        if kind == "malformed":
            return malformed(self._rng)
        known = set(teids)
        teid = self._rng.randrange(0x10000, 2 ** 32)
        while teid in known:
            teid += 1
        return gpdu(teid, bytes(64))                # unknown TEID

    def _run(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        tick, seq, owed = 0.02, 0, 0.0
        next_t = time.perf_counter()
        try:
            while not self._stop.is_set():
                owed += self.pps * tick
                burst, owed = int(owed), owed - int(owed)
                for _ in range(burst):
                    try:
                        sock.sendto(self._pick(seq), self.fwd_addr)
                    except OSError:
                        pass
                    seq += 1
                with self._lock:
                    self.sent = seq
                next_t += tick
                delay = next_t - time.perf_counter()
                if delay > 0:
                    self._stop.wait(delay)
                else:
                    next_t = time.perf_counter()     # fell behind: do not burst to catch up
        finally:
            sock.close()
