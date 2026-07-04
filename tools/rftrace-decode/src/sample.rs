//! Sample ingest: raw sigrok stream, and the Saleae `.sal` container.

use crate::types::DeframeCfg;
use std::io::{self, Read};
use std::path::Path;
use std::process::Command;

/// Extract the level (0/1) of one LA channel from packed 1-byte-per-sample data
/// (sigrok `-O binary` unitsize=1: bit0 = D0).
pub fn levels_from_raw(bytes: &[u8], channel: u8) -> Vec<u8> {
    bytes.iter().map(|&b| (b >> channel) & 1).collect()
}

/// Read a whole raw sample file (or stdin if `path == "-"`).
pub fn read_raw(path: &str) -> io::Result<Vec<u8>> {
    if path == "-" {
        let mut v = Vec::new();
        io::stdin().read_to_end(&mut v)?;
        Ok(v)
    } else {
        std::fs::read(path)
    }
}

/// A digital channel as a transition list: `initial_level`, then the sample index of every
/// level flip, over `total` samples. This is the Saleae-native representation; the trace
/// channel of `tx_burst_example.sal` has ~57M edges over 11.5G samples, so per-sample
/// expansion (11.5 GB) is never materialized.
pub struct SalRuns {
    pub initial_level: u8,
    pub edges: Vec<u64>,
    pub total: u64,
}

/// A decoded Saleae capture: transition list for the requested channel + its sample rate.
pub struct SalCapture {
    pub samplerate_hz: f64,
    pub runs: SalRuns,
}

/// Read a `.sal` (ZIP): parse `meta.json` for the sample rate, then decode `digital-<ch>.bin`.
///
/// Uses the system `unzip` to avoid a zip dependency (std-only crate).
pub fn read_sal<P: AsRef<Path>>(path: P, channel: u8) -> io::Result<SalCapture> {
    let path = path.as_ref();
    let meta = unzip_member(path, "meta.json")?;
    let meta = String::from_utf8_lossy(&meta);
    let samplerate_hz = parse_samplerate(&meta).unwrap_or(500_000_000.0);

    let member = format!("digital-{}.bin", channel);
    let bin = unzip_member(path, &member)?;
    let runs = decode_saleae_runs(&bin)?;
    Ok(SalCapture {
        samplerate_hz,
        runs,
    })
}

fn unzip_member(path: &Path, member: &str) -> io::Result<Vec<u8>> {
    let out = Command::new("unzip")
        .arg("-p")
        .arg(path)
        .arg(member)
        .output()?;
    if !out.status.success() {
        return Err(io::Error::other(format!(
            "unzip {member} from {path:?} failed"
        )));
    }
    Ok(out.stdout)
}

/// Pull `sampleRate.digital` out of meta.json with a tiny scanner (no serde).
fn parse_samplerate(meta: &str) -> Option<f64> {
    let key = "\"sampleRate\"";
    let start = meta.find(key)?;
    let tail = &meta[start..];
    let dk = tail.find("\"digital\"")?;
    let tail = &tail[dk + "\"digital\"".len()..];
    let colon = tail.find(':')?;
    let num: String = tail[colon + 1..]
        .chars()
        .skip_while(|c| c.is_whitespace())
        .take_while(|c| c.is_ascii_digit() || *c == '.')
        .collect();
    num.parse().ok()
}

fn bad(msg: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg.into())
}

fn rd_u32(b: &[u8], off: usize) -> io::Result<u32> {
    Ok(u32::from_le_bytes(
        b.get(off..off + 4).ok_or_else(|| bad("truncated"))?.try_into().unwrap(),
    ))
}

fn rd_u64(b: &[u8], off: usize) -> io::Result<u64> {
    Ok(u64::from_le_bytes(
        b.get(off..off + 8).ok_or_else(|| bad("truncated"))?.try_into().unwrap(),
    ))
}

