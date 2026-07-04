//! Golden-vector + unit tests, plus a subprocess test proving the built `tracedecode`
//! binary emits pcap output the shared `tilogger` dissector consumes (same DLT + `||`
//! columns as the ITM/UART path).

use rftrace_decode::crc5::crc5_usb;
use rftrace_decode::dbgid;
use rftrace_decode::packet::{classify, PacketAssembler};
use rftrace_decode::record::{args_from_params, format_c};
use rftrace_decode::timestamp::TsState;
use rftrace_decode::types::{DeframeCfg, Health, Word, WordKind};
use rftrace_decode::{output, synth};

// ---- dbgid parser ----

#[test]
fn dbgid_parses_real_lines() {
    let body = r#"
DBG_DEF(DBGID_genericCmd_c_183, 3, DBGCH1, -2, "TestRCL_DefaultCallback: LRF Events: 0x%08X, RCL Events: 0x%08X", "/home/x/genericCmd.c", 183)
DBG_DEF(DBGID_PBE_RAM_BANK0_ASM_1367, 128, DBGCH2, 0, "______hey____CRC error", "pbe_ram_bank0.asm", 1367)
// a comment line, ignored
DBG_DEF(DBGID_x_770, 40, DBGCH1, 3, "GracefulStopTime (0.25us): %d, StartTime (0.25us): %d, extra %d", "generic.c", 770)
"#;
    let defs = dbgid::parse_str(body);
    assert_eq!(defs.len(), 3);
    // field 0: 32-bit args (negative), fmt keeps its embedded comma
    assert_eq!(defs[0].dbgid, 3);
    assert_eq!(defs[0].channel, 1);
    assert_eq!(defs[0].arg_count, -2);
    assert!(defs[0].fmt.contains("RCL Events"));
    assert_eq!(defs[0].expected_par_cnt(), 4); // 2 x 32-bit = 4 words
    // field 2 has commas inside the quoted fmt - must not split
    assert_eq!(defs[2].arg_count, 3);
    assert_eq!(defs[2].expected_par_cnt(), 3);
    assert_eq!(defs[1].basename(), "pbe_ram_bank0.asm");
}

// ---- CRC-5/USB anchor ----

#[test]
fn crc5_usb_check_value() {
    assert_eq!(crc5_usb(b"123456789"), 0x19);
}

// ---- word classify ----

#[test]
fn classify_masks() {
    assert_eq!(classify(Word::new(0x000)), WordKind::Nop);
    assert_eq!(classify(Word::new(0x002)), WordKind::Overflow(1));
    assert_eq!(
        classify(Word::new(0x031)),
        WordKind::TsMsb { ts_en: true, ts_msb: 1 }
    );
    // SOP ch1, ts_en, seq3 => (1<<6)|(1<<4)|3 = 0x53
    assert_eq!(
        classify(Word::new(0x53)),
        WordKind::Sop { ch: 1, ts_en: true, seq: 3 }
    );
    // EOP ch1, crc 0x1f => (1<<6)|0x20|0x1f = 0x7F
    assert_eq!(classify(Word::new(0x7F)), WordKind::Eop { ch: 1, crc5: 0x1f });
    // Data ch2 byte 0xAB
    assert_eq!(classify(Word::new(0x2AB)), WordKind::Data { ch: 2, byte: 0xAB });
}

// ---- packet SM + CRC round-trip via synth (no deframe needed) ----

#[test]
fn packet_roundtrip_through_assembler() {
    let params = synth::args_to_params16(&[0x1234_5678], true); // one 32-bit arg
    let words = synth::encode_packet(1, 5, &params, Some(0x0102), 7);
    let mut asm = PacketAssembler::new();
    let mut health = Health::default();
    let mut got = None;
    for w in &words {
        if let Some(p) = asm.push(classify(*w), &mut health) {
            got = Some(p);
        }
    }
    let p = got.expect("assembler yielded a packet");
    assert_eq!(p.channel, 1);
    assert_eq!(p.dbgid, 5);
    assert_eq!(p.seq, 7);
    assert_eq!(p.ts_delta, Some(0x0102));
    assert!(p.crc_ok, "CRC-5 must validate (synth encode == assembler decode)");
    assert_eq!(p.params, vec![0x5678, 0x1234]); // low half first
    assert_eq!(health.crc_errors, 0);
}

// ---- printf ----

#[test]
fn printf_subset() {
    assert_eq!(format_c("%08X", &[0xdead]), "0000DEAD");
    assert_eq!(format_c("%02X %02X", &[0x0a, 0x0b]), "0A 0B");
    assert_eq!(format_c("cnt: %d", &[42]), "cnt: 42");
    assert_eq!(format_c("%1d", &[7]), "7");
    assert_eq!(format_c("v=%d", &[(-5i32) as u32]), "v=-5");
    assert_eq!(format_c("100%%", &[]), "100%");
}

