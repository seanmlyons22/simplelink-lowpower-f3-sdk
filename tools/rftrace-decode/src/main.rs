//! `tracedecode` CLI — LA backend for the RF-core trace sink.
//!
//! Usage:
//!   tracedecode replay <file.sal> --channel 4 --dbgid A.h [--dbgid B.h] [--alias rftrc] (stdout | pcap --out f.pcap)
//!   tracedecode decode --raw <file|-> --channel 4 --dbgid A.h stdout
//!   tracedecode synth  --dbgid A.h --out synth.raw
//!   tracedecode tail   <file.sal> --dbgid A.h [--last 50]
//!   tracedecode retain <file.sal> --dbgid A.h --out ring [--files 4] [--filesize 1024]
//!   tracedecode wireshark --extcap-interfaces | --extcap-dlts | --extcap-config
//!   tracedecode wireshark --capture --fifo <path> --sal <file.sal> --dbgid A.h [--channel 4]

use rftrace_decode::deframe::deframe_edges;
use rftrace_decode::output::{stdout_line, Output, PcapSink, StdoutSink};
use rftrace_decode::packet::{classify, PacketAssembler};
use rftrace_decode::record::resolve;
use rftrace_decode::sample::{levels_from_raw, read_raw, read_sal, SalRuns};
use rftrace_decode::synth;
use rftrace_decode::timestamp::TsState;
use rftrace_decode::types::{DeframeCfg, Health, LogRecord, Word};
use rftrace_decode::{dbgid, DbgIdDb};
use std::collections::VecDeque;
use std::fs::File;
use std::process::ExitCode;

struct Cli {
    subcommand: String,
    input: Option<String>, // .sal path (replay/tail/retain) or raw path (decode)
    dbgid_paths: Vec<String>,
    channel: u8,
    alias: String,
    out: Option<String>,
    samplerate: Option<f64>,
    baud: Option<f64>,
    invert: bool,
    divide_time_by_2: bool,
    outputs: Vec<String>, // "stdout", "pcap"
    // extcap / retain / tail
    fifo: Option<String>,
    extcap_interfaces: bool,
    extcap_dlts: bool,
    extcap_config: bool,
    capture: bool,
    last: usize,
    ring_files: u32,
    ring_filesize_kb: u32,
}

fn parse_cli() -> Result<Cli, String> {
    let mut a = std::env::args().skip(1);
    let subcommand = a
        .next()
        .ok_or("missing subcommand (replay|decode|synth|tail|retain|wireshark)")?;
    let mut cli = Cli {
        subcommand,
        input: None,
        dbgid_paths: Vec::new(),
        channel: 4,
        alias: "rftrc".to_string(),
        out: None,
        samplerate: None,
        baud: None,
        invert: true,
        divide_time_by_2: false,
        outputs: Vec::new(),
        fifo: None,
        extcap_interfaces: false,
        extcap_dlts: false,
        extcap_config: false,
        capture: false,
        last: 50,
        ring_files: 4,
        ring_filesize_kb: 1024,
    };
    let args: Vec<String> = a.collect();
    let mut i = 0;
    while i < args.len() {
        let t = &args[i];
        match t.as_str() {
            "--dbgid" => {
                cli.dbgid_paths.push(next(&args, &mut i)?);
            }
            "--channel" => cli.channel = next(&args, &mut i)?.parse().map_err(|_| "bad --channel")?,
            "--alias" => cli.alias = next(&args, &mut i)?,
            "--out" => cli.out = Some(next(&args, &mut i)?),
            "--raw" => cli.input = Some(next(&args, &mut i)?),
            "--sal" => cli.input = Some(next(&args, &mut i)?),
            "--samplerate" => {
                cli.samplerate = Some(next(&args, &mut i)?.parse().map_err(|_| "bad --samplerate")?)
            }
            "--baud" => cli.baud = Some(next(&args, &mut i)?.parse().map_err(|_| "bad --baud")?),
            "--invert" => cli.invert = true,
            "--no-invert" => cli.invert = false,
            "--divide-time-by-2" => cli.divide_time_by_2 = true,
            "--fifo" => cli.fifo = Some(next(&args, &mut i)?),
            "--extcap-interfaces" => cli.extcap_interfaces = true,
            "--extcap-dlts" => cli.extcap_dlts = true,
            "--extcap-config" => cli.extcap_config = true,
            "--capture" => cli.capture = true,
            "--extcap-interface" | "--extcap-version" => {
                let _ = next(&args, &mut i); // value optional in --extcap-version=x form
            }
            "--last" => cli.last = next(&args, &mut i)?.parse().map_err(|_| "bad --last")?,
            "--files" => cli.ring_files = next(&args, &mut i)?.parse().map_err(|_| "bad --files")?,
            "--filesize" => {
                cli.ring_filesize_kb = next(&args, &mut i)?.parse().map_err(|_| "bad --filesize")?
            }
            "stdout" => cli.outputs.push("stdout".into()),
            "pcap" | "wireshark" => cli.outputs.push("pcap".into()),
            other if other.starts_with("--extcap-version=") => {}
            other if !other.starts_with("--") && cli.input.is_none() => {
                cli.input = Some(other.to_string())
            }
            other => return Err(format!("unknown arg: {other}")),
        }
        i += 1;
    }
    Ok(cli)
}

