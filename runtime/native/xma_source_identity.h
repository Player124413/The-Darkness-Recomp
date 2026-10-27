#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace DarkRecomp::Native::XmaSourceIdentity {

// Native11, streamed 827D92E0 path only. The caller must keep the original
// L+88 -> O -> Q -> F ownership alive and establish that sampled fields stay
// stable while reading. Audit02 proves retention/initial copies, NOT complete
// concurrent immutability. No new guest/global lock is justified by this API;
// without a stability contract, snapshots are diagnostic only. This is NOT a
// lifetime/immutability proof,
// a context owner, an allocation-generation manager, or an audio selector.
// Reader is a callable bool(uint32_t, std::span<uint8_t>): false on any
// inaccessible byte. Reader must not throw. No guest bytes are written.

enum class Rejection : uint8_t {
    None, InvalidScope, EpochMismatch, MissingObservation,
    InconsistentObservation, OpenerMismatch, InvalidPod, WrongAllocationSite,
    AddressOverflow, ReadFailed, NullPointer, UnexpectedVtable,
    DecoderOwnerMismatch, UnexpectedStreamMode, UnexpectedStreamOwner,
    RetainedSourceMismatch, SliceMismatch, UnsupportedString,
    InvalidBankPath, UnterminatedBankPath, InvalidGroup,
    RecordMismatch, OutputMismatch
};

struct OpenerPod {
    uint32_t container = 0; // Opaque this; never dereferenced.
    int32_t localIndex = -1;
    uint32_t slice = 0; // Observer first gates openerOut, then requires r3 == r27.
    uint32_t bankFile = 0;
    uint32_t offset32 = 0;
    uint32_t length32 = 0;
    uint32_t openerOut = 0; // Original getter's r16, expected L+104.
    bool operator==(const OpenerPod&) const = default;
};

struct Scope {
    uint32_t voice = 0; // V
    uint32_t frame = 0; // L, after the original 320-byte loader prologue.
    uint64_t epoch = 0; // Fresh nonzero host load epoch.
    uint32_t expectedOpenerOut = 0; // Must equal checked L+104.
    std::optional<OpenerPod> captured;
    uint64_t capturedEpoch = 0;
    Rejection observationRejection = Rejection::None; // Sticky until new Scope.
};

struct AllocationSite {
    uint64_t epoch = 0; // Active scope epoch at the allocation boundary.
    uint32_t lr = 0;
    uint32_t r16D = 0;
    uint32_t r17V = 0;
    uint32_t r24StreamIndex = 0;
    uint32_t r26Group = 0;
    uint32_t r30Record = 0;
    uint32_t r31Out = 0;
    uint32_t r3Out = 0;
};

// Total read budget for a pathname, INCLUDING its required NUL.
// Thus at most 1023 printable ASCII characters are accepted.
inline constexpr size_t MaxBankPathBytes = 1024;
struct BankPath {
    std::array<char, MaxBankPathBytes> bytes{};
    uint16_t length = 0;
    std::string_view view() const noexcept { return {bytes.data(), length}; }
};

struct Identity {
    BankPath bankPath; // Owned copy, never a view into guest memory.
    int32_t localIndex = -1;
    uint64_t absSliceOffset = 0;
    uint64_t length = 0;
    uint32_t streamIndex = 0;
    uint64_t epoch = 0;
    // Opaque diagnostic addresses only; do not dereference after the scope.
    uint32_t voice = 0, frame = 0, container = 0, outerSource = 0;
    uint32_t stream = 0, slice = 0, bankFile = 0, group = 0;
    uint32_t record = 0, output = 0;
};

struct SnapshotResult {
    Rejection rejection = Rejection::None;
    std::optional<Identity> identity;
    explicit operator bool() const noexcept {
        return rejection == Rejection::None && identity.has_value();
    }
};

