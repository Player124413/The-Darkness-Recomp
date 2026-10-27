#pragma once
#include <algorithm>
#include <cstdint>
#include <string_view>
#include "runtime/native/graphics_settings.h"

struct NativeDisplaySize { uint32_t width, height; };
// Rasterize more pixels on the PC while retaining the console's bounded
// allocation dimensions. Integer scaling keeps tile/resolve edges coincident.
inline uint32_t nativeResolutionScale(NativeDisplaySize render) {
    return (std::max)({1u, (render.height + 719) / 720, (render.width + 2559) / 2560});
}
inline bool parseDisplayDimension(std::wstring_view text, uint32_t& result) {
    if (text.empty()) return false;
    uint32_t number = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9' || number > 16384 / 10) return false;
        number = number * 10 + uint32_t(c - L'0');
        if (number > 16384) return false;
    }
    if (!number) return false;
    result = number;
    return true;
}
// Keep the established render height unless explicitly requested otherwise.
// The world expands horizontally on wider displays; pixel count stays bounded.
inline NativeDisplaySize renderSizeForDisplay(NativeDisplaySize output, uint32_t requestedHeight = 720,
                                              DarkRecomp::Native::DisplayAspectMode aspectMode =
                                                  DarkRecomp::Native::DisplayAspectMode::Auto) {
    if (!output.width || !output.height || !requestedHeight) return {};
    if (aspectMode == DarkRecomp::Native::DisplayAspectMode::Stretch4x3) {
        // The guest video-mode gate requires at least 320 pixels wide. A 4:3
        // mode therefore needs a 240-pixel minimum height even when the saved
        // render-height request is lower.
        const uint64_t targetHeight = (std::max)(uint64_t(240),
            uint64_t((std::min)(requestedHeight, DarkRecomp::Native::kMaximumRenderHeight)));
        NativeDisplaySize selected{};
        uint64_t selectedHeight = 0;
        for (uint64_t scale = 1; scale <= 3; ++scale) {
            // Exact 4:3 dimensions must also remain four-pixel aligned after
            // nativeResolutionScale divides the host raster dimensions. Try
            // each supported scale because rounding across a scale boundary
            // can make the largest candidate for one scale belong to another.
            const uint64_t alignment = 12 * scale;
            const uint64_t limit = (std::min)(targetHeight, 720 * scale);
            const uint64_t height = (std::max)(alignment, limit / alignment * alignment);
            const uint64_t width = height * 4 / 3;
            const auto actualScale = nativeResolutionScale({uint32_t(width), uint32_t(height)});
            if (actualScale == scale && height > selectedHeight) {
                selected = {uint32_t(width), uint32_t(height)};
                selectedHeight = height;
            }
        }
        return selected;
    }
    // Keep host targets bounded; the guest sees these dimensions divided by
    // nativeResolutionScale. The original camera rounds its viewport to four
    // logical pixels, so keep both extents aligned before publishing the mode.
    uint64_t height = (std::min)(requestedHeight, DarkRecomp::Native::kMaximumRenderHeight);
    uint64_t width = (uint64_t(output.width) * height + output.height / 2) / output.height;
    if (width > 4096) { height = height * 4096 / width; width = 4096; }
    for (;;) {
        const uint64_t alignment = 4 * nativeResolutionScale({uint32_t(width), uint32_t(height)});
        width = (std::max)(alignment, width / alignment * alignment);
        height = (std::max)(alignment, height / alignment * alignment);
        if (4 * nativeResolutionScale({uint32_t(width), uint32_t(height)}) == alignment)
            return {uint32_t(width), uint32_t(height)};
    }
}
