//! Word classification and the per-channel packet state machines.
//!
//! Three independent state machines (channels 1..=3) whose words interleave on the wire.
//! Per packet: SOP(ch, ts_en, seq) -> [timestamp high, low] -> header (dbgid) -> data bytes
//! -> EOP(ch, crc5). The synth encoder (`synth.rs`) is the exact inverse, so
//! `words -> assemble -> DecodedPacket` round-trips without needing the deframer.

use crate::crc5::Crc5;
use crate::types::{eop_top3, sop_byte, DecodedPacket, Health, Word, WordKind};

/// Classify a 10-bit word `w` by masks:
///
/// | kind      | detect                                     |
/// |-----------|--------------------------------------------|
/// | DATA      | `w[9:8] != 0` (that value is the channel)  |
/// | EOP       | top bits 0, `w[5]` set, `w[7:6] != 0`      |
/// | TS-MSB    | top bits 0, `w[5]` set, `w[7:6] == 0`      |
/// | SOP       | top bits 0, `w[5]` clear, `w[7:6] != 0`    |
/// | Overflow  | top bits 0, `w & 0x00E != 0`               |
/// | NOP       | `w == 0`                                   |
///
/// Note the channel sits in `w[7:6]` for SOP/EOP but in `w[9:8]` for DATA.
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
    /// Last CRC-good seq per channel, for dropped-packet detection (seq wraps at 16).
    last_seq: [Option<u8>; 4],
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
                st.crc.update_byte(sop_byte(ch, ts_en, seq));
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
                st.crc.update_bits(eop_top3(ch), 3);
                let crc_ok = st.crc.value() == crc5;
                if !crc_ok {
                    health.crc_errors += 1;
                } else {
                    // Seq-gap accounting between CRC-good packets: a packet the tracer
                    // dropped (or one we discarded for CRC) shows up as missing seqnums.
                    if let Some(prev) = self.last_seq[ch as usize] {
                        health.dropped_seq += ((st.seq + 16 - prev - 1) & 0xF) as u64;
                    }
                    self.last_seq[ch as usize] = Some(st.seq);
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::synth;

    fn run(words: &[Word]) -> (Vec<DecodedPacket>, Health) {
        let mut asm = PacketAssembler::new();
        let mut health = Health::default();
        let mut pkts = Vec::new();
        for &w in words {
            if let Some(p) = asm.push(classify(w), &mut health) {
                pkts.push(p);
            }
        }
        (pkts, health)
    }

    #[test]
    fn classify_mask_table() {
        assert_eq!(classify(Word::new(0x000)), WordKind::Nop);
        assert_eq!(classify(Word::new(0x002)), WordKind::Overflow(1));
        assert_eq!(classify(Word::new(0x00E)), WordKind::Overflow(7));
        assert_eq!(classify(Word::new(0x031)), WordKind::TsMsb { ts_en: true, ts_msb: 1 });
        assert_eq!(classify(Word::new(0x020)), WordKind::TsMsb { ts_en: false, ts_msb: 0 });
        assert_eq!(classify(Word::new(0x053)), WordKind::Sop { ch: 1, ts_en: true, seq: 3 });
        assert_eq!(classify(Word::new(0x0CF)), WordKind::Sop { ch: 3, ts_en: false, seq: 15 });
        assert_eq!(classify(Word::new(0x07F)), WordKind::Eop { ch: 1, crc5: 0x1f });
        assert_eq!(classify(Word::new(0x2AB)), WordKind::Data { ch: 2, byte: 0xAB });
    }

    #[test]
    fn stray_data_and_eop_without_sop_are_ignored() {
        let (pkts, health) = run(&[Word::new(0x2AB), Word::new(0x0AF)]); // DATA ch2, EOP ch2
        assert!(pkts.is_empty());
        assert_eq!(health.crc_errors, 0);
    }

    #[test]
    fn corrupt_byte_fails_crc_and_counts() {
        let mut words = synth::encode_packet(1, 5, &[0x1234], Some(1), 0);
        let hdr = words[3].0; // SOP TSH TSL HDR
        words[3] = Word::new(hdr ^ 0x01);
        let (pkts, health) = run(&words);
        assert_eq!(pkts.len(), 1);
        assert!(!pkts[0].crc_ok);
        assert_eq!(health.crc_errors, 1);
    }

    #[test]
    fn seq_gap_is_counted_per_channel() {
        let mut words = Vec::new();
        // ch1: seq 0, 1, then 4 (two packets lost); ch2 unaffected by ch1's gap.
        for seq in [0u8, 1, 4] {
            words.extend(synth::encode_packet(1, 5, &[], None, seq));
        }
        words.extend(synth::encode_packet(2, 7, &[], None, 9));
        words.extend(synth::encode_packet(2, 7, &[], None, 10));
        let (pkts, health) = run(&words);
        assert_eq!(pkts.len(), 5);
        assert_eq!(health.dropped_seq, 2);
    }

    #[test]
    fn seq_wrap_without_loss_is_not_a_gap() {
        let mut words = Vec::new();
        for seq in [14u8, 15, 0, 1] {
            words.extend(synth::encode_packet(3, 9, &[], None, seq));
        }
        let (_, health) = run(&words);
        assert_eq!(health.dropped_seq, 0);
    }

    #[test]
    fn interleaved_channels_assemble_independently() {
        let a = synth::encode_packet(1, 5, &[0x1111], Some(0x10), 0);
        let b = synth::encode_packet(2, 7, &[0x2222], Some(0x20), 0);
        // Interleave word-by-word, as the wire does.
        let mut words = Vec::new();
        for i in 0..a.len().max(b.len()) {
            if let Some(w) = a.get(i) {
                words.push(*w);
            }
            if let Some(w) = b.get(i) {
                words.push(*w);
            }
        }
        let (pkts, health) = run(&words);
        assert_eq!(pkts.len(), 2);
        assert!(pkts.iter().all(|p| p.crc_ok));
        assert_eq!(health.crc_errors, 0);
        let ch1 = pkts.iter().find(|p| p.channel == 1).unwrap();
        assert_eq!((ch1.dbgid, ch1.params.clone()), (5, vec![0x1111]));
        let ch2 = pkts.iter().find(|p| p.channel == 2).unwrap();
        assert_eq!((ch2.dbgid, ch2.params.clone()), (7, vec![0x2222]));
    }

    #[test]
    fn sop_mid_packet_restarts_the_channel() {
        // A truncated packet (SOP, no EOP) followed by a complete one: only the
        // complete one is yielded, and its CRC still validates.
        let mut words = synth::encode_packet(1, 5, &[0x1234], Some(1), 0);
        words.truncate(3); // SOP TSH TSL, then the wire "restarts"
        words.extend(synth::encode_packet(1, 6, &[0x5678], Some(2), 1));
        let (pkts, health) = run(&words);
        assert_eq!(pkts.len(), 1);
        assert_eq!(pkts[0].dbgid, 6);
        assert!(pkts[0].crc_ok);
        assert_eq!(health.crc_errors, 0);
    }

    #[test]
    fn overflow_words_are_counted() {
        let (_, health) = run(&[Word::new(0x002), Word::new(0x00E)]);
        assert_eq!(health.overflow_words, 2);
    }

    #[test]
    fn no_timestamp_packet_has_none_delta() {
        let words = synth::encode_packet(1, 5, &[0xAA55], None, 2);
        let (pkts, _) = run(&words);
        assert_eq!(pkts[0].ts_delta, None);
        assert!(pkts[0].crc_ok);
    }
}
