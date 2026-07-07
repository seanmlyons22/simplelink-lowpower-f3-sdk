# Fable one-shot prompt — clean up, re-architect, and re-test the RF-trace log tooling

Paste the block below to Fable. Attach / make available the whole worktree, in
particular:

- Rust decoder crate: `tools/rftrace-decode/` (all of `src/`, `tests/`, `extcap/`, `scripts/`)
- Python transport: `tools/log/tiutils/streams/rftrace/` (`tilogger_rftrace/*.py`, `setup.py`)
- Architecture doc: `trace-pipeline-architecture.md.mkd` (repo root) - this is the rewritten,
  self-contained ground truth (wire format, module map, CLI surface, decision log, and the
  open language question in section 12). Read it first; keep it in sync as you change code.
  Also reconcile `tools/rftrace-decode/README.md`, which still points at the old section layout.
- The oracle capture set, for benchmarks and golden checks:
  `tx_burst_example.sal`, `tx_burst_example_decoded.txt`,
  `rcl_..._dbgid.h` (app) + `_dbgid_pbe_generic.h` (LRF).

---

You are reviewing and cleaning up a working, already-shipped set of host tools that
capture the TI CC-series **RF-core trace pin** off a logic analyzer and pipe the decoded
logs into Wireshark and stdout, byte-for-byte identical to the existing ITM/UART
`tilogger` path. Two code trees plus glue make up the pipeline:

1. **Rust decoder** `tools/rftrace-decode/` (`tracedecode` binary): deframes LA samples
   into 10-bit words, reassembles per-channel packets (CRC-5), resolves them against
   `elf2dbgid` `DBG_DEF` tables into log records, and writes stdout / pcap. Subcommands:
   `replay`, `decode`, `tail`, `retain`, `wireshark` (extcap), `synth`.
2. **Python transport** `tools/log/tiutils/streams/rftrace/` (`tilogger rftrace`): a
   `tilogger` stream plugin that sources samples (`--sal` / `--raw` / `--sigrok` /
   `--logic2` Saleae Automation), extracts dbgid tables (`--elf`), spawns `tracedecode`,
   and hands its pcap to Wireshark / stdout.
3. **Wireshark + Logic 2 glue**: `extcap/rftrace-decode`, `scripts/sigrok-to-wireshark.sh`,
   `scripts/tilogger-rftrace.sh`, the reused Lua dissector, and the Logic 2 capture loop.

The crate compiles and `cargo test` is green; the golden `tx_burst_example.sal` decodes
end-to-end and matches the oracle. **This is a cleanup and hardening pass, not a rewrite.**
Do not change on-wire behavior or output format. Deliver a tree that a stranger can read,
trust, and extend, plus a rewritten architecture doc that matches the code exactly.

## Hard invariants (never break these)

- **Output stays byte-identical to `tilogger`**: pcap linktype `DLT_USER0 = 147`, one packet
  per log record, payload `alias||ts_local(.9f)||opcode||module||level||file||line||text`,
  consumed by the existing `tilogger_dissector.lua` unchanged; stdout matches the tilogger
  line format. Add a regression test that pins this if one does not already exist.
- **Metadata comes only from `elf2dbgid` `DBG_DEF` files.** The decoder never parses an ELF.
- Every test that is green today stays green.

## Task 1 — Decide the language question with a real benchmark, not opinion

The decoder core (deframe + packet reassembly + record resolve) is currently Rust. Decide,
**by measurement**, whether the whole inner decode can move to Python and let us drop Rust
entirely, collapsing the project to one language.

- Build a like-for-like Python decoder for the hot path (pure-Python first; if that is the
  only thing standing between us and dropping Rust, also try a NumPy-vectorized deframe).
  It must produce **identical records** to the Rust decoder on `tx_burst_example.sal` and on
  synthetic captures (assert equality against the Rust output, not by eye).
- Benchmark both on the Task 2 datasets (dense and sparse, ~5 GB). Report wall-clock,
  throughput in MS/s decoded, and peak RSS for each.
- The live use case is a **500 MS/s** stream that must decode faster than it arrives on a
  laptop with bounded memory. Use that as the bar.
- **Decide and write it down.** If Python (pure or NumPy) clears the bar with margin, port
  the core to Python, delete the Rust crate, and fold the decoder into the
  `tilogger_rftrace` package (one language, one home). If it does not, **keep Rust for the
  inner loop only**, and record the measured numbers that justify the split so nobody
  re-litigates it. Put the verdict, the numbers, and the reasoning in the architecture doc
  as a decision record.

## Task 2 — Massive dense + sparse decoder performance tests

Add a benchmark harness (a script or a Rust `--release` bench / Python bench — your call)
that generates and decodes two ~5 GB synthetic captures. Generate them programmatically
from the existing `synth` encoder path (reuse it; do not hand-roll wire bytes) so they are
reproducible and self-checking, and stream them (never hold 5 GB in RAM):

- **Dense**: back-to-back real packets across several channels, minimal idle — worst case
  for the reassembler and record resolver.
- **Sparse**: mostly NOP idle words with occasional packet bursts — worst case for the
  deframe clock-recovery / idle-skipping path (this is what a real mostly-quiet radio line
  looks like).

Each run asserts a correctness invariant cheaply (e.g. record count and a rolling checksum
of resolved records match what was synthesized) so the benchmark is also a stress test, and
prints throughput + peak memory. Gate them behind a marker / feature so ordinary `cargo test`
/ `pytest` stays fast, and document how to run them.