namespace Detail {
inline bool add(uint32_t base, uint32_t offset, uint32_t& result) noexcept {
    const uint64_t sum = uint64_t(base) + offset;
    if (sum > std::numeric_limits<uint32_t>::max()) return false;
    result = static_cast<uint32_t>(sum);
    return true;
}
inline bool spanFits(uint32_t address, size_t bytes) noexcept {
    return bytes <= (uint64_t{1} << 32) - uint64_t(address);
}
inline Rejection scopeLayout(const Scope& scope) noexcept {
    if (!scope.voice || !scope.frame || !scope.epoch)
        return Rejection::InvalidScope;
    uint32_t expected = 0, decoder = 0, owner = 0;
    if (!add(scope.frame, 104, expected) || !spanFits(expected, 4) ||
        !add(scope.frame, 88, owner) || !spanFits(owner, 4) ||
        !add(scope.voice, 64, decoder) || !spanFits(decoder, 4))
        return Rejection::AddressOverflow;
    return scope.expectedOpenerOut == expected
        ? Rejection::None : Rejection::OpenerMismatch;
}
inline Rejection podLayout(const Scope& scope, const OpenerPod& pod) noexcept {
    if (pod.openerOut != scope.expectedOpenerOut)
        return Rejection::OpenerMismatch;
    if (!pod.container || pod.localIndex < 0 || !pod.slice || !pod.bankFile ||
        pod.offset32 > 0x7fffffffu || pod.length32 > 0x7fffffffu)
        return Rejection::InvalidPod; // Original constructor sign-extends.
    return Rejection::None;
}

template<class Reader> struct GuestReader {
    Reader& readBytes;
    Rejection error = Rejection::None;

    bool bytes(uint32_t base, uint32_t offset, std::span<uint8_t> destination) noexcept {
        uint32_t address = 0;
        if (!add(base, offset, address) || !spanFits(address, destination.size())) {
            error = Rejection::AddressOverflow;
            return false;
        }
        if (!readBytes(address, destination)) {
            error = Rejection::ReadFailed;
            return false;
        }
        return true;
    }
    bool be16(uint32_t base, uint32_t offset, uint16_t& value) noexcept {
        std::array<uint8_t, 2> data{};
        if (!bytes(base, offset, data)) return false;
        value = uint16_t((uint16_t(data[0]) << 8) | data[1]);
        return true;
    }
    bool be32(uint32_t base, uint32_t offset, uint32_t& value) noexcept {
        std::array<uint8_t, 4> data{};
        if (!bytes(base, offset, data)) return false;
        value = 0;
        for (const auto b : data) value = (value << 8) | b;
        return true;
    }
    bool be64(uint32_t base, uint32_t offset, uint64_t& value) noexcept {
        std::array<uint8_t, 8> data{};
        if (!bytes(base, offset, data)) return false;
        value = 0;
        for (const auto b : data) value = (value << 8) | b;
        return true;
    }
    bool pointer(uint32_t base, uint32_t offset, uint32_t& value) noexcept {
        if (!be32(base, offset, value)) return false;
        if (!value) { error = Rejection::NullPointer; return false; }
        return true;
    }
    bool word(uint32_t base, uint32_t offset, uint32_t expected,
              Rejection mismatch) noexcept {
        uint32_t value = 0;
        if (!be32(base, offset, value)) return false;
        if (value != expected) { error = mismatch; return false; }
        return true;
    }
};
} // namespace Detail

// Feed only an observation belonging to this scoped opener. At 827BD27C the
// observer must first gate openerOut==expectedOpenerOut, then require r3==r27.
// Latch observationRejection=InvalidPod on that register mismatch, without
// capturing a POD. This pure API receives only the normalized slice address.
// Wrong epochs,
// wrong output slots and inconsistent repeated observations poison this Scope.
// Identical repeats are idempotent. No guest memory is accessed here.
inline Rejection recordObservation(Scope& scope, uint64_t epoch,
                                   const OpenerPod& pod) noexcept {
    if (scope.observationRejection != Rejection::None)
        return scope.observationRejection;
    auto reject = [&](Rejection why) {
        scope.observationRejection = why;
        return why;
    };
    if (const auto why = Detail::scopeLayout(scope); why != Rejection::None)
        return reject(why);
    if (epoch != scope.epoch) return reject(Rejection::EpochMismatch);
    if (scope.captured && (scope.capturedEpoch != epoch || *scope.captured != pod))
        return reject(Rejection::InconsistentObservation);
    if (const auto why = Detail::podLayout(scope, pod); why != Rejection::None)
        return reject(why);
    scope.captured = pod;
    scope.capturedEpoch = epoch;
    return Rejection::None;
}

