# Fable one-shot prompt — finish `rftrace-decode`

Paste the block below to Fable, with these files attached/available:
`trace-pipeline-architecture.md.mkd` (Part B is the spec), `rf_tracer_spec.md`,
`elf2dbgid.py`, and the oracle capture set:
`tx_burst_example.sal`, `tx_burst_example_decoded.txt`, `*_dbgid.h` (app) + `_dbgid_pbe_generic.h`.

---

You are completing a Rust program, `rftrace-decode` (crate at `tools/rftrace-decode/`), that
decodes the TI CC-series **RF-core LRFDTRC trace pin** captured by a logic analyzer and pipes
the logs into **Wireshark and stdout, byte-identically to the existing ITM/UART `tilogger`
path**. The authoritative spec is `trace-pipeline-architecture.md.mkd` **Part B (§11–§18)**;
the wire ground truth is `rf_tracer_spec.md`.

The crate already **compiles and `cargo test` is green** (9 pass). DO NOT rewrite or regress the
working, tested modules: `types`, `dbgid`, `crc5`, `packet` (classify + `PacketAssembler` state
machine), `timestamp`, `record` (printf + resolve), `output` (stdout + pcap `||` payload),
`synth` (encoder). They encode the spec — read them as reference.

## Implement exactly these, then make the two `#[ignore]`d tests pass

### 1. `src/deframe.rs::deframe(levels: &[u8], cfg: &DeframeCfg) -> Vec<Word>`
The physical hot loop. `levels` are per-sample line values (0/1) for the trace channel.
Confirmed params (`cfg` defaults, from the `tx_burst_example.sal` analyzer): **inverted,
10 data bits, MSB-first, 24 Mbaud, 1 start / 1 stop, no parity, 500 MS/s** →
`samples_per_bit ≈ 20.83`.
Algorithm (§11.1, ADR-005/007):
- De-invert when `cfg.invert` (line should read idle-high, start bit = 1).
- Lock the frame period from start-pulse edges; keep a low-passed estimate; resync phase each
  start edge (fractional bit-center accumulator — the ratio is non-integer).
- Per frame: sample `2 + data_bits` bits at bit centers, verify the stop bit (count framing
  errors, don't panic), then call the provided `decode_frame(bits, cfg)` to get the `Word`.
- Emit every word including idle NOP (`0x000`). Stream; don't buffer the whole capture.
Reuse `decode_frame`/`encode_frame` (already implemented, shared with `synth`).
**Gate:** un-ignore `synth_roundtrip_via_deframe` in `tests/integration.rs` — it must pass
(`synth::words_to_samples → deframe → classify → PacketAssembler` round-trips a packet).

### 2. `src/sample.rs::decode_saleae_digital(bin: &[u8], samplerate_hz: f64) -> io::Result<Vec<u8>>`
Decode a Saleae `.sal` `digital-N.bin` (transition list) into per-sample levels.
Ground-truth header bytes of the real file are in the function's doc comment. Layout is
Saleae's digital binary: `"<SALEAE>"` magic, then version/flags, `initial_state`, begin/end
time (f64), `num_transitions` (u64), then `num_transitions` edge timestamps (f64 seconds).
**Finalize the exact field offsets empirically against `tx_burst_example.sal`** (print parsed
values; the transition count must satisfy `header_len + 8*count == file_len`). Expand to a
level timeline sampled at `samplerate_hz`, starting at `initial_state`, toggling at each edge.
`meta.json` (already parsed for sample rate) also lists the analyzer settings if you want to
auto-config the deframer.

### 3. End-to-end gate + polish
- Wire up `golden_sal_end_to_end` in `tests/integration.rs`: `read_sal(tx_burst_example.sal, 4)`
  → deframe → pipeline → collect `(file, line, text)`; parse `tx_burst_example_decoded.txt`
  (4-line blocks: alias / abs-time / duration / `file:line >> text`) into expected records;
  assert the **app+pbe-channel subset matches in order** (radio `rfe`/`mce` dbgids aren't
  supplied — skip unknown dbgids).
- Add the `wireshark` (extcap + named-pipe/fifo), `retain` (dumpcap ring), and `tail`
  subcommands (§14/§15). extcap: `--extcap-interfaces/-dlts/-config/--capture --fifo`; write
  the same pcap stream to the fifo.

## Two empirical unknowns to resolve against the golden capture
- **CRC-5 bit convention.** `crc5::Crc5` is MSB-first (poly 0x05, init 0x1F) and self-consistent
  with `synth`. If real packets from `tx_burst_example.sal` report `crc_ok == false`, switch to
  the reflected form (`crc5::crc5_usb` shows that arithmetic) until residue/pass matches the RTL.
- **`.sal` transition offsets** (task 2).

## Hard constraints
- **Output must stay byte-identical to `tilogger`**: pcap linktype `DLT_USER0 = 147`, one packet
  per log, payload `alias||ts_local(.9f)||opcode||module||level||file||line||text`, consumed by
  the existing `tilogger_dissector.lua` **unchanged**. stdout matches the `tilogger` line format.
  The golden `.txt` is a **correctness oracle only — NOT the output format.**
- Metadata comes only from `elf2dbgid` `DBG_DEF` files; never parse the ELF in this program.
- Keep all currently-passing tests passing.
- You MAY add deps (`clap`, `zip`, `serde_json`, `thiserror`) — the skeleton is std-only merely
  to bootstrap; swap the `unzip` subprocess in `sample.rs` for the `zip` crate if you add it.

## Done =
`cargo test` fully green (incl. the two un-ignored tests); `cargo run -- replay
tx_burst_example.sal --channel 4 --dbgid <app> --dbgid <pbe> stdout` prints the app+pbe log
lines matching the oracle; `... wireshark --extcap` shows them in Wireshark via the existing
Lua dissector, indistinguishable from ITM.