## Task 3 — Comment, idiom, and structure cleanup

Go module by module in both trees.

**Comments — rewrite, do not just trim:**
- Strip every reference to things outside this tree: no `§`/section numbers, no `ADR-NNN`,
  no `rf_tracer_spec.md` / `.mkd` / `PacketParser.cpp` / internal-spec pointers, and **no
  mention of RTL or VHDL**. Where such a comment carried real knowledge (e.g. "CRC-5 per
  §11.4", "timestamp rollover per PacketParser.cpp"), **inline the actual fact** — the poly,
  the init value, the tick period, the rollover rule — so the comment stands on its own.
- Remove all references to Claude / AI authorship and any "AI-speak" phrasing.
- **ASCII only.** Replace `§`, `÷`, `…`, `->` arrows-as-glyphs, non-breaking spaces, smart
  quotes, and Markdown `**bold**` living inside code comments with plain ASCII.
- Comments should say **why**, not **what**. Delete comments that merely restate the code;
  keep and sharpen the ones that explain a non-obvious decision, a hardware quirk, an
  empirically-found constant, or a subtle invariant.

**Idiom & SOLID/DRY:**
- Make each module idiomatic for its language (Rust: `Result`/`?`, iterators, `thiserror`
  or plain enums, no needless clones; Python: type hints, `pathlib`, dataclasses, context
  managers, no bare `except`). Fix anything that reads as ported-from-the-other-language.
- Hunt duplication across the deframe/synth boundary and across the Rust/Python split;
  factor shared constants and layouts to a single source of truth. Each module should have
  one clear responsibility — call out and fix any that have grown two.
- Do not add speculative abstraction. Collapse indirection that only has one implementation.

**Folder structure:**
- Confirm each piece lives in a sane place. The Python plugin under
  `tools/log/tiutils/streams/rftrace/` is a `tilogger` stream and belongs there; the extcap
  wrapper and shell glue belong with whatever they drive. If Task 1 drops Rust, move the
  decoder into the Python package and delete the crate cleanly (no dangling references in
  the transport, README, scripts, or extcap).

## Task 4 — Test coverage: every module, every CLI option, synthetic data first

Most of the weight goes on **module-level / unit tests using synthetic data**, then
integration, then the HIL/live path.

- **Unit**: each decoder module (deframe, packet SM + CRC-5, timestamp reconstruction,
  dbgid parse + printf substitution, output formatting, sample/`.sal` reader, synth encoder)
  gets focused tests driven by synthesized inputs, including edge cases: framing errors,
  CRC failures, sequence gaps, timestamp rollover, unknown dbgid, empty/idle-only input,
  truncated frames.
- **CLI**: **every** flag and subcommand gets a test that exercises it and asserts behavior.
  Enumerate and cover, at minimum —
  - `tracedecode` subcommands: `replay`, `decode`, `tail`, `retain`, `wireshark`, `synth`;
    flags: `--channel`, `--alias`, `--dbgid`, `--samplerate`, `--baud`,
    `--divide-time-by-2`, `--last`, `--out`, `--raw`, and the stdout/pcap output selectors.
  - `tilogger rftrace` options: `--elf`, `--dbgid`, `--sal`, `--raw`, `--sigrok`,
    `--logic2`, `--duration`, `--trigger`, `--trigger-channel`, `--after`, `--buffer-mb`,
    `--loop`, `--count`, `--logic2-port`, `--logic2-address`, `--logic2-device`,
    `--tracedecode`, `--channel`, `--samplerate`, `--baud`, `--divide-time-by-2`.
  - Cover the mutually-exclusive-source guard (needs one of `--sal`/`--raw`/`--sigrok`) and
    the `--logic2` device-poll path with the automation client mocked/faked (no hardware in
    CI).
- **Integration**: the golden `tx_burst_example.sal` decode-to-oracle test stays; add a
  full synth-roundtrip (records -> wire -> deframe -> reassemble -> records) that needs no
  external files.
- **HIL / live**: keep one documented, hardware-gated end-to-end check (Saleae -> decoder
  -> Wireshark). It must be clearly skipped when no device is present, never failing CI.

## Task 5 — Rewrite the architecture document completely

Rewrite `trace-pipeline-architecture.md.mkd` (and reconcile `tools/rftrace-decode/README.md`)
so it describes the **code as it now stands after this pass**, not the original plan:

- Same clean-comment rules apply: ASCII only, no RTL/VHDL, no AI references, why over what.
- Describe the final module layout, the data flow end to end, the wire format facts inline
  (physical layer, 10-bit word, packet state machine, CRC-5, timestamp) so the doc is
  self-contained ground truth rather than a pointer to an external spec.
- Fold in the Task 1 language verdict with its measured numbers as a decision record, and
  document the benchmark harness (Task 2) and how to run the full test suite.
- Keep a short, honest decision log; drop stale open-questions that the shipped code has
  already answered.

## Done =

`cargo test` (or `pytest`, if Task 1 collapsed to Python) fully green including the new
per-module and per-CLI-option tests; the two 5 GB dense/sparse benchmarks run and report
throughput + memory with correctness asserted; the golden `.sal` still decodes to the oracle
and Wireshark output is byte-identical to ITM; every comment is ASCII, why-focused, and free
of spec/RTL/AI references; and `trace-pipeline-architecture.md.mkd` matches the final tree.
