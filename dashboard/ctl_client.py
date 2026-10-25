"""Client for the gtpu-fwd control socket (see src/ctl.h)."""
import json
import socket
import threading


class CtlError(Exception):
    """The forwarder is unreachable or rejected a command."""


class CtlClient:
    def __init__(self, host="127.0.0.1", port=9100, timeout=1.0):
        self.addr = (host, port)
        self.timeout = timeout
        self._lock = threading.Lock()           # one request in flight keeps replies paired
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.settimeout(timeout)

    def call(self, command):
        with self._lock:
            try:
                self._drain()
                self._sock.sendto(command.encode(), self.addr)
                data, _ = self._sock.recvfrom(65535)
            except (socket.timeout, OSError) as exc:
                raise CtlError(f"forwarder control socket unreachable: {exc}") from exc
        try:
            return json.loads(data)
        except ValueError as exc:
            raise CtlError("invalid reply from forwarder") from exc

    def _drain(self):
        """Discard late replies from earlier timed-out requests."""
        self._sock.setblocking(False)
        try:
            while True:
                self._sock.recvfrom(65535)
        except (BlockingIOError, OSError):
            pass
        finally:
            self._sock.settimeout(self.timeout)

    def stats(self):
        return self.call("stats")

    def routes(self):
        out, offset = [], 0
        while True:
            page = self.call(f"routes {offset} 200")
            if not page.get("ok"):
                raise CtlError(page.get("error", "routes failed"))
            out.extend(page["routes"])
            offset += len(page["routes"])
            if not page["routes"] or offset >= page["total"]:
                return out

    def add_route(self, in_teid, out_teid, next_hop):
        return self.call(f"add {int(in_teid)} {int(out_teid)} {_clean_hop(next_hop)}")

    def del_route(self, in_teid):
        return self.call(f"del {int(in_teid)}")

    def inspect(self, hex_str):
        cleaned = "".join(hex_str.split())
        if len(cleaned) > 4096:
            return {"ok": False, "error": "packet too large"}
        return self.call(f"inspect {cleaned}")


def _clean_hop(value):
    value = str(value).strip()
    if not value or any(c.isspace() for c in value):
        raise ValueError("bad next hop")
    return value
