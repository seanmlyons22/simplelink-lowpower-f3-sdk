# rftrace-decode

Logic-analyzer backend for the CC-series **RF-core (LRFDTRC) trace sink**
(`source/ti/log/LogSinkTraceLPF3`). Takes LA samples of the `rfctrc_out` pin →
deframes → decodes the tracer packet protocol → resolves dbgids → emits logs to
**Wireshark and stdout, identically to the ITM/UART `tilogger` path**.

Full spec: **`../../trace-pipeline-architecture.md.mkd`, Part B (§11–§18)**.
Wire ground truth: `rf_tracer_spec.md`. Metadata: `elf2dbgid.py` `DBG_DEF` files.

## Pipeline

```
samples(u8) → deframe → Word(10b) → classify → WordKind
            → PacketAssembler (3× per-channel SM + CRC-5 + timestamp) → DecodedPacket
            → resolve(&DbgIdDb)  → LogRecord → Output (stdout / pcap DLT_USER0=147, || payload)
```

## Status

Skeleton is **std-only, compiles offline, `cargo test` green** (9 pass, 2 `#[ignore]`).

Implemented + tested: `types`, `dbgid` (parser), `crc5`, `packet` (classify + full
state machine), `timestamp`, `record` (printf + resolve), `output` (stdout + pcap
`||`, tilogger-identical), `synth` (encoder), raw sample reader, `.sal` zip/meta parse.

**Stubs for Fable** (see `FABLE_PROMPT.md`):
1. `deframe::deframe` — clock recovery + bit sampling (samples → words).
2. `sample::decode_saleae_digital` — `.sal` `digital-N.bin` transition list → levels.
3. `golden_sal_end_to_end` test wiring (+ optional extcap/retain/tail).

## Run

```
cargo test
cargo run -- synth  --dbgid app_dbgid.h --out synth.raw
cargo run -- replay tx_burst_example.sal --channel 4 --dbgid app_dbgid.h --dbgid pbe_dbgid.h stdout
cargo run -- decode --raw synth.raw --channel 4 --dbgid app_dbgid.h stdout
```
