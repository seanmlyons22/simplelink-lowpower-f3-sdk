#!/usr/bin/env bash
# End-to-end launcher for the tilogger `rftrace` transport.
#
# Builds the tracedecode backend if needed, exports $TRACEDECODE, and runs
# `tilogger rftrace <your args>`. Everything after this script's own flags is passed
# straight through to `tilogger rftrace` (source + outputs).
#
# Examples:
#   # replay a capture to Wireshark (auto-launch + configure)
#   scripts/tilogger-rftrace.sh --sal cap.sal --dbgid app_dbgid.h --divide-time-by-2 wireshark --start
#   # endless live capture from a Saleae, to stdout
#   scripts/tilogger-rftrace.sh --sigrok "--driver saleae-logic-pro-16 --config samplerate=500M -C D4" \
#       --dbgid app_dbgid.h --channel 0 --divide-time-by-2 stdout
#
# Needs `tilogger` on PATH with the rftrace transport installed:
#   pip install -e core -e streams/rftrace -e streams/stdout -e streams/wireshark   (from tools/log/tiutils)
set -euo pipefail

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
crate="$here/.."
bin="${TRACEDECODE:-$crate/target/release/tracedecode}"

if [[ ! -x "$bin" ]]; then
  echo "building tracedecode…" >&2
  ( cd "$crate" && cargo build --release )
  bin="$crate/target/release/tracedecode"
fi
export TRACEDECODE="$bin"

if ! command -v tilogger >/dev/null 2>&1; then
  echo "error: 'tilogger' not on PATH. Install it from tools/log/tiutils (see README)." >&2
  exit 1
fi

exec tilogger rftrace "$@"
