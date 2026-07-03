//! `tracedecode` CLI — LA backend for the RF-core trace sink.
//!
//! Skeleton uses a hand-rolled arg parser (zero deps). TODO(fable): switch to `clap` and add
//! the `wireshark --extcap`, `retain`, and `tail` subcommands (§14/§15).
//!
//! Usage (current skeleton):
//!   tracedecode replay <file.sal> --channel 4 --dbgid A.h [--dbgid B.h] [--alias rftrc] (stdout | pcap --out f.pcap)
//!   tracedecode decode --raw <file|-> --channel 4 --dbgid A.h stdout
//!   tracedecode synth  --dbgid A.h --out synth.raw

use rftrace_decode::deframe::deframe;
use rftrace_decode::output::{Output, PcapSink, StdoutSink};
use rftrace_decode::packet::{classify, PacketAssembler};
use rftrace_decode::record::resolve;
use rftrace_decode::sample::{levels_from_raw, read_raw, read_sal};
use rftrace_decode::synth;
use rftrace_decode::timestamp::TsState;
use rftrace_decode::types::{DeframeCfg, Health};
use rftrace_decode::{dbgid, DbgIdDb};
use std::fs::File;
use std::process::ExitCode;

struct Cli {
    subcommand: String,
    input: Option<String>, // .sal path (replay) or raw path (decode)
    dbgid_paths: Vec<String>,
    channel: u8,
    alias: String,
    out: Option<String>,
    samplerate: Option<f64>,
    baud: Option<f64>,
    invert: bool,
    divide_time_by_2: bool,
    outputs: Vec<String>, // "stdout", "pcap"
}

fn parse_cli() -> Result<Cli, String> {
    let mut a = std::env::args().skip(1);
    let subcommand = a.next().ok_or("missing subcommand (replay|decode|synth)")?;
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
            "--samplerate" => {
                cli.samplerate = Some(next(&args, &mut i)?.parse().map_err(|_| "bad --samplerate")?)
            }
            "--baud" => cli.baud = Some(next(&args, &mut i)?.parse().map_err(|_| "bad --baud")?),
            "--invert" => cli.invert = true,
            "--no-invert" => cli.invert = false,
            "--divide-time-by-2" => cli.divide_time_by_2 = true,
            "stdout" => cli.outputs.push("stdout".into()),
            "pcap" | "wireshark" => cli.outputs.push("pcap".into()),
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

fn run_pipeline(levels: &[u8], cfg: &DeframeCfg, db: &DbgIdDb, alias: &str, outs: &mut [Box<dyn Output>]) {
    let words = deframe(levels, cfg);
    let mut asm = PacketAssembler::new();
    let mut ts = TsState::new();
    let mut health = Health::default();
    for w in words {
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

fn main() -> ExitCode {
    let cli = match parse_cli() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("error: {e}");
            return ExitCode::from(2);
        }
    };

    let db = if cli.dbgid_paths.is_empty() {
        DbgIdDb::default()
    } else {
        match dbgid::load(&cli.dbgid_paths) {
            Ok(db) => db,
            Err(e) => {
                eprintln!("error loading dbgid: {e}");
                return ExitCode::FAILURE;
            }
        }
    };
    eprintln!("[info] dbgid entries: {}", db.len());

    match cli.subcommand.as_str() {
        "replay" => {
            let path = match &cli.input {
                Some(p) => p.clone(),
                None => {
                    eprintln!("error: replay needs a .sal file");
                    return ExitCode::from(2);
                }
            };
            match read_sal(&path, cli.channel) {
                Ok(cap) => {
                    let cfg = build_cfg(&cli, cap.samplerate_hz);
                    let mut outs = match build_outputs(&cli) {
                        Ok(o) => o,
                        Err(e) => {
                            eprintln!("error: {e}");
                            return ExitCode::from(2);
                        }
                    };
                    run_pipeline(&cap.levels, &cfg, &db, &cli.alias, &mut outs);
                }
                Err(e) => {
                    eprintln!("error reading .sal: {e}");
                    return ExitCode::FAILURE;
                }
            }
        }
        "decode" => {
            let path = cli.input.clone().unwrap_or_else(|| "-".to_string());
            let bytes = match read_raw(&path) {
                Ok(b) => b,
                Err(e) => {
                    eprintln!("error reading raw: {e}");
                    return ExitCode::FAILURE;
                }
            };
            let levels = levels_from_raw(&bytes, cli.channel);
            let cfg = build_cfg(&cli, 500_000_000.0);
            let mut outs = match build_outputs(&cli) {
                Ok(o) => o,
                Err(e) => {
                    eprintln!("error: {e}");
                    return ExitCode::from(2);
                }
            };
            run_pipeline(&levels, &cfg, &db, &cli.alias, &mut outs);
        }
        "synth" => {
            let cfg = build_cfg(&cli, 500_000_000.0);
            let out = cli.out.clone().unwrap_or_else(|| "synth.raw".to_string());
            // Demo: emit each known def once with dummy args.
            let mut words = Vec::new();
            let mut seq = 0u8;
            for def in db.by_key.values().take(16) {
                let nargs = def.arg_count.unsigned_abs() as usize;
                let args: Vec<u32> = (0..nargs).map(|k| 0x1000 + k as u32).collect();
                let params = synth::args_to_params16(&args, def.arg_count < 0);
                words.extend(synth::encode_packet(def.channel, def.dbgid, &params, Some(seq as u16 * 3), seq & 0xF));
                seq = seq.wrapping_add(1);
            }
            let samples = synth::words_to_samples(&words, &cfg, 8);
            // Pack levels into 1 byte/sample on bit `channel`.
            let packed: Vec<u8> = samples.iter().map(|&l| l << cli.channel).collect();
            if let Err(e) = std::fs::write(&out, &packed) {
                eprintln!("error writing synth: {e}");
                return ExitCode::FAILURE;
            }
            eprintln!("[info] wrote {} samples to {out}", packed.len());
        }
        other => {
            eprintln!("unknown subcommand: {other}");
            return ExitCode::from(2);
        }
    }
    ExitCode::SUCCESS
}
