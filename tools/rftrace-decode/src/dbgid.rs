//! Parser for `elf2dbgid` `DBG_DEF` files (§12).
//!
//! Line form (8 fields, some quoted and may contain commas):
//! `DBG_DEF(NAME, ID, DBGCH<n>, ARGCOUNT, "FMT", "FILE", LINE)`
//! - ID: 3..=255 (0..2 reserved)
//! - DBGCH<n>: channel 1..=3
//! - ARGCOUNT: >0 => 16-bit args; <0 => 32-bit args; 0 => none
//!
//! File-name routing (reference parser): `*_mst.h` master-only, `*_slv.h` slave-only,
//! `*.h` both. This single-stream skeleton merges all into one DB (TODO(fable): split
//! master/slave if two trace pins are captured).

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
