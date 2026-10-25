# Changelog

## 1.1.0 - 2026-10-25

### Added
- **Control room**: web dashboard with live throughput, drop reasons, route table, packet lab and traffic generator.
- **Control socket** (`-c`, `-C`): `stats`, `routes`, `add`, `del`, `inspect`, JSON replies.
- **Token authentication** for the control socket (`-t` or `GTPU_CTL_TOKEN`), constant-time comparison.
- **Multi-threaded workers** (`-w N`) on `SO_REUSEPORT` sockets, order preserved per flow.
- **IPv6**: listen on IPv6 or dual-stack `::`, IPv4 and IPv6 next hops (`[addr]:port`).
- **Strict inner-IP validation** (`-i`) for IPv4 (including header checksum) and IPv6.
- **GTP-U Error Indication** for unknown TEIDs (`-e`), rate limited.
- Runtime route add/delete, per-route packet and byte counters.
- `docker compose` setup, dashboard image, systemd unit, ThreadSanitizer target (`make tsan`), `-V`.

### Changed
- Strict validation of `-p`, `-s`, `-w` values (invalid values exit with status 2).
- Main loop uses `poll()` plus non-blocking `recvmmsg`.

### Fixed
- Shutdown flag is a proper atomic (found by ThreadSanitizer once workers were introduced).

## 1.0.0

- GTP-U forwarder with TEID rewrite, Echo handling, per-reason drop counters, unit and integration tests, Docker image and CI.
