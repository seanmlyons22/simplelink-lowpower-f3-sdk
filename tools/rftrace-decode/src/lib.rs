//! RF-core trace-sink logic-analyzer backend.
//!
//! Decodes logic-analyzer captures of the CC-series RF-core tracer pin into log records,
//! presented byte-identically to the ITM/UART `tilogger` path:
//!
//! ```text
//! samples(u8) -> deframe -> Word(10b) -> classify -> WordKind
//!             -> PacketAssembler(3x per-channel SM + CRC-5 + timestamp) -> DecodedPacket
//!             -> resolve(&DbgIdDb) -> LogRecord -> Output (stdout / pcap DLT_USER0=147)
//! ```
//!
//! Wire facts (confirmed against the golden `tx_burst_example.sal` capture): inverted
//! async-serial line, 12-bit frame (start, 10 data bits MSB-first, trailing zero),
//! 24 Mbaud, NOP idle fill, CRC-5 MSB-first (poly 0x05, init 0x1F), 16-bit hardware
//! timestamp at 2 MHz. Metadata comes only from `elf2dbgid` `DBG_DEF` files; the decoder
//! never parses an ELF. See `trace-pipeline-architecture.md.mkd` at the repo root for the
//! full pipeline description.

pub mod crc5;
pub mod dbgid;
pub mod deframe;
pub mod output;
pub mod packet;
pub mod record;
pub mod sample;
pub mod synth;
pub mod timestamp;
pub mod types;

pub use types::*;
