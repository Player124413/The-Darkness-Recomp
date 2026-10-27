#include "runtime/native/startup_trace.h"
#include <sys/stat.h>
#include <io.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace Trace = DarkRecomp::Native::StartupTrace;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static std::string read(std::FILE* file) {
    check(std::fseek(file, 0, SEEK_SET) == 0, "cannot rewind fixture");
    std::string result;
    char buffer[1024];
    while (const auto count = std::fread(buffer, 1, sizeof(buffer), file)) result.append(buffer, count);
    check(!std::ferror(file), "cannot read fixture");
    return result;
}
static long long bytesOnDisk(std::FILE* file) {
    struct _stat64 status{};
    check(_fstat64(_fileno(file), &status) == 0, "cannot stat fixture");
    return status.st_size;
}
struct Fixture {
    std::FILE* file = std::tmpfile();
    Fixture() { check(file != nullptr, "cannot create private temporary fixture"); }
    ~Fixture() { std::fclose(file); }
};
int main() {
    try {
        check(SetEnvironmentVariableA("DARK_STARTUP_TRACE", nullptr) != 0, "cannot clear process-local opt-in");
        check(!Trace::environmentEnabled(), "trace enabled by default");
        Trace::mark("must-not-write-default");
        Fixture disabled;
        Trace::Recorder off(Trace::environmentEnabled(), disabled.file);
        for (unsigned i = 0; i < Trace::maxEvents + 1; ++i) off.mark("disabled");
        check(bytesOnDisk(disabled.file) == 0 && read(disabled.file).empty(), "disabled trace wrote output");
        for (const auto* value : {"0", "true", "1x", " 1"}) {
            check(SetEnvironmentVariableA("DARK_STARTUP_TRACE", value) != 0, "cannot set opt-in fixture");
            check(!Trace::environmentEnabled(), "non-exact opt-in enabled trace");
        }
        check(SetEnvironmentVariableA("DARK_STARTUP_TRACE", "1") != 0 && Trace::environmentEnabled(), "exact opt-in did not enable trace");
        Fixture enabled;
        Trace::Recorder on(Trace::environmentEnabled(), enabled.file);
        const std::string longStage(Trace::maxStageLength + 10, 's'), longTag(Trace::maxTagLength + 10, 't');
        on.mark(longStage.c_str(), longTag.c_str());
        check(bytesOnDisk(enabled.file) > 0, "marker not flushed before return");
        on.mark("sanitize\nstage", "tag\r\nwith space");
        for (unsigned i = 0; i < Trace::maxDetailEvents + 5; ++i) on.mark("shader", "existing-name", true);
        for (unsigned i = 0; i < 40; ++i) on.mark("boundary");
        const auto output = read(enabled.file);
        unsigned expectedSequence = 1;
        long long previousQpc = 0;
        for (size_t begin = 0; begin < output.size();) {
            const auto end = output.find('\n', begin);
            check(end != std::string::npos, "partial marker line");
            const auto line = output.substr(begin, end - begin);
            unsigned sequence = 0; long long qpc = 0, frequency = 0; unsigned long thread = 0;
            check(std::sscanf(line.c_str(), "[StartupTrace] v=1 seq=%u qpc=%lld frequency=%lld thread=%lu", &sequence, &qpc, &frequency, &thread) == 4, "invalid clock/identity schema");
            check(sequence == expectedSequence++ && qpc >= previousQpc && frequency > 0 && thread != 0, "marker order/clock/thread invalid");
            previousQpc = qpc;
            check(line.size() < 400, "marker byte budget exceeded");
            begin = end + 1;
        }
        check(expectedSequence == Trace::maxEvents + 1, "global event cap not exact");
        check(output.find("stage=" + std::string(Trace::maxStageLength, 's') + " tag=" + std::string(Trace::maxTagLength, 't') + " clock_ok=1 stage_truncated=1 tag_truncated=1") != std::string::npos, "tag/stage truncation missing");
        check(output.find("stage=sanitize_stage tag=tag__with_space") != std::string::npos, "control/space token sanitization failed");
        check(output.find("stage=boundary tag=-") != std::string::npos && output.find("detail_dropped=5") != std::string::npos, "detail quota did not preserve boundaries/report drops");
        check(output.find("detail_limit=1 limit=1") != std::string::npos, "event saturation not reported");

        Fixture concurrent;
        Trace::Recorder shared(true, concurrent.file);
        auto produce = [&] { for (unsigned i = 0; i < 16; ++i) shared.mark("thread"); };
        std::thread worker(produce); produce(); worker.join();
        const auto threadedOutput = read(concurrent.file);
        unsigned lines = 0;
        for (char c : threadedOutput) if (c == '\n') ++lines;
        check(lines == 32 && threadedOutput.find("seq=32 ") != std::string::npos, "concurrent marker lines lost/interleaved");
        Fixture summaryOutput;
        Trace::Recorder summaryRecorder(true, summaryOutput.file);
        Trace::PumpSummary summary{735, 12, 300000, true, true};
        summaryRecorder.mark("startup-pump.summary", nullptr, false, &summary);
        const auto summaryText = read(summaryOutput.file);
        check(summaryText.find("startup_calls=735 startup_messages=12 startup_max_gap_qpc=300000 startup_gap_clock_ok=1 startup_canceled=1") != std::string::npos,
              "startup summary values/clock units/cancel flag missing");
        check(summaryText.size() < 512, "startup summary byte budget exceeded");
        std::puts("StartupTraceContract PASS: default-off/exact opt-in, flush, 256-event cap, 224-detail reserve, order/clock/thread, truncation/sanitization, concurrent emission, startup summary; CPU-only.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "StartupTraceContract FAIL: %s\n", error.what());
        return 1;
    }
}
