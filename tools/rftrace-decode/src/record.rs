//! DecodedPacket + dbgid DB -> LogRecord, including C-`printf` substitution.

use crate::timestamp::{ticks_to_us, TsState};
use crate::types::{DbgDef, DbgIdDb, DecodedPacket, Health, LogRecord};

/// Reconstruct 32-bit args from the 16-bit wire params using the def's arg width (sign).
pub fn args_from_params(def: &DbgDef, params: &[u16]) -> Vec<u32> {
    if def.arg_count < 0 {
        // 32-bit args: low half first (par0=low16, par1=high16).
        params
            .chunks(2)
            .map(|c| c[0] as u32 | ((*c.get(1).unwrap_or(&0) as u32) << 16))
            .collect()
    } else {
        params.iter().map(|&w| w as u32).collect()
    }
}

/// Reconstruct device-time ticks for a packet, advancing the rollover state.
fn ticks_for(pkt: &DecodedPacket, ts: &mut TsState) -> u64 {
    match pkt.ts_delta {
        Some(d) => ts.reconstruct(d),
        None => ts.hold(),
    }
}

/// Resolve a packet to a display record. `None` if the `(channel, dbgid)` is unknown
/// (the caller surfaces those via [`unknown_record`] so the traffic stays visible).
///
/// When the def is found but the wire carried a different number of param words than the
/// def declares (`expected_par_cnt`), the record is still rendered best-effort, but the text
/// is annotated and `health.arg_mismatch` is bumped - it points at a stale ELF/dbgid.
pub fn resolve(
    pkt: &DecodedPacket,
    db: &DbgIdDb,
    alias: &str,
    ts: &mut TsState,
    divide_time_by_2: bool,
    health: &mut Health,
) -> Option<LogRecord> {
    let def = db.get(pkt.channel, pkt.dbgid)?;
    let args = args_from_params(def, &pkt.params);
    let mut text = format_c(&def.fmt, &args);
    if pkt.params.len() != def.expected_par_cnt() {
        health.arg_mismatch += 1;
        text.push_str(&format!(
            "  [!dbgid arg mismatch: wire={} par words, def expects {} - stale ELF/dbgid?]",
            pkt.params.len(),
            def.expected_par_cnt()
        ));
    }
    let ticks = ticks_for(pkt, ts);
    Some(LogRecord {
        alias: alias.to_string(),
        channel: pkt.channel,
        dbgid: pkt.dbgid,
        seq: pkt.seq,
        ts_ticks: ticks,
        ts_us: ticks_to_us(ticks, divide_time_by_2),
        file: def.basename().to_string(),
        line: def.line,
        level: "INFO",
        text,
    })
}

/// Build a placeholder record for a packet whose `(channel, dbgid)` isn't in any loaded DB, so
/// the traffic is visible in the log/Wireshark instead of vanishing. Shows the raw fields and
/// which dbgid files to add. Advances the timestamp state like a resolved packet would.
pub fn unknown_record(
    pkt: &DecodedPacket,
    alias: &str,
    ts: &mut TsState,
    divide_time_by_2: bool,
) -> LogRecord {
    let params: Vec<String> = pkt.params.iter().map(|w| format!("0x{w:04X}")).collect();
    let text = format!(
        "<no dbgid: ch{} id=0x{:02X} seq={} par=[{}]> add --dbgid (pbe/rfe/mce)",
        pkt.channel,
        pkt.dbgid,
        pkt.seq,
        params.join(", ")
    );
    let ticks = ticks_for(pkt, ts);
    LogRecord {
        alias: alias.to_string(),
        channel: pkt.channel,
        dbgid: pkt.dbgid,
        seq: pkt.seq,
        ts_ticks: ticks,
        ts_us: ticks_to_us(ticks, divide_time_by_2),
        file: "<unknown dbgid>".to_string(),
        line: 0,
        level: "WARN",
        text,
    }
}

/// Cap printf field width/precision. A malformed dbgid (`%999999999d`) would otherwise drive a
/// multi-GB `" ".repeat(width)` and could overflow `usize` while parsing the digits. Real
/// embedded format strings never approach this, so clamping is invisible in practice.
const MAX_FIELD_WIDTH: usize = 4096;

