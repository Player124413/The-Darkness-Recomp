use darkrecomp_texture_tool::{parse_dds, Image, MAX_FILE_BYTES};
use std::{
    env,
    error::Error,
    fs::{File, OpenOptions},
    io::{Read, Write},
    path::Path,
};

fn read_bounded(path: &Path, limit: usize) -> Result<Vec<u8>, Box<dyn Error>> {
    let file = File::open(path)?;
    if file.metadata()?.len() > limit as u64 {
        return Err("input exceeds size limit".into());
    }
    let mut bytes = Vec::new();
    file.take(limit as u64 + 1).read_to_end(&mut bytes)?;
    if bytes.len() > limit {
        return Err("input grew beyond size limit".into());
    }
    Ok(bytes)
}
fn main() -> Result<(), Box<dyn Error>> {
    let args: Vec<_> = env::args_os().skip(1).collect();
    match args.first().and_then(|x| x.to_str()) {
        Some("inspect") if args.len() == 2 => {
            let image = parse_dds(&read_bounded(Path::new(&args[1]), MAX_FILE_BYTES)?)?;
            println!("RGBA8_UNORM {}x{} levels={} bytes={}", image.width, image.height, image.levels.len(), image.bytes());
        }
        Some("from-rgba") if args.len() == 5 => {
            let width: u32 = args[1].to_str().ok_or("non-UTF8 width")?.parse()?;
            let height: u32 = args[2].to_str().ok_or("non-UTF8 height")?.parse()?;
            if width == 0 || height == 0 || width > 4096 || height > 4096 { return Err("dimensions must be 1..4096".into()); }
            let expected = width as usize * height as usize * 4;
            let pixels = read_bounded(Path::new(&args[3]), expected)?;
            let image = Image::from_rgba(width, height, pixels)?;
            let data = image.to_dds()?;
            // Exclusive creation preserves any existing asset. On an I/O error
            // a partial new output is retained for inspection, never deleted.
            let mut output = OpenOptions::new().write(true).create_new(true).open(&args[4])?;
            output.write_all(&data)?; output.sync_all()?;
            println!("Wrote new DDS: {}x{} levels={} bytes={}", width, height, image.levels.len(), data.len());
        }
        _ => return Err("Usage: darkrecomp-texture-tool inspect <dds> | from-rgba <width> <height> <raw-rgba> <new-dds>".into()),
    }
    Ok(())
}
