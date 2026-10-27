#pragma once
#include <array>
// Requires an explicitly selected private pack and a matching original video.
// Exercises the real guest filesystem ABI; does not decode or display media.
static void testVideoModGuest(PPCContext& ctx, const char* relative) {
    auto* base = memory->base();
    const auto original = memory->gameDirectory() / relative;
    const auto replacement = memory->videoModFile(original);
    check(bool(replacement), "Requested video pack is not active for the original target");
    check(replacement->bytes >= 64, "Video integration fixture is too small");
    const uint32_t scratch = memory->allocate(4096);
    check(scratch != 0, "Video I/O scratch allocation failed");
    memset(base + scratch, 0, 4096);
    const std::string name = std::string("game:\\") + relative;
    check(name.size() < 512, "Video guest target is too long");
    memcpy(base + scratch + 512, name.c_str(), name.size() + 1);
    memory->write32(scratch, 0xfffffffd);
    memory->write32(scratch + 4, scratch + 16);
    memory->write32(scratch + 8, 0x40);
    memory->write32(scratch + 16, uint32_t(name.size() << 16) | uint32_t(name.size() + 1));
    memory->write32(scratch + 20, scratch + 512);
    const auto open = [&](uint32_t sharing, uint32_t access = GENERIC_READ | SYNCHRONIZE) {
        ctx.r3.u64 = scratch + 80; ctx.r4.u64 = access;
        ctx.r5.u64 = scratch; ctx.r6.u64 = scratch + 64; ctx.r7.u64 = sharing; ctx.r8.u64 = 0x60;
        __imp__NtOpenFile(ctx, base);
        return ctx.r3.u32;
    };
    const auto attributes = [&] {
        ctx.r3.u64 = scratch; ctx.r4.u64 = scratch + 128;
        __imp__NtQueryFullAttributesFile(ctx, base);
        check(ctx.r3.u32 == 0, "Replacement path attributes failed");
        check(_byteswap_uint64(*reinterpret_cast<uint64_t*>(base + scratch + 168)) == replacement->bytes,
              "Path attributes exposed stock size instead of replacement size");
    };
    attributes();
    check(open(FILE_SHARE_READ) == 0, "First guest replacement open failed");
    const uint32_t first = memory->read32(scratch + 80);
    check(object(first)->path == std::filesystem::weakly_canonical(original), "Replacement changed the logical guest source path");
    check(open(FILE_SHARE_READ) == 0, "Second guest replacement open failed");
    const uint32_t second = memory->read32(scratch + 80);
    check(open(0) == 0xc0000043, "Replacement lost exclusive read conflicts");
    check(open(FILE_SHARE_READ, GENERIC_WRITE | SYNCHRONIZE) == 0xc00000a2, "Read-only replacement allowed a guest write");
    ctx.r3.u64 = first; ctx.r4.u64 = scratch + 64; ctx.r5.u64 = scratch + 192; ctx.r6.u64 = 24; ctx.r7.u64 = 5;
    __imp__NtQueryInformationFile(ctx, base);
    check(ctx.r3.u32 == 0 && _byteswap_uint64(*reinterpret_cast<uint64_t*>(base + scratch + 200)) == replacement->bytes,
          "Open-handle size differs from replacement size");
    std::ifstream payload(replacement->path, std::ios::binary);
    std::array<char, 64> head{}; payload.read(head.data(), head.size());
    check(bool(payload), "Cannot inspect replacement fixture");
    const auto read = [&](uint32_t handle, const char* expected, const uint64_t* offset = nullptr) {
        if (offset) *reinterpret_cast<uint64_t*>(base + scratch + 48) = _byteswap_uint64(*offset);
        ctx.r3.u64 = handle; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0;
        ctx.r7.u64 = scratch + 64; ctx.r8.u64 = scratch + 1024; ctx.r9.u64 = 32;
        ctx.r10.u64 = offset ? scratch + 48 : 0;
        __imp__NtReadFile(ctx, base);
        check(ctx.r3.u32 == 0 && memory->read32(scratch + 68) == 32 && !memcmp(base + scratch + 1024, expected, 32),
              "Guest read did not return the selected replacement bytes");
    };
    read(first, head.data());
    read(second, head.data());
    read(first, head.data() + 32);
    std::array<char, 32> tail{};
    payload.seekg(std::streamoff(replacement->bytes - tail.size())); payload.read(tail.data(), tail.size());
    check(bool(payload), "Cannot inspect replacement tail");
    const uint64_t tailOffset = replacement->bytes - tail.size();
    read(second, tail.data(), &tailOffset);
    ctx.r3.u64 = first; __imp__NtClose(ctx, base);
    ctx.r3.u64 = second; __imp__NtClose(ctx, base);
    check(open(0) == 0, "Exclusive read did not resume after the final shared close");
    const uint32_t exclusive = memory->read32(scratch + 80);
    check(open(FILE_SHARE_READ) == 0xc0000043, "Exclusive replacement handle did not protect its guest sharing");
    attributes();
    ctx.r3.u64 = exclusive; __imp__NtClose(ctx, base);
    check(memory->release(scratch), "Video I/O scratch release failed");
    puts("Video replacement guest ABI: path/handle sizes, logical identity, independent cursors, tail seek, read-only access and sharing transitions passed.");
}
