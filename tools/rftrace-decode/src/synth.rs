//! Synthetic wire generator: records -> tracer words -> LA sample levels.
//! Exact inverse of the decode path; the basis of every hardware-free self-test and of
//! the large benchmark captures.

use crate::crc5::Crc5;
use crate::deframe::encode_frame;
use crate::types::{eop_top3, sop_byte, DeframeCfg, Word};

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

    let sop = sop_byte(channel, ts_en, seq);
    crc.update_byte(sop);
    words.push(Word::new(sop as u16));

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
    crc.update_bits(eop_top3(channel), 3);
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn arg_width_to_params() {
        // 32-bit args become two 16-bit words each, low half first.
        assert_eq!(args_to_params16(&[0x1234_5678], true), vec![0x5678, 0x1234]);
        assert_eq!(args_to_params16(&[0xBEEF, 0xCAFE], false), vec![0xBEEF, 0xCAFE]);
        assert_eq!(args_to_params16(&[], true), Vec::<u16>::new());
    }

    #[test]
    fn packet_word_sequence_shape() {
        let words = encode_packet(2, 130, &[0xAABB], Some(0x0102), 1);
        // SOP, TSH, TSL, HDR, 2 data bytes, EOP
        assert_eq!(words.len(), 7);
        assert_eq!(words[0].0, 0x91); // ch2, ts_en, seq1
        assert_eq!(words[1].0, 0x201); // ch2 data: ts high byte
        assert_eq!(words[2].0, 0x202); // ch2 data: ts low byte
        assert_eq!(words[3].0, 0x282); // ch2 data: dbgid 130
        assert_eq!(words[4].0, 0x2AA); // param high byte first
        assert_eq!(words[5].0, 0x2BB);
        assert_eq!(words[6].0 & 0x3E0, 0x0A0); // EOP: ch2 + bit5
    }

    #[test]
    fn sample_expansion_size() {
        let cfg = DeframeCfg::default();
        let spb = cfg.samples_per_bit().round() as usize;
        let samples = words_to_samples(&[nop()], &cfg, 3);
        assert_eq!(samples.len(), 4 * 12 * spb); // (3 idle + 1) frames x 12 bits
    }
}
