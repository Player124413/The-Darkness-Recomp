#pragma once
#include <algorithm>
#include <cstdint>

namespace DarkRecomp {
struct DisplayRect { float x, y, width, height; };
// Fit an image without stretching or cropping, including narrow resized windows.
inline DisplayRect fitDisplay(uint32_t imageWidth, uint32_t imageHeight,
                              uint32_t outputWidth, uint32_t outputHeight) {
    if (!imageWidth || !imageHeight || !outputWidth || !outputHeight) return {};
    const float scale = (std::min)(float(outputWidth) / imageWidth, float(outputHeight) / imageHeight);
    const float width = imageWidth * scale, height = imageHeight * scale;
    return {(outputWidth - width) * .5f, (outputHeight - height) * .5f, width, height};
}
inline DisplayRect presentationRect(uint32_t imageWidth, uint32_t imageHeight,
                                    uint32_t outputWidth, uint32_t outputHeight,
                                    bool stretchToFill = false) {
    if (stretchToFill) {
        if (!imageWidth || !imageHeight || !outputWidth || !outputHeight) return {};
        return {0, 0, float(outputWidth), float(outputHeight)};
    }
    return fitDisplay(imageWidth, imageHeight, outputWidth, outputHeight);
}
}
