use darkrecomp_video_pack::{encode, media, parse, target_path, Container, Entry, MAX_VIDEO_BYTES};
use std::{fs::{self, File, OpenOptions}, io::{Read, Write}, path::{Component, Path, PathBuf}, process::{Command, Stdio}, time::{Duration, Instant}};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
fn absolute(path: &Path) -> Result<PathBuf> {
    if path.components().any(|c| c == Component::ParentDir) { return Err("parent path segments are unsupported".into()); }
    Ok(if path.is_absolute() { path.to_path_buf() } else { std::env::current_dir()?.join(path) })
}
fn ordinary(path: &Path) -> Result<()> {
    for ancestor in path.ancestors() {
        let meta = fs::symlink_metadata(ancestor)?;
        if meta.file_type().is_symlink() { return Err("symlink paths are unsupported".into()); }
        #[cfg(windows)] {
            use std::os::windows::fs::MetadataExt;
            if meta.file_attributes() & 0x400 != 0 { return Err("reparse paths are unsupported".into()); }
        }
        if ancestor != path && !meta.is_dir() { return Err("ancestor is not a directory".into()); }
    }
    Ok(())
}
fn source(path: &Path) -> Result<File> {
    ordinary(path)?;
    let mut options = OpenOptions::new(); options.read(true);
    #[cfg(windows)] {
        use std::os::windows::fs::OpenOptionsExt;
        options.share_mode(1).custom_flags(0x0020_0000);
    }
    let file = options.open(path)?;
    let meta = file.metadata()?;
    #[cfg(windows)] {
        use std::os::windows::fs::MetadataExt;
        if meta.file_attributes() & 0x400 != 0 { return Err("opened source is a reparse point".into()); }
    }
    if !meta.is_file() || meta.len() == 0 || meta.len() > MAX_VIDEO_BYTES { return Err("video must be an ordinary file of 1 byte through 2 GiB".into()); }
    Ok(file)
}
fn probe(path: &Path) -> Result<(media::Video, String)> {
    let program = std::env::var_os("DARK_FFPROBE").unwrap_or_else(|| "ffprobe".into());
    let mut command = Command::new(program);
    command.args(["-v", "error", "-protocol_whitelist", "file", "-probesize", "8388608", "-analyzeduration", "3000000", "-show_entries",
        "stream=codec_type,codec_name,pix_fmt,width,height,r_frame_rate,start_time,duration,sample_rate,channels:format=duration", "-of", "default"])
        .arg(path).stdout(Stdio::piped()).stderr(Stdio::null());
    #[cfg(windows)] {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x0800_0000);
    }
    let mut child = command.spawn().map_err(|e| format!("cannot start ffprobe (install FFmpeg or set DARK_FFPROBE): {e}"))?;
    let stdout = child.stdout.take().ok_or("ffprobe stdout unavailable")?;
    let reader = std::thread::spawn(move || {
        let mut bytes = Vec::new(); stdout.take(65537).read_to_end(&mut bytes).map(|_| bytes)
    });
    let deadline = Instant::now() + Duration::from_secs(30);
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) => (),
            Err(error) => {
                let _ = child.kill(); let _ = child.wait(); let _ = reader.join(); return Err(error.into());
            }
        }
        if Instant::now() >= deadline { let _ = child.kill(); let _ = child.wait(); let _ = reader.join(); return Err("ffprobe exceeded 30 seconds".into()); }
        std::thread::sleep(Duration::from_millis(25));
    };
    let bytes = reader.join().map_err(|_| "ffprobe output reader failed")??;
    if bytes.len() > 65536 || !status.success() { return Err("ffprobe rejected the media or exceeded its output bound".into()); }
    let text = String::from_utf8(bytes)?;
    Ok((media::parse_probe(&text)?, text))
}
fn container(file: &File, kind: Container) -> Result<()> {
    let mut reader = file.try_clone()?;
    let mut magic = [0; 16]; reader.read_exact(&mut magic)?;
    let valid = match kind {
        Container::Theora => &magic[..5] == b"OggS\0",
        Container::Asf => magic == [0x30,0x26,0xb2,0x75,0x8e,0x66,0xcf,0x11,0xa6,0xd9,0,0xaa,0,0x62,0xce,0x6c],
    };
    if !valid { return Err("media container differs from its target".into()); }
    Ok(())
}
fn create(args: &[std::ffi::OsString]) -> Result<()> {
    if args.len() < 4 || args.len() > 5 || (args.len() == 5 && args[4] != "--allow-aspect-change") { return Err("create GAME_DIR RELATIVE_VIDEO REPLACEMENT NEW_PACK_DIR [--allow-aspect-change]".into()); }
    let game = absolute(Path::new(&args[0]))?; ordinary(&game)?;
    let relative = args[1].to_str().ok_or("video target must be ASCII")?;
    let (key, kind) = target_path(relative)?;
    let original = game.join(relative.replace('\\', "/"));
    let replacement = absolute(Path::new(&args[2]))?;
    let output = absolute(Path::new(&args[3]))?;
    ordinary(output.parent().ok_or("pack parent missing")?)?;
    if output.try_exists()? { return Err("new pack directory already exists; nothing overwritten".into()); }
    let original_lock = source(&original)?; let mut replacement_lock = source(&replacement)?;
    container(&original_lock, kind)?; container(&replacement_lock, kind)?;
    let (stock, stock_probe) = probe(&original)?; let (authored, authored_probe) = probe(&replacement)?;
    media::compatible(&stock, &authored, args.len() == 5)?;
    let payload = match kind { Container::Theora => "video.ogg", Container::Asf => "video.wmv" };
    let length = replacement_lock.metadata()?.len();
    let manifest = encode(&[Entry { target: key, payload: payload.into(), bytes: length }])?;
    fs::create_dir(&output)?; ordinary(&output)?;
    // No cleanup/overwrite on failure: an incomplete output is retained. The
    // manifest is published last, after the complete payload has been synced.
    let mut file = OpenOptions::new().write(true).create_new(true).open(output.join(payload))?;
    use std::io::{Seek, SeekFrom};
    replacement_lock.seek(SeekFrom::Start(0))?;
    if std::io::copy(&mut replacement_lock, &mut file)? != length { return Err("replacement changed during import; incomplete pack retained".into()); }
    file.sync_all()?; drop(file);
    let mut report = OpenOptions::new().write(true).create_new(true).open(output.join("media.txt"))?;
    writeln!(report, "Original ffprobe\n{stock_probe}\nReplacement ffprobe\n{authored_probe}\nAspect change allowed: {}", args.len() == 5)?; report.sync_all()?;
    let mut file = OpenOptions::new().write(true).create_new(true).open(output.join("videos.tsv"))?;
    file.write_all(&manifest)?; file.sync_all()?;
    println!("Created {} video pack: {}x{} -> {}x{}, {:.6}s. Set DARK_VIDEO_MOD_DIR to {} before launch.", authored.codec, stock.width, stock.height, authored.width, authored.height, authored.duration, output.display());
    Ok(())
}
fn main() {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    let result = match args.first().and_then(|x| x.to_str()) {
        Some("create") => create(&args[1..]),
        Some("check") if args.len() == 2 => (|| -> Result<()> {
            let path = absolute(Path::new(&args[1]))?; ordinary(&path)?;
            let mut bytes = Vec::new(); File::open(path)?.take(65537).read_to_end(&mut bytes)?;
            let entries = parse(&bytes)?; println!("{} video entries; manifest syntax is valid.", entries.len()); Ok(())
        })(),
        _ => Err("usage: darkrecomp-video-pack create GAME_DIR RELATIVE_VIDEO REPLACEMENT NEW_PACK_DIR [--allow-aspect-change]\n       darkrecomp-video-pack check MANIFEST".into()),
    };
    if let Err(error) = result { eprintln!("Video pack: {error}"); std::process::exit(1); }
}
