//! Shared test helpers: build a real Saleae `.sal` container from sample levels, so the
//! `.sal`-consuming subcommands (replay, tail, retain, wireshark --capture) are testable
//! without hardware or oracle files. The crate is std-only, hence the tiny store-only ZIP
//! writer and CRC-32 below instead of a zip dependency.

/// CRC-32 (reflected, poly 0xEDB88320) as required by the ZIP format.
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0xEDB8_8320
            } else {
                crc >> 1
            };
        }
    }
    !crc
}

/// Minimal store-only (no compression) ZIP archive.
pub fn zip_store(members: &[(&str, &[u8])]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut central = Vec::new();
    for (name, data) in members {
        let offset = out.len() as u32;
        let crc = crc32(data);
        let n = data.len() as u32;
        // local file header
        out.extend(0x0403_4B50u32.to_le_bytes());
        out.extend(20u16.to_le_bytes()); // version needed
        out.extend(0u16.to_le_bytes()); // flags
        out.extend(0u16.to_le_bytes()); // method = store
        out.extend(0u32.to_le_bytes()); // mod time/date
        out.extend(crc.to_le_bytes());
        out.extend(n.to_le_bytes()); // compressed size
        out.extend(n.to_le_bytes()); // uncompressed size
        out.extend((name.len() as u16).to_le_bytes());
        out.extend(0u16.to_le_bytes()); // extra len
        out.extend(name.as_bytes());
        out.extend(*data);
        // central directory entry
        central.extend(0x0201_4B50u32.to_le_bytes());
        central.extend(20u16.to_le_bytes()); // version made by
        central.extend(20u16.to_le_bytes()); // version needed
        central.extend(0u16.to_le_bytes()); // flags
        central.extend(0u16.to_le_bytes()); // method
        central.extend(0u32.to_le_bytes()); // mod time/date
        central.extend(crc.to_le_bytes());
        central.extend(n.to_le_bytes());
        central.extend(n.to_le_bytes());
        central.extend((name.len() as u16).to_le_bytes());
        central.extend(0u16.to_le_bytes()); // extra
        central.extend(0u16.to_le_bytes()); // comment
        central.extend(0u16.to_le_bytes()); // disk
        central.extend(0u16.to_le_bytes()); // internal attrs
        central.extend(0u32.to_le_bytes()); // external attrs
        central.extend(offset.to_le_bytes());
        central.extend(name.as_bytes());
    }
    let cd_offset = out.len() as u32;
    let cd_size = central.len() as u32;
    out.extend(&central);
    // end of central directory
    out.extend(0x0605_4B50u32.to_le_bytes());
    out.extend(0u16.to_le_bytes()); // disk
    out.extend(0u16.to_le_bytes()); // cd disk
    out.extend((members.len() as u16).to_le_bytes());
    out.extend((members.len() as u16).to_le_bytes());
    out.extend(cd_size.to_le_bytes());
    out.extend(cd_offset.to_le_bytes());
    out.extend(0u16.to_le_bytes()); // comment len
    out
}

/// Encode one run length in the Saleae big-endian varint form (stored value = run - 1).
fn sal_varint(v: u64) -> Vec<u8> {
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

/// Build a `digital-N.bin` (one block) from per-sample levels.
pub fn sal_digital_bin(levels: &[u8]) -> Vec<u8> {
    assert!(!levels.is_empty());
    let mut runs: Vec<u64> = Vec::new();
    let mut cur = levels[0] & 1;
    let mut len = 0u64;
    for &l in levels {
        if l & 1 == cur {
            len += 1;
        } else {
            runs.push(len);
            cur = l & 1;
            len = 1;
        }
    }
    runs.push(len);
    let total = levels.len() as u64;

    let mut payload = Vec::new();
    for &r in &runs {
        payload.extend(sal_varint(r - 1));
    }
    let mut bin = Vec::new();
    bin.extend(b"<SALEAE>");
    bin.extend(1u32.to_le_bytes()); // version
    bin.resize(0x33, 0); // rest of the header is not read by the decoder
    bin.extend(0u64.to_le_bytes()); // begin
    bin.extend(total.to_le_bytes()); // end
    bin.extend(total.to_le_bytes()); // num samples
    bin.extend(500_000_000u64.to_le_bytes());
    bin.extend(1u64.to_le_bytes());
    bin.extend((payload.len() as u64).to_le_bytes());
    bin.extend(&payload);
    bin.extend(1u32.to_le_bytes()); // jump table entries
    bin.extend(0u32.to_le_bytes()); // pad
    bin.extend(0u64.to_le_bytes()); // entry 0: sample offset
    bin.extend(0u64.to_le_bytes()); // entry 0: run index
    bin.extend(((levels[0] & 1) as u32).to_le_bytes()); // entry 0: block start level
    bin
}

/// Write a complete `.sal` (meta.json + one digital channel) to `path`.
pub fn write_sal(path: &std::path::Path, levels: &[u8], channel: u8, samplerate_hz: u64) {
    let meta = format!(
        r#"{{"data":{{"sampleRate":{{"digital": {samplerate_hz}, "analog": 0}}}}}}"#
    );
    let bin = sal_digital_bin(levels);
    let member = format!("digital-{channel}.bin");
    let zip = zip_store(&[("meta.json", meta.as_bytes()), (&member, &bin)]);
    std::fs::write(path, zip).unwrap();
}
