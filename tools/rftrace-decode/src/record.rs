//! DecodedPacket + dbgid DB -> LogRecord, including C-`printf` substitution (§12).

use crate::timestamp::{ticks_to_us, TsState};
use crate::types::{DbgDef, DbgIdDb, DecodedPacket, LogRecord};

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

/// Resolve a packet to a display record. `None` if the `(channel, dbgid)` is unknown.
pub fn resolve(
    pkt: &DecodedPacket,
    db: &DbgIdDb,
    alias: &str,
    ts: &mut TsState,
    divide_time_by_2: bool,
) -> Option<LogRecord> {
    let def = db.get(pkt.channel, pkt.dbgid)?;
    let args = args_from_params(def, &pkt.params);
    let text = format_c(&def.fmt, &args);
    let ticks = match pkt.ts_delta {
        Some(d) => ts.reconstruct(d),
        None => ts.hold(),
    };
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
            out.push(b[i] as char);
            i += 1;
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
            width = width * 10 + (b[i] - b'0') as usize;
            has_w = true;
            i += 1;
        }
        // precision
        let mut prec: Option<usize> = None;
        if i < b.len() && b[i] == b'.' {
            i += 1;
            let mut p = 0usize;
            while i < b.len() && b[i].is_ascii_digit() {
                p = p * 10 + (b[i] - b'0') as usize;
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
