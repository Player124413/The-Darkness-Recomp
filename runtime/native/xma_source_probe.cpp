#include "xma_source_probe.h"
#include "ppc_context.h"
#include "ppc_recomp_shared.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace DarkRecomp::Native {
namespace {
namespace Source = XmaSourceIdentity;
struct LoadScope;
thread_local LoadScope* activeLoad = nullptr;
std::atomic<uint64_t> nextEpoch{0};

struct LoadScope {
    LoadScope* previous = nullptr;
    PPCContext* context = nullptr;
    uint8_t* base = nullptr;
    Source::Scope source;
    explicit LoadScope(PPCContext& ctx, uint8_t* memoryBase) noexcept {
        if (!xmaSourceProbeEnabled()) return;
        previous = activeLoad;
        context = &ctx;
        base = memoryBase;
        source.voice = ctx.r3.u32;
        const auto entryStack = ctx.r1.u32;
        if (entryStack >= 320) source.frame = entryStack - 320;
        if (source.frame <= std::numeric_limits<uint32_t>::max() - 104)
            source.expectedOpenerOut = source.frame + 104;
        // Saturate rather than recycle an epoch after the theoretical wrap.
        auto prior = nextEpoch.load(std::memory_order_relaxed);
        while (prior != std::numeric_limits<uint64_t>::max()) {
            if (nextEpoch.compare_exchange_weak(prior, prior + 1, std::memory_order_relaxed)) {
                source.epoch = prior + 1;
                break;
            }
        }
        activeLoad = this;
    }
    ~LoadScope() { if (context) activeLoad = previous; }
    LoadScope(const LoadScope&) = delete;
    LoadScope& operator=(const LoadScope&) = delete;
};

// Snapshot-local region cache. Retained source references keep sampled objects
// alive; Memory's mutex serializes host address-space changes during this read.
// The source validator checks all pointer arithmetic before invoking the reader.
struct SnapshotReader {
    uint8_t* base;
    std::array<MEMORY_BASIC_INFORMATION, 16> regions{};
    size_t count = 0;
    bool operator()(uint32_t address, std::span<uint8_t> destination) noexcept {
        if (!base || !address || destination.size() > (uint64_t{1} << 32) - address)
            return false;
        auto* cursor = base + address;
        size_t remaining = destination.size(), copied = 0;
        while (remaining) {
            MEMORY_BASIC_INFORMATION info{};
            bool found = false;
            const auto at = reinterpret_cast<uintptr_t>(cursor);
            for (size_t i = 0; i < count; ++i) {
                const auto begin = reinterpret_cast<uintptr_t>(regions[i].BaseAddress);
                if (at >= begin && at - begin < regions[i].RegionSize) {
                    info = regions[i]; found = true; break;
                }
            }
            if (!found) {
                if (!VirtualQuery(cursor, &info, sizeof(info))) return false;
                if (count < regions.size()) regions[count++] = info;
            }
            const DWORD protection = info.Protect & 0xff;
            if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
                (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
                 protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
                 protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY))
                return false;
            const auto begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
            if (at < begin || at - begin >= info.RegionSize) return false;
            const auto bytes = (std::min)(remaining, info.RegionSize - size_t(at - begin));
            std::memcpy(destination.data() + copied, cursor, bytes);
            cursor += bytes; copied += bytes; remaining -= bytes;
        }
        return true;
    }
};

// Accepted pathnames contain printable ASCII. Escape only JSON delimiters into
// fixed storage; diagnostics cannot allocate or retain a guest string pointer.
auto escapedPath(const Source::BankPath& path) noexcept {
    std::array<char, Source::MaxBankPathBytes * 2 + 1> text{};
    size_t out = 0;
    for (const char c : path.view()) {
        if (c == '\\' || c == '"') text[out++] = '\\';
        text[out++] = c;
    }
    return text;
}
}

