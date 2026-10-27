//! Interchange contract for optional read-only video replacement packs.
use std::{collections::HashSet, fmt};
pub mod media;

pub const HEADER: &str = "DARKRECOMP_VIDEO_V1";
pub const MAX_MANIFEST_BYTES: usize = 64 * 1024;
pub const MAX_ENTRIES: usize = 128;
pub const MAX_VIDEO_BYTES: u64 = 2 * 1024 * 1024 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(pub &'static str);
impl fmt::Display for Error {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result { out.write_str(self.0) }
}
impl std::error::Error for Error {}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Container { Asf, Theora }

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    /// Lowercase, slash-separated guest path under Content/Videos.
    pub target: String,
    /// A plain basename in the pack directory, with matching container suffix.
    pub payload: String,
    pub bytes: u64,
}

fn component(value: &str) -> bool {
    if value.is_empty() || value.len() > 128 || value == "." || value == ".." || value.ends_with('.') {
        return false;
    }
    if !value.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-' | b'.')) {
        return false;
    }
    let stem = value.split('.').next().unwrap().to_ascii_lowercase();
    if matches!(stem.as_str(), "con" | "prn" | "aux" | "nul" | "conin$" | "conout$") {
        return false;
    }
    if stem.len() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
        matches!(stem.as_bytes()[3], b'1'..=b'9') { return false; }
    true
}

pub fn target_path(value: &str) -> Result<(String, Container), Error> {
    if value.len() > 240 || !value.is_ascii() { return Err(Error("invalid target length or encoding")); }
    let path = value.replace('\\', "/").to_ascii_lowercase();
    let parts: Vec<_> = path.split('/').collect();
    if parts.len() < 4 || parts.iter().any(|p| !component(p)) || parts[0] != "content" || parts[1] != "videos" {
        return Err(Error("target must be a relative Content/Videos path"));
    }
    let kind = match parts[2] {
        "wmv" if path.ends_with(".wmv") => Container::Asf,
        "theora" if path.ends_with(".ogg") => Container::Theora,
        _ => return Err(Error("target codec directory and suffix must agree")),
    };
    Ok((path, kind))
}

pub fn payload_name(value: &str, container: Container) -> Result<(), Error> {
    if !component(value) || value.contains(['/', '\\']) { return Err(Error("payload must be a plain basename")); }
    let name = value.to_ascii_lowercase();
    let suffix = match container { Container::Asf => ".wmv", Container::Theora => ".ogg" };
    if !name.ends_with(suffix) { return Err(Error("payload suffix differs from target container")); }
    Ok(())
}

/// Syntax and declared size only. Filesystem identity and media validity are
/// separate obligations for the importer/runtime, including reparse checks.
pub fn parse(bytes: &[u8]) -> Result<Vec<Entry>, Error> {
    if bytes.len() > MAX_MANIFEST_BYTES { return Err(Error("manifest exceeds 64 KiB")); }
    let text = std::str::from_utf8(bytes).map_err(|_| Error("manifest must be UTF-8"))?;
    if !text.ends_with('\n') || text.contains('\0') { return Err(Error("manifest requires complete non-NUL lines")); }
    let mut lines = text.lines();
    if lines.next() != Some(HEADER) { return Err(Error("unsupported manifest header")); }
    let mut seen = HashSet::new();
    let mut entries = Vec::new();
    for line in lines {
        if entries.len() >= MAX_ENTRIES { return Err(Error("too many video entries")); }
        let mut fields = line.split('\t');
        let target = fields.next().ok_or(Error("missing target"))?;
        let payload = fields.next().ok_or(Error("missing payload"))?;
        let size = fields.next().ok_or(Error("missing size"))?;
        if fields.next().is_some() || size.is_empty() || !size.bytes().all(|b| b.is_ascii_digit()) {
            return Err(Error("expected target, payload and decimal size"));
        }
        let (target, kind) = target_path(target)?;
        payload_name(payload, kind)?;
        let length = size.parse::<u64>().map_err(|_| Error("size overflow"))?;
        if length == 0 || length > MAX_VIDEO_BYTES { return Err(Error("video size must be 1 byte through 2 GiB")); }
        if !seen.insert(target.clone()) { return Err(Error("duplicate target")); }
        entries.push(Entry { target, payload: payload.to_string(), bytes: length });
    }
    if entries.is_empty() { return Err(Error("empty video pack")); }
    Ok(entries)
}

