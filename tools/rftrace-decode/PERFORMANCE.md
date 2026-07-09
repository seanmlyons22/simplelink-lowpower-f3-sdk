# tracedecode performance notes (huge files, long captures)

Measured on a release build decoding a synthetic **1.96 GB** raw capture (600k packets)
on this workstation. The short version: the streaming decode paths are already
bounded-memory and fast, so an hours-long capture is not a problem as long as you use a
streaming subcommand.

## Measured

| path | input | time | peak RSS | notes |
|---|---|---|---|---|
| `decode --raw big.raw ... stdout` | 1.96 GB samples, 600k pkts | ~2.9 s | **5.4 MB** | streaming; RSS is independent of file size |

- **Memory is bounded on the streaming paths.** `decode --raw` / `decode --words` read the
  input in fixed chunks (1 MiB samples / 4 KiB bytes) and emit records as they go, keeping
  only the current chunk plus a one-frame carry. A 2 GB or a 200 GB capture both decode in a
  few MB of RAM.
- **The sample deframer is the throughput limit**, ~700 MB/s of samples here (edge detection +
  bit-center sampling), not the output. Output buffering (below) is in the noise for the
  sample path; it only matters when there is no deframe to dominate (the word-stream path).

## Which subcommand for a big capture

- **`decode --raw <file|->` and `decode --words <file|->` stream** — bounded memory, no
  ceiling. Prefer these for huge or long captures. `--words` (an RFT1 byte stream from the
  Pico) skips sample deframing entirely, so it is the fastest offline path.
- **`replay <file.sal>` is batch** — it decodes the whole `.sal` edge list into one
  `Vec<Word>` before formatting. For a Saleae capture this is fine in practice: the `.sal`
  RLE and the edge-based deframer cost nothing on an idle line (no edges -> no words), so
  memory scales with *actual logged volume*, not wall-clock time. Only a capture that is
  genuinely busy for many hours would grow the word vector into the hundreds of MB. If that
  ever bites, decode the underlying samples with `decode --raw` (streaming) instead.

## stdout buffering

Rust's stdout is line-buffered, so an unbuffered `println!` per record is a write syscall per
line. `StdoutSink` therefore buffers when stdout is **not** a terminal (redirected to a file
or pipe: bulk decode) and flushes every line when it **is** a terminal (a human watching
live). This is measured-neutral on the deframe-bound sample path above, but removes the
per-line syscall on the output-bound `decode --words` replay path and never regresses live
use (the tilogger integration uses the pcap sink, not this one). Output bytes are identical
either way.

`PcapSink` still flushes every record: the live consumers (the Wireshark FIFO, the tilogger
transport) need each record immediately. A `pcap --out <regular file>` bulk export pays ~4x
for that; if bulk pcap export ever becomes a real workflow, gate the per-record flush on
whether the sink is a regular file (only flush pipes/fifos).