fn next(args: &[String], i: &mut usize) -> Result<String, String> {
    *i += 1;
    args.get(*i).cloned().ok_or_else(|| "missing value".into())
}

fn build_cfg(cli: &Cli, samplerate: f64) -> DeframeCfg {
    DeframeCfg {
        samplerate_hz: cli.samplerate.unwrap_or(samplerate),
        baud_hz: cli.baud.unwrap_or(24_000_000.0),
        invert: cli.invert,
        divide_time_by_2: cli.divide_time_by_2,
        ..DeframeCfg::default()
    }
}

fn build_outputs(cli: &Cli) -> Result<Vec<Box<dyn Output>>, String> {
    let mut outs: Vec<Box<dyn Output>> = Vec::new();
    for name in &cli.outputs {
        match name.as_str() {
            "stdout" => outs.push(Box::new(StdoutSink)),
            "pcap" => {
                let path = cli.out.clone().ok_or("pcap/wireshark output needs --out <file>")?;
                let f = File::create(&path).map_err(|e| e.to_string())?;
                outs.push(Box::new(PcapSink::new(f).map_err(|e| e.to_string())?));
            }
            _ => {}
        }
    }
    if outs.is_empty() {
        outs.push(Box::new(StdoutSink));
    }
    Ok(outs)
}

/// Words -> packets -> records -> outputs. Shared by every input path.
fn run_words(
    words: &[Word],
    framing_errors: u64,
    cfg: &DeframeCfg,
    db: &DbgIdDb,
    alias: &str,
    outs: &mut [Box<dyn Output>],
) {
    let mut asm = PacketAssembler::new();
    let mut ts = TsState::new();
    let mut health = Health::default();
    health.framing_errors = framing_errors;
    for &w in words {
        if let Some(pkt) = asm.push(classify(w), &mut health) {
            if !pkt.crc_ok {
                continue;
            }
            match resolve(&pkt, db, alias, &mut ts, cfg.divide_time_by_2) {
                Some(rec) => {
                    for o in outs.iter_mut() {
                        o.on_record(&rec);
                    }
                }
                None => health.unknown_dbgid += 1,
            }
        }
    }
    for o in outs.iter_mut() {
        o.on_health(&health);
        o.finish();
    }
}

fn deframe_sal(runs: &SalRuns, cfg: &DeframeCfg) -> (Vec<Word>, u64) {
    deframe_edges(runs.initial_level, &runs.edges, runs.total, cfg)
}

/// Load the .sal named by `cli.input` and run the full pipeline into `outs`.
fn replay_into(cli: &Cli, outs: &mut [Box<dyn Output>]) -> Result<(), String> {
    let path = cli.input.clone().ok_or("need a .sal file")?;
    let cap = read_sal(&path, cli.channel).map_err(|e| format!("reading .sal: {e}"))?;
    let cfg = build_cfg(cli, cap.samplerate_hz);
    let db = load_db(cli)?;
    let (words, framing) = deframe_sal(&cap.runs, &cfg);
    run_words(&words, framing, &cfg, &db, &cli.alias, outs);
    Ok(())
}

fn load_db(cli: &Cli) -> Result<DbgIdDb, String> {
    let db = if cli.dbgid_paths.is_empty() {
        DbgIdDb::default()
    } else {
        dbgid::load(&cli.dbgid_paths).map_err(|e| format!("loading dbgid: {e}"))?
    };
    eprintln!("[info] dbgid entries: {}", db.len());
    Ok(db)
}

/// In-memory last-N ring, printed at end of stream (§15.5).
struct TailSink {
    ring: VecDeque<String>,
    n: usize,
}

impl Output for TailSink {
    fn on_record(&mut self, r: &LogRecord) {
        if self.ring.len() == self.n {
            self.ring.pop_front();
        }
        self.ring.push_back(stdout_line(r));
    }
    fn finish(&mut self) {
        for l in &self.ring {
            println!("{l}");
        }
    }
}