/// Minimal C-`printf` engine covering the specifiers seen in dbgid strings
/// (`%d %i %u %x %X %o %c %p %s %%`, with flags `- 0`, width, precision, length mods ignored).
/// Args are `u32`; signed conversions reinterpret as `i32`.
pub fn format_c(fmt: &str, args: &[u32]) -> String {
    let b = fmt.as_bytes();
    let mut out = String::with_capacity(fmt.len() + 16);
    let mut i = 0;
    let mut ai = 0;
    while i < b.len() {
        if b[i] != b'%' {
            // Copy the literal span up to the next '%' as a str slice, not byte by
            // byte: pushing raw bytes as chars would double-encode any non-ASCII
            // character in the format string (a real dbgid file contains "us" with
            // a micro sign).
            let start = i;
            while i < b.len() && b[i] != b'%' {
                i += 1;
            }
            out.push_str(&fmt[start..i]); // '%' is ASCII, so both ends are char boundaries
            continue;
        }
        i += 1;
        if i < b.len() && b[i] == b'%' {
            out.push('%');
            i += 1;
            continue;
        }
        // flags
        let mut left = false;
        let mut zero = false;
        while i < b.len() && matches!(b[i], b'-' | b'+' | b' ' | b'#' | b'0') {
            if b[i] == b'-' {
                left = true;
            }
            if b[i] == b'0' {
                zero = true;
            }
            i += 1;
        }
        // width
        let mut width = 0usize;
        let mut has_w = false;
        while i < b.len() && b[i].is_ascii_digit() {
            width = (width * 10 + (b[i] - b'0') as usize).min(MAX_FIELD_WIDTH);
            has_w = true;
            i += 1;
        }
        // precision
        let mut prec: Option<usize> = None;
        if i < b.len() && b[i] == b'.' {
            i += 1;
            let mut p = 0usize;
            while i < b.len() && b[i].is_ascii_digit() {
                p = (p * 10 + (b[i] - b'0') as usize).min(MAX_FIELD_WIDTH);
                i += 1;
            }
            prec = Some(p);
        }
        // length modifiers (ignored)
        while i < b.len() && matches!(b[i], b'l' | b'h' | b'z' | b'j' | b't' | b'L') {
            i += 1;
        }
        if i >= b.len() {
            out.push('%');
            break;
        }
        if !b[i].is_ascii() {
            // "%<non-ascii>": emit the '%' and let the literal copier take the
            // multi-byte character whole on the next pass.
            out.push('%');
            continue;
        }
        let conv = b[i] as char;
        i += 1;
        let arg = args.get(ai).copied();
        let mut consumed = true;
        let body = match conv {
            'd' | 'i' => fmt_int(arg.unwrap_or(0) as i32 as i64, prec),
            'u' => fmt_uint(arg.unwrap_or(0) as u64, 10, false, prec),
            'x' => fmt_uint(arg.unwrap_or(0) as u64, 16, false, prec),
            'X' => fmt_uint(arg.unwrap_or(0) as u64, 16, true, prec),
            'o' => fmt_uint(arg.unwrap_or(0) as u64, 8, false, prec),
            'p' => format!("0x{:x}", arg.unwrap_or(0)),
            'c' => ((arg.unwrap_or(0) as u8) as char).to_string(),
            's' => format!("0x{:X}", arg.unwrap_or(0)), // tracer has no string args
            other => {
                consumed = false;
                format!("%{}", other)
            }
        };
        if consumed {
            ai += 1;
        }
        out.push_str(&pad(&body, width, has_w, left, zero));
    }
    out
}

fn fmt_int(v: i64, prec: Option<usize>) -> String {
    let neg = v < 0;
    let mag = v.unsigned_abs();
    let mut digits = mag.to_string();
    if let Some(p) = prec {
        while digits.len() < p {
            digits.insert(0, '0');
        }
    }
    if neg {
        digits.insert(0, '-');
    }
    digits
}

