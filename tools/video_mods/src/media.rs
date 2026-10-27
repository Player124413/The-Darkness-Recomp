use crate::Error;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Audio {
    pub codec: String,
    pub rate: u32,
    pub channels: u32,
}

#[derive(Clone, Debug)]
pub struct Video {
    pub codec: String,
    pub pixel_format: String,
    pub width: u32,
    pub height: u32,
    pub fps: (u32, u32),
    pub duration: f64,
    pub start: f64,
    pub audio: Vec<Audio>,
}

fn positive_ratio(value: &str) -> Result<(u32, u32), Error> {
    let (a, b) = value.split_once('/').ok_or(Error("invalid video frame rate"))?;
    let pair = (a.parse().map_err(|_| Error("invalid frame rate"))?, b.parse().map_err(|_| Error("invalid frame rate"))?);
    if pair.0 == 0 || pair.1 == 0 { return Err(Error("invalid video frame rate")); }
    Ok(pair)
}

/// The ffprobe default writer emits only selected, numeric/codec fields. No
/// titles or arbitrary metadata are requested or interpreted as instructions.
pub fn parse_probe(text: &str) -> Result<Video, Error> {
    use std::collections::BTreeMap;
    let mut streams = Vec::new();
    let mut current = None;
    let mut format_duration = None;
    let mut in_format = false;
    for line in text.lines() {
        match line {
            "[STREAM]" => {
                if current.is_some() || streams.len() >= 16 { return Err(Error("too many or nested media streams")); }
                current = Some(BTreeMap::new());
            }
            "[/STREAM]" => streams.push(current.take().ok_or(Error("invalid stream boundary"))?),
            "[FORMAT]" => in_format = true,
            "[/FORMAT]" => in_format = false,
            _ => {
                if let Some((key, value)) = line.split_once('=') {
                    if let Some(stream) = current.as_mut() {
                        if stream.insert(key.to_string(), value.to_string()).is_some() { return Err(Error("duplicate media field")); }
                    } else if in_format && key == "duration" {
                        format_duration = Some(value.to_string());
                    }
                }
            }
        }
    }
    if current.is_some() { return Err(Error("incomplete media stream")); }
    let get = |stream: &BTreeMap<String, String>, key: &str| stream.get(key).cloned().ok_or(Error("missing media field"));
    let number = |stream: &BTreeMap<String, String>, key: &str| -> Result<u32, Error> {
        get(stream, key)?.parse().map_err(|_| Error("invalid media number"))
    };
    let videos: Vec<_> = streams.iter().filter(|s| s.get("codec_type").map(String::as_str) == Some("video")).collect();
    if videos.len() != 1 { return Err(Error("media must contain exactly one video stream")); }
    if streams.iter().any(|s| !matches!(s.get("codec_type").map(String::as_str), Some("video" | "audio"))) {
        return Err(Error("unsupported additional media stream"));
    }
    let stream = videos[0];
    let duration = get(stream, "duration").ok().filter(|s| s != "N/A").or(format_duration)
        .ok_or(Error("missing media duration"))?.parse::<f64>().map_err(|_| Error("invalid media duration"))?;
    let start = get(stream, "start_time")?.parse::<f64>().map_err(|_| Error("invalid media start time"))?;
    if !duration.is_finite() || duration <= 0.0 || !start.is_finite() || start < 0.0 {
        return Err(Error("invalid media timeline"));
    }
    let audio = streams.iter().filter(|s| s.get("codec_type").map(String::as_str) == Some("audio"))
        .map(|s| Ok(Audio { codec: get(s, "codec_name")?, rate: number(s, "sample_rate")?, channels: number(s, "channels")? }))
        .collect::<Result<Vec<_>, Error>>()?;
    Ok(Video { codec: get(stream, "codec_name")?, pixel_format: get(stream, "pix_fmt")?,
        width: number(stream, "width")?, height: number(stream, "height")?, fps: positive_ratio(&get(stream, "r_frame_rate")?)?,
        duration, start, audio })
}

