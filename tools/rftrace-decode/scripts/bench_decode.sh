#!/usr/bin/env bash
#
# Decode throughput + memory benchmark for the RF-trace decoder.
#
# Generates a large synthetic capture with the decoder's own synth encoder (so the
# expected content is known exactly), streams it through a decode, and reports MS/s
# and peak RSS. Each run asserts correctness cheaply - exact record count plus
# all-zero health counters (framing / CRC / seq-gap) - so the benchmark doubles as
# a stress test. Two profiles:
#
#   dense   back-to-back packets across all three channels, minimal idle
#           (worst case for the packet reassembler and record resolver)
#   sparse  ~99% NOP idle frames with periodic packet bursts, i.e. what a real
#           mostly-quiet radio line looks like (worst case for the deframe
#           idle-skipping path; NOP frames still carry edges every frame, so
#           this is NOT a static level run)
#
# The point is to size the decoder against the 500 MS/s live line and to feed the
# language decision (trace-pipeline-architecture.md.mkd, "Decoder language") with
# real numbers.
#
# DECODE names the decoder under test; SYNTH names the generator (defaults to
# DECODE). Splitting them lets an alternative decoder implementation be measured
# against captures produced by the reference synth path:
#
#   DECODE=tracedecode SIZE_GB=5 ./bench_decode.sh dense
#   DECODE="python3 pydecode.py" SYNTH=tracedecode SIZE_GB=5 ./bench_decode.sh sparse
#
# Defaults are small so it is runnable in CI; set SIZE_GB=5 for the real numbers.
# Captures land in a mktemp dir under TMPDIR; point TMPDIR at a real disk (not a
# small tmpfs) for multi-GB runs.

set -euo pipefail

PROFILE="${1:-dense}"
DECODE="${DECODE:-${TRACEDECODE:-tracedecode}}" # decoder under test
SYNTH="${SYNTH:-$DECODE}"                       # capture generator (reference synth)
CHANNEL="${CHANNEL:-4}"
SIZE_GB="${SIZE_GB:-0.2}"                       # target raw-capture size

case "$PROFILE" in
  dense) idle_frames=2 ;;
  sparse) idle_frames=4000 ;;
  *) echo "usage: $0 {dense|sparse}" >&2; exit 2 ;;
esac

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
dbgid="$work/bench_dbgid.h"
cap="$work/capture.raw"

# Three defs, one per channel, mixing 32-bit, 16-bit, and zero-arg packets.
cat > "$dbgid" <<'EOF'
DBG_DEF(DBGID_bench_c_1, 5, DBGCH1, -1, "val %08X", "bench.c", 1)
DBG_DEF(DBGID_bench_c_2, 6, DBGCH2, 2, "a=%d b=%d", "bench.c", 2)
DBG_DEF(DBGID_bench_c_3, 7, DBGCH3, 0, "marker", "bench.c", 3)
EOF
ndefs=3

# Size one repetition, then scale the repeat count to the target size.
$SYNTH synth --dbgid "$dbgid" --channel "$CHANNEL" --idle-frames "$idle_frames" \
    --repeat 1 --out "$cap" 2>/dev/null
unit_bytes="$(stat -c%s "$cap")"
target_bytes="$(awk -v g="$SIZE_GB" 'BEGIN{printf "%d", g*1024*1024*1024}')"
repeats="$(( target_bytes / unit_bytes ))"
[ "$repeats" -lt 1 ] && repeats=1
expect_records="$(( ndefs * repeats ))"

$SYNTH synth --dbgid "$dbgid" --channel "$CHANNEL" --idle-frames "$idle_frames" \
    --repeat "$repeats" --out "$cap" 2>/dev/null

cap_bytes="$(stat -c%s "$cap")"
echo "profile=$PROFILE size=$cap_bytes bytes repeats=$repeats expect_records=$expect_records"

# Measure wall time + peak RSS around a streaming decode to stdout.
timelog="$work/time.txt"
errlog="$work/stderr.txt"
records="$(
  /usr/bin/time -v -o "$timelog" \
    $DECODE decode --raw "$cap" --channel "$CHANNEL" --dbgid "$dbgid" stdout \
    2>"$errlog" | grep -c '^rftrc ' || true
)"

# Correctness invariants: every synthesized packet decoded, and the decoder's own
# health counters agree that nothing was dropped or corrupted on the way.
if [ "$records" != "$expect_records" ]; then
  echo "FAIL: decoded $records records, expected $expect_records" >&2
  exit 1
fi
if ! grep -q 'framing=0 crc=0 overflow=0 unknown_dbgid=0 dropped_seq=0' "$errlog"; then
  echo "FAIL: nonzero health counters:" >&2
  grep '\[health\]' "$errlog" >&2 || cat "$errlog" >&2
  exit 1
fi

elapsed="$(awk -F': ' '/Elapsed \(wall clock\)/{print $2}' "$timelog")"
rss_kb="$(awk -F': ' '/Maximum resident set size/{print $2}' "$timelog")"
secs="$(awk -v t="$elapsed" 'BEGIN{n=split(t,a,":"); print (n==3)?a[1]*3600+a[2]*60+a[3]:a[1]*60+a[2]}')"
mss="$(awk -v b="$cap_bytes" -v s="$secs" 'BEGIN{ if(s>0) printf "%.0f", b/s/1e6; else print "n/a" }')"

echo "OK: $records records | ${secs}s | ${mss} MS/s | peak RSS ${rss_kb} KB"
echo "    (live line = 500 MS/s; ratio $(awk -v m="$mss" 'BEGIN{printf "%.1fx", m/500}'))"