// Snapshot before XMACreateContext publishes its output. This does NOT read
// D+56 (the group is not published there yet) or *r3Out (context not written).
// On success Root must bind to the fresh Memory allocation generation under
// its ownership synchronization; epoch alone is not an allocation generation.
template<class Reader>
SnapshotResult snapshot(const Scope& scope, const AllocationSite& site,
                        Reader&& readBytes) noexcept {
    auto reject = [](Rejection why) -> SnapshotResult { return {why, std::nullopt}; };
    if (scope.observationRejection != Rejection::None)
        return reject(scope.observationRejection);
    if (const auto why = Detail::scopeLayout(scope); why != Rejection::None)
        return reject(why);
    if (site.epoch != scope.epoch || (scope.captured &&
        scope.capturedEpoch != scope.epoch)) return reject(Rejection::EpochMismatch);
    if (!scope.captured) return reject(Rejection::MissingObservation);
    const auto& pod = *scope.captured;
    if (const auto why = Detail::podLayout(scope, pod); why != Rejection::None)
        return reject(why);
    uint32_t decoder = 0;
    if (!Detail::add(scope.voice, 64, decoder))
        return reject(Rejection::AddressOverflow);
    if (site.lr != 0x828B1898u || site.r16D != decoder || site.r17V != scope.voice)
        return reject(Rejection::WrongAllocationSite);

    Detail::GuestReader<std::remove_reference_t<Reader>> memory{readBytes};
    if (!memory.word(decoder, 0, scope.voice, Rejection::DecoderOwnerMismatch))
        return reject(memory.error);
    uint32_t outer = 0, stream = 0, slice = 0, file = 0;
    if (!memory.pointer(scope.frame, 88, outer) ||
        !memory.word(outer, 0, 0x82096E68u, Rejection::UnexpectedVtable))
        return reject(memory.error);
    if (!Detail::add(outer, 8, stream)) return reject(Rejection::AddressOverflow);
    if (!memory.word(stream, 0, 0x820658FCu, Rejection::UnexpectedVtable) ||
        !memory.word(stream, 20, 0, Rejection::UnexpectedStreamMode) ||
        !memory.word(stream, 24, 1, Rejection::UnexpectedStreamOwner) ||
        !memory.pointer(stream, 16, slice)) return reject(memory.error);
    if (slice != pod.slice) return reject(Rejection::RetainedSourceMismatch);
    if (!memory.word(slice, 0, 0x8209DBA8u, Rejection::UnexpectedVtable) ||
        !memory.pointer(slice, 12, file)) return reject(memory.error);
    if (file != pod.bankFile) return reject(Rejection::RetainedSourceMismatch);
    if (!memory.word(file, 0, 0x820658FCu, Rejection::UnexpectedVtable))
        return reject(memory.error);
    uint64_t offset = 0, length = 0;
    if (!memory.be64(slice, 16, offset) || !memory.be64(slice, 24, length))
        return reject(memory.error);
    // Negative original int32 values were rejected by podLayout. Equality in
    // 64 bits rejects mismatched upper halves or a negative retained extent.
    if (offset != uint64_t(pod.offset32) || length != uint64_t(pod.length32))
        return reject(Rejection::SliceMismatch);

    if (!site.r26Group) return reject(Rejection::NullPointer);
    uint32_t count = 0, records = 0, record = 0, output = 0;
    if (!memory.be32(site.r26Group, 0, count) ||
        !memory.pointer(site.r26Group, 8, records)) return reject(memory.error);
    if (!count || count > 16 || site.r24StreamIndex >= count)
        return reject(Rejection::InvalidGroup);
    // Count is bounded before multiplication. Check the declared array's
    // address extent, but do not read its records or future output storage.
    if (!Detail::spanFits(records, size_t(count) * 96) ||
        !Detail::add(records, site.r24StreamIndex * 96, record) ||
        !Detail::add(record, 64, output) || !Detail::spanFits(output, 4))
        return reject(Rejection::AddressOverflow);
    if (record != site.r30Record) return reject(Rejection::RecordMismatch);
    if (output != site.r31Out || output != site.r3Out)
        return reject(Rejection::OutputMismatch);

    uint32_t backing = 0;
    uint16_t tag = 0;
    if (!memory.word(file, 8, 0x82065568u, Rejection::UnexpectedVtable) ||
        !memory.pointer(file, 12, backing) || !memory.be16(backing, 0, tag))
        return reject(memory.error);
    if (tag & 0xa000u) return reject(Rejection::UnsupportedString);
    Identity identity;
    bool terminated = false;
    for (uint32_t i = 0; i < MaxBankPathBytes; ++i) {
        std::array<uint8_t, 1> byte{};
        uint32_t text = 0;
        if (!Detail::add(backing, 2, text)) return reject(Rejection::AddressOverflow);
        if (!memory.bytes(text, i, byte)) return reject(memory.error);
        if (!byte[0]) {
            if (!i) return reject(Rejection::InvalidBankPath);
            identity.bankPath.length = static_cast<uint16_t>(i);
            terminated = true;
            break;
        }
        if (byte[0] < 0x20 || byte[0] > 0x7e)
            return reject(Rejection::InvalidBankPath);
        identity.bankPath.bytes[i] = static_cast<char>(byte[0]);
    }
    if (!terminated) return reject(Rejection::UnterminatedBankPath);
    identity.localIndex = pod.localIndex;
    identity.absSliceOffset = offset;
    identity.length = length;
    identity.streamIndex = site.r24StreamIndex;
    identity.epoch = scope.epoch;
    identity.voice = scope.voice;
    identity.frame = scope.frame;
    identity.container = pod.container;
    identity.outerSource = outer;
    identity.stream = stream;
    identity.slice = slice;
    identity.bankFile = file;
    identity.group = site.r26Group;
    identity.record = record;
    identity.output = output;
    return {Rejection::None, identity};
}
} // namespace DarkRecomp::Native::XmaSourceIdentity
