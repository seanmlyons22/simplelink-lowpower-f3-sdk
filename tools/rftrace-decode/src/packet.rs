//! Word classification (§11.2) and the per-channel packet state machine (§11.3).
//!
//! `classify` and `PacketAssembler` are IMPLEMENTED (mechanical from the spec). The synth
//! encoder (`synth.rs`) is their inverse, so `words -> assemble -> DecodedPacket` round-trips
//! without needing the deframer.

use crate::crc5::Crc5;
use crate::types::{DecodedPacket, Health, Word, WordKind};

/// Classify a 10-bit word per the §11.2 mask table.
pub fn classify(w: Word) -> WordKind {
    let w = w.0 & 0x3FF;
    let chx = (w >> 8) & 0x3; // bits [9:8]
    if chx != 0 {
        // Non-zero top bits => DATA / TS byte for that channel.
        return WordKind::Data {
            ch: chx as u8,
            byte: (w & 0xFF) as u8,
        };
    }
    // chx == 0 => control class (NOP / overflow / TS-MSB / SOP / EOP).
    let ch76 = ((w >> 6) & 0x3) as u8; // channel field for SOP/EOP
    let bit5 = w & 0x20 != 0;
    if bit5 {
        if ch76 != 0 {
            WordKind::Eop {
                ch: ch76,
                crc5: (w & 0x1F) as u8,
            }
        } else {
            WordKind::TsMsb {
                ts_en: w & 0x10 != 0,
                ts_msb: (w & 1) as u8,
            }
        }
    } else if ch76 != 0 {
        WordKind::Sop {
            ch: ch76,
            ts_en: w & 0x10 != 0,
            seq: (w & 0xF) as u8,
        }
    } else if w & 0x00E != 0 {
        WordKind::Overflow(((w >> 1) & 0x7) as u8)
    } else {
        WordKind::Nop
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Field {
    TsH,
    TsL,
    Hdr,
    Data,
}

#[derive(Clone)]
struct ChState {
    active: bool,
    ts_en: bool,
    seq: u8,
    expect: Field,
    ts_hi: u8,
    ts_lo: u8,
    dbgid: u8,
    data: Vec<u8>, // param bytes, high-byte-first pairs
    crc: Crc5,
}

impl Default for ChState {
    fn default() -> Self {
        ChState {
            active: false,
            ts_en: false,
            seq: 0,
            expect: Field::Hdr,
            ts_hi: 0,
            ts_lo: 0,
            dbgid: 0,
            data: Vec::new(),
            crc: Crc5::new(),
        }
    }
}

/// Assembles classified words into CRC-checked packets. Feed every `WordKind`; it returns a
/// packet on each EOP. NOP/overflow/TS-MSB are consumed as state.
#[derive(Default)]
pub struct PacketAssembler {
    ch: [ChState; 4], // index by channel 1..=3 (0 unused)
    /// Latest ch0 timestamp MSB seen (kept for the timestamp reconstructor upstream).
    pub last_ts_msb: u8,
}

impl PacketAssembler {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn push(&mut self, wk: WordKind, health: &mut Health) -> Option<DecodedPacket> {
        match wk {
            WordKind::Nop => None,
            WordKind::Overflow(_) => {
                health.overflow_words += 1;
                None
            }
            WordKind::TsMsb { ts_msb, .. } => {
                self.last_ts_msb = ts_msb;
                None
            }
            WordKind::Sop { ch, ts_en, seq } => {
                if !(1..=3).contains(&ch) {
                    return None;
                }
                let st = &mut self.ch[ch as usize];
                *st = ChState::default();
                st.active = true;
                st.ts_en = ts_en;
                st.seq = seq;
                st.expect = if ts_en { Field::TsH } else { Field::Hdr };
                // CRC covers word[7:0] from SOP inclusive.
                let sop_byte = ((ch as u16) << 6) | ((ts_en as u16) << 4) | seq as u16;
                st.crc.update_byte(sop_byte as u8);
                None
            }
            WordKind::Data { ch, byte } => {
                if !(1..=3).contains(&ch) {
                    return None;
                }
                let st = &mut self.ch[ch as usize];
                if !st.active {
                    return None; // stray data with no SOP
                }
                st.crc.update_byte(byte);
                match st.expect {
                    Field::TsH => {
                        st.ts_hi = byte;
                        st.expect = Field::TsL;
                    }
                    Field::TsL => {
                        st.ts_lo = byte;
                        st.expect = Field::Hdr;
                    }
                    Field::Hdr => {
                        st.dbgid = byte;
                        st.expect = Field::Data;
                    }
                    Field::Data => st.data.push(byte),
                }
                None
            }
            WordKind::Eop { ch, crc5 } => {
                if !(1..=3).contains(&ch) {
                    return None;
                }
                let st = &mut self.ch[ch as usize];
                if !st.active {
                    return None;
                }
                // EOP contributes its top 3 bits (word[7:5]) to the CRC.
                let eop_top3 = ((ch << 1) | 1) & 0x7; // bits7:6=ch, bit5=1
                st.crc.update_bits(eop_top3, 3);
                let crc_ok = st.crc.value() == crc5;
                if !crc_ok {
                    health.crc_errors += 1;
                }
                // params: high-byte-first 16-bit words.
                let params: Vec<u16> = st
                    .data
                    .chunks(2)
                    .map(|c| ((c[0] as u16) << 8) | *c.get(1).unwrap_or(&0) as u16)
                    .collect();
                let pkt = DecodedPacket {
                    channel: ch,
                    dbgid: st.dbgid,
                    seq: st.seq,
                    ts_delta: if st.ts_en {
                        Some(((st.ts_hi as u16) << 8) | st.ts_lo as u16)
                    } else {
                        None
                    },
                    params,
                    crc_ok,
                };
                st.active = false;
                Some(pkt)
            }
        }
    }
}
