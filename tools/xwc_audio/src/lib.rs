//! Read-only inspection of the observed Xbox XWC2 footer layout.
//! Packed wave words remain untyped until their guest consumers are verified.
use std::collections::BTreeSet;
use std::io::{Read, Seek, SeekFrom};

pub mod xma;

type Result<T> = std::result::Result<T, String>;
const HEADER: u64 = 48;
const ENTRY: u64 = 48;
const MAX_ENTRIES: u64 = 4096;
const MAX_METADATA: u64 = 16 * 1024 * 1024;

#[derive(Debug)]
pub struct Section {
    pub name: String,
    pub directory_position: u64,
    pub words: [u32; 8],
}
impl Section {
    pub fn data_offset(&self) -> u64 {
        self.words[4] as u64
    }
    pub fn byte_count(&self) -> u64 {
        self.words[5] as u64
    }
    pub fn count(&self) -> u32 {
        self.words[6]
    }
}

#[derive(Debug)]
pub struct Wave {
    pub name: String,
    pub words: [u32; 6],
}

#[derive(Debug)]
pub struct Bank {
    pub file_bytes: u64,
    pub directory_offset: u64,
    pub sections: Vec<Section>,
    pub waves: Vec<Wave>,
}

fn read_at<R: Read + Seek>(reader: &mut R, at: u64, bytes: usize) -> Result<Vec<u8>> {
    reader
        .seek(SeekFrom::Start(at))
        .map_err(|e| e.to_string())?;
    let mut output = vec![0; bytes];
    reader.read_exact(&mut output).map_err(|e| e.to_string())?;
    Ok(output)
}

fn word(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap())
}

fn required<'a>(sections: &'a [Section], name: &str) -> Result<&'a Section> {
    sections
        .iter()
        .find(|s| s.name == name)
        .ok_or_else(|| format!("Missing {name}"))
}

fn metadata<R: Read + Seek>(reader: &mut R, section: &Section) -> Result<Vec<u8>> {
    if section.byte_count() > MAX_METADATA {
        return Err(format!("{} exceeds the metadata bound", section.name));
    }
    read_at(reader, section.data_offset(), section.byte_count() as usize)
}

pub fn inspect<R: Read + Seek>(reader: &mut R) -> Result<Bank> {
    let file_bytes = reader.seek(SeekFrom::End(0)).map_err(|e| e.to_string())?;
    if file_bytes < HEADER {
        return Err("Short MOS header".into());
    }
    let header = read_at(reader, 0, HEADER as usize)?;
    if &header[..16] != b"MOS DATAFILE2.0\0" {
        return Err("Not the observed MOS DATAFILE2.0 signature".into());
    }
    let directory_offset = u64::from_le_bytes(header[24..32].try_into().unwrap());
    if directory_offset < HEADER || directory_offset >= file_bytes {
        return Err("Directory is outside the file payload".into());
    }
    let directory_bytes = file_bytes - directory_offset;
    if directory_bytes % ENTRY != 0 || directory_bytes / ENTRY > MAX_ENTRIES {
        return Err("Unsupported XWC footer size".into());
    }
    let directory = read_at(reader, directory_offset, directory_bytes as usize)?;
    let mut sections = Vec::new();
    let mut names = BTreeSet::new();
    for (index, raw) in directory.chunks_exact(ENTRY as usize).enumerate() {
        let end = raw[..16].iter().position(|&v| v == 0).unwrap_or(16);
        if end == 0
            || raw[..end].iter().any(|&v| !(32..127).contains(&v))
            || raw[end..16].iter().any(|&v| v != 0)
        {
            return Err("Invalid section name".into());
        }
        let name = String::from_utf8(raw[..end].to_vec()).unwrap();
        if !names.insert(name.clone()) {
            return Err("Duplicate section name".into());
        }
        let words = std::array::from_fn(|i| word(raw, 16 + i * 4));
        let position = directory_offset + index as u64 * ENTRY;
        let next = words[2] as u64 | ((words[3] as u64) << 32);
        let expected = if index + 1 == directory.len() / ENTRY as usize {
            0
        } else {
            position + ENTRY
        };
        if next != expected {
            return Err("Unsupported section link chain".into());
        }
        let section = Section {
            name,
            directory_position: position,
            words,
        };
        let data_end = section
            .data_offset()
            .checked_add(section.byte_count())
            .ok_or("Section overflow")?;
        if section.data_offset() < HEADER || data_end > directory_offset {
            return Err(format!("{} data is outside the payload", section.name));
        }
        sections.push(section);
    }
    if sections.first().map(|s| s.name.as_str()) != Some("WAVEDIRECTORY2") {
        return Err("Not the observed XWC2 wave directory".into());
    }
    let wave_data = required(&sections, "WAVEDATA")?;
    let descriptions = required(&sections, "WAVEDATA2")?;
    if descriptions.byte_count() != descriptions.count() as u64 * 24
        || descriptions.count() != wave_data.count()
    {
        return Err("Inconsistent WAVEDATA/WAVEDATA2 records".into());
    }
    let strings = metadata(reader, required(&sections, "STRINGS")?)?;
    let descriptions = metadata(reader, descriptions)?;
    let mut waves = Vec::new();
    for record in descriptions.chunks_exact(24) {
        let words = std::array::from_fn(|i| word(record, i * 4));
        let start = words[2] as usize;
        let remainder = strings
            .get(start..)
            .ok_or("Wave name offset outside STRINGS")?;
        let end = remainder
            .iter()
            .position(|&v| v == 0)
            .ok_or("Unterminated wave name")?;
        let name =
            String::from_utf8(remainder[..end].to_vec()).map_err(|_| "Non-UTF-8 wave name")?;
        waves.push(Wave { name, words });
    }
    Ok(Bank {
        file_bytes,
        directory_offset,
        sections,
        waves,
    })
}

