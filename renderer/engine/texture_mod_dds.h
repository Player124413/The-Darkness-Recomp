#pragma once
// Narrow DDS interchange contract shared with tools/texture_mods.
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace DarkRecomp::TextureMods {
struct DdsImage {
    uint32_t width = 0, height = 0;
    std::vector<std::vector<uint8_t>> levels; // Owned RGBA8 UNORM, base through1x1.
};
inline constexpr size_t ddsMaxBytes = 128u * 1024 * 1024;
inline constexpr unsigned ddsMaxDimension = 4096;

// Returns an error literal on rejection; output is committed only on success.
// Allocation failure propagates to the optional loader's ordinary fallback.
inline const char* parseDds(std::span<const uint8_t> data, DdsImage& output) {
    if (data.size() < 128 || data.size() > ddsMaxBytes + 148)
        return "DDS size outside bounds";
    const auto word = [&](size_t at) -> uint32_t {
        return uint32_t(data[at]) | uint32_t(data[at+1]) << 8 |
            uint32_t(data[at+2]) << 16 | uint32_t(data[at+3]) << 24;
    };
    if (word(0) != 0x20534444 || word(4) != 124 || word(76) != 32)
        return "DDS magic/header size invalid";
    const uint32_t width = word(16), height = word(12), flags = word(8);
    if (!width || !height || width > ddsMaxDimension || height > ddsMaxDimension)
        return "DDS dimensions outside1..4096";
    if ((flags & 0x100f) != 0x100f || (flags & (0x00800000 | 0x00080000)) ||
        word(20) != width * 4 || word(24) > 1 || word(112) || word(116) || word(120) || word(124))
        return "DDS is not tightly packed2D";
    const unsigned levels = std::bit_width((std::max)(width, height));
    const unsigned declared = (std::max)(1u, word(28));
    if (declared != levels || !(word(108) & 0x1000) ||
        (levels > 1 && (!(flags & 0x20000) || (word(108) & 0x00401008) != 0x00401008)))
        return "DDS mip chain incomplete";
    bool bgra = false;
    size_t payload = 128;
    if (word(80) == 0x41 && word(84) == 0 && word(88) == 32 &&
        word(96) == 0x0000ff00 && word(104) == 0xff000000) {
        if (word(92) == 0xff && word(100) == 0x00ff0000) bgra = false;
        else if (word(92) == 0x00ff0000 && word(100) == 0xff) bgra = true;
        else return "DDS channel masks unsupported";
    } else if (word(80) == 4 && word(84) == 0x30315844) {
        if (data.size() < 148) return "DDS DX10 header truncated";
        if (word(132) != 3 || word(136) || word(140) != 1 || word(144) > 1)
            return "DDS DX10 array/dimension/alpha unsupported";
        if (word(128) == 28) bgra = false;
        else if (word(128) == 87) bgra = true;
        else return "DDS DX10 requires RGBA8/BGRA8 UNORM";
        payload = 148;
    } else return "DDS pixel format unsupported";
    uint64_t total = 0;
    for (unsigned mip = 0; mip < levels; ++mip)
        total += uint64_t((std::max)(1u, width >> mip)) * (std::max)(1u, height >> mip) * 4;
    if (total > ddsMaxBytes || uint64_t(payload) + total != data.size())
        return "DDS payload truncated or has trailing bytes";
    DdsImage next;
    next.width = width; next.height = height; next.levels.reserve(levels);
    size_t at = payload;
    for (unsigned mip = 0; mip < levels; ++mip) {
        const size_t count = size_t((std::max)(1u, width >> mip)) * (std::max)(1u, height >> mip) * 4;
        next.levels.emplace_back(data.begin() + at, data.begin() + at + count);
        auto& pixels = next.levels.back();
        if (bgra) for (size_t i = 0; i < count; i += 4) std::swap(pixels[i], pixels[i+2]);
        at += count;
    }
    output = std::move(next);
    return nullptr;
}
} // namespace DarkRecomp::TextureMods
