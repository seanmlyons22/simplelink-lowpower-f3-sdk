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

/// A decoded Saleae capture: per-sample levels for the requested channel + its sample rate.
pub struct SalCapture {
    pub samplerate_hz: f64,
    pub levels: Vec<u8>,
}

/// Read a `.sal` (ZIP): parse `meta.json` for the sample rate, then decode `digital-<ch>.bin`.
///
/// Uses the system `unzip` to avoid a zip dependency in this skeleton
/// (TODO(fable): swap to the `zip` + `serde_json` crates).
pub fn read_sal<P: AsRef<Path>>(path: P, channel: u8) -> io::Result<SalCapture> {
    let path = path.as_ref();
    let meta = unzip_member(path, "meta.json")?;
    let meta = String::from_utf8_lossy(&meta);
    let samplerate_hz = parse_samplerate(&meta).unwrap_or(500_000_000.0);

    let member = format!("digital-{}.bin", channel);
    let bin = unzip_member(path, &member)?;
    let levels = decode_saleae_digital(&bin, samplerate_hz)?;
    Ok(SalCapture {
        samplerate_hz,
        levels,
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

/// Decode a Saleae `.sal` `digital-N.bin` into per-sample levels at `samplerate_hz`.
///
/// TODO(fable): FINALIZE THE TRANSITION-ARRAY OFFSETS AGAINST `tx_burst_example.sal`.
///
/// Confirmed header of `digital-0.bin` (first 96 bytes, little-endian):
/// ```text
/// 3c 53 41 4c 45 41 45 3e   "<SALEAE>"  (8-byte magic)
/// 01 00 00 00               u32 version = 1
/// 64 00 00 00               u32 ? = 100
/// 01 00 00 00               u32 initial_state = 1   (idle high)
/// 00 65 cd bd 41 3a 29 5a   ... begin/end time (f64) + u64 transition_count + f64[] times
/// ...
/// ```
/// The tail is a transition list (edge timestamps in seconds, f64). Expand it to a level
/// timeline sampled at `samplerate_hz`: start at `initial_state`, toggle at each edge.
/// The exact byte offsets of `initial_state` / times must be locked by printing parsed
/// values and comparing against Logic 2 for this file. (Documented Logic 2 binary export:
/// magic, i32 version, i32 type, u32 initial_state, f64 begin, f64 end, u64 num, f64[] times.)
pub fn decode_saleae_digital(bin: &[u8], _samplerate_hz: f64) -> io::Result<Vec<u8>> {
    if bin.len() < 8 || &bin[0..8] != b"<SALEAE>" {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "not a Saleae digital .bin (missing <SALEAE> magic)",
        ));
    }
    Err(io::Error::new(
        io::ErrorKind::Other,
        "TODO(fable): decode_saleae_digital transition array — finalize offsets vs tx_burst_example.sal",
    ))
}

/// Apply the confirmed `tx_burst_example.sal` analyzer settings as decoder defaults.
pub fn cfg_from_sal(samplerate_hz: f64) -> DeframeCfg {
    DeframeCfg {
        samplerate_hz,
        ..DeframeCfg::default()
    }
}
