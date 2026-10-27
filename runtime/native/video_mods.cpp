#include "video_mods.h"
#include "user_data_paths.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace DarkRecomp::Native {
namespace {
constexpr uint64_t maxVideoBytes = 2ull * 1024 * 1024 * 1024;
constexpr size_t maxManifestBytes = 64 * 1024, maxEntries = 128;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void require(bool good, const char* error) { if (!good) throw std::runtime_error(error); }
std::string lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
bool component(std::string_view value) {
    if (value.empty() || value.size() > 128 || value == "." || value == ".." || value.back() == '.') return false;
    for (unsigned char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    const auto stem = lower(std::string(value.substr(0, value.find('.'))));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
    return !(stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
             stem[3] >= '1' && stem[3] <= '9');
}
enum class Container { Wmv, Theora };
std::pair<std::string, Container> target(std::string value) {
    require(value.size() <= 240, "Video target is too long");
    std::replace(value.begin(), value.end(), '\\', '/');
    value = lower(std::move(value));
    size_t begin = 0;
    for (;;) {
        const auto end = value.find('/', begin);
        require(component(std::string_view(value).substr(begin, end == std::string::npos ? end : end - begin)),
                "Video target contains an invalid path component");
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (value.starts_with("content/videos/wmv/") && value.ends_with(".wmv")) return {value, Container::Wmv};
    if (value.starts_with("content/videos/theora/") && value.ends_with(".ogg")) return {value, Container::Theora};
    throw std::runtime_error("Video target codec directory and suffix must agree");
}
std::vector<uint8_t> readPrefix(const std::filesystem::path& path, size_t maximum,
                              uint64_t* size = nullptr) {
    require(ordinaryUserDataPath(path, UserDataPathKind::File), "Video file or ancestor is not ordinary");
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    require(file.value != INVALID_HANDLE_VALUE, "Cannot open video pack file");
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(file.value, &info) && ordinaryUserDataAttributes(info.dwFileAttributes) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY), "Video pack entry is not an ordinary file");
    const uint64_t length = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size) *size = length;
    std::vector<uint8_t> bytes(size_t((std::min)(length, uint64_t(maximum))));
    DWORD count = 0;
    require(bytes.empty() || (ReadFile(file.value, bytes.data(), DWORD(bytes.size()), &count, nullptr) &&
                             count == bytes.size()), "Cannot read video pack file");
    return bytes;
}
bool signature(const std::vector<uint8_t>& bytes, Container kind) {
    if (kind == Container::Wmv) {
        constexpr std::array<uint8_t, 16> asf{0x30,0x26,0xb2,0x75,0x8e,0x66,0xcf,0x11,0xa6,0xd9,0,0xaa,0,0x62,0xce,0x6c};
        return bytes.size() >= asf.size() && std::equal(asf.begin(), asf.end(), bytes.begin());
    }
    if (bytes.size() < 27 || std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) != "OggS" ||
        bytes[4] != 0 || !(bytes[5] & 2) || !bytes[26]) return false;
    const size_t header = 27 + bytes[26];
    return bytes.size() >= header + 7 && bytes[27] >= 7 && bytes[header] == 0x80 &&
        std::string_view(reinterpret_cast<const char*>(bytes.data() + header + 1), 6) == "theora";
}
}

VideoModFile::~VideoModFile() { if (pin_ != INVALID_HANDLE_VALUE) CloseHandle(pin_); }

std::shared_ptr<const VideoModReadLease> VideoModReadLease::acquire(
    std::shared_ptr<const VideoModFile> file, uint32_t access, uint32_t sharing) {
    auto lease = std::shared_ptr<VideoModReadLease>(new VideoModReadLease(std::move(file)));
    // Attribute-only access does not participate in Windows read sharing.
    const bool reads = (access & (GENERIC_READ | GENERIC_EXECUTE | FILE_READ_DATA | FILE_EXECUTE)) != 0;
    if (!reads) return lease;
    std::lock_guard lock(lease->file_->sharingMutex_);
    const bool denies = !(sharing & FILE_SHARE_READ);
    if (lease->file_->denyReaders_ || (denies && lease->file_->readers_)) return {};
    ++lease->file_->readers_;
    if (denies) ++lease->file_->denyReaders_;
    lease->deniesRead_ = denies;
    lease->active_ = true;
    return lease;
}
VideoModReadLease::~VideoModReadLease() {
    if (!active_) return;
    std::lock_guard lock(file_->sharingMutex_);
    --file_->readers_;
    if (deniesRead_) --file_->denyReaders_;
}