/// Decode a Saleae `.sal` `digital-N.bin` into a transition list.
///
/// Layout, reverse-engineered against `tx_burst_example.sal` (all 6 channels of the file
/// parse to exactly their byte length, with sample coverage matching
/// `meta.json.captureProgress`, 11,553,909,408 samples = 23.1078 s @ 500 MS/s):
///
/// ```text
/// header (0x33 bytes):
///   "<SALEAE>"            8-byte magic
///   u32 version = 1
///   u32 = 100
///   u32 = 1
///   u8  = 0
///   u64 capture start (unix ms)     -- matches meta.json captureStartTime
///   f64 fractional ms
///   u16 = 0
///   u64 block count
/// then `block count` blocks, each:
///   u64 begin_sample, u64 end_sample, u64 num_samples (== end-begin)
///   u64 sample_rate (Hz), u64 = 1, u64 payload_len
///   payload_len bytes: run lengths. Each run is a big-endian varint:
///     first byte:  bit6 = continue, bits[5:0] = MSBs of the value
///     later bytes: bit7 = continue, bits[6:0] appended below
///     run = value + 1 samples; runs alternate level and sum to num_samples.
///   jump table: u32 n, u32 pad, then n x 20-byte entries
///     (u64 sample_offset_in_block, u64 run_index, u32 level);
///     entry 0 = (0, 0, level at block start).
/// ```
pub fn decode_saleae_runs(bin: &[u8]) -> io::Result<SalRuns> {
    if bin.len() < 0x33 || &bin[0..8] != b"<SALEAE>" {
        return Err(bad("not a Saleae digital .bin (missing <SALEAE> magic)"));
    }
    let version = rd_u32(bin, 8)?;
    if version != 1 {
        return Err(bad(format!("unsupported <SALEAE> bin version {version}")));
    }
    let mut pos = 0x33usize;
    let mut edges: Vec<u64> = Vec::new();
    let mut initial_level = 0u8;
    let mut cur_level: Option<u8> = None;
    let mut expect_begin = 0u64;
    while pos + 48 <= bin.len() {
        let begin = rd_u64(bin, pos)?;
        let end = rd_u64(bin, pos + 8)?;
        let nsamples = rd_u64(bin, pos + 16)?;
        let payload_len = rd_u64(bin, pos + 40)? as usize;
        if nsamples != end - begin || begin != expect_begin {
            return Err(bad(format!("inconsistent block header at offset {pos}")));
        }
        let payload = bin
            .get(pos + 48..pos + 48 + payload_len)
            .ok_or_else(|| bad("truncated block payload"))?;
        let tpos = pos + 48 + payload_len;
        let n_idx = rd_u32(bin, tpos)? as usize;
        if n_idx == 0 {
            return Err(bad("block jump table empty"));
        }
        let block_level = (rd_u32(bin, tpos + 8 + 16)? & 1) as u8;

        // Level continuity across blocks; a mismatch means an edge exactly on the boundary.
        match cur_level {
            None => initial_level = block_level,
            Some(l) if l != block_level => edges.push(begin),
            _ => {}
        }

        // Decode run-length varints; runs alternate level within the block.
        let mut s = begin;
        let mut i = 0usize;
        let mut nruns = 0usize;
        while i < payload.len() {
            let b = payload[i];
            i += 1;
            let mut v = (b & 0x3F) as u64;
            let mut more = b & 0x40 != 0;
            while more {
                let c = *payload.get(i).ok_or_else(|| bad("truncated run varint"))?;
                i += 1;
                v = (v << 7) | (c & 0x7F) as u64;
                more = c & 0x80 != 0;
            }
            s += v + 1;
            nruns += 1;
            if s < end {
                edges.push(s);
            }
        }
        if s != end {
            return Err(bad(format!(
                "block at {pos}: runs sum to {} but block has {} samples",
                s - begin,
                nsamples
            )));
        }
        // Level after the block: block start level ^ parity of (runs - 1).
        cur_level = Some(block_level ^ (((nruns.max(1) - 1) & 1) as u8));
        expect_begin = end;
        pos = tpos + 8 + 20 * n_idx;
    }
    if pos != bin.len() {
        return Err(bad(format!(
            "trailing bytes: consumed {pos} of {} bytes",
            bin.len()
        )));
    }
    Ok(SalRuns {
        initial_level,
        edges,
        total: expect_begin,
    })
}

/// Decode a Saleae `.sal` `digital-N.bin` into per-sample levels at `samplerate_hz`
/// (which must match the rate the file was captured at - it is only used to guard size).
///
/// Convenience/API-compat wrapper over [`decode_saleae_runs`]; refuses captures that would
/// expand to more than 1 GiB (the real trace capture is 11.5 G samples - use the runs form).
pub fn decode_saleae_digital(bin: &[u8], _samplerate_hz: f64) -> io::Result<Vec<u8>> {
    let runs = decode_saleae_runs(bin)?;
    if runs.total > (1 << 30) {
        return Err(io::Error::other(format!(
            "capture is {} samples; too large to expand per-sample - use decode_saleae_runs",
            runs.total
        )));
    }
    let mut levels = Vec::with_capacity(runs.total as usize);
    let mut lvl = runs.initial_level;
    for &e in &runs.edges {
        levels.resize(e as usize, lvl);
        lvl ^= 1;
    }
    levels.resize(runs.total as usize, lvl);
    Ok(levels)
}

