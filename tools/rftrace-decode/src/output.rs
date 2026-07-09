//! Output sinks. All produce the same presentation as the ITM/UART `tilogger` path:
//! a stdout line, and pcap `DLT_USER0=147` with the `||`-delimited payload consumed by
//! `tilogger_dissector.lua`. Keeping the bytes identical is what lets the existing
//! dissector and every tilogger output work unchanged on RF-tracer logs.

use crate::types::{Health, LogRecord};
use std::io::{self, Write};

pub trait Output {
    fn on_record(&mut self, r: &LogRecord);
    fn on_health(&mut self, _h: &Health) {}
    fn finish(&mut self) {}
}

/// The `||`-delimited Wireshark payload, byte-identical to what `tilogger` emits:
/// `alias || ts_local(.9f s) || opcode || module || level || file || line || text`.
pub fn wireshark_payload(r: &LogRecord) -> String {
    format!(
        "{}||{:.9}||{}||DBGCH{}||{}||{}||{}||{}",
        r.alias,
        r.ts_us / 1_000_000.0,
        "LOG_OPCODE_FORMATED_TEXT",
        r.channel,
        r.level,
        r.file,
        r.line,
        r.text,
    )
}

/// Human line, matching the `tilogger` stdout default `{I} | {T} | {M} | {L} | {F} | {D}`.
pub fn stdout_line(r: &LogRecord) -> String {
    format!(
        "{} | {:.9} | DBGCH{} | {} | {}:{} | {}",
        r.alias,
        r.ts_us / 1_000_000.0,
        r.channel,
        r.level,
        r.file,
        r.line,
        r.text,
    )
}

/// stdout sink.
pub struct StdoutSink;

impl Output for StdoutSink {
    fn on_record(&mut self, r: &LogRecord) {
        println!("{}", stdout_line(r));
    }
    fn on_health(&mut self, h: &Health) {
        eprintln!(
            "[health] framing={} crc={} overflow={} unknown_dbgid={} arg_mismatch={} dropped_seq={}",
            h.framing_errors, h.crc_errors, h.overflow_words, h.unknown_dbgid, h.arg_mismatch, h.dropped_seq
        );
    }
}

/// pcap writer (classic pcap; the dissector is container-agnostic).
pub struct PcapSink<W: Write> {
    w: W,
}

const PCAP_MAGIC: u32 = 0xA1B2_C3D4;
const DLT_USER0: u32 = 147;

impl<W: Write> PcapSink<W> {
    pub fn new(mut w: W) -> io::Result<Self> {
        // Global header.
        w.write_all(&PCAP_MAGIC.to_le_bytes())?;
        w.write_all(&2u16.to_le_bytes())?; // version major
        w.write_all(&4u16.to_le_bytes())?; // version minor
        w.write_all(&0i32.to_le_bytes())?; // thiszone
        w.write_all(&0u32.to_le_bytes())?; // sigfigs
        w.write_all(&256u32.to_le_bytes())?; // snaplen
        w.write_all(&DLT_USER0.to_le_bytes())?; // network
        Ok(PcapSink { w })
    }

    pub fn write_record(&mut self, r: &LogRecord) -> io::Result<()> {
        let payload = wireshark_payload(r);
        let bytes = payload.as_bytes();
        let ts_sec = (r.ts_us / 1_000_000.0) as u32;
        let ts_usec = (r.ts_us as u64 % 1_000_000) as u32;
        self.w.write_all(&ts_sec.to_le_bytes())?;
        self.w.write_all(&ts_usec.to_le_bytes())?;
        self.w.write_all(&(bytes.len() as u32).to_le_bytes())?;
        self.w.write_all(&(bytes.len() as u32).to_le_bytes())?;
        self.w.write_all(bytes)?;
        Ok(())
    }
}

impl<W: Write> Output for PcapSink<W> {
    fn on_record(&mut self, r: &LogRecord) {
        let _ = self.write_record(r);
        // Flush each record so live consumers (Wireshark fifo, tilogger transport) see it now.
        // This costs ~4x on a bulk `pcap --out <file>` export (a 5 GB capture writes at roughly
        // 150 MS/s vs ~600 MS/s to stdout) because every record forces a syscall. That trade is
        // right for the live path, which is the design target. If bulk file export ever matters,
        // skip the flush when the sink is a regular file (only flush pipes/fifos).
        let _ = self.w.flush();
    }
    fn finish(&mut self) {
        let _ = self.w.flush();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rec() -> LogRecord {
        LogRecord {
            alias: "rftrc".into(),
            channel: 2,
            dbgid: 7,
            seq: 1,
            ts_ticks: 3,
            ts_us: 1.5,
            file: "t.c".into(),
            line: 42,
            level: "INFO",
            text: "hello".into(),
        }
    }

    #[test]
    fn payload_is_tilogger_shaped() {
        // The exact 8-column contract tilogger_dissector.lua splits on. Changing any
        // column breaks Wireshark field parity with the ITM/UART path.
        assert_eq!(
            wireshark_payload(&rec()),
            "rftrc||0.000001500||LOG_OPCODE_FORMATED_TEXT||DBGCH2||INFO||t.c||42||hello"
        );
    }

    #[test]
    fn stdout_line_matches_tilogger_default() {
        assert_eq!(
            stdout_line(&rec()),
            "rftrc | 0.000001500 | DBGCH2 | INFO | t.c:42 | hello"
        );
    }

    #[test]
    fn pcap_stream_shape() {
        let mut buf = Vec::new();
        {
            let mut sink = PcapSink::new(&mut buf).unwrap();
            sink.write_record(&rec()).unwrap();
        }
        assert_eq!(&buf[0..4], &0xA1B2_C3D4u32.to_le_bytes()); // classic pcap magic
        assert_eq!(u32::from_le_bytes(buf[20..24].try_into().unwrap()), 147); // DLT_USER0
        let caplen = u32::from_le_bytes(buf[32..36].try_into().unwrap()) as usize;
        let orig = u32::from_le_bytes(buf[36..40].try_into().unwrap()) as usize;
        assert_eq!(caplen, orig);
        assert_eq!(buf.len(), 24 + 16 + caplen); // exactly one record
        let payload = std::str::from_utf8(&buf[40..40 + caplen]).unwrap();
        assert_eq!(payload, wireshark_payload(&rec()));
    }
}
