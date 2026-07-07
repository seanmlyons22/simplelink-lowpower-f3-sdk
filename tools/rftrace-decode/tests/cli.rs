//! CLI contract tests: every `tracedecode` subcommand and flag, driven through the built
//! binary exactly as tilogger / Wireshark / shell users invoke it. Synthetic captures only;
//! no hardware, no oracle files. Subcommands that need external programs (dumpcap) skip
//! cleanly when those are absent.

mod common;

use std::path::{Path, PathBuf};
use std::process::{Command, Output};

const DBGID_LINE: &[u8] =
    b"DBG_DEF(DBGID_t_c_42, 5, DBGCH1, -1, \"val %08X\", \"t.c\", 42)\n";

fn bin() -> &'static str {
    env!("CARGO_BIN_EXE_tracedecode")
}

/// Fresh temp dir with a one-def dbgid file; returns (dir, dbgid_path).
fn setup(tag: &str) -> (PathBuf, PathBuf) {
    let dir = std::env::temp_dir().join(format!("rftrace-cli-{tag}-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let dbgid = dir.join("t_dbgid.h");
    std::fs::write(&dbgid, DBGID_LINE).unwrap();
    (dir, dbgid)
}

fn run(args: &[&str]) -> Output {
    Command::new(bin()).args(args).output().expect("spawn tracedecode")
}

fn run_ok(args: &[&str]) -> Output {
    let out = run(args);
    assert!(
        out.status.success(),
        "tracedecode {args:?} failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    out
}

fn stdout_records(out: &Output) -> Vec<String> {
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .filter(|l| l.contains(" | "))
        .map(|l| l.to_string())
        .collect()
}

/// Synth a raw capture into `raw` and return its bytes. Extra args splice into the synth call.
fn synth_raw(dbgid: &Path, raw: &Path, extra: &[&str]) -> Vec<u8> {
    let mut args = vec!["synth", "--dbgid", dbgid.to_str().unwrap(), "--out", raw.to_str().unwrap()];
    args.extend_from_slice(extra);
    run_ok(&args);
    std::fs::read(raw).unwrap()
}

/// Levels of one channel from a packed raw capture (bit `ch` of each byte).
fn levels(raw_bytes: &[u8], ch: u8) -> Vec<u8> {
    raw_bytes.iter().map(|b| (b >> ch) & 1).collect()
}

// ---------------------------------------------------------------- synth ----

#[test]
fn synth_repeat_and_idle_frames_scale_the_capture() {
    let (dir, dbgid) = setup("synth");
    let one = dir.join("one.raw");
    let many = dir.join("many.raw");
    let padded = dir.join("padded.raw");
    let n1 = synth_raw(&dbgid, &one, &["--channel", "4"]).len();
    let n5 = synth_raw(&dbgid, &many, &["--channel", "4", "--repeat", "5"]).len();
    assert_eq!(n5, 5 * n1, "--repeat tiles the whole pattern");
    let np = synth_raw(&dbgid, &padded, &["--channel", "4", "--idle-frames", "16"]).len();
    assert!(np > n1, "--idle-frames adds NOP frames");
    // Decoding the repeated capture yields exactly one record per repetition.
    let out = run_ok(&[
        "decode", "--raw", many.to_str().unwrap(), "--channel", "4",
        "--dbgid", dbgid.to_str().unwrap(), "stdout",
    ]);
    assert_eq!(stdout_records(&out).len(), 5);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn synth_channel_places_the_bit() {
    let (dir, dbgid) = setup("synthch");
    let raw = dir.join("ch2.raw");
    let bytes = synth_raw(&dbgid, &raw, &["--channel", "2"]);
    assert!(bytes.iter().any(|b| b & 0x04 != 0), "channel 2 = bit 2 toggles");
    assert!(bytes.iter().all(|b| b & !0x04 == 0), "only bit 2 is driven");
    let _ = std::fs::remove_dir_all(&dir);
}

// ---------------------------------------------------------------- decode ----

#[test]
fn decode_flags_alias_channel_and_stdin() {
    let (dir, dbgid) = setup("decode");
    let raw = dir.join("c.raw");
    let bytes = synth_raw(&dbgid, &raw, &["--channel", "7"]);

    // --channel + --alias show up in the output lines.
    let out = run_ok(&[
        "decode", "--raw", raw.to_str().unwrap(), "--channel", "7",
        "--dbgid", dbgid.to_str().unwrap(), "--alias", "myboard", "stdout",
    ]);
    let recs = stdout_records(&out);
    assert_eq!(recs.len(), 1);
    assert!(recs[0].starts_with("myboard | "), "{}", recs[0]);
    assert!(recs[0].contains("t.c:42"), "{}", recs[0]);
    assert!(recs[0].contains("val 00001000"), "{}", recs[0]);

    // `--raw -` reads the same capture from stdin.
    use std::io::Write;
    let mut child = Command::new(bin())
        .args(["decode", "--raw", "-", "--channel", "7", "--dbgid", dbgid.to_str().unwrap(), "stdout"])
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::null())
        .spawn()
        .unwrap();
    child.stdin.take().unwrap().write_all(&bytes).unwrap();
    let out = child.wait_with_output().unwrap();
    assert!(out.status.success());
    assert_eq!(stdout_records(&out).len(), 1);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn decode_divide_time_by_2_halves_timestamps() {
    let (dir, dbgid) = setup("div2");
    let raw = dir.join("c.raw");
    synth_raw(&dbgid, &raw, &["--channel", "4", "--repeat", "2"]);
    let base = run_ok(&[
        "decode", "--raw", raw.to_str().unwrap(), "--channel", "4",
        "--dbgid", dbgid.to_str().unwrap(), "stdout",
    ]);
    let halved = run_ok(&[
        "decode", "--raw", raw.to_str().unwrap(), "--channel", "4",
        "--dbgid", dbgid.to_str().unwrap(), "--divide-time-by-2", "stdout",
    ]);
    let t = |o: &Output| -> f64 {
        stdout_records(o)[1].split(" | ").nth(1).unwrap().trim().parse().unwrap()
    };
    let (tb, th) = (t(&base), t(&halved));
    assert!(tb > 0.0);
    assert!((th - tb / 2.0).abs() < 1e-12, "expected half of {tb}, got {th}");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn decode_samplerate_and_baud_must_match_the_capture() {
    let (dir, dbgid) = setup("rate");
    let raw = dir.join("c.raw");
    // Synth at non-default physics: 100 MS/s, 10 Mbaud.
    synth_raw(&dbgid, &raw, &["--channel", "4", "--samplerate", "100000000", "--baud", "10000000"]);
    let matched = run_ok(&[
        "decode", "--raw", raw.to_str().unwrap(), "--channel", "4",
        "--samplerate", "100000000", "--baud", "10000000",
        "--dbgid", dbgid.to_str().unwrap(), "stdout",
    ]);
    assert_eq!(stdout_records(&matched).len(), 1);
    // Decoding with the (wrong) defaults must produce no records - the flags matter.
    let wrong = run_ok(&[
        "decode", "--raw", raw.to_str().unwrap(), "--channel", "4",
        "--dbgid", dbgid.to_str().unwrap(), "stdout",
    ]);
    assert_eq!(stdout_records(&wrong).len(), 0);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn decode_invert_flags_are_hints_polarity_is_detected() {
    let (dir, dbgid) = setup("invert");
    let raw = dir.join("c.raw");
    synth_raw(&dbgid, &raw, &["--channel", "4", "--no-invert"]);
    // Auto-detection decodes the non-inverted capture even with the default hint,
    // and both explicit hints are accepted.
    for flag in [None, Some("--invert"), Some("--no-invert")] {
        let mut args = vec![
            "decode", "--raw", raw.to_str().unwrap(), "--channel", "4",
            "--dbgid", dbgid.to_str().unwrap(),
        ];
        if let Some(f) = flag {
            args.push(f);
        }
        args.push("stdout");
        let out = run_ok(&args);
        assert_eq!(stdout_records(&out).len(), 1, "flag {flag:?}");
    }
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn decode_pcap_out_file_and_stdout_dash() {
    let (dir, dbgid) = setup("pcap");
    let raw = dir.join("c.raw");
    synth_raw(&dbgid, &raw, &["--channel", "4"]);
    // pcap --out <file>
    let pf = dir.join("out.pcap");
    run_ok(&[
        "decode", "--raw", raw.to_str().unwrap(), "--channel", "4",
        "--dbgid", dbgid.to_str().unwrap(), "pcap", "--out", pf.to_str().unwrap(),
    ]);
    let pcap = std::fs::read(&pf).unwrap();
    assert_eq!(&pcap[0..4], &0xA1B2C3D4u32.to_le_bytes());
    assert_eq!(u32::from_le_bytes(pcap[20..24].try_into().unwrap()), 147);
    // pcap --out - (stdout) is byte-identical.
    let out = run_ok(&[
        "decode", "--raw", raw.to_str().unwrap(), "--channel", "4",
        "--dbgid", dbgid.to_str().unwrap(), "pcap", "--out", "-",
    ]);
    assert_eq!(out.stdout, pcap);
    let _ = std::fs::remove_dir_all(&dir);
}

// ---------------------------------------------------------------- replay ----

/// Build a synthetic `.sal` holding the given synth capture's trace channel.
fn synth_sal(dir: &Path, dbgid: &Path, repeat: &str, samplerate: u64) -> PathBuf {
    let raw = dir.join("for_sal.raw");
    let bytes = synth_raw(dbgid, &raw, &["--channel", "4", "--repeat", repeat]);
    let sal = dir.join("synth.sal");
    common::write_sal(&sal, &levels(&bytes, 4), 4, samplerate);
    sal
}

#[test]
fn replay_decodes_a_sal_container() {
    let (dir, dbgid) = setup("replay");
    let sal = synth_sal(&dir, &dbgid, "3", 500_000_000);
    let out = run_ok(&[
        "replay", sal.to_str().unwrap(), "--channel", "4",
        "--dbgid", dbgid.to_str().unwrap(), "stdout",
    ]);
    let recs = stdout_records(&out);
    assert_eq!(recs.len(), 3);
    assert!(recs[0].contains("val 00001000"));
    // Health counters report a clean decode.
    let err = String::from_utf8_lossy(&out.stderr);
    assert!(err.contains("crc=0"), "{err}");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn replay_reads_samplerate_from_meta_json() {
    let (dir, dbgid) = setup("replaymeta");
    // Capture synthesized at the default 500 MS/s but stored in a .sal claiming
    // 250 MS/s: replay must honor meta.json and fail to frame it, unless the CLI
    // overrides the rate back.
    let sal = synth_sal(&dir, &dbgid, "1", 250_000_000);
    let bad = run_ok(&[
        "replay", sal.to_str().unwrap(), "--channel", "4",
        "--dbgid", dbgid.to_str().unwrap(), "stdout",
    ]);
    assert_eq!(stdout_records(&bad).len(), 0);
    let good = run_ok(&[
        "replay", sal.to_str().unwrap(), "--channel", "4", "--samplerate", "500000000",
        "--dbgid", dbgid.to_str().unwrap(), "stdout",
    ]);
    assert_eq!(stdout_records(&good).len(), 1);
    let _ = std::fs::remove_dir_all(&dir);
}

// ---------------------------------------------------------------- tail ----

#[test]
fn tail_keeps_only_the_last_n() {
    let (dir, dbgid) = setup("tail");
    let sal = synth_sal(&dir, &dbgid, "6", 500_000_000);
    let out = run_ok(&[
        "tail", sal.to_str().unwrap(), "--last", "2", "--channel", "4",
        "--dbgid", dbgid.to_str().unwrap(),
    ]);
    assert_eq!(stdout_records(&out).len(), 2, "only the last N records print");
    let _ = std::fs::remove_dir_all(&dir);
}

// ---------------------------------------------------------------- retain ----

#[test]
fn retain_writes_a_dumpcap_ring() {
    if Command::new("dumpcap").arg("--version").output().is_err() {
        eprintln!("retain_writes_a_dumpcap_ring: dumpcap not installed; skipping");
        return;
    }
    let (dir, dbgid) = setup("retain");
    let sal = synth_sal(&dir, &dbgid, "4", 500_000_000);
    let ring = dir.join("ring.pcap");
    let out = run(&[
        "retain", sal.to_str().unwrap(), "--channel", "4",
        "--dbgid", dbgid.to_str().unwrap(),
        "--out", ring.to_str().unwrap(), "--files", "2", "--filesize", "1024",
    ]);
    assert!(
        out.status.success(),
        "retain failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    // dumpcap names ring files <base>_NNNNN_<date>.pcap; at least one must exist.
    let found = std::fs::read_dir(&dir)
        .unwrap()
        .filter_map(|e| e.ok())
        .any(|e| e.file_name().to_string_lossy().starts_with("ring"));
    assert!(found, "no ring file created");
    let _ = std::fs::remove_dir_all(&dir);
}

// ---------------------------------------------------------------- wireshark ----

#[test]
fn extcap_discovery_queries() {
    let ifaces = run_ok(&["wireshark", "--extcap-interfaces"]);
    let s = String::from_utf8_lossy(&ifaces.stdout);
    assert!(s.contains("interface {value=rftrc}"), "{s}");

    let dlts = run_ok(&["wireshark", "--extcap-dlts", "--extcap-interface", "rftrc"]);
    let s = String::from_utf8_lossy(&dlts.stdout);
    assert!(s.contains("dlt {number=147}"), "{s}");

    let cfg = run_ok(&["wireshark", "--extcap-config", "--extcap-interface", "rftrc"]);
    let s = String::from_utf8_lossy(&cfg.stdout);
    for arg in ["--sal", "--dbgid", "--channel", "--divide-time-by-2"] {
        assert!(s.contains(&format!("call={arg}")), "missing {arg} in {s}");
    }
}

#[test]
fn extcap_capture_writes_pcap_to_fifo() {
    let (dir, dbgid) = setup("extcap");
    let sal = synth_sal(&dir, &dbgid, "2", 500_000_000);
    let fifo = dir.join("fifo.pcap"); // extcap accepts a plain file path
    run_ok(&[
        "wireshark", "--capture", "--fifo", fifo.to_str().unwrap(),
        "--sal", sal.to_str().unwrap(), "--channel", "4",
        "--dbgid", dbgid.to_str().unwrap(),
    ]);
    let pcap = std::fs::read(&fifo).unwrap();
    assert_eq!(&pcap[0..4], &0xA1B2C3D4u32.to_le_bytes());
    assert_eq!(u32::from_le_bytes(pcap[20..24].try_into().unwrap()), 147);
    assert!(pcap.len() > 40, "no records in extcap pcap");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn extcap_capture_without_fifo_fails() {
    let out = run(&["wireshark", "--capture"]);
    assert!(!out.status.success());
    assert!(String::from_utf8_lossy(&out.stderr).contains("--fifo"));
}

// ---------------------------------------------------------------- errors ----

#[test]
fn error_paths_exit_nonzero() {
    // unknown subcommand
    assert!(!run(&["frobnicate"]).status.success());
    // unknown flag
    let out = run(&["decode", "--bogus"]);
    assert_eq!(out.status.code(), Some(2));
    // missing subcommand
    assert_eq!(run(&[]).status.code(), Some(2));
    // pcap output without --out
    let (dir, dbgid) = setup("errs");
    let raw = dir.join("c.raw");
    synth_raw(&dbgid, &raw, &["--channel", "4"]);
    let out = run(&[
        "decode", "--raw", raw.to_str().unwrap(), "--channel", "4",
        "--dbgid", dbgid.to_str().unwrap(), "pcap",
    ]);
    assert!(!out.status.success());
    assert!(String::from_utf8_lossy(&out.stderr).contains("--out"));
    // missing dbgid file
    let out = run(&["decode", "--raw", raw.to_str().unwrap(), "--dbgid", "/nonexistent.h", "stdout"]);
    assert!(!out.status.success());
    // nonexistent .sal
    let out = run(&["replay", "/nonexistent.sal", "--dbgid", dbgid.to_str().unwrap(), "stdout"]);
    assert!(!out.status.success());
    let _ = std::fs::remove_dir_all(&dir);
}

// ------------------------------------------- Python decoder equivalence ----

/// The Python port must stay byte-identical to this decoder on synthetic captures
/// (its whole reason to exist is keeping the language benchmark honest).
#[test]
fn pydecode_matches_rust_on_synth_capture() {
    if Command::new("python3").arg("--version").output().is_err() {
        eprintln!("pydecode_matches_rust_on_synth_capture: python3 missing; skipping");
        return;
    }
    let pydecode = Path::new(env!("CARGO_MANIFEST_DIR")).join("scripts/pydecode.py");
    let (dir, _) = setup("pyeq");
    // Multi-channel, multi-width defs to exercise params, printf, and seq tracking.
    let dbgid = dir.join("multi_dbgid.h");
    std::fs::write(
        &dbgid,
        b"DBG_DEF(A, 5, DBGCH1, -1, \"val %08X\", \"a.c\", 1)\n\
          DBG_DEF(B, 6, DBGCH2, 2, \"a=%d b=%d\", \"b.c\", 2)\n\
          DBG_DEF(C, 7, DBGCH3, 0, \"marker\", \"c.c\", 3)\n",
    )
    .unwrap();
    let raw = dir.join("eq.raw");
    synth_raw(&dbgid, &raw, &["--channel", "4", "--repeat", "40", "--idle-frames", "3"]);

    let rust = run_ok(&[
        "decode", "--raw", raw.to_str().unwrap(), "--channel", "4",
        "--dbgid", dbgid.to_str().unwrap(), "stdout",
    ]);
    for extra in [&[][..], &["--numpy"][..]] {
        if !extra.is_empty() {
            let has_numpy = Command::new("python3")
                .args(["-c", "import numpy"])
                .output()
                .map(|o| o.status.success())
                .unwrap_or(false);
            if !has_numpy {
                eprintln!("pydecode numpy comparison skipped: numpy not importable");
                continue;
            }
        }
        let mut args = vec![
            pydecode.to_str().unwrap().to_string(),
            "decode".into(), "--raw".into(), raw.to_str().unwrap().into(),
            "--channel".into(), "4".into(),
            "--dbgid".into(), dbgid.to_str().unwrap().into(),
            "stdout".into(),
        ];
        args.extend(extra.iter().map(|s| s.to_string()));
        let py = Command::new("python3").args(&args).output().unwrap();
        assert!(py.status.success(), "pydecode failed: {}", String::from_utf8_lossy(&py.stderr));
        assert_eq!(
            String::from_utf8_lossy(&rust.stdout),
            String::from_utf8_lossy(&py.stdout),
            "pydecode {extra:?} output must be byte-identical to Rust"
        );
    }
    let _ = std::fs::remove_dir_all(&dir);
}

/// Full golden-capture equivalence for the Python port. Costs ~30 s, so it only runs
/// when RFTRACE_PY_GOLDEN=1 is set (and the oracle files exist); CI stays fast.
#[test]
fn pydecode_matches_rust_on_golden_sal() {
    if std::env::var("RFTRACE_PY_GOLDEN").ok().as_deref() != Some("1") {
        eprintln!("pydecode_matches_rust_on_golden_sal: set RFTRACE_PY_GOLDEN=1 to run; skipping");
        return;
    }
    const SAL: &str = "/home/seanlyons/Downloads/tx_burst_example.sal";
    const DBGID_APP: &str =
        "/home/seanlyons/Downloads/rcl_generic_tx_burst_lp_em_cc2745r10_q1_nortos_llvm_dbgid.h";
    const DBGID_PBE: &str = "/home/seanlyons/Downloads/_dbgid_pbe_generic.h";
    for p in [SAL, DBGID_APP, DBGID_PBE] {
        if !Path::new(p).exists() {
            eprintln!("pydecode_matches_rust_on_golden_sal: {p} missing; skipping");
            return;
        }
    }
    let pydecode = Path::new(env!("CARGO_MANIFEST_DIR")).join("scripts/pydecode.py");
    let rust = run_ok(&[
        "replay", SAL, "--channel", "4", "--dbgid", DBGID_APP, "--dbgid", DBGID_PBE, "stdout",
    ]);
    let py = Command::new("python3")
        .args([
            pydecode.to_str().unwrap(), "--numpy", "replay", SAL,
            "--channel", "4", "--dbgid", DBGID_APP, "--dbgid", DBGID_PBE, "stdout",
        ])
        .output()
        .unwrap();
    assert!(py.status.success(), "{}", String::from_utf8_lossy(&py.stderr));
    assert_eq!(
        String::from_utf8_lossy(&rust.stdout),
        String::from_utf8_lossy(&py.stdout)
    );
}
