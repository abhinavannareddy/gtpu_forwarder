# Security

## Reporting a vulnerability

Please open a private security advisory on GitHub (Security tab, "Report a vulnerability") rather than a
public issue. Include the version (`gtpu-fwd -V`), the command line and, if possible, a packet or control
request that reproduces the problem.

## Threat model

| Surface | Exposure | Protection |
|---|---|---|
| GTP-U port (UDP 2152) | untrusted network | Every field is bounds-checked; the parser is fuzzed and runs clean under ASan, UBSan and Valgrind in CI. Malformed packets are dropped and counted, never trusted. |
| Control socket (`-c`) | administrative | Binds to `127.0.0.1` by default. Optional shared token (`-t` / `GTPU_CTL_TOKEN`), compared in constant time. The forwarder prints a warning when the socket is exposed without a token. |
| Dashboard (`dashboard/server.py`) | administrative | Binds to `127.0.0.1` by default; serves only files under `dashboard/static` (path traversal tested); request bodies are size limited; control commands are built from validated values only (injection tested). It has **no login of its own**. |
| Error Indication (`-e`) | untrusted network | Off by default. Replies go to the datagram's source address, which may be spoofed, so they are rate limited (1000/s per worker) and are small. |

## Operating guidance

* The control protocol is a shared secret over UDP **without encryption**. Keep it on loopback or on a private
  network such as a container network. Do not expose it to the internet.
* If you expose the dashboard beyond localhost, put it behind a reverse proxy that provides TLS and authentication.
* Run the forwarder as an unprivileged user. Port 2152 is above 1024, so no capabilities are required;
  `deploy/gtpu-fwd.service` shows a sandboxed configuration.
* Set `GTPU_CTL_TOKEN` to a long random value (`openssl rand -hex 24`). The compose file ships a demo
  token that you must replace outside a demo.