#[test]
fn args_reconstruction() {
    // 32-bit arg reconstruction from 16-bit params (low half first).
    let def = &dbgid::parse_str(
        r#"DBG_DEF(DBGID_a, 3, DBGCH1, -1, "x %08X", "a.c", 1)"#,
    )[0];
    assert_eq!(args_from_params(def, &[0x5678, 0x1234]), vec![0x1234_5678]);
}

// ---- timestamp rollover ----

#[test]
fn timestamp_rollover() {
    let mut ts = TsState::new();
    assert_eq!(ts.reconstruct(0xFFF0), 0xFFF0); // first sample seeds
    let t = ts.reconstruct(0x0005); // wrapped: way behind -> +0x10000
    assert_eq!(t, 0x1_0005);
}

// ---- Wireshark payload shape (tilogger-identical) ----

#[test]
fn wireshark_payload_shape() {
    let r = rftrace_decode::types::LogRecord {
        alias: "rftrc".into(),
        channel: 1,
        dbgid: 5,
        seq: 0,
        ts_ticks: 2,
        ts_us: 1.0,
        file: "RCL.c".into(),
        line: 884,
        level: "INFO",
        text: "hello".into(),
    };
    let p = output::wireshark_payload(&r);
    let fields: Vec<&str> = p.split("||").collect();
    assert_eq!(fields.len(), 8);
    assert_eq!(fields[0], "rftrc");
    assert_eq!(fields[2], "LOG_OPCODE_FORMATED_TEXT");
    assert_eq!(fields[3], "DBGCH1");
    assert_eq!(fields[5], "RCL.c");
    assert_eq!(fields[7], "hello");
}

// ---- golden .txt parser (oracle) ----

/// Parse the golden decode `.txt` (4-line blocks: alias / abs-time / duration / `file:line >> text`).
fn parse_golden(txt: &str) -> Vec<(String, u32, String)> {
    let lines: Vec<&str> = txt.lines().collect();
    let mut out = Vec::new();
    let mut i = 0;
    while i + 3 < lines.len() + 1 && i + 3 <= lines.len() {
        let rec = lines.get(i + 3);
        if let Some(rec) = rec {
            if let Some((loc, text)) = rec.split_once(" >> ") {
                if let Some((file, line)) = loc.rsplit_once(':') {
                    if let Ok(n) = line.trim().parse::<u32>() {
                        out.push((file.to_string(), n, text.to_string()));
                    }
                }
            }
        }
        i += 4;
    }
    out
}

#[test]
fn golden_txt_parser() {
    let block = "rftrc\n8.509 076 990 s\n6.478 \u{b5}s\nRCL.c:884 >> RCL_open: Git SHA: 82d8ed17bae09623\n";
    let recs = parse_golden(block);
    assert_eq!(recs.len(), 1);
    assert_eq!(recs[0].0, "RCL.c");
    assert_eq!(recs[0].1, 884);
    assert!(recs[0].2.starts_with("RCL_open: Git SHA"));
}

// ================= end-to-end tests over the implemented deframe / .sal path =================

#[test]
fn synth_roundtrip_via_deframe() {
    use rftrace_decode::deframe::deframe;
    let cfg = DeframeCfg::default();
    let params = synth::args_to_params16(&[0xABCD], false); // 16-bit arg
    let words = synth::encode_packet(2, 130, &params, Some(0x0044), 1);
    let samples = synth::words_to_samples(&words, &cfg, 8);
    let decoded = deframe(&samples, &cfg);

    let mut asm = PacketAssembler::new();
    let mut health = Health::default();
    let mut got = None;
    for w in decoded {
        if let Some(p) = asm.push(classify(w), &mut health) {
            got = Some(p);
        }
    }
    let p = got.expect("round-trip packet");
    assert_eq!(p.channel, 2);
    assert_eq!(p.dbgid, 130);
    assert!(p.crc_ok);
}

/// Normalize text for content comparison against the golden `.txt`: its generator
/// double-encoded UTF-8 (micro sign u{b5} became u{c2}u{b5}) and collapsed some doubled
/// spaces in format strings. Our decoder emits clean UTF-8, so this only really
/// rewrites the golden side; applying it to both is harmless.
fn norm(s: &str) -> String {
    let mut t = s.replace('\u{c2}', ""); // strip the mojibake prefix byte
    while t.contains("  ") {
        t = t.replace("  ", " ");
    }
    t.trim().to_string()
}

