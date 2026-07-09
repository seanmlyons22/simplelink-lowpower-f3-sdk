//! Core data model shared across the pipeline.

/// One 10-bit tracer word lifted off the wire: `[9:8]=chx_id`, `[7:0]=payload`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Word(pub u16); // value masked to 0x3FF

impl Word {
    pub fn new(raw10: u16) -> Self {
        Word(raw10 & 0x3FF)
    }
    pub fn chx_id(self) -> u8 {
        ((self.0 >> 8) & 0x3) as u8
    }
    pub fn payload(self) -> u8 {
        (self.0 & 0xFF) as u8
    }
}

/// SOP payload byte: bits [7:6] = channel (1..=3), bit [4] = ts_en, bits [3:0] = seq.
/// Single source of truth for the layout, shared by the packet assembler (CRC re-feed)
/// and the synth encoder.
pub fn sop_byte(ch: u8, ts_en: bool, seq: u8) -> u8 {
    (ch << 6) | ((ts_en as u8) << 4) | (seq & 0xF)
}

/// The three EOP bits the CRC covers (`word[7:5]`): bits [7:6] = channel, bit [5] = 1.
pub fn eop_top3(ch: u8) -> u8 {
    ((ch << 1) | 1) & 0x7
}

/// Classification of a 10-bit word (see `packet::classify` for the mask table).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WordKind {
    /// Idle NOP (`0x000`) - dropped.
    Nop,
    /// NOP carrying per-channel overflow flags in bits [3:1] (bit3=ch3, 2=ch2, 1=ch1).
    Overflow(u8),
    /// Channel-0 timestamp-MSB sync word: carries `ts_counter[15]` in `ts_msb` (bit 0).
    TsMsb { ts_en: bool, ts_msb: u8 },
    /// Start-of-packet for `ch` (1..=3).
    Sop { ch: u8, ts_en: bool, seq: u8 },
    /// End-of-packet for `ch`, carrying the 5-bit CRC in [4:0].
    Eop { ch: u8, crc5: u8 },
    /// Data/timestamp byte for `ch` (1..=3); meaning set by the channel SM state.
    Data { ch: u8, byte: u8 },
}

/// A fully-assembled, CRC-checked tracer packet (pre-metadata-resolve).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DecodedPacket {
    pub channel: u8,          // 1..=3
    pub dbgid: u8,            // 3..=255
    pub seq: u8,              // 0..15
    pub ts_delta: Option<u16>,
    pub params: Vec<u16>,     // par_cnt 16-bit words, wire order (low half first for 32-bit args)
    pub crc_ok: bool,
}

/// One row of the dbgid database (from an `elf2dbgid` `DBG_DEF` line).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DbgDef {
    pub dbgid: u8,
    pub channel: u8,
    /// >0 => that many 16-bit args; <0 => `|n|` 32-bit args; 0 => none.
    pub arg_count: i32,
    pub fmt: String,
    pub file: String, // full path as in the dbgid file
    pub line: u32,
}

impl DbgDef {
    /// basename of `file` (after the last `/` or `\\`).
    pub fn basename(&self) -> &str {
        self.file
            .rsplit(['/', '\\'])
            .next()
            .unwrap_or(&self.file)
    }
    /// Number of 16-bit words expected on the wire for this def (`par_cnt`).
    pub fn expected_par_cnt(&self) -> usize {
        if self.arg_count < 0 {
            (self.arg_count.unsigned_abs() as usize) * 2 // 32-bit args = 2 words each
        } else {
            self.arg_count as usize
        }
    }
}

/// dbgid DB keyed by `(channel, dbgid)`.
#[derive(Clone, Debug, Default)]
pub struct DbgIdDb {
    pub by_key: std::collections::HashMap<(u8, u8), DbgDef>,
}

impl DbgIdDb {
    pub fn get(&self, channel: u8, dbgid: u8) -> Option<&DbgDef> {
        self.by_key.get(&(channel, dbgid))
    }
    pub fn len(&self) -> usize {
        self.by_key.len()
    }
    pub fn is_empty(&self) -> bool {
        self.by_key.is_empty()
    }
}

/// Final decoded record handed to Outputs.
#[derive(Clone, Debug)]
pub struct LogRecord {
    pub alias: String,
    pub channel: u8,
    pub dbgid: u8,
    pub seq: u8,
    pub ts_ticks: u64, // 0.5 us ticks, rollover-unwrapped
    pub ts_us: f64,
    pub file: String,  // basename
    pub line: u32,
    pub level: &'static str, // dbgid has no level field -> default "INFO"
    pub text: String,        // fmt % args
}

/// Health / error counters (never abort the hot loop).
#[derive(Clone, Copy, Debug, Default)]
pub struct Health {
    pub framing_errors: u64,
    pub crc_errors: u64,
    pub overflow_words: u64,
    pub unknown_dbgid: u64,
    /// Packets whose wire param-word count didn't match the dbgid def's `expected_par_cnt`
    /// (a stale ELF/dbgid vs firmware). Distinct from `unknown_dbgid` (the def was found).
    pub arg_mismatch: u64,
    pub dropped_seq: u64,
}

/// Deframe configuration. Defaults are the confirmed `tx_burst_example.sal` values.
#[derive(Clone, Copy, Debug)]
pub struct DeframeCfg {
    pub samplerate_hz: f64,
    pub baud_hz: f64,
    pub data_bits: u8,
    pub msb_first: bool,
    pub invert: bool,
    pub divide_time_by_2: bool,
}

impl Default for DeframeCfg {
    fn default() -> Self {
        DeframeCfg {
            samplerate_hz: 500_000_000.0,
            baud_hz: 24_000_000.0,
            data_bits: 10,
            msb_first: true,
            invert: true,
            divide_time_by_2: false,
        }
    }
}

impl DeframeCfg {
    pub fn samples_per_bit(&self) -> f64 {
        self.samplerate_hz / self.baud_hz
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn word_fields() {
        let w = Word::new(0x2AB);
        assert_eq!(w.chx_id(), 2);
        assert_eq!(w.payload(), 0xAB);
        assert_eq!(Word::new(0xFFFF).0, 0x3FF); // masked to 10 bits
    }

    #[test]
    fn sop_eop_layout() {
        // ch=1, ts_en, seq=3 -> 0b01_0_1_0011 = 0x53
        assert_eq!(sop_byte(1, true, 3), 0x53);
        assert_eq!(sop_byte(3, false, 0xF), 0xCF);
        assert_eq!(sop_byte(2, false, 0x13), 0x83); // seq masked to 4 bits
        // EOP word[7:5] for ch: [7:6]=ch, [5]=1
        assert_eq!(eop_top3(1), 0b011);
        assert_eq!(eop_top3(3), 0b111);
    }

    #[test]
    fn dbgdef_accessors() {
        let d = DbgDef {
            dbgid: 5,
            channel: 1,
            arg_count: -2,
            fmt: "x".into(),
            file: r"C:\src\a.c".into(),
            line: 1,
        };
        assert_eq!(d.basename(), "a.c"); // backslash paths from Windows builds
        assert_eq!(d.expected_par_cnt(), 4); // two 32-bit args = four 16-bit words
        let d16 = DbgDef { arg_count: 3, ..d.clone() };
        assert_eq!(d16.expected_par_cnt(), 3);
    }

    #[test]
    fn samples_per_bit_default() {
        // 500 MS/s over 24 Mbaud is deliberately non-integer (20.83); the deframer
        // must handle fractional bit spacing.
        let cfg = DeframeCfg::default();
        assert!((cfg.samples_per_bit() - 20.8333).abs() < 0.001);
    }
}
