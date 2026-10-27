#pragma once
#include "xma_source_identity.h"
#include <array>
#include <cstdio>
#include <optional>

namespace DarkRecomp::Native {

bool xmaSourceProbeEnabled() noexcept;

// Diagnostic ownership only. Memory calls these under its allocation mutex;
// no record is used to select, replace, decode or trim audio.
class XmaSourceProbe {
public:
    explicit XmaSourceProbe(std::FILE* output = stderr) noexcept : output_(output) {}
    void created(uint8_t* base, uint32_t slot, uint32_t context) noexcept;
    void freed(uint32_t slot, uint32_t context) noexcept;
    void reset(uint32_t slot, uint32_t context) noexcept;
private:
    struct Allocation {
        uint64_t generation = 0;
        uint32_t context = 0;
        std::optional<XmaSourceIdentity::Identity> identity;
    };
    std::array<Allocation, 320> allocations_{};
    uint64_t generation_ = 0;
    unsigned events_ = 0;
    std::FILE* output_;
    void lifecycle(const char* phase, uint32_t slot, const Allocation& allocation) noexcept;
};
}