/// Apply the confirmed `tx_burst_example.sal` analyzer settings as decoder defaults.
pub fn cfg_from_sal(samplerate_hz: f64) -> DeframeCfg {
    DeframeCfg {
        samplerate_hz,
        ..DeframeCfg::default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn levels_pick_the_channel_bit() {
        let bytes = [0b0001_0000u8, 0b0000_0000, 0b0001_0000];
        assert_eq!(levels_from_raw(&bytes, 4), vec![1, 0, 1]);
        assert_eq!(levels_from_raw(&bytes, 0), vec![0, 0, 0]);
    }

    #[test]
    fn samplerate_scanner() {
        let meta = r#"{"data":{"sampleRate":{"digital": 500000000, "analog": 0}}}"#;
        assert_eq!(parse_samplerate(meta), Some(500_000_000.0));
        assert_eq!(parse_samplerate("{}"), None);
    }

    // ---- synthetic digital-N.bin builder for decoder tests ----

    fn varint(v: u64) -> Vec<u8> {
        // Big-endian: first byte carries the top 6 bits (bit6 = continue), later
        // bytes 7 bits each (bit7 = continue).
        if v <= 0x3F {
            return vec![v as u8];
        }
        let mut groups = Vec::new();
        let mut rest = v;
        while rest > 0x3F {
            groups.push((rest & 0x7F) as u8);
            rest >>= 7;
        }
        let mut out = vec![(rest as u8) | 0x40];
        for (i, g) in groups.iter().rev().enumerate() {
            let last = i == groups.len() - 1;
            out.push(g | if last { 0 } else { 0x80 });
        }
        out
    }

    fn block(begin: u64, runs: &[u64], level: u8) -> Vec<u8> {
        let total: u64 = runs.iter().sum();
        let mut payload = Vec::new();
        for &r in runs {
            payload.extend(varint(r - 1)); // stored value = run length - 1
        }
        let mut b = Vec::new();
        b.extend((begin).to_le_bytes());
        b.extend((begin + total).to_le_bytes());
        b.extend(total.to_le_bytes());
        b.extend(500_000_000u64.to_le_bytes());
        b.extend(1u64.to_le_bytes());
        b.extend((payload.len() as u64).to_le_bytes());
        b.extend(&payload);
        b.extend(1u32.to_le_bytes()); // jump table: one entry
        b.extend(0u32.to_le_bytes());
        b.extend(0u64.to_le_bytes());
        b.extend(0u64.to_le_bytes());
        b.extend((level as u32).to_le_bytes());
        b
    }

    fn bin_with(blocks: &[Vec<u8>]) -> Vec<u8> {
        let mut bin = Vec::new();
        bin.extend(b"<SALEAE>");
        bin.extend(1u32.to_le_bytes());
        bin.resize(0x33, 0);
        for b in blocks {
            bin.extend(b);
        }
        bin
    }

    #[test]
    fn runs_decode_to_edges() {
        // One block, level 1 for 5 samples, 0 for 3, 1 for 300 (multi-byte varint).
        let bin = bin_with(&[block(0, &[5, 3, 300], 1)]);
        let runs = decode_saleae_runs(&bin).unwrap();
        assert_eq!(runs.initial_level, 1);
        assert_eq!(runs.edges, vec![5, 8]);
        assert_eq!(runs.total, 308);
    }

    #[test]
    fn block_boundary_level_flip_is_an_edge() {
        // Block 1 ends at level 1 (one run), block 2 starts at level 0: that flip is
        // an edge exactly on the block boundary.
        let bin = bin_with(&[block(0, &[10], 1), block(10, &[10], 0)]);
        let runs = decode_saleae_runs(&bin).unwrap();
        assert_eq!(runs.edges, vec![10]);
        assert_eq!(runs.total, 20);
    }

    #[test]
    fn rejects_bad_magic_and_truncation() {
        assert!(decode_saleae_runs(b"NOTSALEAE").is_err());
        let mut bin = bin_with(&[block(0, &[5, 3], 1)]);
        bin.truncate(bin.len() - 4); // chop the jump table
        assert!(decode_saleae_runs(&bin).is_err());
        // Version != 1 must be refused, not misparsed.
        let mut v2 = bin_with(&[]);
        v2[8] = 2;
        assert!(decode_saleae_runs(&v2).is_err());
    }

    #[test]
    fn per_sample_expansion_matches_runs() {
        let bin = bin_with(&[block(0, &[5, 3, 4], 1)]);
        let levels = decode_saleae_digital(&bin, 500e6).unwrap();
        assert_eq!(levels.len(), 12);
        assert_eq!(&levels[..], &[1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1]);
    }
}
