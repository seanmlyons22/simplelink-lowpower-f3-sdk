//! Physical deframe: LA sample levels / transition lists -> 10-bit tracer words (§11.1).
//!
//! `decode_frame`/`encode_frame` are shared with the synth encoder; `deframe_edges` is the
//! hot path (edge-based, so the 11.5 G-sample golden capture never expands per-sample).

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
/// Thin wrapper over [`deframe_edges`]: compresses the sample vector into a transition list
/// (Saleae-native representation) and runs the edge-based deframer.
pub fn deframe(levels: &[u8], cfg: &DeframeCfg) -> Vec<Word> {
    if levels.is_empty() {
        return Vec::new();
    }
    let initial = levels[0] & 1;
    let mut edges = Vec::new();
    for i in 1..levels.len() {
        if levels[i] & 1 != levels[i - 1] & 1 {
            edges.push(i as u64);
        }
    }
    deframe_edges(initial, &edges, levels.len() as u64, cfg).0
}

/// Edge-based deframer: `initial_level` + transition positions (sample index where the line
/// flips) + total sample count -> 10-bit words. Returns `(words, framing_errors)`.
///
/// Algorithm (§11.1, ADR-005/007): find each start-bit edge, place `2 + data_bits` bit
/// centers at nominal `samples_per_bit()` spacing from it, sample, verify the stop bit, and
/// lift the word with `decode_frame`. Phase resyncs on every start edge, so only intra-frame
/// (12-bit) clock drift matters.
///
/// ponytail: no low-passed period estimate — LA and tracer clocks are both crystal-derived,
/// so intra-frame drift is <<0.5 bit; add period tracking only if a capture shows drift.
///
/// Start-bit polarity is auto-detected: the tracer streams NOP frames (1 start bit + 11 zero
/// bits) whenever it is on, so the start-bit level is always the minority level over the
/// capture. This resolves the "inverted" ambiguity between the Saleae analyzer convention
/// (start = high on the real `tx_burst_example.sal` wire) and the synth encoder convention
/// (start = low when `cfg.invert`), so `cfg.invert` is only a hint here.
pub fn deframe_edges(
    initial_level: u8,
    edges: &[u64],
    total: u64,
    cfg: &DeframeCfg,
) -> (Vec<Word>, u64) {
    let start_level = minority_start_level(initial_level, edges, total, cfg);
    let (words, fe, _consumed) = deframe_edges_core(initial_level, edges, total, cfg, start_level);
    (words, fe)
}

/// Start-bit polarity = the minority line level over the window (the tracer idles with NOP
/// frames, so idle dominates and the start pulse is the minority). See the note above.
pub fn minority_start_level(initial_level: u8, edges: &[u64], total: u64, cfg: &DeframeCfg) -> u8 {
    if total == 0 {
        return if cfg.invert { 0 } else { 1 };
    }
    let mut hi = 0u64;
    let mut lvl = initial_level & 1;
    let mut prev = 0u64;
    for &e in edges {
        if lvl == 1 {
            hi += e - prev;
        }
        prev = e;
        lvl ^= 1;
    }
    if lvl == 1 {
        hi += total - prev;
    }
    if hi * 2 > total {
        0
    } else if hi * 2 < total {
        1
    } else if cfg.invert {
        0 // tie: fall back to the cfg hint (synth convention: invert => start low)
    } else {
        1
    }
}

/// Core deframer with an explicit `start_level`. Returns `(words, framing_errors, consumed)`
/// where `consumed` is the sample index up to which framing is complete — i.e. the end of the
/// last emitted frame. Streaming callers keep samples `[consumed..]` as carry for the next
/// chunk; any frame that would run past `total` is left for that carry.
pub fn deframe_edges_core(
    initial_level: u8,
    edges: &[u64],
    total: u64,
    cfg: &DeframeCfg,
    start_level: u8,
) -> (Vec<Word>, u64, u64) {
    let spb = cfg.samples_per_bit();
    let bits_per_frame = 2 + cfg.data_bits as usize;
    let mut words = Vec::new();
    let mut framing_errors = 0u64;
    if total == 0 {
        return (words, framing_errors, 0);
    }

    // level_at(pos) via a monotonic cursor: level = initial ^ (edges <= pos).
    let mut cur = 0usize; // number of edges consumed
    let level_at = |pos: f64, cur: &mut usize| -> u8 {
        while *cur < edges.len() && (edges[*cur] as f64) <= pos {
            *cur += 1;
        }
        initial_level ^ ((*cur & 1) as u8)
    };

    // Runs: run i starts at (i==0 ? 0 : edges[i-1]) with level initial ^ (i&1).
    let mut frame_bits = vec![0u8; bits_per_frame];
    let mut pos_limit = 0f64;
    for i in 0..=edges.len() {
        let s = if i == 0 { 0 } else { edges[i - 1] };
        let lvl = initial_level ^ ((i & 1) as u8);
        if lvl != start_level || (s as f64) < pos_limit {
            continue;
        }
        // Candidate frame starting at s: sample bit centers.
        let sf = s as f64;
        if sf + (bits_per_frame as f64 - 0.5) * spb > total as f64 {
            break; // frame would run past the window; streaming carry re-tries it next chunk
        }
        for (k, b) in frame_bits.iter_mut().enumerate() {
            let raw = level_at(sf + (k as f64 + 0.5) * spb, &mut cur);
            *b = (raw == start_level) as u8; // start bit = logical 1
        }
        if frame_bits[bits_per_frame - 1] != 0 {
            framing_errors += 1;
            pos_limit = sf + 1.0;
            // cursor may have run ahead; rewind so later, earlier positions still resolve
            while cur > 0 && edges[cur - 1] as f64 > pos_limit {
                cur -= 1;
            }
            continue;
        }
        words.push(decode_frame(&frame_bits, cfg));
        pos_limit = sf + (bits_per_frame as f64 - 0.5) * spb;
        while cur > 0 && edges[cur - 1] as f64 > pos_limit {
            cur -= 1;
        }
    }
    (words, framing_errors, pos_limit as u64)
}
