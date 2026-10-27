use std::env;
use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::PathBuf;

fn run() -> Result<(), String> {
    let mut args = env::args_os().skip(1);
    if let Some(first) = args.next() {
        if first == "validate-xma" {
            let input = args.next().map(PathBuf::from).ok_or("Missing input XWC")?;
            if args.next().is_some() {
                return Err("Too many arguments".into());
            }
            let mut file = File::open(&input).map_err(|e| format!("{}: {e}", input.display()))?;
            let bank = darkrecomp_xwc_audio::inspect(&mut file)?;
            let mut mono = 0usize;
            let mut stereo = 0usize;
            for wave in &bank.waves {
                let xma = darkrecomp_xwc_audio::xma::describe(&mut file, &bank, wave)
                    .map_err(|error| format!("{}: {error}", wave.name))?;
                if xma.channels == 1 {
                    mono += 1;
                } else {
                    stereo += 1;
                }
            }
            println!("{{\"kind\":\"xma-envelope-validation\",\"waves\":{},\"mono\":{},\"stereo\":{},\"audioDecoded\":false}}",bank.waves.len(),mono,stereo);
            return Ok(());
        }
        if first == "raw-timeline" {
            let input = args.next().map(PathBuf::from).ok_or("Missing input XWC")?;
            let name = args
                .next()
                .ok_or("Missing exact wave name")?
                .into_string()
                .map_err(|_| "Wave name must be UTF-8")?;
            if args.next().is_some() {
                return Err("Too many arguments".into());
            }
            let mut file = File::open(&input).map_err(|e| format!("{}: {e}", input.display()))?;
            let bank = darkrecomp_xwc_audio::inspect(&mut file)?;
            let matches = bank
                .waves
                .iter()
                .filter(|w| w.name == name)
                .collect::<Vec<_>>();
            if matches.len() != 1 {
                return Err("Wave name must identify exactly one record".into());
            }
            let timeline =
                darkrecomp_xwc_audio::xma::header_derived_timeline(&mut file, &bank, matches[0])?;
            let header_derived_minus_metadata =
                i128::from(timeline.header_derived_frames_per_channel)
                    - i128::from(timeline.metadata_samples_per_channel);
            println!("{{\"kind\":\"xma-header-derived-timeline\",\"channels\":{},\"rate\":{},\"packets\":{},\"headerDerivedCodecFrames\":{},\"headerDerivedFramesPerChannel\":{},\"metadataSamplesPerChannel\":{},\"headerDerivedMinusMetadataSamplesPerChannel\":{},\"audioDecoded\":false}}",
                timeline.channels, timeline.rate, timeline.packets,
                timeline.header_derived_codec_frames, timeline.header_derived_frames_per_channel,
                timeline.metadata_samples_per_channel, header_derived_minus_metadata);
            return Ok(());
        }
        if first == "export-xma" {
            let input = args.next().map(PathBuf::from).ok_or("Missing input XWC")?;
            let name = args
                .next()
                .ok_or("Missing exact wave name")?
                .into_string()
                .map_err(|_| "Wave name must be UTF-8")?;
            let output = args
                .next()
                .map(PathBuf::from)
                .ok_or("Missing NEW output RIFF path")?;
            if args.next().is_some() {
                return Err("Too many arguments".into());
            }
            let mut file = File::open(&input).map_err(|e| format!("{}: {e}", input.display()))?;
            let bank = darkrecomp_xwc_audio::inspect(&mut file)?;
            let matches = bank
                .waves
                .iter()
                .filter(|w| w.name == name)
                .collect::<Vec<_>>();
            if matches.len() != 1 {
                return Err("Wave name must identify exactly one record".into());
            }
            let xma = darkrecomp_xwc_audio::xma::describe(&mut file, &bank, matches[0])?;
            let mut out = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&output)
                .map_err(|e| e.to_string())?;
            xma.export(&mut file, &mut out)?;
            out.sync_all().map_err(|e| e.to_string())?;
            println!("XMA2 RIFF exported: channels={} rate={} metadata_samples={} encoded_bytes={} (audio not decoded; delay/trimming not certified)",xma.channels,xma.rate,xma.samples,xma.data_bytes);
            return Ok(());
        }
        args = env::args_os().skip(1);
    }
    let input = args
        .next()
        .map(PathBuf::from)
        .ok_or("Usage: darkrecomp-xwc-audio INPUT.xwc [NEW_REPORT.json]; validate-xma INPUT.xwc; export-xma INPUT.xwc EXACT_WAVE_NAME NEW_OUTPUT.xma.wav; raw-timeline INPUT.xwc EXACT_WAVE_NAME")?;
    let output = args.next().map(PathBuf::from);
    if args.next().is_some() {
        return Err("Too many arguments".into());
    }
    let mut file = File::open(&input).map_err(|e| format!("{}: {e}", input.display()))?;
    let bank = darkrecomp_xwc_audio::inspect(&mut file)?;
    if let Some(output) = output {
        let mut report = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&output)
            .map_err(|e| e.to_string())?;
        report
            .write_all(bank.json().as_bytes())
            .map_err(|e| e.to_string())?;
        println!(
            "sections={} waves={} metadata_only=1",
            bank.sections.len(),
            bank.waves.len()
        );
    } else {
        print!("{}", bank.json());
    }
    Ok(())
}
fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}
