//! Darkness version-3 XMA wrapper -> RIFF with the original XMA2 chunk.
//! No sample conversion, delay correction, or guessed codec flags are applied.
//! Layout references: vgmstream src/meta/xwc.c and FFmpeg wav_parse_xma2_tag.
use super::{read_at, required, word, Bank, Result, Wave};
use std::io::{Read, Seek, SeekFrom, Write};

#[derive(Debug)]
pub struct Xma {
    pub channels: u32,
    pub rate: u32,
    pub samples: u32,
    pub data_offset: u64,
    pub data_bytes: u32,
    pub format: Vec<u8>,
}

fn region(start: u64, length: u64, lower: u64, upper: u64) -> Result<()> {
    if start < lower || start.checked_add(length).ok_or("Range overflow")? > upper {
        return Err("Wave data range outside its container".into());
    }
    Ok(())
}

pub fn describe<R: Read + Seek>(reader: &mut R, bank: &Bank, wave: &Wave) -> Result<Xma> {
    let data = required(&bank.sections, "WAVEDATA")?;
    let start = u64::from(wave.words[3]);
    let bytes = u64::from(wave.words[4]);
    region(
        start,
        bytes,
        data.data_offset(),
        data.data_offset() + data.byte_count(),
    )?;
    if bytes < 44 {
        return Err("Short XMA wrapper".into());
    }
    let end = start + bytes;
    let header = read_at(reader, start, 44)?;
    if header[..8] != [0, 3, 0, 0, 0, 0x90, 0, 0] || &header[28..32] != b"XMA\0" {
        return Err("Only Darkness version-3 XMA wrappers are supported".into());
    }
    if u64::from(word(&header, 8)) + 28 != bytes {
        return Err("XMA wrapper size disagrees with wave record".into());
    }
    let channels = word(&header, 12);
    let samples = word(&header, 32);
    let packed = word(&header, 36);
    let rate = packed >> 14;
    if !(1..=2).contains(&channels)
        || channels != (packed >> 6) & 15
        || rate == 0
        || samples == 0
        || samples != wave.words[0] & 0x7fff_ffff
        || packed != wave.words[1]
    {
        return Err("XMA wrapper metadata disagrees with wave record".into());
    }
    let seek_bytes = u64::from(word(&header, 40));
    let size_offset = start + 44 + seek_bytes;
    region(size_offset, 4, start, end)?;
    let format_bytes = word(&read_at(reader, size_offset, 4)?, 0);
    // These two original XMA2 versions have a bounded per-stream structure.
    if !(36..=72).contains(&format_bytes) {
        return Err("Unsupported original XMA2 format size".into());
    }
    let format_offset = size_offset + 4;
    region(format_offset, u64::from(format_bytes) + 4, start, end)?;
    let format = read_at(reader, format_offset, format_bytes as usize)?;
    let channels_offset = match format[0] {
        3 => 32,
        4 => 40,
        _ => return Err("Unsupported XMA2 version".into()),
    };
    let streams = usize::from(format[1]);
    if streams != 1 || format.len() != channels_offset + streams * 4 {
        return Err("Only observed one-stream mono/stereo XMA2 is supported".into());
    }
    let format_rate = u32::from_be_bytes(format[12..16].try_into().unwrap());
    if format_rate != rate || u32::from(format[channels_offset]) != channels {
        return Err("XMA2 format disagrees with wrapper rate/channels".into());
    }
    let length_offset = format_offset + u64::from(format_bytes);
    let data_bytes = word(&read_at(reader, length_offset, 4)?, 0);
    let relative_data = (length_offset + 4 - start + 2047) & !2047;
    let data_offset = start + relative_data;
    if data_bytes == 0 || data_bytes % 2048 != 0 {
        return Err("XMA payload must contain complete 2048-byte packets".into());
    }
    region(data_offset, u64::from(data_bytes), start, end)?;
    Ok(Xma {
        channels,
        rate,
        samples,
        data_offset,
        data_bytes,
        format,
    })
}

/// Packet-header arithmetic over a validated payload; no audio is decoded.
#[derive(Debug, PartialEq, Eq)]
pub struct HeaderDerivedTimeline {
    pub channels: u32,
    pub rate: u32,
    pub packets: u64,
    pub header_derived_codec_frames: u64,
    pub header_derived_frames_per_channel: u64,
    pub metadata_samples_per_channel: u32,
}