pub fn encode(entries: &[Entry]) -> Result<Vec<u8>, Error> {
    if entries.is_empty() || entries.len() > MAX_ENTRIES { return Err(Error("invalid entry count")); }
    let mut output = format!("{HEADER}\n");
    for entry in entries {
        let (target, container) = target_path(&entry.target)?;
        payload_name(&entry.payload, container)?;
        output.push_str(&format!("{}\t{}\t{}\n", target, entry.payload, entry.bytes));
    }
    let normalized = parse(output.as_bytes())?;
    let mut canonical = format!("{HEADER}\n");
    for entry in normalized { canonical.push_str(&format!("{}\t{}\t{}\n", entry.target, entry.payload, entry.bytes)); }
    Ok(canonical.into_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn record(target: &str, payload: &str, size: &str) -> Vec<u8> {
        format!("{HEADER}\n{target}\t{payload}\t{size}\n").into_bytes()
    }
    #[test] fn case_and_separator_round_trip() {
        let input = record("Content\\Videos\\Theora\\DarknessTV\\Xenon\\ToonTV\\Popeye.ogg", "hd.ogg", "4096");
        let entries = parse(&input).unwrap();
        assert_eq!(entries[0].target, "content/videos/theora/darknesstv/xenon/toontv/popeye.ogg");
        assert_eq!(parse(&encode(&entries).unwrap()).unwrap(), entries);
        let crlf = String::from_utf8(input).unwrap().replace('\n', "\r\n");
        assert!(parse(crlf.as_bytes()).is_ok());
    }
    #[test] fn rejects_escape_and_device_names() {
        for target in ["C:/Content/Videos/Wmv/a.wmv", "/Content/Videos/Wmv/a.wmv", "Content/Videos/Wmv/../a.wmv", "Content/Videos/Wmv/con.wmv", "Content/Videos/Wmv/LPT1.wmv", "Content/Videos/Wmv/a..wmv.", "Content/Videos/Wmv/a.wmv:stream", "Content/Videos/Wmv//a.wmv"] {
            assert!(target_path(target).is_err(), "{target}");
        }
        for payload in ["../a.ogg", "folder/a.ogg", "folder\\a.ogg", "NUL.ogg", "a.ogg:stream", "a.ogg "] {
            assert!(payload_name(payload, Container::Theora).is_err(), "{payload}");
        }
    }
    #[test] fn rejects_codec_path_mismatches() {
        for target in ["Content/Videos/Wmv/a.ogg", "Content/Videos/Theora/a.wmv", "Content/Gui/a.wmv", "Content/Videos/a.wmv", "Content/Videos/Other/a.ogg"] { assert!(target_path(target).is_err()); }
        assert!(parse(&record("Content/Videos/Wmv/a.wmv", "a.ogg", "1")).is_err());
    }
    #[test] fn rejects_duplicate_normalized_targets() {
        let mut input = record("Content/Videos/Wmv/a.wmv", "a.wmv", "1");
        input.extend_from_slice(b"CONTENT\\VIDEOS\\WMV\\A.WMV\tb.wmv\t2\n");
        assert!(parse(&input).is_err());
    }
    #[test] fn rejects_partial_extra_and_control_fields() {
        for input in [format!("{HEADER}\n"), format!("{HEADER}\na\tb\t1"), format!("{HEADER}\ncontent/videos/wmv/a.wmv\ta.wmv\t1\textra\n"), format!("{HEADER}\ncontent/videos/wmv/a.wmv\ta\0.wmv\t1\n"), format!("{HEADER}\n\n")] { assert!(parse(input.as_bytes()).is_err()); }
    }
    #[test] fn bounds_sizes_and_counts() {
        for size in ["0", "+1", "-1", " 1", "2147483649", "18446744073709551616"] { assert!(parse(&record("Content/Videos/Wmv/a.wmv", "a.wmv", size)).is_err()); }
        assert!(parse(&record("Content/Videos/Wmv/a.wmv", "a.wmv", "2147483648")).is_ok());
        let mut input = format!("{HEADER}\n");
        for n in 0..=MAX_ENTRIES { input.push_str(&format!("content/videos/wmv/{n}.wmv\ta.wmv\t1\n")); }
        assert!(parse(input.as_bytes()).is_err());
        assert!(parse(&vec![b'a'; MAX_MANIFEST_BYTES + 1]).is_err());
    }
}
