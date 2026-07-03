//! RF-core trace-sink logic-analyzer backend.
//!
//! Pipeline (see `trace-pipeline-architecture.md.mkd` Part B, §11-§18):
//!
//! ```text
//! samples(u8) -> deframe -> Word(10b) -> classify -> WordKind
//!             -> PacketAssembler(3x SM + CRC-5 + timestamp) -> DecodedPacket
//!             -> resolve(&DbgIdDb) -> LogRecord -> Output (stdout / pcap|wireshark)
//! ```
//!
//! Ground truth wire spec: `rf_tracer_spec.md`. Metadata: `elf2dbgid.py` `DBG_DEF` files.
//! Confirmed deframe (from `tx_burst_example.sal`): inverted async-serial, 10 data bits,
//! MSB-first, 24 Mbaud, 1 start/1 stop, no parity, trace pin = LA channel 4, 500 MS/s.
//!
//! Fully implemented: types, dbgid parser, CRC-5 (MSB-first — validated 28/28 against the
//! golden capture), word classify, packet state machine, printf, timestamp reconstruct,
//! pcap+stdout emit (`||` tilogger-identical), synth encoder, raw reader, `.sal` reader
//! (block/varint run-length format, reverse-engineered; see `sample::decode_saleae_runs`),
//! edge-based deframer, extcap/retain/tail subcommands.

pub mod types;
pub mod dbgid;
pub mod crc5;
pub mod deframe;
pub mod packet;
pub mod timestamp;
pub mod record;
pub mod sample;
pub mod synth;
pub mod output;

pub use types::*;
