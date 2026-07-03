//! Physical deframe: LA sample levels -> 10-bit tracer words (§11.1).
//!
//! `decode_frame` (deterministic) is IMPLEMENTED and shared with the synth encoder.
//! `deframe` (clock recovery + bit-center sampling) is the one hot-path STUB — TODO(fable).

use crate::types::{DeframeCfg, Word};

/// Turn the 12 sampled frame bits (already de-inverted, in transmission order:
/// `[start, d(msb..lsb) x data_bits, stop, ...]`) into a 10-bit word.
///
/// After de-inversion the line is: idle-high, start=1, `data_bits` data bits (MSB-first when
/// `cfg.msb_first`), trailing 0. We take the data field and assemble the 10-bit word.
pub fn decode_frame(frame_bits: &[u8], cfg: &DeframeCfg) -> Word {
    let n = cfg.data_bits as usize;
    // frame_bits[0] = start; data is frame_bits[1..1+n]; last = stop.
    let data = &frame_bits[1..1 + n];
    let mut w: u16 = 0;
    if cfg.msb_first {
        for &b in data {
            w = (w << 1) | (b as u16 & 1);
        }
    } else {
        for (i, &b) in data.iter().enumerate() {
            w |= (b as u16 & 1) << i;
        }
    }
    Word::new(w)
}

/// Encode a 10-bit word into 12 transmission-order frame bits (pre-inversion):
/// `[start=1, data(msb-first), stop=0]`. Inverse of `decode_frame`. Used by synth.
pub fn encode_frame(word: Word, cfg: &DeframeCfg) -> Vec<u8> {
    let n = cfg.data_bits as usize;
    let mut bits = Vec::with_capacity(n + 2);
    bits.push(1u8); // start
    if cfg.msb_first {
        for i in (0..n).rev() {
            bits.push(((word.0 >> i) & 1) as u8);
        }
    } else {
        for i in 0..n {
            bits.push(((word.0 >> i) & 1) as u8);
        }
    }
    bits.push(0u8); // stop
    bits
}

/// Recover 10-bit words from a stream of per-sample line levels (0/1) for the trace channel.
///
/// TODO(fable) — the core hot loop. Algorithm (§11.1, ADR-005/007):
///  1. De-invert if `cfg.invert` (idle should read high).
///  2. Track frame period from start-pulse edges: nominal `cfg.samples_per_bit()` (~20.83 @
///     500 MS/s / 24 Mbaud); maintain a low-passed estimate; resync phase on each start edge.
///  3. On a detected start bit, place bit centers with a fractional accumulator and sample
///     `2 + data_bits` bits; validate the stop bit (count framing errors otherwise).
///  4. `decode_frame(bits, cfg)` -> Word; push. Idle NOP words (0x000) stream continuously.
///
/// Verify: `synth::words_to_samples` -> `deframe` round-trips the word stream (see the
/// `#[ignore]`d `synth_roundtrip` test — un-ignore once implemented).
pub fn deframe(_levels: &[u8], _cfg: &DeframeCfg) -> Vec<Word> {
    // Not yet implemented — returns nothing so the pipeline runs gracefully.
    Vec::new()
}
