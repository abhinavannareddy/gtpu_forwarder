#!/bin/sh
# Runs the TSan build with ASLR off: newer kernels map memory where TSan cannot live.
here=$(dirname "$0")
if command -v setarch >/dev/null 2>&1; then exec setarch "$(uname -m)" -R "$here/../gtpu-fwd-tsan" "$@"; fi
exec "$here/../gtpu-fwd-tsan" "$@"