const SAL: &str = "/home/seanlyons/Downloads/tx_burst_example.sal";
const GOLDEN: &str = "/home/seanlyons/Pictures/tx_burst_example_decoded.txt";
const DBGID_APP: &str =
    "/home/seanlyons/Downloads/rcl_generic_tx_burst_lp_em_cc2745r10_q1_nortos_llvm_dbgid.h";
const DBGID_PBE: &str = "/home/seanlyons/Downloads/_dbgid_pbe_generic.h";

#[test]
fn golden_sal_end_to_end() {
    use rftrace_decode::deframe::deframe_edges;
    use rftrace_decode::record::resolve;
    use rftrace_decode::sample::{cfg_from_sal, read_sal};
    use rftrace_decode::timestamp::TsState;

    for p in [SAL, GOLDEN, DBGID_APP, DBGID_PBE] {
        if !std::path::Path::new(p).exists() {
            eprintln!("golden_sal_end_to_end: oracle file {p} missing; skipping");
            return;
        }
    }

    let db = dbgid::load(&[DBGID_APP, DBGID_PBE]).expect("load dbgid files");
    assert!(db.len() >= 190, "expected ~193 dbgid defs, got {}", db.len());

    let cap = read_sal(SAL, 4).expect("read .sal channel 4");
    assert_eq!(cap.samplerate_hz, 500_000_000.0);
    let cfg = cfg_from_sal(cap.samplerate_hz);
    let (words, framing) =
        deframe_edges(cap.runs.initial_level, &cap.runs.edges, cap.runs.total, &cfg);
    assert!(words.len() > 1_000_000, "deframe produced too few words");

    let mut asm = PacketAssembler::new();
    let mut health = Health::default();
    let mut ts = TsState::new();
    let mut got: Vec<(String, u32, String)> = Vec::new();
    let mut crc_bad = 0u64;
    let mut last_ticks = 0u64;
    for w in words {
        if let Some(p) = asm.push(classify(w), &mut health) {
            if !p.crc_ok {
                crc_bad += 1;
                continue;
            }
            if let Some(r) = resolve(&p, &db, "rftrc", &mut ts, cfg.divide_time_by_2) {
                assert!(r.ts_ticks >= last_ticks, "timestamps must be monotonic");
                last_ticks = r.ts_ticks;
                got.push((r.file.clone(), r.line, norm(&r.text)));
            }
        }
    }
    assert_eq!(crc_bad, 0, "all real packets must pass CRC-5 (MSB-first)");
    eprintln!("[golden] words framing_errors={framing} records={}", got.len());

    // Expected = golden records whose file appears in the loaded DBs (radio rfe/mce
    // dbgids are not provided -> skipped, matching resolve() returning None).
    let known: std::collections::HashSet<&str> =
        db.by_key.values().map(|d| d.basename()).collect();
    let golden_txt = std::fs::read_to_string(GOLDEN).expect("read golden");
    let expected: Vec<(String, u32, String)> = parse_golden(&golden_txt)
        .into_iter()
        .filter(|(f, _, _)| known.contains(f.as_str()))
        .map(|(f, l, t)| (f, l, norm(&t)))
        .collect();
    assert_eq!(expected.len(), 23, "golden app+pbe subset should be 23 records");
    assert_eq!(got, expected, "decoded records must match the golden app+pbe subset");
}

// ============ subprocess: binary launches + emits dissector-compatible pcap ============

/// Parse the tilogger `||` fields the Lua dissector splits from a DLT_USER0 payload.
/// Mirrors `tilogger_dissector.lua`: 8 columns, alias/ts/opcode/module/level/file/line/string.
fn dissector_columns(payload: &[u8]) -> Vec<String> {
    std::str::from_utf8(payload)
        .unwrap()
        .split("||")
        .map(|s| s.to_string())
        .collect()
}

