#pragma once
#include <utility>

// Host fixtures use the retail WMV/Theora container/owner pairs and the
// 112-byte timestamp/CImage layout allocated by original Theora 827A0388.
// Decoder playback remains a separate in-game qualification.
template<class FinishFrame>
static void testVideoObserver(Memory& memory, EnginePreviewD3D11& renderer,
                              uint32_t attributes, uint32_t indices, FinishFrame finish) {
    auto* base = memory.base();
    const auto scratch = memory.allocate(4 * 1024 * 1024);
    require(scratch != 0, "Video observer fixture allocation failed");
    std::array<uint8_t, 160> savedAttributes{};
    std::memcpy(savedAttributes.data(), base + attributes, savedAttributes.size());
    struct Restore {
        Memory& memory; uint8_t* base; uint32_t scratch, attributes;
        const std::array<uint8_t, 160>& saved;
        ~Restore() {
            std::memcpy(base + attributes, saved.data(), saved.size());
            memory.release(scratch);
        }
    } restore{memory, base, scratch, attributes, savedAttributes};
    std::memset(base + scratch, 0, 4096);
    const auto container = scratch, array = scratch + 128, entries = scratch + 256;
    const auto owner = scratch + 512, frame = scratch + 768;
    const auto program = scratch + 1024, name = scratch + 1088;
    const auto yPixels = scratch + 4096, vuPixels = scratch + 0x1d0000;
    put32(base, container + 56, array); put32(base, array + 4, 1);
    put32(base, array + 24, entries); put32(base, entries, owner);
    put32(base, owner + 80, frame);
    put16(base, owner + 126, 0x7e0); put16(base, owner + 128, 0x7e1);
    put32(base, program, 5); put32(base, program + 4, name);
    std::strcpy(reinterpret_cast<char*>(base + name), "CMWnd_ModTexture_PaintVideo_YUV2RGB");
    put32(base, attributes, program);
    put16(base, attributes + 8, 0x7e0); put16(base, attributes + 10, 0x7e1);
    auto source = [&](unsigned width, unsigned height, double timestamp, uint8_t y, uint8_t u, uint8_t v) {
        const auto time = std::bit_cast<uint64_t>(timestamp);
        put32(base, frame, uint32_t(time >> 32)); put32(base, frame + 4, uint32_t(time));
        for (unsigned slot = 0; slot < 2; ++slot) {
            const auto image = frame + (slot ? 60 : 12), pixels = slot ? vuPixels : yPixels;
            const unsigned w = slot ? width / 2 : width, h = slot ? height / 2 : height, bpp = slot ? 2 : 1;
            put32(base, image, 0x82097610); put32(base, image + 8, pixels);
            put32(base, image + 12, w * h * bpp); put32(base, image + 16, w); put32(base, image + 20, h);
            put32(base, image + 24, w * bpp); put32(base, image + 28, bpp);
            put32(base, image + 32, slot ? 0x20000 : 0x2000); put32(base, image + 40, 0x810);
        }
        std::memset(base + yPixels, y, width * height);
        for (unsigned i = 0; i < width * height / 2; i += 2) {
            base[vuPixels + i] = v; base[vuPixels + i + 1] = u;
        }
    };
    auto capture = [&](uint32_t localId = 2) {
        previewObserveVideo(base, container, localId);
        previewObserveTriangles(base, indices, 1);
        return finish();
    };
    source(768, 576, 1, 81, 90, 240);
    for (const auto& pair : {std::pair{0x82097C50u, 0x82097E38u},
                             std::pair{0x82097D90u, 0x82097CF8u},
                             std::pair{0x82097D94u, 0x82097E38u}}) {
        put32(base, container, pair.first); put32(base, owner, pair.second);
        require(capture().empty(), "Mismatched or unknown video object types published a frame");
    }
    put32(base, container, 0x82097D90); put32(base, owner, 0x82097E38);
    require(capture(1).empty() && capture(5).empty(), "Wrong plane or out-of-range local video ID published a frame");
    for (const auto& pair : {std::pair{0x82097C50u, 0x82097CF8u},
                             std::pair{0x82097D90u, 0x82097E38u}}) {
        put32(base, container, pair.first); put32(base, owner, pair.second);
        source(768, 576, 1, 81, 90, 240);
        const auto first = capture();
        require(first.size() == 1 && first[0].video && first[0].video->width == 768 &&
                first[0].video->height == 576 && first[0].video->timestampSeconds == 1,
                "Original video container did not publish its selected frame");
        renderer.render(first); nearByte(renderer.readPixel(32, 32), 0, 254);
        source(1536, 1152, 2, 235, 128, 128);
        const auto second = capture();
        require(second.size() == 1 && second[0].video && second[0].video != first[0].video &&
                second[0].video->width == 1536 && second[0].video->height == 1152 &&
                second[0].video->timestampSeconds == 2 && first[0].video->luma.front() == 81 &&
                first[0].video->chroma[0] == 240 && first[0].video->chroma[1] == 90,
                "Video resolution change lost dimensions or mutated an older queued frame");
        renderer.render(second); nearByte(renderer.readPixel(32, 32), 0, 255);
        renderer.render(first); nearByte(renderer.readPixel(32, 32), 0, 254);
        const auto unchanged = capture();
        require(unchanged.size() == 1 && unchanged[0].video == second[0].video,
                "Unchanged selected video frame was copied again");
    }
    // A different decoder family may reuse all guest addresses, IDs and the
    // timestamp. Its concrete owner type must invalidate the cached snapshot.
    put32(base, container, 0x82097C50); put32(base, owner, 0x82097CF8);
    source(1536, 1152, 2, 145, 54, 34);
    const auto reused = capture();
    require(reused.size() == 1 && reused[0].video && reused[0].video->luma.front() == 145 &&
            reused[0].video->chroma[0] == 34 && reused[0].video->chroma[1] == 54,
            "Reused WMV/Theora object addresses retained a different decoder's frame");
    renderer.render(reused); nearByte(renderer.readPixel(32, 32), 1, 255);
    std::puts("VideoObserver: WMV/Theora type guards, original/HD dimensions, owned generations, GPU resize and decoder-family address reuse passed.");
}
