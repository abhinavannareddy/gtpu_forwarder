# Architecture

## Data path

```
            UDP :2152
   peer ───────────────►  worker 0 ─┐
   peer ───────────────►  worker 1 ─┼─► parse ─► classify ─► [inner check] ─► lookup ─► rewrite ─► sendto
   peer ───────────────►  worker N ─┘       │                                    │
                                            └─► drop + count (per reason)        └─► unknown TEID: drop (+ Error Indication)
   control client ──► main thread ──► ctl_handle ──► JSON reply
```

1. **Receive.** Each worker owns a UDP socket and pulls up to 32 datagrams per `recvmmsg` call into
   preallocated buffers. When a batch comes back full the worker skips `poll()` and reads again.
2. **Validate** (`gtpu_parse`). Version must be 1, the PT bit must be set, the length field must equal the
   datagram size minus 8, and optional header fields and the extension-header chain are walked with
   bounds checks. A zero-length extension header is rejected, otherwise a hostile packet could stall the parser.
3. **Classify.** Echo Request is answered on the receiving socket; G-PDU continues; anything else is counted
   as unsupported.
4. **Inner check** (`-i`). The payload must be a plausible IPv4 packet (version, IHL, total length within the
   payload, valid header checksum) or IPv6 packet (version, payload length within the payload).
5. **Look up and rewrite.** TEID to route in an open-addressing hash table; the 4-byte TEID is overwritten in
   place and the datagram is sent to the route's next hop. The next hop may be IPv4 or IPv6: a worker uses its
   receive socket when the family matches and a send-only socket of the other family otherwise.
6. **Unknown TEID.** Counted as `no_route`. With `-e` the worker replies with an Error Indication
   (TS 29.281 section 7.3.1: TEID Data I plus GTP-U Peer Address), limited to 1000 per second per worker
   because the source address of an unknown packet may be spoofed.

## Threading model

* `-w N` creates N workers, each bound to the same port with `SO_REUSEPORT`. The kernel hashes the
  4-tuple, so one flow always reaches the same worker and **packet order within a flow is preserved**.
* The **main thread** serves the control socket, the periodic statistics print and shutdown. Workers never
  block on it.
* **Counters.** Each worker is the only writer of its `stats_t`; the control thread sums them with relaxed
  atomic loads. No lock sits on the hot path for statistics.
* **Route table.** Protected by a read-write lock. A worker takes the read lock once per batch (not per
  packet); the control thread is the only writer, for `add` and `del`. Per-route packet and byte counters are
  updated with atomic adds because several workers may forward through the same route.
* **Shutdown.** `SIGINT`/`SIGTERM` set an atomic flag; workers notice within 200 ms, are joined, and the
  final totals are printed.

ThreadSanitizer runs the integration tests (including route changes under load) in `make tsan`.

## Control channel

A UDP socket speaking one text command per datagram and replying with JSON (`src/ctl.h`). Design choices:

* `ctl_handle` is a pure function of its inputs and a bounded output buffer, so it is unit tested without
  sockets and cannot overrun even with a tiny buffer.
* It binds to loopback by default. A token (`-t` or `GTPU_CTL_TOKEN`) is compared in constant time.
* Replies larger than a datagram are replaced by an error; the route list is paged (200 per page).

## Route table

Open addressing with linear probing and a 32-bit integer hash. Deletion uses backward-shift (no tombstones),
so lookups never degrade after churn. Capacity is a power of two chosen so the load factor stays below 0.5;
adding to a full table fails cleanly.

## Dashboard

`dashboard/server.py` samples `stats` twice a second, computes rates server-side and keeps two minutes of
history so every browser sees the same chart. It serves the UI and a JSON API and streams snapshots over
Server-Sent Events. The front end is dependency-free; charts are drawn on canvas. The packet builder
(`static/packet.js`) is shared by the browser and the Node tests, and the pytest suite replays every
scenario through the real C parser.

## Deliberately out of scope

* GTP-C, session state, charging and QoS.
* An eBPF/XDP fast path. A kernel-bypass comparison needs root and a BPF toolchain and was not built or
  measured, so nothing here claims a result for it.