/// End-to-end through the *installed CLI* (as tilogger/Wireshark/extcap launch it): synth a
/// raw capture, then `decode ... pcap --out -`, and assert the emitted pcap is byte-shaped
/// exactly like the ITM/UART path - DLT_USER0=147 + the 8 `||` columns the shared dissector
/// reads. This is the real "swap ITM for rftrace" contract: same presentation, same wire.
#[test]
fn cli_emits_tilogger_pcap_over_stdout() {
    use std::io::Write;
    use std::process::Command;

    let bin = env!("CARGO_BIN_EXE_tracedecode");
    let dir = std::env::temp_dir().join(format!("rftrace-cli-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let dbgid_path = dir.join("t_dbgid.h");
    let raw_path = dir.join("synth.raw");
    std::fs::File::create(&dbgid_path)
        .unwrap()
        .write_all(b"DBG_DEF(DBGID_t_c_42, 5, DBGCH1, -1, \"val %08X\", \"t.c\", 42)\n")
        .unwrap();

    // synth a raw capture on channel 4 from that single dbgid def.
    let synth = Command::new(bin)
        .args(["synth", "--dbgid"])
        .arg(&dbgid_path)
        .args(["--channel", "4", "--out"])
        .arg(&raw_path)
        .output()
        .expect("run tracedecode synth");
    assert!(synth.status.success(), "synth failed: {}", String::from_utf8_lossy(&synth.stderr));

    // decode -> pcap on stdout, exactly as the sigrok pipe / extcap fifo consume it.
    let decode = Command::new(bin)
        .args(["decode", "--raw"])
        .arg(&raw_path)
        .args(["--channel", "4", "--dbgid"])
        .arg(&dbgid_path)
        .args(["pcap", "--out", "-"])
        .output()
        .expect("run tracedecode decode");
    assert!(decode.status.success(), "decode failed: {}", String::from_utf8_lossy(&decode.stderr));
    let pcap = decode.stdout;

    // pcap global header: magic + DLT_USER0.
    assert!(pcap.len() > 24, "no pcap emitted");
    assert_eq!(&pcap[0..4], &0xA1B2C3D4u32.to_le_bytes(), "pcap magic");
    let dlt = u32::from_le_bytes(pcap[20..24].try_into().unwrap());
    assert_eq!(dlt, 147, "must be DLT_USER0 so tilogger_dissector.lua binds it");

    // first record payload -> dissector columns.
    let caplen = u32::from_le_bytes(pcap[32..36].try_into().unwrap()) as usize;
    let cols = dissector_columns(&pcap[40..40 + caplen]);
    assert_eq!(cols.len(), 8, "dissector expects 8 || columns, got {cols:?}");
    assert_eq!(cols[0], "rftrc", "alias");
    assert_eq!(cols[2], "LOG_OPCODE_FORMATED_TEXT", "opcode");
    assert_eq!(cols[3], "DBGCH1", "module = channel");
    assert_eq!(cols[4], "INFO", "level");
    assert_eq!(cols[5], "t.c", "file from dbgid");
    assert_eq!(cols[6], "42", "line from dbgid");
    assert_eq!(cols[7], "val 00001000", "printf-formatted text");

    let _ = std::fs::remove_dir_all(&dir);
}

// ============ streaming decode: bounded memory, correct across chunk boundaries ============

/// The `decode` path streams in 1 MiB chunks with a one-frame carry. Synth one capture, tile
/// it past several chunk boundaries, decode, and assert every packet survives, i.e. frames
/// straddling a chunk boundary are neither dropped nor duplicated (guards `stream_decode`).
#[test]
fn stream_decode_crosses_chunk_boundaries() {
    use std::io::Write;
    use std::process::Command;

    let bin = env!("CARGO_BIN_EXE_tracedecode");
    let dir = std::env::temp_dir().join(format!("rftrace-stream-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let dbgid_path = dir.join("t_dbgid.h");
    let one = dir.join("one.raw");
    let big = dir.join("big.raw");
    std::fs::File::create(&dbgid_path)
        .unwrap()
        .write_all(b"DBG_DEF(DBGID_t_c_42, 5, DBGCH1, -1, \"val %08X\", \"t.c\", 42)\n")
        .unwrap();

    // one synth capture = 1 packet.
    let s = Command::new(bin)
        .args(["synth", "--dbgid"]).arg(&dbgid_path)
        .args(["--channel", "4", "--out"]).arg(&one)
        .output().unwrap();
    assert!(s.status.success());
    let onebytes = std::fs::read(&one).unwrap();

    // tile past 3 MiB (>= 3 chunk boundaries at 1 MiB/chunk).
    let tiles = (3 * (1 << 20) / onebytes.len()) + 4;
    let mut f = std::fs::File::create(&big).unwrap();
    for _ in 0..tiles {
        f.write_all(&onebytes).unwrap();
    }
    drop(f);

    let out = Command::new(bin)
        .args(["decode", "--raw"]).arg(&big)
        .args(["--channel", "4", "--dbgid"]).arg(&dbgid_path)
        .arg("stdout")
        .output().unwrap();
    assert!(out.status.success());
    let records = String::from_utf8_lossy(&out.stdout)
        .lines()
        .filter(|l| l.starts_with("rftrc "))
        .count();
    assert_eq!(records, tiles, "one packet per tile must survive chunk boundaries");
    // health line reports zero framing/crc errors across the boundaries.
    let health = String::from_utf8_lossy(&out.stderr);
    assert!(health.contains("framing=0 crc=0"), "no boundary framing/crc errors: {health}");

    let _ = std::fs::remove_dir_all(&dir);
}
