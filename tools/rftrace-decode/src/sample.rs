//! Sample ingest: raw sigrok stream, and the Saleae `.sal` container (§16).

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
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!("unzip {member} from {path:?} failed"),
        ));
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
/// (which must match the rate the file was captured at — it is only used to guard size).
///
/// Convenience/API-compat wrapper over [`decode_saleae_runs`]; refuses captures that would
/// expand to more than 1 GiB (the real trace capture is 11.5 G samples — use the runs form).
pub fn decode_saleae_digital(bin: &[u8], _samplerate_hz: f64) -> io::Result<Vec<u8>> {
    let runs = decode_saleae_runs(bin)?;
    if runs.total > (1 << 30) {
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!(
                "capture is {} samples; too large to expand per-sample — use decode_saleae_runs",
                runs.total
            ),
        ));
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
