#pragma once
#include <Windows.h>
#include <cstdio>

namespace DarkRecomp::Native::StartupTrace {
inline constexpr unsigned maxEvents = 256;
inline constexpr unsigned maxDetailEvents = 224; // Reserve 32 entries for stage boundaries.
inline constexpr unsigned maxStageLength = 48;
inline constexpr unsigned maxTagLength = 64;
struct PumpSummary {
    unsigned long long calls = 0, messages = 0;
    long long maxGapQpc = 0;
    bool gapClockOk = true, canceled = false;
};

inline bool environmentEnabled() noexcept {
    char value[2]{};
    return GetEnvironmentVariableA("DARK_STARTUP_TRACE", value, sizeof(value)) == 1 && value[0] == '1';
}

// Startup only: fixed-size tokens, no background collector, graphics calls or message pumping.
// Each emitted line is explicitly flushed. Opt-in synchronous I/O perturbs startup;
// QPC spans describe these instrumented boundaries, not CPU/GPU execution time.
class Recorder {
    bool enabled_;
    std::FILE* output_;
    SRWLOCK lock_ = SRWLOCK_INIT;
    LARGE_INTEGER frequency_{};
    unsigned emitted_ = 0, details_ = 0, detailDropped_ = 0;

    template<unsigned N> static bool copyToken(char (&target)[N], const char* source) noexcept {
        if (!source) source = "-";
        unsigned i = 0;
        for (; i + 1 < N && source[i]; ++i) {
            const unsigned char c = static_cast<unsigned char>(source[i]);
            target[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':')
                ? char(c) : '_';
        }
        target[i] = '\0';
        return source[i] != '\0';
    }
public:
    explicit Recorder(bool enabled, std::FILE* output = stderr) noexcept
        : enabled_(enabled && output), output_(output) {
        if (enabled_) QueryPerformanceFrequency(&frequency_);
    }
    bool enabled() const noexcept { return enabled_; }
    void mark(const char* stage, const char* tag = nullptr, bool detail = false,
              const PumpSummary* summary = nullptr) noexcept {
        if (!enabled_) return;
        AcquireSRWLockExclusive(&lock_);
        if (emitted_ == maxEvents) { ReleaseSRWLockExclusive(&lock_); return; }
        if (detail && details_ == maxDetailEvents) {
            ++detailDropped_;
            ReleaseSRWLockExclusive(&lock_);
            return;
        }
        if (detail) ++details_;
        char stageToken[maxStageLength + 1]{}, tagToken[maxTagLength + 1]{};
        const bool stageTruncated = copyToken(stageToken, stage);
        const bool tagTruncated = copyToken(tagToken, tag);
        LARGE_INTEGER qpc{};
        const bool clockOk = QueryPerformanceCounter(&qpc) && frequency_.QuadPart > 0;
        char suffix[192]{};
        if (summary) std::snprintf(suffix, sizeof(suffix),
            " startup_calls=%llu startup_messages=%llu startup_max_gap_qpc=%lld startup_gap_clock_ok=%u startup_canceled=%u",
            summary->calls, summary->messages, summary->maxGapQpc, unsigned(summary->gapClockOk), unsigned(summary->canceled));
        ++emitted_;
        std::fprintf(output_, "[StartupTrace] v=1 seq=%u qpc=%lld frequency=%lld thread=%lu stage=%s tag=%s clock_ok=%u stage_truncated=%u tag_truncated=%u detail=%u detail_dropped=%u detail_limit=%u limit=%u%s\n",
            emitted_, qpc.QuadPart, frequency_.QuadPart, GetCurrentThreadId(), stageToken, tagToken, unsigned(clockOk),
            unsigned(stageTruncated), unsigned(tagTruncated), unsigned(detail), detailDropped_,
            unsigned(details_ == maxDetailEvents), unsigned(emitted_ == maxEvents), suffix);
        std::fflush(output_);
        ReleaseSRWLockExclusive(&lock_);
    }
};

inline Recorder& recorder() noexcept {
    static Recorder value(environmentEnabled()); // Read the opt-in once, on first startup mark.
    return value;
}
inline void mark(const char* stage, const char* tag = nullptr, bool detail = false) noexcept {
    recorder().mark(stage, tag, detail);
}
inline bool enabled() noexcept { return recorder().enabled(); }
inline void pumpSummary(const PumpSummary& summary) noexcept {
    recorder().mark("startup-pump.summary", nullptr, false, &summary);
}
}
