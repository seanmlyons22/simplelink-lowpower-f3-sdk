//! Synthetic wire generator (§16.3): records -> tracer words -> LA sample levels.
//! Inverse of the decode path; used for self-tests without hardware.

use crate::crc5::Crc5;
use crate::deframe::encode_frame;
use crate::types::{DeframeCfg, Word};

/// Convert 32-bit args to the 16-bit wire params (`arg32` => two words per arg, low half first).
pub fn args_to_params16(args: &[u32], arg32: bool) -> Vec<u16> {
    if arg32 {
        let mut p = Vec::with_capacity(args.len() * 2);
        for &a in args {
            p.push((a & 0xFFFF) as u16);
            p.push((a >> 16) as u16);
        }
        p
    } else {
        args.iter().map(|&a| a as u16).collect()
    }
}

/// Encode one packet into its tracer word sequence: SOP [TSH TSL] HDR DATA.. EOP (with CRC-5).
pub fn encode_packet(
    channel: u8,
    dbgid: u8,
    params16: &[u16],
    ts_delta: Option<u16>,
    seq: u8,
) -> Vec<Word> {
    assert!((1..=3).contains(&channel));
    let mut words = Vec::new();
    let mut crc = Crc5::new();
    let ts_en = ts_delta.is_some();

    // SOP: bits [7:6]=channel, [4]=ts_en, [3:0]=seq (top bits [9:8]=00).
    let sop_byte = ((channel as u16) << 6) | ((ts_en as u16) << 4) | (seq as u16 & 0xF);
    crc.update_byte(sop_byte as u8);
    words.push(Word::new(sop_byte));

    let push_data = |b: u8, words: &mut Vec<Word>, crc: &mut Crc5| {
        crc.update_byte(b);
        words.push(Word::new(((channel as u16) << 8) | b as u16));
    };

    if let Some(delta) = ts_delta {
        push_data((delta >> 8) as u8, &mut words, &mut crc); // TSH
        push_data((delta & 0xFF) as u8, &mut words, &mut crc); // TSL
    }
    push_data(dbgid, &mut words, &mut crc); // HDR
    for &p in params16 {
        push_data((p >> 8) as u8, &mut words, &mut crc); // high byte first
        push_data((p & 0xFF) as u8, &mut words, &mut crc);
    }

    // EOP: bits [7:6]=channel, [5]=1, [4:0]=CRC. CRC covers the 3 MSBs (word[7:5]) too.
    let eop_top3 = ((channel << 1) | 1) & 0x7;
    crc.update_bits(eop_top3, 3);
    let eop = ((channel as u16) << 6) | 0x20 | (crc.value() as u16 & 0x1F);
    words.push(Word::new(eop));
    words
}

/// A NOP idle word (`0x000`).
pub fn nop() -> Word {
    Word::new(0)
}

/// Encode words into LA-observed sample levels (post-inversion), oversampled at `cfg.samplerate`.
/// Prepends `idle_frames` NOP frames so the deframer can lock its clock.
pub fn words_to_samples(words: &[Word], cfg: &DeframeCfg, idle_frames: usize) -> Vec<u8> {
    let spb = cfg.samples_per_bit().round().max(1.0) as usize;
    let mut out = Vec::new();
    let emit = |w: Word, out: &mut Vec<u8>| {
        for bit in encode_frame(w, cfg) {
            let level = if cfg.invert { 1 - bit } else { bit };
            for _ in 0..spb {
                out.push(level);
            }
        }
    };
    for _ in 0..idle_frames {
        emit(nop(), &mut out);
    }
    for &w in words {
        emit(w, &mut out);
    }
    out
}