/// Restrict this diagnostic to the observed TV profile. Export remains broader.
pub fn header_derived_timeline<R: Read + Seek>(
    reader: &mut R,
    bank: &Bank,
    wave: &Wave,
) -> Result<HeaderDerivedTimeline> {
    let xma = describe(reader, bank, wave)?;
    if xma.format[0] != 4
        || xma.format[1] != 1
        || !(1..=2).contains(&xma.channels)
        || !matches!(xma.rate, 44100 | 48000)
    {
        return Err(
            "Header-derived timeline requires XMA2 version 4, one stream, mono/stereo, 44100 or 48000 Hz"
                .into(),
        );
    }
    reader
        .seek(SeekFrom::Start(xma.data_offset))
        .map_err(|e| e.to_string())?;
    let packets = u64::from(xma.data_bytes / 2048);
    let mut packet = [0u8; 2048];
    let mut header_derived_codec_frames = 0u64;
    for index in 0..packets {
        // Read the entire packet so even a truncated final body is rejected.
        reader
            .read_exact(&mut packet)
            .map_err(|e| format!("Cannot read complete XMA packet {index}: {e}"))?;
        let header = u32::from_be_bytes(packet[..4].try_into().unwrap());
        if header & 0xff != 0 {
            return Err(format!(
                "Header-derived timeline does not support nonzero packet skip at packet {index}"
            ));
        }
        header_derived_codec_frames = header_derived_codec_frames
            .checked_add(u64::from(header >> 26))
            .ok_or("Header-derived codec frame count overflow")?;
    }
    let header_derived_frames_per_channel = header_derived_codec_frames
        .checked_mul(512)
        .ok_or("Header-derived frame count overflow")?;
    Ok(HeaderDerivedTimeline {
        channels: xma.channels,
        rate: xma.rate,
        packets,
        header_derived_codec_frames,
        header_derived_frames_per_channel,
        metadata_samples_per_channel: xma.samples,
    })
}
impl Xma {
    pub fn riff_header(&self) -> Result<Vec<u8>> {
        // RIFF/WAVE + XMA2(original codec format) + fact(sample count) + data.
        let riff_bytes = 32u64 + self.format.len() as u64 + u64::from(self.data_bytes);
        let riff_bytes = u32::try_from(riff_bytes).map_err(|_| "RIFF exceeds 32-bit size")?;
        let mut out = Vec::with_capacity(40 + self.format.len());
        out.extend_from_slice(b"RIFF");
        out.extend_from_slice(&riff_bytes.to_le_bytes());
        out.extend_from_slice(b"WAVEXMA2");
        out.extend_from_slice(&(self.format.len() as u32).to_le_bytes());
        out.extend_from_slice(&self.format);
        out.extend_from_slice(b"fact\x04\0\0\0");
        out.extend_from_slice(&self.samples.to_le_bytes());
        out.extend_from_slice(b"data");
        out.extend_from_slice(&self.data_bytes.to_le_bytes());
        Ok(out)
    }

