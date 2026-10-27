//! A deliberately narrow DDS interchange contract for an optional texture mod.
//! This draft is not yet connected to the game. Pixels are owned RGBA8 UNORM.
use std::fmt;

pub const MAX_DIMENSION: u32 = 4096;
pub const MAX_BYTES: usize = 128 * 1024 * 1024;
pub const MAX_FILE_BYTES: usize = MAX_BYTES + 148;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(pub &'static str);
impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.0)
    }
}
impl std::error::Error for Error {}
type Result<T> = std::result::Result<T, Error>;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Image {
    pub width: u32,
    pub height: u32,
    /// Base followed by every floor-halved level through 1x1, tightly packed.
    pub levels: Vec<Vec<u8>>,
}

fn layout(width: u32, height: u32) -> Result<Vec<usize>> {
    if width == 0 || height == 0 || width > MAX_DIMENSION || height > MAX_DIMENSION {
        return Err(Error("dimensions must be 1..4096"));
    }
    let (mut w, mut h, mut total) = (width, height, 0usize);
    let mut sizes = Vec::new();
    loop {
        let bytes = (w as usize)
            .checked_mul(h as usize)
            .and_then(|n| n.checked_mul(4))
            .ok_or(Error("texture size overflow"))?;
        total = total
            .checked_add(bytes)
            .ok_or(Error("texture size overflow"))?;
        if total > MAX_BYTES {
            return Err(Error("mip chain exceeds 128 MiB"));
        }
        sizes.push(bytes);
        if w == 1 && h == 1 {
            break;
        }
        w = (w / 2).max(1);
        h = (h / 2).max(1);
    }
    Ok(sizes)
}

fn word(bytes: &[u8], at: usize) -> Result<u32> {
    let raw: [u8; 4] = bytes
        .get(at..at + 4)
        .ok_or(Error("truncated DDS header"))?
        .try_into()
        .map_err(|_| Error("truncated DDS header"))?;
    Ok(u32::from_le_bytes(raw))
}

impl Image {
    pub fn validate(&self) -> Result<()> {
        let sizes = layout(self.width, self.height)?;
        if self.levels.len() != sizes.len()
            || self.levels.iter().zip(sizes).any(|(p, n)| p.len() != n)
        {
            return Err(Error(
                "expected one complete, tightly packed RGBA8 mip chain",
            ));
        }
        Ok(())
    }
    pub fn bytes(&self) -> usize {
        self.levels.iter().map(Vec::len).sum()
    }

    /// Generate a simple per-channel box chain with no gamma or alpha transform.
    /// Authors should supply authored DDS mips when their material needs one.
    pub fn from_rgba(width: u32, height: u32, pixels: Vec<u8>) -> Result<Self> {
        let sizes = layout(width, height)?;
        if pixels.len() != sizes[0] {
            return Err(Error("RGBA input length does not match dimensions"));
        }
        let mut levels = vec![pixels];
        let (mut sw, mut sh) = (width, height);
        for &size in &sizes[1..] {
            let (dw, dh) = ((sw / 2).max(1), (sh / 2).max(1));
            let source = levels.last().ok_or(Error("missing mip source"))?;
            let mut target = vec![0; size];
            for y in 0..dh {
                for x in 0..dw {
                    let (x0, x1) = (x * sw / dw, (x + 1) * sw / dw);
                    let (y0, y1) = (y * sh / dh, (y + 1) * sh / dh);
                    let count = (x1 - x0) * (y1 - y0);
                    let mut sums = [0u32; 4];
                    for sy in y0..y1 {
                        for sx in x0..x1 {
                            let at = (sy as usize * sw as usize + sx as usize) * 4;
                            for c in 0..4 {
                                sums[c] += u32::from(source[at + c]);
                            }
                        }
                    }
                    let at = (y as usize * dw as usize + x as usize) * 4;
                    for c in 0..4 {
                        target[at + c] = ((sums[c] + count / 2) / count) as u8;
                    }
                }
            }
            levels.push(target);
            sw = dw;
            sh = dh;
        }
        Ok(Self {
            width,
            height,
            levels,
        })
    }

