//! Parser for `elf2dbgid` `DBG_DEF` files - the only metadata source the decoder reads
//! (it never parses an ELF; the Python front-end bridges ELFs to this format).
//!
//! Line form (quoted fields may contain commas, so split only on top-level commas):
//! `DBG_DEF(NAME, ID, DBGCH<n>, ARGCOUNT, "FMT", "FILE", LINE)`
//! - ID: 3..=255 (0..2 are reserved for debug/timestamp/sim-end and never dispatch as logs)
//! - DBGCH<n>: channel 1..=3
//! - ARGCOUNT: >0 => that many 16-bit args; <0 => that many 32-bit args; 0 => none
//!
//! All files merge into one DB keyed on (channel, dbgid). The `*_mst.h` / `*_slv.h`
//! master/slave naming convention only matters if two trace pins are probed at once,
//! which a single-pin decode never needs.

use crate::types::{DbgDef, DbgIdDb};
use std::path::Path;

/// Parse one `DBG_DEF(...)` line. Returns `None` for non-matching / comment lines.
pub fn parse_line(line: &str) -> Option<DbgDef> {
    let line = line.trim();
    let rest = line.strip_prefix("DBG_DEF")?.trim_start();
    let inner = rest.strip_prefix('(')?;
    let inner = inner.trim_end();
    let inner = inner.strip_suffix(')')?;

    let fields = split_top_level(inner);
    if fields.len() != 7 {
        return None;
    }
    let dbgid: u8 = fields[1].trim().parse().ok()?;
    let channel: u8 = fields[2]
        .trim()
        .strip_prefix("DBGCH")?
        .trim()
        .parse()
        .ok()?;
    let arg_count: i32 = fields[3].trim().parse().ok()?;
    let fmt = unquote(fields[4].trim());
    let file = unquote(fields[5].trim());
    let lineno: u32 = fields[6].trim().parse().ok()?;

    if !(3..=255).contains(&dbgid) || !(1..=3).contains(&channel) {
        return None;
    }
    Some(DbgDef {
        dbgid,
        channel,
        arg_count,
        fmt,
        file,
        line: lineno,
    })
}

/// Parse a whole dbgid file body into defs.
pub fn parse_str(body: &str) -> Vec<DbgDef> {
    body.lines().filter_map(parse_line).collect()
}

/// Load and merge one or more dbgid files into a DB keyed on `(channel, dbgid)`.
pub fn load<P: AsRef<Path>>(paths: &[P]) -> std::io::Result<DbgIdDb> {
    let mut db = DbgIdDb::default();
    for p in paths {
        let body = std::fs::read_to_string(p.as_ref())?;
        for def in parse_str(&body) {
            db.by_key.insert((def.channel, def.dbgid), def);
        }
    }
    Ok(db)
}

/// Split on top-level commas (commas outside double-quoted strings). Handles `\"` escapes.
fn split_top_level(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut in_q = false;
    let mut prev_bs = false;
    for c in s.chars() {
        match c {
            '"' if !prev_bs => {
                in_q = !in_q;
                cur.push(c);
            }
            ',' if !in_q => {
                out.push(cur.clone());
                cur.clear();
            }
            _ => cur.push(c),
        }
        prev_bs = c == '\\' && !prev_bs;
    }
    out.push(cur);
    out
}

/// Strip surrounding quotes and unescape common C escapes.
fn unquote(s: &str) -> String {
    let s = s.trim();
    let s = s.strip_prefix('"').unwrap_or(s);
    let s = s.strip_suffix('"').unwrap_or(s);
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars();
    while let Some(c) = chars.next() {
        if c == '\\' {
            match chars.next() {
                Some('n') => out.push('\n'),
                Some('t') => out.push('\t'),
                Some('r') => out.push('\r'),
                Some('"') => out.push('"'),
                Some('\\') => out.push('\\'),
                Some(other) => {
                    out.push('\\');
                    out.push(other);
                }
                None => out.push('\\'),
            }
        } else {
            out.push(c);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_real_lines_with_embedded_commas() {
        let body = r#"
DBG_DEF(DBGID_genericCmd_c_183, 3, DBGCH1, -2, "LRF Events: 0x%08X, RCL Events: 0x%08X", "/home/x/genericCmd.c", 183)
DBG_DEF(DBGID_PBE_1367, 128, DBGCH2, 0, "______hey____CRC error", "pbe_ram_bank0.asm", 1367)
// a comment line, ignored
"#;
        let defs = parse_str(body);
        assert_eq!(defs.len(), 2);
        assert_eq!((defs[0].dbgid, defs[0].channel, defs[0].arg_count), (3, 1, -2));
        assert!(defs[0].fmt.contains("RCL Events")); // comma inside quotes survives
        assert_eq!(defs[1].basename(), "pbe_ram_bank0.asm");
    }

    #[test]
    fn rejects_out_of_range_ids_and_channels() {
        // dbgid 0..2 are reserved control values, channel 0 is the timestamp channel;
        // neither may enter the DB or they would shadow control words.
        assert!(parse_line(r#"DBG_DEF(N, 2, DBGCH1, 0, "x", "f.c", 1)"#).is_none());
        assert!(parse_line(r#"DBG_DEF(N, 3, DBGCH0, 0, "x", "f.c", 1)"#).is_none());
        assert!(parse_line(r#"DBG_DEF(N, 3, DBGCH4, 0, "x", "f.c", 1)"#).is_none());
        assert!(parse_line(r#"DBG_DEF(N, 256, DBGCH1, 0, "x", "f.c", 1)"#).is_none());
        assert!(parse_line(r#"DBG_DEF(N, 3, DBGCH1, 0, "x", "f.c")"#).is_none()); // 6 fields
        assert!(parse_line("int x = 5;").is_none());
    }

    #[test]
    fn unescapes_c_escapes_in_quoted_fields() {
        let def = parse_line(r#"DBG_DEF(N, 3, DBGCH1, 0, "a\nb\t\"q\"\\", "f.c", 1)"#).unwrap();
        assert_eq!(def.fmt, "a\nb\t\"q\"\\");
    }

    #[test]
    fn later_files_override_earlier_keys() {
        let dir = std::env::temp_dir().join(format!("rftrace-dbgid-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let a = dir.join("a.h");
        let b = dir.join("b.h");
        std::fs::write(&a, "DBG_DEF(N, 5, DBGCH1, 0, \"old\", \"a.c\", 1)\n").unwrap();
        std::fs::write(&b, "DBG_DEF(N, 5, DBGCH1, 0, \"new\", \"b.c\", 2)\n").unwrap();
        let db = load(&[&a, &b]).unwrap();
        assert_eq!(db.len(), 1);
        assert_eq!(db.get(1, 5).unwrap().fmt, "new");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn missing_file_is_an_error() {
        assert!(load(&["/nonexistent/nope_dbgid.h"]).is_err());
    }
}