pub fn compatible(original: &Video, replacement: &Video, allow_aspect_change: bool) -> Result<(), Error> {
    if !matches!(original.codec.as_str(), "theora" | "wmv3") || replacement.codec != original.codec {
        return Err(Error("replacement must retain the original Theora or WMV3 codec"));
    }
    if original.pixel_format != "yuv420p" || replacement.pixel_format != "yuv420p" {
        return Err(Error("video must use 8-bit YUV 4:2:0"));
    }
    for value in [original, replacement] {
        if value.width == 0 || value.height == 0 || value.width > 2048 || value.height > 2048 || value.width % 2 != 0 || value.height % 2 != 0 {
            return Err(Error("native video dimensions must be even and at most 2048"));
        }
        if value.fps.0 == 0 || value.fps.1 == 0 || !value.duration.is_finite() || value.duration <= 0.0 || !value.start.is_finite() || value.start < 0.0 {
            return Err(Error("invalid video timeline"));
        }
    }
    if u64::from(original.fps.0) * u64::from(replacement.fps.1) != u64::from(replacement.fps.0) * u64::from(original.fps.1) {
        return Err(Error("replacement frame rate differs from the original"));
    }
    let frame = f64::from(original.fps.1) / f64::from(original.fps.0);
    if (original.duration - replacement.duration).abs() > frame + 0.000001 || (original.start - replacement.start).abs() > frame + 0.000001 {
        return Err(Error("replacement duration/start differs by more than one original frame"));
    }
    if original.audio != replacement.audio { return Err(Error("replacement audio stream layout differs from the original")); }
    if !allow_aspect_change && u64::from(original.width) * u64::from(replacement.height) != u64::from(replacement.width) * u64::from(original.height) {
        return Err(Error("replacement aspect differs; pad/crop it or explicitly use --allow-aspect-change"));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn video() -> Video { Video { codec: "theora".into(), pixel_format: "yuv420p".into(), width: 200, height: 152, fps: (30,1), duration: 4.0, start: 0.0, audio: vec![] } }
    #[test] fn accepts_higher_resolution_with_same_timeline() {
        let a = video(); let mut b = a.clone(); b.width *= 4; b.height *= 4; b.fps = (60,2);
        assert!(compatible(&a, &b, false).is_ok()); b.duration += 0.01; assert!(compatible(&a, &b, false).is_ok());
        b.duration += 0.1; assert!(compatible(&a, &b, false).is_err());
    }
    #[test] fn rejects_codec_timing_and_audio_mismatch() {
        let a = video();
        let mut b = a.clone(); b.codec = "wmv2".into(); assert!(compatible(&a, &b, false).is_err());
        b = a.clone(); b.fps = (30000,1001); assert!(compatible(&a, &b, false).is_err());
        b = a.clone(); b.audio.push(Audio { codec: "vorbis".into(), rate: 44100, channels: 2 }); assert!(compatible(&a, &b, false).is_err());
        b = a.clone(); b.start = f64::NAN; assert!(compatible(&a, &b, false).is_err());
        b = a.clone(); b.width = 4096; assert!(compatible(&a, &b, false).is_err());
    }
    #[test] fn aspect_change_requires_explicit_choice() {
        let a = video(); let mut b = a.clone(); b.width = 640; b.height = 480;
        assert!(compatible(&a, &b, false).is_err()); assert!(compatible(&a, &b, true).is_ok());
    }
    #[test] fn parses_writer_and_rejects_invalid_streams() {
        let input = "[STREAM]\ncodec_name=theora\ncodec_type=video\nwidth=200\nheight=152\npix_fmt=yuv420p\nr_frame_rate=30/1\nstart_time=0.000000\nduration=N/A\n[/STREAM]\n[FORMAT]\nduration=4.000000\n[/FORMAT]\n";
        assert_eq!(parse_probe(input).unwrap().duration, 4.0);
        assert!(parse_probe(&input.replace("duration=4.000000", "duration=NaN")).is_err());
        assert!(parse_probe(&input.replace("r_frame_rate=30/1", "r_frame_rate=0/0")).is_err());
        assert!(parse_probe(&(input.to_string() + input)).is_err());
        assert!(parse_probe(&input.replace("[/STREAM]", "")).is_err());
    }
}