    /// Canonical legacy DDS, RGBA byte order, straight/unspecified alpha.
    pub fn to_dds(&self) -> Result<Vec<u8>> {
        self.validate()?;
        let mut out = vec![0u8; 128];
        out[..4].copy_from_slice(b"DDS ");
        let mut put = |at: usize, v: u32| out[at..at + 4].copy_from_slice(&v.to_le_bytes());
        put(4, 124);
        put(8, 0x0002_100f);
        put(12, self.height);
        put(16, self.width);
        put(20, self.width * 4);
        put(28, self.levels.len() as u32);
        put(76, 32);
        put(80, 0x41);
        put(88, 32);
        put(92, 0x0000_00ff);
        put(96, 0x0000_ff00);
        put(100, 0x00ff_0000);
        put(104, 0xff00_0000);
        put(
            108,
            if self.levels.len() > 1 {
                0x0040_1008
            } else {
                0x1000
            },
        );
        for pixels in &self.levels {
            out.extend_from_slice(pixels);
        }
        Ok(out)
    }
}

/// Accept only 2D single-image RGBA8/BGRA8 UNORM DDS with a complete mip chain.
/// Reject compression, sRGB, arrays, cubes, volume, premultiplied alpha and tails.
pub fn parse_dds(bytes: &[u8]) -> Result<Image> {
    if bytes.len() > MAX_FILE_BYTES {
        return Err(Error("DDS exceeds size limit"));
    }
    if bytes.len() < 128
        || &bytes[..4] != b"DDS "
        || word(bytes, 4)? != 124
        || word(bytes, 76)? != 32
    {
        return Err(Error("invalid DDS magic or header size"));
    }
    let flags = word(bytes, 8)?;
    let (width, height) = (word(bytes, 16)?, word(bytes, 12)?);
    let sizes = layout(width, height)?;
    if flags & 0x100f != 0x100f
        || flags & (0x0080_0000 | 0x0008_0000) != 0
        || word(bytes, 20)? != width * 4
        || word(bytes, 24)? > 1
        || word(bytes, 112)? != 0
        || word(bytes, 116)? != 0
        || word(bytes, 120)? != 0
        || word(bytes, 124)? != 0
    {
        return Err(Error(
            "expected tightly packed 2D DDS with no cube/volume flags",
        ));
    }
    let mip_count = word(bytes, 28)?.max(1) as usize;
    if mip_count != sizes.len()
        || (mip_count > 1
            && (flags & 0x20000 == 0 || word(bytes, 108)? & 0x0040_1008 != 0x0040_1008))
        || word(bytes, 108)? & 0x1000 == 0
    {
        return Err(Error("DDS must contain every mip through 1x1"));
    }
    let (payload, bgra) = match (word(bytes, 80)?, word(bytes, 84)?) {
        (0x41, 0)
            if word(bytes, 88)? == 32
                && word(bytes, 96)? == 0x0000_ff00
                && word(bytes, 104)? == 0xff00_0000 =>
        {
            match (word(bytes, 92)?, word(bytes, 100)?) {
                (0x0000_00ff, 0x00ff_0000) => (128usize, false),
                (0x00ff_0000, 0x0000_00ff) => (128usize, true),
                _ => return Err(Error("unsupported DDS channel masks")),
            }
        }
        (4, 0x3031_5844) => {
            if bytes.len() < 148 {
                return Err(Error("truncated DDS DX10 header"));
            }
            if word(bytes, 132)? != 3
                || word(bytes, 136)? != 0
                || word(bytes, 140)? != 1
                || word(bytes, 144)? > 1
            {
                return Err(Error(
                    "DDS DX10 must be 2D, single-image, straight/unspecified alpha",
                ));
            }
            match word(bytes, 128)? {
                28 => (148usize, false),
                87 => (148usize, true),
                _ => {
                    return Err(Error(
                        "DDS DX10 requires RGBA8/BGRA8 UNORM; sRGB/compression unsupported",
                    ))
                }
            }
        }
        _ => return Err(Error("unsupported DDS pixel format")),
    };
    let total: usize = sizes.iter().sum();
    if payload.checked_add(total) != Some(bytes.len()) {
        return Err(Error("truncated DDS pixels or unexpected trailing bytes"));
    }
    let mut offset = payload;
    let mut levels = Vec::with_capacity(sizes.len());
    for size in sizes {
        let mut pixels = bytes[offset..offset + size].to_vec();
        if bgra {
            for p in pixels.chunks_exact_mut(4) {
                p.swap(0, 2);
            }
        }
        offset += size;
        levels.push(pixels);
    }
    Ok(Image {
        width,
        height,
        levels,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn sample() -> Image {
        Image::from_rgba(3, 2, (0..24).collect()).unwrap()
    }
    fn put(out: &mut [u8], at: usize, v: u32) {
        out[at..at + 4].copy_from_slice(&v.to_le_bytes());
    }
    fn dx10() -> Vec<u8> {
        let legacy = sample().to_dds().unwrap();
        let mut out = legacy[..128].to_vec();
        put(&mut out, 80, 4);
        put(&mut out, 84, 0x3031_5844);
        for at in [88, 92, 96, 100, 104] {
            put(&mut out, at, 0);
        }
        for value in [28u32, 3, 0, 1, 1] {
            out.extend_from_slice(&value.to_le_bytes());
        }
        out.extend_from_slice(&legacy[128..]);
        out
    }
    #[test]
    fn rgba_roundtrip_keeps_every_byte() {
        let s = sample();
        assert_eq!(parse_dds(&s.to_dds().unwrap()).unwrap(), s);
    }
    #[test]
    fn dx10_rgba_roundtrip() {
        assert_eq!(parse_dds(&dx10()).unwrap(), sample());
    }
    #[test]
    fn bgra_legacy_normalizes_all_mips() {
        let s = sample();
        let mut d = s.to_dds().unwrap();
        put(&mut d, 92, 0xff0000);
        put(&mut d, 100, 0xff);
        for p in d[128..].chunks_exact_mut(4) {
            p.swap(0, 2);
        }
        assert_eq!(parse_dds(&d).unwrap(), s);
    }
    #[test]
    fn bgra_dx10_normalizes_all_mips() {
        let mut d = dx10();
        put(&mut d, 128, 87);
        for p in d[148..].chunks_exact_mut(4) {
            p.swap(0, 2);
        }
        assert_eq!(parse_dds(&d).unwrap(), sample());
    }
    #[test]
    fn rejects_every_truncation() {
        let d = dx10();
        for n in 0..d.len() {
            assert!(parse_dds(&d[..n]).is_err(), "cut {n}");
        }
    }
    #[test]
    fn rejects_trailing_data() {
        let mut d = dx10();
        d.push(0);
        assert!(parse_dds(&d).is_err());
    }
    #[test]
    fn rejects_bad_dimensions() {
        for v in [0, 4097, u32::MAX] {
            for at in [12, 16] {
                let mut d = dx10();
                put(&mut d, at, v);
                assert!(parse_dds(&d).is_err());
            }
        }
    }
    #[test]
    fn rejects_unsupported_dx10() {
        for (at, values) in [
            (128, vec![29, 88, 91, 71]),
            (132, vec![0, 2, 4]),
            (136, vec![4, 0xffff]),
            (140, vec![0, 2]),
            (144, vec![2, 3, 8]),
        ] {
            for v in values {
                let mut d = dx10();
                put(&mut d, at, v);
                assert!(parse_dds(&d).is_err(), "{at} {v}");
            }
        }
    }
    #[test]
    fn rejects_pitch_cube_volume_and_incomplete_mips() {
        for (at, v) in [
            (20, 16),
            (24, 2),
            (112, 0x200),
            (28, 1),
            (28, u32::MAX),
            (108, 0x1000),
            (8, 0x0082_100f),
            (8, 0x000a_100f),
        ] {
            let mut d = sample().to_dds().unwrap();
            put(&mut d, at, v);
            assert!(parse_dds(&d).is_err(), "{at} {v}");
        }
    }
    #[test]
    fn rejects_wrong_masks_and_header_sizes() {
        for (at, v) in [
            (4, 120),
            (76, 28),
            (80, 0x40),
            (88, 24),
            (92, 0xf),
            (96, 0xf000),
            (104, 0),
        ] {
            let mut d = sample().to_dds().unwrap();
            put(&mut d, at, v);
            assert!(parse_dds(&d).is_err());
        }
    }
    #[test]
    fn box_chain_covers_odd_edges_and_alpha_independently() {
        let s = sample();
        assert_eq!(s.levels.len(), 2);
        assert_eq!(s.levels[1], vec![10, 11, 12, 13]);
    }
    #[test]
    fn one_pixel_and_skinny_chains() {
        for (w, h, count) in [(1, 1, 1), (1, 8, 4), (8, 1, 4), (5, 3, 3)] {
            let image = Image::from_rgba(w, h, vec![77; (w * h * 4) as usize]).unwrap();
            assert_eq!(image.levels.len(), count);
            assert!(image.levels.iter().flatten().all(|&x| x == 77));
            assert_eq!(parse_dds(&image.to_dds().unwrap()).unwrap(), image);
        }
    }
    #[test]
    fn rejects_malformed_owned_image_without_allocating() {
        assert!(Image {
            width: 4097,
            height: 1,
            levels: vec![]
        }
        .to_dds()
        .is_err());
        let mut image = sample();
        image.levels.pop();
        assert!(image.to_dds().is_err());
        assert!(Image::from_rgba(4, 4, vec![0; 63]).is_err());
    }
}
