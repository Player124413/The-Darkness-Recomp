#include "runtime/native/xma_source_identity.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace DarkRecomp::Native::XmaSourceIdentity;
namespace {
int failures = 0;
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct ReadCall { uint32_t address; size_t size; };
struct Memory {
    std::unordered_map<uint32_t, uint8_t> data;
    std::vector<ReadCall> reads;
    void word(uint32_t address, uint64_t value, uint32_t bytes = 4) {
        if (uint64_t(address) + bytes > (uint64_t{1} << 32)) std::abort();
        for (uint32_t i = 0; i < bytes; ++i)
            data[address + i] = uint8_t(value >> (8 * (bytes - 1 - i)));
    }
    bool operator()(uint32_t address, std::span<uint8_t> out) noexcept {
        reads.push_back({address, out.size()});
        if (out.size() > (uint64_t{1} << 32) - uint64_t(address)) return false;
        for (size_t i = 0; i < out.size(); ++i) {
            const auto it = data.find(address + static_cast<uint32_t>(i));
            if (it == data.end()) return false;
            out[i] = it->second;
        }
        return true;
    }
    void text(uint32_t backing, const std::string& text, bool nul = true) {
        for (uint32_t i = 0; i < MaxBankPathBytes; ++i)
            data.erase(backing + 2 + i);
        for (size_t i = 0; i < text.size(); ++i)
            data[backing + 2 + static_cast<uint32_t>(i)] = uint8_t(text[i]);
        if (nul) data[backing + 2 + static_cast<uint32_t>(text.size())] = 0;
    }
};
struct Fixture {
    static constexpr uint32_t V = 0x1000, L = 0x2000, C = 0x3000;
    static constexpr uint32_t O = 0x4000, S = O+8, Q = 0x5000;
    static constexpr uint32_t F = 0x6000, G = 0x7000, R = 0x8000, B = 0x9000;
    Memory memory;
    Scope scope;
    OpenerPod pod;
    AllocationSite site;
    Fixture() {
        scope.voice = V; scope.frame = L; scope.epoch = 19;
        scope.expectedOpenerOut = L+104;
        pod = {C, 37, Q, F, 0x123456, 0x45678, L+104};
        check(recordObservation(scope, 19, pod) == Rejection::None, "fixture observation");
        site = {19, 0x828B1898, V+64, V, 1, G, R+96, R+96+64, R+96+64};
        memory.word(V+64, V);
        memory.word(L+88, O);
        memory.word(O, 0x82096E68);
        memory.word(S, 0x820658FC);
        memory.word(S+20, 0);
        memory.word(S+24, 1);
        memory.word(S+16, Q);
        memory.word(Q, 0x8209DBA8);
        memory.word(Q+12, F);
        memory.word(Q+16, pod.offset32, 8);
        memory.word(Q+24, pod.length32, 8);
        memory.word(F, 0x820658FC);
        memory.word(F+8, 0x82065568);
        memory.word(F+12, B);
        memory.word(B, 0x4001, 2); // Valid ordinary narrow CStr with reference bits.
        memory.text(B, "Content\\Sound\\TV.xwc");
        memory.word(G, 2);
        memory.word(G+8, R);
        // Deliberately absent: D+56, *out, C/registry and whole record spans.
    }
    SnapshotResult run() { return snapshot(scope, site, memory); }
};
void expect(Fixture& f, Rejection expected, const char* message, bool noReads = false) {
    const auto result = f.run();
    check(result.rejection == expected, message);
    check(!result && !result.identity, "rejection exposes no partial identity");
    if (noReads) check(f.memory.reads.empty(), "reject before any guest reads");
}
void positiveAndOwnership() {
    Fixture f;
    const auto result = f.run();
    check(bool(result), "positive retained source");
    if (!result) return;
    const auto& id = *result.identity;
    check(id.bankPath.view() == "Content\\Sound\\TV.xwc", "copied path");
    check(id.localIndex == 37 && id.absSliceOffset == 0x123456 &&
          id.length == 0x45678 && id.streamIndex == 1 && id.epoch == 19,
          "logical identity and BE64 extent");
    check(id.voice == Fixture::V && id.frame == Fixture::L &&
          id.container == Fixture::C && id.outerSource == Fixture::O &&
          id.stream == Fixture::S && id.slice == Fixture::Q &&
          id.bankFile == Fixture::F && id.group == Fixture::G &&
          id.record == Fixture::R+96 && id.output == Fixture::R+160,
          "diagnostics refer to validated allocation site");
    const auto ownedCopy = id;
    f.memory.text(Fixture::B, "changed");
    check(ownedCopy.bankPath.view() == "Content\\Sound\\TV.xwc", "no retained guest text");
    check(std::all_of(f.memory.reads.begin(), f.memory.reads.end(),
                     [](const auto& read) { return read.size <= 8; }),
          "only small bounded reader requests");
    // The sparse fixture cannot satisfy a D+56, *out or registry read.
}
struct BadCase {
    const char* name;
    void (*change)(Fixture&);
    Rejection expected;
    bool noReads = false;
};
void rejectionCases() {
    const BadCase cases[] = {
        {"outer vtable", [](Fixture& f){f.memory.word(Fixture::O, 1);}, Rejection::UnexpectedVtable},
        {"stream vtable", [](Fixture& f){f.memory.word(Fixture::S, 1);}, Rejection::UnexpectedVtable},
        {"slice vtable", [](Fixture& f){f.memory.word(Fixture::Q, 1);}, Rejection::UnexpectedVtable},
        {"file vtable", [](Fixture& f){f.memory.word(Fixture::F, 1);}, Rejection::UnexpectedVtable},
        {"CStr vtable", [](Fixture& f){f.memory.word(Fixture::F+8, 1);}, Rejection::UnexpectedVtable},
        {"direct source mode required", [](Fixture& f){f.memory.word(Fixture::S+20, 2);}, Rejection::UnexpectedStreamMode},
        {"source ownership required", [](Fixture& f){f.memory.word(Fixture::S+24, 0);}, Rejection::UnexpectedStreamOwner},
        {"ownership is exactly one", [](Fixture& f){f.memory.word(Fixture::S+24, 2);}, Rejection::UnexpectedStreamOwner},
        {"decoder owner mismatch", [](Fixture& f){f.memory.word(Fixture::V+64, 2);}, Rejection::DecoderOwnerMismatch},
        {"null outer reference", [](Fixture& f){f.memory.word(Fixture::L+88, 0);}, Rejection::NullPointer},
        {"slice observation mismatch", [](Fixture& f){f.memory.word(Fixture::S+16, 0x5100);}, Rejection::RetainedSourceMismatch},
        {"file observation mismatch", [](Fixture& f){f.memory.word(Fixture::Q+12, 0x6100);}, Rejection::RetainedSourceMismatch},
        {"null filename", [](Fixture& f){f.memory.word(Fixture::F+12, 0);}, Rejection::NullPointer},
        {"wide string rejected", [](Fixture& f){f.memory.word(Fixture::B, 0x8001, 2);}, Rejection::UnsupportedString},
        {"unknown string tag", [](Fixture& f){f.memory.word(Fixture::B, 0x2001, 2);}, Rejection::UnsupportedString},
        {"offset high half matters", [](Fixture& f){f.memory.word(Fixture::Q+16, uint64_t{1}<<32, 8);}, Rejection::SliceMismatch},
        {"negative retained length", [](Fixture& f){f.memory.word(Fixture::Q+24, uint64_t(-1), 8);}, Rejection::SliceMismatch},
        {"negative observed offset", [](Fixture& f){f.scope.captured->offset32=0x80000000;}, Rejection::InvalidPod, true},
        {"negative observed length", [](Fixture& f){f.scope.captured->length32=0xffffffff;}, Rejection::InvalidPod, true},
        {"bad local index", [](Fixture& f){f.scope.captured->localIndex=-1;}, Rejection::InvalidPod, true},
        {"wrong opener output", [](Fixture& f){f.scope.captured->openerOut+=4;}, Rejection::OpenerMismatch, true},
        {"wrong expected output", [](Fixture& f){f.scope.expectedOpenerOut+=4;}, Rejection::OpenerMismatch, true},
        {"missing observation", [](Fixture& f){f.scope.captured.reset();}, Rejection::MissingObservation, true},
        {"allocation epoch mismatch", [](Fixture& f){++f.site.epoch;}, Rejection::EpochMismatch, true},
        {"captured epoch mismatch", [](Fixture& f){++f.scope.capturedEpoch;}, Rejection::EpochMismatch, true},
        {"zero epoch", [](Fixture& f){f.scope.epoch=0;}, Rejection::InvalidScope, true},
        {"wrong import return", [](Fixture& f){f.site.lr+=4;}, Rejection::WrongAllocationSite, true},
        {"wrong decoder register", [](Fixture& f){f.site.r16D+=4;}, Rejection::WrongAllocationSite, true},
        {"wrong voice register", [](Fixture& f){f.site.r17V+=4;}, Rejection::WrongAllocationSite, true},
        {"zero streams", [](Fixture& f){f.memory.word(Fixture::G, 0);}, Rejection::InvalidGroup},
        {"too many streams", [](Fixture& f){f.memory.word(Fixture::G, 17);}, Rejection::InvalidGroup},
        {"stream beyond count", [](Fixture& f){f.site.r24StreamIndex=2;}, Rejection::InvalidGroup},
        {"very large stream index", [](Fixture& f){f.site.r24StreamIndex=0xffffffff;}, Rejection::InvalidGroup},
        {"wrong record", [](Fixture& f){f.site.r30Record+=4;}, Rejection::RecordMismatch},
        {"wrong r31 output", [](Fixture& f){f.site.r31Out+=4;}, Rejection::OutputMismatch},
        {"wrong r3 output", [](Fixture& f){f.site.r3Out+=4;}, Rejection::OutputMismatch},
        {"frame arithmetic overflow", [](Fixture& f){f.scope.frame=0xfffffff0;}, Rejection::AddressOverflow, true},
        {"voice arithmetic overflow", [](Fixture& f){f.scope.voice=0xfffffff0;}, Rejection::AddressOverflow, true},
        {"outer arithmetic overflow", [](Fixture& f){f.memory.word(Fixture::L+88, 0xfffffff8); f.memory.word(0xfffffff8, 0x82096E68);}, Rejection::AddressOverflow},
        {"slice field span overflow", [](Fixture& f){f.scope.captured->slice=0xfffffff0; f.memory.word(Fixture::S+16, 0xfffffff0); f.memory.word(0xfffffff0, 0x8209DBA8); f.memory.word(0xfffffffc, Fixture::F);}, Rejection::AddressOverflow},
        {"file field arithmetic overflow", [](Fixture& f){f.scope.captured->bankFile=0xfffffff8; f.memory.word(Fixture::Q+12, 0xfffffff8); f.memory.word(0xfffffff8, 0x820658FC);}, Rejection::AddressOverflow},
        {"group field arithmetic overflow", [](Fixture& f){f.site.r26Group=0xfffffffc; f.memory.word(0xfffffffc, 2);}, Rejection::AddressOverflow},
        {"record array extent overflow", [](Fixture& f){f.memory.word(Fixture::G+8, 0xffffffd0);}, Rejection::AddressOverflow},
        {"filename offset overflow", [](Fixture& f){f.memory.word(Fixture::F+12, 0xfffffffe); f.memory.word(0xfffffffe, 0x4001, 2);}, Rejection::AddressOverflow},
        {"empty bank path", [](Fixture& f){f.memory.text(Fixture::B, "");}, Rejection::InvalidBankPath},
        {"control byte in bank path", [](Fixture& f){f.memory.text(Fixture::B, std::string(1, char(0x1f)));}, Rejection::InvalidBankPath},
        {"high byte in bank path", [](Fixture& f){f.memory.text(Fixture::B, "a"); f.memory.data[Fixture::B+2]=0x80;}, Rejection::InvalidBankPath},
        {"DEL in bank path", [](Fixture& f){f.memory.text(Fixture::B, std::string(1, char(0x7f)));}, Rejection::InvalidBankPath},
        {"bounded nontermination", [](Fixture& f){f.memory.text(Fixture::B, std::string(MaxBankPathBytes, 'a'), false);}, Rejection::UnterminatedBankPath},
        {"truncated bank path", [](Fixture& f){f.memory.text(Fixture::B, "abc", false);}, Rejection::ReadFailed},
    };
    for (const auto& test : cases) {
        Fixture f;
        test.change(f);
        expect(f, test.expected, test.name, test.noReads);
    }
}
void observationState() {
    Fixture f;
    check(recordObservation(f.scope, 19, f.pod) == Rejection::None,
          "identical observation idempotent");
    auto changed = f.pod; ++changed.localIndex;
    check(recordObservation(f.scope, 19, changed) == Rejection::InconsistentObservation,
          "inconsistent observation rejected");
    check(recordObservation(f.scope, 19, f.pod) == Rejection::InconsistentObservation,
          "cannot heal poisoned epoch");
    expect(f, Rejection::InconsistentObservation, "poisoned snapshot", true);

    Fixture epoch;
    check(recordObservation(epoch.scope, 20, epoch.pod) == Rejection::EpochMismatch,
          "observation epoch rejected");
    expect(epoch, Rejection::EpochMismatch, "wrong observation epoch sticks", true);

    Fixture registerMismatch;
    // Observer gates the output slot first, then compares live r3 and r27.
    // Their mismatch cannot be reconstructed from the normalized POD.
    registerMismatch.scope.captured.reset();
    registerMismatch.scope.observationRejection = Rejection::InvalidPod;
    check(recordObservation(registerMismatch.scope, 19, registerMismatch.pod) ==
          Rejection::InvalidPod, "observer register mismatch stays sticky");
    expect(registerMismatch, Rejection::InvalidPod, "observer rejection honored", true);
}
void pathBoundaries() {
    Fixture minimal;
    minimal.memory.text(Fixture::B, "x");
    check(bool(minimal.run()), "two mapped text bytes suffice; no maximum-span read");

    Fixture limit;
    limit.memory.text(Fixture::B, std::string(MaxBankPathBytes-1, '~'));
    const auto result = limit.run();
    check(result && result.identity->bankPath.length == MaxBankPathBytes-1,
          "last allowed byte is terminating NUL");
    check(std::none_of(limit.memory.reads.begin(), limit.memory.reads.end(),
                       [](const auto& r){return r.address == Fixture::B+2+MaxBankPathBytes;}),
          "no read past pathname budget");

    Fixture end;
    end.memory.word(Fixture::F+12, 0xfffffffc);
    end.memory.word(0xfffffffc, 0x4001, 2);
    end.memory.data[0xfffffffe] = 'a';
    end.memory.data[0xffffffff] = 'b';
    expect(end, Rejection::AddressOverflow, "text scan cannot wrap through zero");

    Fixture exactEnd;
    exactEnd.memory.word(Fixture::F+12, 0xfffffffc);
    exactEnd.memory.word(0xfffffffc, 0x4001, 2);
    exactEnd.memory.data[0xfffffffe] = 'a';
    exactEnd.memory.data[0xffffffff] = 0;
    check(bool(exactEnd.run()), "NUL at last u32 address succeeds");

    Fixture maxExtent;
    maxExtent.pod.offset32=0x7fffffff;
    maxExtent.pod.length32=0x7fffffff;
    maxExtent.scope.captured=maxExtent.pod;
    maxExtent.memory.word(Fixture::Q+16, 0x7fffffff, 8);
    maxExtent.memory.word(Fixture::Q+24, 0x7fffffff, 8);
    check(bool(maxExtent.run()), "maximum nonnegative signed extents");
}
void scalarTruncation() {
    Fixture valid;
    check(bool(valid.run()), "truncation baseline");
    size_t tested = 0;
    for (const auto read : valid.memory.reads) {
        if (read.size < 2) continue;
        Fixture broken;
        broken.memory.data.erase(read.address + static_cast<uint32_t>(read.size-1));
        expect(broken, Rejection::ReadFailed, "each scalar read rejects truncation");
        ++tested;
    }
    check(tested >= 16, "exercise retained chain and group scalar reads");
    std::printf("Scalar truncation cases: %zu\n", tested);
}
} // namespace

int main() {
    positiveAndOwnership();
    rejectionCases();
    observationState();
    pathBoundaries();
    scalarTruncation();
    std::printf("XmaSourceIdentity: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
