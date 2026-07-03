#!/usr/bin/env bash
# Live: sigrok logic analyzer -> rftrace decoder -> Wireshark, in one pipe.
#
#   sigrok-cli -O binary  |  tracedecode decode --raw -  pcap --out -  |  wireshark -k -i -
#
# The three stages are a straight pipeline: sigrok streams 1 byte/sample on the trace
# channel, the decoder deframes + resolves to the tilogger `||` pcap stream, and Wireshark
# reads that pcap on stdin. The DLT_USER 147 dissector must be installed (see README).
#
# The decoder streams: it reads samples in chunks and emits records live with bounded memory,
# so this pipe runs ENDLESSLY (no --samples/--time needed — omit them for continuous capture).
# Measured >780 MS/s worst-case, >1 GS/s idle, single core (> the 500 MS/s line rate).
#
# Usage:
#   scripts/sigrok-to-wireshark.sh --dbgid app_dbgid.h [--dbgid pbe_dbgid.h] \
#       [--driver saleae-logic-pro-16] [--channel 4] [--samplerate 500M] \
#       [--samples N | --time MS] [--divide-time-by-2]
#   (omit --samples/--time to run forever)
set -euo pipefail

driver="saleae-logic-pro-16"
channel=4
samplerate="500M"
bound=()           # endless by default; --samples/--time bound it if you want a fixed capture
dbgids=()
extra=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --driver)     driver="$2"; shift 2 ;;
    --channel)    channel="$2"; shift 2 ;;
    --samplerate) samplerate="$2"; shift 2 ;;
    --samples)    bound=(--samples "$2"); shift 2 ;;
    --time)       bound=(--time "$2"); shift 2 ;;
    --dbgid)      dbgids+=(--dbgid "$2"); shift 2 ;;
    --divide-time-by-2) extra+=(--divide-time-by-2); shift ;;
    -h|--help)    grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [[ ${#dbgids[@]} -eq 0 ]]; then
  echo "error: need at least one --dbgid <file> (elf2dbgid output)" >&2
  exit 2
fi

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
tracedecode="${TRACEDECODE:-$here/../target/release/tracedecode}"
wireshark_bin="${WIRESHARK:-wireshark}"

# sigrok packs enabled channels into bytes; capture only the trace channel so it lands on
# bit0 (decode reads bit `--channel`, so pass 0 downstream — the analog channel index only
# matters to sigrok's -C selector).
exec sigrok-cli \
    --driver "$driver" \
    --config samplerate="$samplerate" \
    -C "D$channel" \
    "${bound[@]}" \
    -O binary \
  | "$tracedecode" decode --raw - --channel 0 --samplerate "${samplerate/M/000000}" \
      "${dbgids[@]}" "${extra[@]}" pcap --out - \
  | "$wireshark_bin" -k -i -