    pub fn export<R: Read + Seek, W: Write>(&self, reader: &mut R, out: &mut W) -> Result<()> {
        out.write_all(&self.riff_header()?)
            .map_err(|e| e.to_string())?;
        reader
            .seek(SeekFrom::Start(self.data_offset))
            .map_err(|e| e.to_string())?;
        let mut remaining = u64::from(self.data_bytes);
        let mut buffer = [0u8; 65536];
        while remaining != 0 {
            let count = remaining.min(buffer.len() as u64) as usize;
            reader
                .read_exact(&mut buffer[..count])
                .map_err(|e| e.to_string())?;
            out.write_all(&buffer[..count]).map_err(|e| e.to_string())?;
            remaining -= count as u64;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::super::Section;
    use super::*;
    use std::io::Cursor;

    fn fixture() -> (Vec<u8>, Bank, Wave) {
        let start = 48;
        let mut bytes = vec![0u8; start + 4096];
        let h = &mut bytes[start..];
        h[..8].copy_from_slice(&[0, 3, 0, 0, 0, 0x90, 0, 0]);
        let samples = 1000u32;
        let packed = (44100u32 << 14) | (2 << 10) | (1 << 6) | 2;
        for (at, value) in [
            (8, 4096 - 28),
            (12, 1),
            (32, samples),
            (36, packed),
            (40, 0),
            (44, 44),
            (92, 2048),
        ] {
            h[at..at + 4].copy_from_slice(&value.to_le_bytes());
        }
        h[28..32].copy_from_slice(b"XMA\0");
        h[48] = 4;
        h[49] = 1;
        h[60..64].copy_from_slice(&44100u32.to_be_bytes());
        h[88] = 1;
        h[2048..].fill(0x55);
        let bank = Bank {
            file_bytes: bytes.len() as u64,
            directory_offset: bytes.len() as u64,
            sections: vec![Section {
                name: "WAVEDATA".into(),
                directory_position: 0,
                words: [0, 0, 0, 0, 48, 4096, 1, 0],
            }],
            waves: vec![],
        };
        let wave = Wave {
            name: "synthetic".into(),
            words: [samples | 0x8000_0000, packed, 0, 48, 4096, 0],
        };
        (bytes, bank, wave)
    }

    #[test]
    fn exports_original_codec_format_and_exact_packet_bytes() {
        let (bytes, bank, wave) = fixture();
        let mut input = Cursor::new(&bytes);
        let xma = describe(&mut input, &bank, &wave).unwrap();
        let mut out = Vec::new();
        xma.export(&mut input, &mut out).unwrap();
        assert_eq!(&out[12..16], b"XMA2");
        assert_eq!(&out[20..64], &bytes[96..140]);
        assert_eq!(word(&out, 4) as usize + 8, out.len());
        assert_eq!(&out[84..], &bytes[2096..]);
        assert_eq!((xma.channels, xma.rate, xma.samples), (1, 44100, 1000));
    }

    #[test]
    fn rejects_bad_ranges_and_inconsistent_metadata() {
        for (offset, value) in [
            (8, 1),
            (12, 2),
            (32, 2000),
            (40, u32::MAX),
            (44, u32::MAX),
            (92, 2049),
        ] {
            let (mut bytes, bank, wave) = fixture();
            bytes[48 + offset..52 + offset].copy_from_slice(&value.to_le_bytes());
            assert!(
                describe(&mut Cursor::new(bytes), &bank, &wave).is_err(),
                "offset {offset}"
            );
        }
        let (bytes, bank, mut wave) = fixture();
        wave.words[3] = 0;
        assert!(describe(&mut Cursor::new(bytes), &bank, &wave).is_err());
    }

    #[test]
    fn rejects_codec_version_rate_and_channel_changes() {
        for offset in [28usize, 48, 49, 60, 88] {
            let (mut bytes, bank, wave) = fixture();
            bytes[48 + offset] ^= 1;
            assert!(
                describe(&mut Cursor::new(bytes), &bank, &wave).is_err(),
                "offset {offset}"
            );
        }
    }

    #[test]
    fn short_input_cannot_create_plausible_export() {
        let (mut bytes, bank, wave) = fixture();
        bytes.truncate(60);
        assert!(describe(&mut Cursor::new(bytes), &bank, &wave).is_err());
    }

    fn header_derived_fixture(headers: &[u32], rate: u32, channels: u32) -> (Vec<u8>, Bank, Wave) {
        let (mut bytes, mut bank, mut wave) = fixture();
        let wrapper_bytes = (headers.len() as u32 + 1) * 2048;
        bytes.resize(48 + wrapper_bytes as usize, 0xa5);
        bank.file_bytes = bytes.len() as u64;
        bank.directory_offset = bank.file_bytes;
        bank.sections[0].words[5] = wrapper_bytes;
        wave.words[4] = wrapper_bytes;
        wave.words[1] = (rate << 14) | (2 << 10) | (channels << 6) | 2;
        for (offset, value) in [
            (8, wrapper_bytes - 28),
            (12, channels),
            (36, wave.words[1]),
            (92, headers.len() as u32 * 2048),
        ] {
            bytes[48 + offset..52 + offset].copy_from_slice(&value.to_le_bytes());
        }
        bytes[108..112].copy_from_slice(&rate.to_be_bytes());
        bytes[136] = channels as u8;
        for (index, header) in headers.iter().enumerate() {
            let at = 48 + 2048 + index * 2048;
            bytes[at..at + 4].copy_from_slice(&header.to_be_bytes());
        }
        (bytes, bank, wave)
    }

    #[test]
    fn header_derived_timeline_aggregates_big_endian_headers_in_exact_payload() {
        let headers = [1 << 26, (17 << 26) | 0x0034_5600, 63 << 26];
        for (rate, channels) in [(44100, 1), (44100, 2), (48000, 1), (48000, 2)] {
            let (mut bytes, bank, wave) = header_derived_fixture(&headers, rate, channels);
            // Bytes outside the selected payload must never contribute or reject it.
            bytes.extend_from_slice(&[0xff; 2048]);
            let timeline = header_derived_timeline(&mut Cursor::new(bytes), &bank, &wave).unwrap();
            assert_eq!(
                timeline,
                HeaderDerivedTimeline {
                    channels,
                    rate,
                    packets: 3,
                    header_derived_codec_frames: 81,
                    header_derived_frames_per_channel: 41472,
                    metadata_samples_per_channel: 1000,
                }
            );
        }
    }

    #[test]
    fn header_derived_timeline_rejects_nonzero_skip_in_later_packet() {
        for skip in [1, 255] {
            let headers = [3 << 26, (17 << 26) | skip];
            let (bytes, bank, wave) = header_derived_fixture(&headers, 44100, 1);
            let error = header_derived_timeline(&mut Cursor::new(bytes), &bank, &wave).unwrap_err();
            assert!(error.contains("nonzero packet skip at packet 1"));
        }
    }

    #[test]
    fn header_derived_timeline_rejects_truncated_headers_bodies_and_partial_packets() {
        let (bytes, bank, wave) = header_derived_fixture(&[3 << 26, 17 << 26], 44100, 1);
        for length in [48 + 4096 + 1, 48 + 4096 + 3, bytes.len() - 1] {
            let error = header_derived_timeline(&mut Cursor::new(&bytes[..length]), &bank, &wave)
                .unwrap_err();
            assert!(error.contains("Cannot read complete XMA packet 1"));
        }
        let mut partial = bytes;
        partial[140..144].copy_from_slice(&2049u32.to_le_bytes());
        let error = header_derived_timeline(&mut Cursor::new(partial), &bank, &wave).unwrap_err();
        assert!(error.contains("complete 2048-byte packets"));
    }

    #[test]
    fn header_derived_timeline_rejects_unsupported_formats_without_narrowing_export() {
        for rate in [22050, 96000] {
            let (bytes, bank, wave) = header_derived_fixture(&[3 << 26], rate, 1);
            let mut input = Cursor::new(bytes);
            let xma = describe(&mut input, &bank, &wave).unwrap();
            xma.export(&mut input, &mut Vec::new()).unwrap();
            let error = header_derived_timeline(&mut input, &bank, &wave).unwrap_err();
            assert!(error.contains("Header-derived timeline requires"));
        }

        // A valid original version-3 format still exports, but has no timing claim.
        let (mut version3, bank, wave) = header_derived_fixture(&[3 << 26], 44100, 1);
        version3[92..96].copy_from_slice(&36u32.to_le_bytes());
        version3[96] = 3;
        version3[128] = 1;
        version3[132..136].copy_from_slice(&2048u32.to_le_bytes());
        let mut input = Cursor::new(version3);
        let xma = describe(&mut input, &bank, &wave).unwrap();
        xma.export(&mut input, &mut Vec::new()).unwrap();
        let error = header_derived_timeline(&mut input, &bank, &wave).unwrap_err();
        assert!(error.contains("Header-derived timeline requires"));

        for (offset, value) in [(96, 5), (97, 2), (136, 3)] {
            let (mut bytes, bank, wave) = header_derived_fixture(&[3 << 26], 44100, 1);
            bytes[offset] = value;
            assert!(header_derived_timeline(&mut Cursor::new(bytes), &bank, &wave).is_err());
        }
    }
}