fn fmt_uint(v: u64, base: u64, upper: bool, prec: Option<usize>) -> String {
    if v == 0 && prec == Some(0) {
        return String::new();
    }
    let mut n = v;
    let mut s = Vec::new();
    if n == 0 {
        s.push(b'0');
    }
    while n > 0 {
        let d = (n % base) as u8;
        let c = if d < 10 {
            b'0' + d
        } else if upper {
            b'A' + (d - 10)
        } else {
            b'a' + (d - 10)
        };
        s.push(c);
        n /= base;
    }
    s.reverse();
    let mut out = String::from_utf8(s).unwrap();
    if let Some(p) = prec {
        while out.len() < p {
            out.insert(0, '0');
        }
    }
    out
}

fn pad(s: &str, width: usize, has_w: bool, left: bool, zero: bool) -> String {
    if !has_w || s.len() >= width {
        return s.to_string();
    }
    let fill = width - s.len();
    if left {
        format!("{}{}", s, " ".repeat(fill))
    } else if zero {
        // Keep a leading sign in front of the zero padding.
        if let Some(rest) = s.strip_prefix('-') {
            format!("-{}{}", "0".repeat(fill), rest)
        } else {
            format!("{}{}", "0".repeat(fill), s)
        }
    } else {
        format!("{}{}", " ".repeat(fill), s)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dbgid;
    use crate::types::DecodedPacket;

    fn def(arg_count: i32, fmt: &str) -> DbgDef {
        DbgDef {
            dbgid: 5,
            channel: 1,
            arg_count,
            fmt: fmt.into(),
            file: "/x/t.c".into(),
            line: 42,
        }
    }

    #[test]
    fn printf_specifiers() {
        assert_eq!(format_c("%d %i %u", &[1, (-2i32) as u32, 3]), "1 -2 3");
        assert_eq!(format_c("%x %X %o", &[0xAB, 0xAB, 8]), "ab AB 10");
        assert_eq!(format_c("%c%c", &[b'h' as u32, b'i' as u32]), "hi");
        assert_eq!(format_c("%p", &[0x2000_0000]), "0x20000000");
        assert_eq!(format_c("100%%", &[]), "100%");
        // The tracer cannot carry string args; %s renders the raw pointer value.
        assert_eq!(format_c("%s", &[0x1234]), "0x1234");
    }

    #[test]
    fn printf_flags_width_precision() {
        assert_eq!(format_c("%08X", &[0xdead]), "0000DEAD");
        assert_eq!(format_c("%8d|", &[42]), "      42|");
        assert_eq!(format_c("%-8d|", &[42]), "42      |");
        assert_eq!(format_c("%05d", &[(-42i32) as u32]), "-0042"); // sign before zeros
        assert_eq!(format_c("%.5u", &[42]), "00042");
        assert_eq!(format_c("%.0u", &[0]), ""); // C: zero with precision 0 prints nothing
        assert_eq!(format_c("%ld %hu", &[7, 8]), "7 8"); // length mods ignored
    }

    #[test]
    fn printf_degenerate_inputs() {
        assert_eq!(format_c("%d %d", &[1]), "1 0"); // missing args render as 0
        assert_eq!(format_c("%q", &[1]), "%q"); // unknown conversion passes through
        assert_eq!(format_c("tail %", &[]), "tail %"); // trailing percent survives
        assert_eq!(format_c("", &[1]), "");
    }

    #[test]
    fn printf_keeps_utf8_intact() {
        // Real dbgid strings contain a micro sign; it must pass through as one
        // character, not as two double-encoded bytes.
        assert_eq!(format_c("%d \u{b5}s", &[50]), "50 \u{b5}s");
        assert_eq!(format_c("%\u{b5}", &[]), "%\u{b5}");
    }

    #[test]
    fn args_widths() {
        // 32-bit args come from word pairs, low half first.
        assert_eq!(args_from_params(&def(-1, ""), &[0x5678, 0x1234]), vec![0x1234_5678]);
        // 16-bit args pass through one word each.
        assert_eq!(args_from_params(&def(2, ""), &[7, 8]), vec![7, 8]);
        // Odd trailing word for a 32-bit def: high half reads as zero.
        assert_eq!(args_from_params(&def(-1, ""), &[0x5678]), vec![0x5678]);
    }

    #[test]
    fn resolve_known_and_unknown() {
        let mut db = DbgIdDb::default();
        let d = def(-1, "v=%08X");
        db.by_key.insert((1, 5), d);
        let mut ts = crate::timestamp::TsState::new();
        let pkt = DecodedPacket {
            channel: 1,
            dbgid: 5,
            seq: 0,
            ts_delta: Some(4),
            params: vec![0x5678, 0x1234],
            crc_ok: true,
        };
        let mut health = Health::default();
        let r = resolve(&pkt, &db, "rftrc", &mut ts, false, &mut health).unwrap();
        assert_eq!(r.text, "v=12345678");
        assert_eq!(r.file, "t.c"); // basename only
        assert_eq!(r.ts_ticks, 4);
        assert_eq!(r.ts_us, 2.0);
        assert_eq!(r.level, "INFO"); // dbgids carry no level
        assert_eq!(health.arg_mismatch, 0); // 2 par words == expected for a -1 (32-bit) def

        let unknown = DecodedPacket { dbgid: 99, ..pkt.clone() };
        assert!(resolve(&unknown, &db, "rftrc", &mut ts, false, &mut health).is_none());
    }

    #[test]
    fn resolve_flags_arg_count_mismatch() {
        // def declares one 32-bit arg (expected_par_cnt == 2); the wire carried only one word.
        let mut db = DbgIdDb::default();
        db.by_key.insert((1, 5), def(-1, "v=%08X"));
        let mut ts = crate::timestamp::TsState::new();
        let mut health = Health::default();
        let pkt = DecodedPacket {
            channel: 1,
            dbgid: 5,
            seq: 0,
            ts_delta: Some(4),
            params: vec![0x5678], // one word, def expects two
            crc_ok: true,
        };
        let r = resolve(&pkt, &db, "rftrc", &mut ts, false, &mut health).unwrap();
        assert_eq!(health.arg_mismatch, 1);
        assert!(r.text.starts_with("v="), "still rendered best-effort: {}", r.text);
        assert!(r.text.contains("arg mismatch"), "annotated: {}", r.text);
    }

    #[test]
    fn unknown_record_is_visible_and_advances_time() {
        let mut ts = crate::timestamp::TsState::new();
        let pkt = DecodedPacket {
            channel: 2,
            dbgid: 0x4F,
            seq: 3,
            ts_delta: Some(10),
            params: vec![0x1234, 0x5678],
            crc_ok: true,
        };
        let r = unknown_record(&pkt, "rftrc", &mut ts, false);
        assert_eq!(r.level, "WARN");
        assert_eq!(r.ts_ticks, 10); // timestamp reconstructed, not dropped
        assert!(r.text.contains("ch2"), "{}", r.text);
        assert!(r.text.contains("0x4F"), "{}", r.text);
        assert!(r.text.contains("0x1234, 0x5678"), "{}", r.text);
        assert!(r.text.contains("--dbgid"), "{}", r.text);
    }

    #[test]
    fn format_c_caps_absurd_width() {
        // A malformed dbgid must not drive a giant allocation; width clamps.
        let out = format_c("%999999999d", &[7]);
        assert!(out.len() <= MAX_FIELD_WIDTH, "clamped to {}", out.len());
        assert!(out.trim_start().starts_with('7') || out.ends_with('7'));
    }

    #[test]
    fn resolve_without_timestamp_holds_last() {
        let db = {
            let mut db = DbgIdDb::default();
            for d in dbgid::parse_str(r#"DBG_DEF(N, 5, DBGCH1, 0, "x", "t.c", 1)"#) {
                db.by_key.insert((d.channel, d.dbgid), d);
            }
            db
        };
        let mut ts = crate::timestamp::TsState::new();
        ts.reconstruct(100);
        let pkt = DecodedPacket {
            channel: 1,
            dbgid: 5,
            seq: 0,
            ts_delta: None,
            params: vec![],
            crc_ok: true,
        };
        let mut health = Health::default();
        let r = resolve(&pkt, &db, "rftrc", &mut ts, false, &mut health).unwrap();
        assert_eq!(r.ts_ticks, 100);
    }
}
