# Contributing

## Build and test

```bash
make            # builds with -Wall -Wextra -Wpedantic -Werror
make test       # C unit tests, pytest, Node tests
make asan       # AddressSanitizer + UBSan
make tsan       # ThreadSanitizer (needs Linux)
```

The forwarder is Linux-only (`recvmmsg`, `SO_REUSEPORT`). On Windows use WSL2. Python 3.9+ and Node 18+ are
needed for the integration and dashboard tests; `pip install pytest` is the only dependency.

## Guidelines

- Keep the hot path allocation-free and bounds-checked; every new packet field needs a unit test and a fuzz-friendly parser.
- New forwarder behaviour must be covered by an integration test against the real binary (`tests/`).
- Counters are single-writer per worker; use the `STAT_*` macros in `src/stats.h`.
- The dashboard has no build step and no dependencies. Keep it that way.
- Anything that touches the control protocol must update `src/ctl.h`, the README table and `SECURITY.md` if the trust model changes.