std::shared_ptr<const VideoModPack> VideoModPack::load(
    const std::filesystem::path& gameDirectory, const std::filesystem::path& packDirectory) {
    if (packDirectory.empty()) return {};
    require(packDirectory.is_absolute() && ordinaryUserDataPath(packDirectory, UserDataPathKind::Directory),
            "Video pack requires an ordinary absolute directory");
    auto pack = std::make_shared<VideoModPack>();
    pack->gameDirectory_ = std::filesystem::weakly_canonical(gameDirectory);
    const auto root = packDirectory.lexically_normal();
    uint64_t manifestSize = 0;
    const auto manifest = readPrefix(root / "videos.tsv", maxManifestBytes + 1, &manifestSize);
    require(manifestSize <= maxManifestBytes && !manifest.empty() && manifest.back() == '\n',
            "Video manifest must be complete and at most 64 KiB");
    require(std::find(manifest.begin(), manifest.end(), 0) == manifest.end(), "Video manifest contains NUL");
    const std::string text(manifest.begin(), manifest.end());
    size_t begin = 0;
    bool first = true;
    while (begin < text.size()) {
        const auto end = text.find('\n', begin);
        std::string_view line(text.data() + begin, end - begin);
        if (line.ends_with('\r')) line.remove_suffix(1);
        begin = end + 1;
        if (first) { require(line == "DARKRECOMP_VIDEO_V1", "Unsupported video manifest header"); first = false; continue; }
        require(pack->entries_.size() < maxEntries, "Too many video entries");
        const auto a = line.find('\t'), b = a == std::string_view::npos ? a : line.find('\t', a + 1);
        require(a != std::string_view::npos && b != std::string_view::npos && line.find('\t', b + 1) == std::string_view::npos,
                "Video manifest expects target, payload and decimal size");
        const auto [key, kind] = target(std::string(line.substr(0, a)));
        const auto payload = line.substr(a + 1, b - a - 1), number = line.substr(b + 1);
        require(component(payload) && lower(std::string(payload)).ends_with(kind == Container::Wmv ? ".wmv" : ".ogg"),
                "Video payload must be a plain basename of the matching container");
        require(!number.empty() && std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; }),
                "Video size must be decimal");
        uint64_t expected = 0;
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), expected);
        require(parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size() && expected && expected <= maxVideoBytes,
                "Video size must be 1 byte through 2 GiB");
        require(!pack->entries_.contains(key), "Duplicate video target");
        require(signature(readPrefix(pack->gameDirectory_ / std::filesystem::path(key), 65536), kind),
                "Original video is missing or has a different container");
        const auto path = root / std::filesystem::path(payload);
        require(ordinaryUserDataPath(path, UserDataPathKind::File), "Video payload or ancestor is not ordinary");
        Handle pin{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        require(pin.value != INVALID_HANDLE_VALUE, "Cannot pin video payload against writes/deletes");
        uint64_t actual = 0;
        require(signature(readPrefix(path, 65536, &actual), kind) && actual == expected,
                "Video payload size or container does not match the manifest");
        auto file = std::shared_ptr<const VideoModFile>(new VideoModFile(path, actual, pin.value));
        pin.value = INVALID_HANDLE_VALUE;
        pack->entries_.emplace(key, std::move(file));
    }
    require(!pack->entries_.empty(), "Video pack is empty");
    return pack;
}

std::shared_ptr<const VideoModFile> VideoModPack::find(const std::filesystem::path& original) const {
    const auto relative = original.lexically_relative(gameDirectory_).generic_string();
    if (relative.empty() || relative.size() > 240) return {};
    auto key = lower(relative);
    std::replace(key.begin(), key.end(), '\\', '/');
    const auto entry = entries_.find(key);
    return entry == entries_.end() ? nullptr : entry->second;
}
}