/// extcap protocol (§15.3). Returns an exit code when an extcap query was handled.
fn extcap(cli: &Cli) -> Result<(), String> {
    if cli.extcap_interfaces {
        println!("extcap {{version=0.1.0}}{{help=https://www.ti.com}}");
        println!("interface {{value=rftrc}}{{display=RF-core trace (LRFDTRC) decoder}}");
        return Ok(());
    }
    if cli.extcap_dlts {
        println!("dlt {{number=147}}{{name=USER0}}{{display=DLT_USER0 (tilogger text)}}");
        return Ok(());
    }
    if cli.extcap_config {
        println!("arg {{number=0}}{{call=--sal}}{{display=Saleae .sal capture}}{{type=fileselect}}{{required=true}}");
        println!("arg {{number=1}}{{call=--dbgid}}{{display=dbgid file (elf2dbgid)}}{{type=fileselect}}{{required=true}}");
        println!("arg {{number=2}}{{call=--channel}}{{display=LA channel of rfctrc_out}}{{type=integer}}{{default=4}}");
        println!("arg {{number=3}}{{call=--divide-time-by-2}}{{display=48 MHz tracer clock (divide time by 2)}}{{type=boolflag}}");
        return Ok(());
    }
    if cli.capture {
        let fifo = cli.fifo.clone().ok_or("--capture needs --fifo <path>")?;
        let f = File::create(&fifo).map_err(|e| format!("opening fifo {fifo}: {e}"))?;
        let sink = PcapSink::new(f).map_err(|e| e.to_string())?;
        let mut outs: Vec<Box<dyn Output>> = vec![Box::new(sink)];
        return replay_into(cli, &mut outs);
    }
    Err("wireshark: expected --extcap-interfaces, --extcap-dlts, --extcap-config, or --capture --fifo <path>".into())
}

/// dumpcap ring-buffer retention (§15.4): pipe our pcap stream into dumpcap.
fn retain(cli: &Cli) -> Result<(), String> {
    let out = cli.out.clone().ok_or("retain needs --out <ring-file-base>")?;
    let mut child = std::process::Command::new("dumpcap")
        .args([
            "-i",
            "-",
            "-w",
            &out,
            "-b",
            &format!("files:{}", cli.ring_files),
            "-b",
            &format!("filesize:{}", cli.ring_filesize_kb),
        ])
        .stdin(std::process::Stdio::piped())
        .spawn()
        .map_err(|e| format!("spawning dumpcap: {e}"))?;
    let stdin = child.stdin.take().ok_or("no dumpcap stdin")?;
    let sink = PcapSink::new(stdin).map_err(|e| e.to_string())?;
    let mut outs: Vec<Box<dyn Output>> = vec![Box::new(sink)];
    replay_into(cli, &mut outs)?;
    drop(outs); // closes dumpcap stdin
    let st = child.wait().map_err(|e| e.to_string())?;
    if !st.success() {
        return Err(format!("dumpcap exited with {st}"));
    }
    Ok(())
}

fn main() -> ExitCode {
    let cli = match parse_cli() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("error: {e}");
            return ExitCode::from(2);
        }
    };

    let res: Result<(), String> = match cli.subcommand.as_str() {
        "replay" => build_outputs(&cli).and_then(|mut outs| replay_into(&cli, &mut outs)),
        "tail" => {
            let mut outs: Vec<Box<dyn Output>> = vec![Box::new(TailSink {
                ring: VecDeque::new(),
                n: cli.last,
            })];
            replay_into(&cli, &mut outs)
        }
        "retain" => retain(&cli),
        "wireshark" => extcap(&cli),
        "decode" => (|| {
            let path = cli.input.clone().unwrap_or_else(|| "-".to_string());
            let bytes = read_raw(&path).map_err(|e| format!("reading raw: {e}"))?;
            let levels = levels_from_raw(&bytes, cli.channel);
            let cfg = build_cfg(&cli, 500_000_000.0);
            let db = load_db(&cli)?;
            let mut outs = build_outputs(&cli)?;
            let words = rftrace_decode::deframe::deframe(&levels, &cfg);
            run_words(&words, 0, &cfg, &db, &cli.alias, &mut outs);
            Ok(())
        })(),
        "synth" => (|| {
            let cfg = build_cfg(&cli, 500_000_000.0);
            let db = load_db(&cli)?;
            let out = cli.out.clone().unwrap_or_else(|| "synth.raw".to_string());
            let mut words = Vec::new();
            let mut seq = 0u8;
            for def in db.by_key.values().take(16) {
                let nargs = def.arg_count.unsigned_abs() as usize;
                let args: Vec<u32> = (0..nargs).map(|k| 0x1000 + k as u32).collect();
                let params = synth::args_to_params16(&args, def.arg_count < 0);
                words.extend(synth::encode_packet(
                    def.channel,
                    def.dbgid,
                    &params,
                    Some(seq as u16 * 3),
                    seq & 0xF,
                ));
                seq = seq.wrapping_add(1);
            }
            let samples = synth::words_to_samples(&words, &cfg, 8);
            let packed: Vec<u8> = samples.iter().map(|&l| l << cli.channel).collect();
            std::fs::write(&out, &packed).map_err(|e| format!("writing synth: {e}"))?;
            eprintln!("[info] wrote {} samples to {out}", packed.len());
            Ok(())
        })(),
        other => Err(format!("unknown subcommand: {other}")),
    };

    match res {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("error: {e}");
            ExitCode::FAILURE
        }
    }
}