bool xmaSourceProbeEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("DARK_XMA_ORIGIN_PROBE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

void XmaSourceProbe::created(uint8_t* base, uint32_t slot, uint32_t context) noexcept {
    if (slot >= allocations_.size()) return;
    auto& allocation = allocations_[slot];
    allocation = {};
    allocation.context = context;
    if (generation_ != std::numeric_limits<uint64_t>::max())
        allocation.generation = ++generation_;
    Source::SnapshotResult result{Source::Rejection::InvalidScope, std::nullopt};
    if (allocation.generation && activeLoad && activeLoad->base == base) {
        const auto& ctx = *activeLoad->context;
        const Source::AllocationSite site{
            activeLoad->source.epoch, uint32_t(ctx.lr), ctx.r16.u32, ctx.r17.u32,
            ctx.r24.u32, ctx.r26.u32, ctx.r30.u32, ctx.r31.u32, ctx.r3.u32};
        result = Source::snapshot(activeLoad->source, site, SnapshotReader{base});
        allocation.identity = result.identity;
    }
    // Unrelated/non-streamed allocation routes still get a fresh generation,
    // but cannot exhaust the scoped diagnostic budget before a streamed load.
    if (!activeLoad || activeLoad->base != base || !output_ || events_ >= 128) return;
    ++events_;
    if (!allocation.identity) {
        std::fprintf(output_, "[XmaOrigin] phase=create slot=%u context=%08X generation=%llu bound=0 rejection=%u\n",
            slot, context, static_cast<unsigned long long>(allocation.generation), unsigned(result.rejection));
        return;
    }
    const auto& identity = *allocation.identity;
    const auto path = escapedPath(identity.bankPath);
    std::fprintf(output_, "[XmaOrigin] phase=create slot=%u context=%08X generation=%llu bound=1 epoch=%llu bank=\"%s\" wave=%d offset=%llu bytes=%llu stream=%u voice=%08X\n",
        slot, context, static_cast<unsigned long long>(allocation.generation),
        static_cast<unsigned long long>(identity.epoch), path.data(), identity.localIndex,
        static_cast<unsigned long long>(identity.absSliceOffset),
        static_cast<unsigned long long>(identity.length), identity.streamIndex, identity.voice);
}

void XmaSourceProbe::lifecycle(const char* phase, uint32_t slot, const Allocation& allocation) noexcept {
    if (!allocation.identity || !output_ || events_ >= 128) return;
    ++events_;
    std::fprintf(output_, "[XmaOrigin] phase=%s slot=%u context=%08X generation=%llu bound=%u\n",
        phase, slot, allocation.context, static_cast<unsigned long long>(allocation.generation),
        unsigned(allocation.identity.has_value()));
}
void XmaSourceProbe::freed(uint32_t slot, uint32_t context) noexcept {
    if (slot >= allocations_.size()) return;
    auto& allocation = allocations_[slot];
    if (allocation.context != context) return;
    lifecycle("free", slot, allocation);
    allocation = {};
}
void XmaSourceProbe::reset(uint32_t slot, uint32_t context) noexcept {
    if (slot >= allocations_.size()) return;
    const auto& allocation = allocations_[slot];
    if (allocation.context == context) lifecycle("reset", slot, allocation);
    // Identity describes this allocation only. A reset is not evidence that a
    // replacement PCM cursor, seek, loop or retarget operation would be safe.
}
}

extern "C" PPC_FUNC(__imp__sub_827D92E0);
PPC_FUNC(sub_827D92E0) {
    DarkRecomp::Native::LoadScope scope(ctx, base);
    __imp__sub_827D92E0(ctx, base);
}

void ObserveXmaSourceSliceMidAsmHook(PPCRegister& container, PPCRegister& index,
    PPCRegister& slice, PPCRegister& expectedSlice, PPCRegister& bankFile,
    PPCRegister& offset, PPCRegister& length, PPCRegister& openerOut) {
    using namespace DarkRecomp::Native;
    auto* load = activeLoad;
    if (!load || openerOut.u32 != load->source.expectedOpenerOut) return;
    if (slice.u32 != expectedSlice.u32) {
        if (load->source.observationRejection == XmaSourceIdentity::Rejection::None)
            load->source.observationRejection = XmaSourceIdentity::Rejection::InvalidPod;
        return;
    }
    const XmaSourceIdentity::OpenerPod pod{
        container.u32, index.s32, slice.u32,
        bankFile.u32, offset.u32, length.u32, openerOut.u32};
    XmaSourceIdentity::recordObservation(load->source, load->source.epoch, pod);
}