fn quote(text: &str) -> String {
    let mut output = String::from("\"");
    for ch in text.chars() {
        match ch {
            '"' => output.push_str("\\\""),
            '\\' => output.push_str("\\\\"),
            '\n' => output.push_str("\\n"),
            '\r' => output.push_str("\\r"),
            '\t' => output.push_str("\\t"),
            c if c < ' ' => output.push_str(&format!("\\u{:04x}", c as u32)),
            c => output.push(c),
        }
    }
    output.push('"');
    output
}

impl Bank {
    pub fn json(&self) -> String {
        let sections = self.sections.iter().map(|s| format!(
            "{{\"name\":{},\"directoryPosition\":{},\"dataOffset\":{},\"bytes\":{},\"count\":{},\"rawWords\":{:?}}}",
            quote(&s.name), s.directory_position, s.data_offset(), s.byte_count(), s.count(), s.words
        )).collect::<Vec<_>>().join(",\n");
        let waves = self
            .waves
            .iter()
            .map(|w| format!("{{\"name\":{},\"rawWords\":{:?}}}", quote(&w.name), w.words))
            .collect::<Vec<_>>()
            .join(",\n");
        format!("{{\"kind\":\"observed-xwc2-metadata\",\"fileBytes\":{},\"directoryOffset\":{},\"waveCount\":{},\"packedWaveFieldsVerified\":false,\"audioDecoded\":false,\"sections\":[{}],\"waves\":[{}]}}\n",
                self.file_bytes, self.directory_offset, self.waves.len(), sections, waves)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;
    fn fixture() -> Vec<u8> {
        let data = 48u32;
        let strings = 64u32;
        let ids = 71u32;
        let desc = 73u32;
        let directory = 97u32;
        let definitions = [
            ("WAVEDIRECTORY2", 48, 0, 0),
            ("WAVEDATA", data, 16, 1),
            ("STRINGS", strings, 7, 7),
            ("WAVEIDS", ids, 2, 1),
            ("WAVEDATA2", desc, 24, 1),
        ];
        let mut bytes = vec![0; directory as usize + definitions.len() * 48];
        bytes[..16].copy_from_slice(b"MOS DATAFILE2.0\0");
        bytes[24..32].copy_from_slice(&(directory as u64).to_le_bytes());
        bytes[strings as usize..strings as usize + 7].copy_from_slice(b"tvclip\0");
        for (i, (name, offset, size, count)) in definitions.iter().enumerate() {
            let at = directory as usize + i * 48;
            bytes[at..at + name.len()].copy_from_slice(name.as_bytes());
            if i + 1 < definitions.len() {
                bytes[at + 24..at + 32].copy_from_slice(&((at + 48) as u64).to_le_bytes());
            }
            for (field, value) in [(32, *offset), (36, *size), (40, *count)] {
                bytes[at + field..at + field + 4].copy_from_slice(&value.to_le_bytes());
            }
        }
        bytes
    }
    fn rejected(mut bytes: Vec<u8>, edit: impl FnOnce(&mut Vec<u8>)) {
        edit(&mut bytes);
        assert!(inspect(&mut Cursor::new(bytes)).is_err());
    }
    #[test]
    fn reads_known_sections_without_reading_encoded_payload() {
        let bank = inspect(&mut Cursor::new(fixture())).unwrap();
        assert_eq!(bank.waves[0].name, "tvclip");
        assert_eq!(bank.waves.len(), 1);
        assert_eq!(bank.sections.len(), 5);
        assert!(bank.json().contains("\"audioDecoded\":false"));
    }
    #[test]
    fn rejects_short_header() {
        rejected(vec![0; 12], |_| {});
    }
    #[test]
    fn rejects_signature() {
        rejected(fixture(), |b| b[0] = 0);
    }
    #[test]
    fn rejects_directory_inside_header() {
        rejected(fixture(), |b| {
            b[24..32].copy_from_slice(&16u64.to_le_bytes())
        });
    }
    #[test]
    fn rejects_directory_past_eof() {
        rejected(fixture(), |b| {
            b[24..32].copy_from_slice(&10000u64.to_le_bytes())
        });
    }
    #[test]
    fn rejects_partial_footer() {
        rejected(fixture(), |b| {
            b.push(0);
        });
    }
    #[test]
    fn rejects_link_cycle() {
        rejected(fixture(), |b| {
            b[121..129].copy_from_slice(&97u64.to_le_bytes())
        });
    }
    #[test]
    fn rejects_payload_in_footer() {
        rejected(fixture(), |b| {
            b[177..181].copy_from_slice(&100u32.to_le_bytes())
        });
    }
    #[test]
    fn rejects_bad_name_offset() {
        rejected(fixture(), |b| {
            b[81..85].copy_from_slice(&999u32.to_le_bytes())
        });
    }
    #[test]
    fn rejects_unterminated_name() {
        rejected(fixture(), |b| b[70] = b'x');
    }
    #[test]
    fn rejects_inconsistent_wave_count() {
        rejected(fixture(), |b| {
            b[329..333].copy_from_slice(&2u32.to_le_bytes())
        });
    }
    #[test]
    fn rejects_non_xwc_mos() {
        rejected(fixture(), |b| b[97] = b'X');
    }
    #[test]
    fn escapes_json_names() {
        assert_eq!(quote("a\"\\\n"), "\"a\\\"\\\\\\n\"");
    }
}
