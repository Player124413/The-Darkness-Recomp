#include "runtime/native/video_mods.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

using namespace DarkRecomp::Native;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::vector<uint8_t> theora(size_t bytes) {
    std::vector<uint8_t> result(bytes, 7);
    std::memcpy(result.data(), "OggS\0", 5); result[5] = 2; result[26] = 1; result[27] = 42;
    result[28] = 0x80; std::memcpy(result.data() + 29, "theora", 6);
    return result;
}
void write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    check(!std::filesystem::exists(path), "Fixture must be new");
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    check(bool(out), "Cannot write fixture");
}
void text(const std::filesystem::path& path, const std::string& value) {
    write(path, std::vector<uint8_t>(value.begin(), value.end()));
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::strcmp(argv[1], "--pack") == 0) {
            auto pack = VideoModPack::load(argv[2], argv[3]);
            const auto file = pack->find(std::filesystem::path(argv[2]) / argv[4]);
            check(bool(file), "Imported pack did not select its original target");
            auto lease = VideoModReadLease::acquire(file, GENERIC_READ, 0);
            check(bool(lease), "Imported pack could not acquire a guest read");
            Handle read{CreateFileW(file->path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
            LARGE_INTEGER size{};
            check(read.value != INVALID_HANDLE_VALUE && GetFileSizeEx(read.value, &size) && uint64_t(size.QuadPart) == file->bytes,
                  "Imported payload could not be opened at its declared size");
            std::printf("Imported pack: selected target, %llu bytes, pinned guest read passed.\n", static_cast<unsigned long long>(file->bytes));
            return 0;
        }
        check(argc == 1, "Unexpected video pack test arguments");
        wchar_t temporary[MAX_PATH]{};
        check(GetTempPathW(MAX_PATH, temporary) != 0, "Temp directory unavailable");
        const auto root = std::filesystem::path(temporary) / (L"DarkVideoModsTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        check(std::filesystem::create_directory(root), "New fixture root required");
        const auto game = root / "game", source = game / "Content/Videos/Theora/TV/Test.ogg";
        write(source, theora(128));
        const auto makePack = [&](const char* label, const std::string& lines, size_t payloadBytes = 256) {
            const auto pack = root / label; std::filesystem::create_directory(pack);
            write(pack / "video.ogg", theora(payloadBytes));
            text(pack / "videos.tsv", "DARKRECOMP_VIDEO_V1\n" + lines);
            return pack;
        };
        const auto valid = makePack("valid", "Content/Videos/Theora/TV/Test.ogg\tvideo.ogg\t256\n");
        auto pack = VideoModPack::load(game, valid);
        check(pack && pack->size() == 1, "Valid pack rejected");
        auto selected = pack->find(source);
        check(selected && selected->bytes == 256 && selected->path == valid / "video.ogg", "Original target did not select its payload");
        check(pack->find(game / "CONTENT/VIDEOS/THEORA/TV/TEST.OGG") == selected, "Case-insensitive target missed");
        check(!pack->find(game / "Content/Videos/Theora/TV/Other.ogg"), "Unlisted original was replaced");
        check(!pack->find(root / "outside.ogg"), "Outside path acquired a replacement");
        check(!VideoModPack::load(game, {}), "Empty pack path did not disable replacement");
        {
            auto exclusive = VideoModReadLease::acquire(selected, GENERIC_READ, 0);
            check(bool(exclusive), "Initial exclusive guest read failed");
            Handle actual{CreateFileW(selected->path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
            check(actual.value != INVALID_HANDLE_VALUE, "Pinned payload could not supply independent guest read handle");
            check(!VideoModReadLease::acquire(selected, GENERIC_READ, FILE_SHARE_READ), "Exclusive guest read allowed another reader");
            check(bool(VideoModReadLease::acquire(selected, FILE_READ_ATTRIBUTES, 0)), "Exclusive guest read blocked attribute access");
        }
        {
            auto a = VideoModReadLease::acquire(selected, GENERIC_READ, FILE_SHARE_READ);
            auto b = VideoModReadLease::acquire(selected, GENERIC_READ, FILE_SHARE_READ);
            check(a && b && !VideoModReadLease::acquire(selected, GENERIC_READ, 0), "Shared readers or exclusive conflict handling failed");
        }
        const auto locked = [&] {
            Handle writer{CreateFileW(selected->path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr)};
            check(writer.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "Payload remained writable during playback");
            Handle deleter{CreateFileW(selected->path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr)};
            check(deleter.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "Payload deletion/rename remained available during playback");
        };
        locked(); pack.reset(); locked(); selected.reset();
        {
            Handle writer{CreateFileW((valid / "video.ogg").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
            check(writer.value != INVALID_HANDLE_VALUE, "File pin leaked after the final owner released it");
        }
        const auto rejected = [&](const std::filesystem::path& path) {
            bool failed = false; try { VideoModPack::load(game, path); } catch (const std::exception&) { failed = true; }
            check(failed, "Invalid pack was published");
        };
        rejected(makePack("wrong-size", "content/videos/theora/tv/test.ogg\tvideo.ogg\t257\n"));
        rejected(makePack("unsafe-payload", "content/videos/theora/tv/test.ogg\t../video.ogg\t256\n"));
        rejected(makePack("wrong-container", "content/videos/wmv/test.wmv\tvideo.ogg\t256\n"));
        const auto duplicate = makePack("duplicate", "content/videos/theora/tv/test.ogg\tvideo.ogg\t256\nCONTENT/VIDEOS/THEORA/TV/TEST.OGG\tvideo.ogg\t256\n");
        rejected(duplicate);
        {
            Handle writer{CreateFileW((duplicate / "video.ogg").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
            check(writer.value != INVALID_HANDLE_VALUE, "Rejected pack leaked a partially published entry");
        }
        const auto reparse = root / "linked-pack";
        if (CreateSymbolicLinkW(reparse.c_str(), valid.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 2)) {
            rejected(reparse);
            puts("Video packs: reparse-directory refusal exercised.");
        } else puts("Video packs: symlink creation unavailable; reparse test not exercised.");
        std::ifstream in(source, std::ios::binary);
        std::vector<uint8_t> original((std::istreambuf_iterator<char>(in)), {});
        check(original == theora(128), "Original video fixture changed");
        std::printf("Video packs: selection, sizes, sharing, immutable lifetime, rejection rollback and original preservation passed. Fixtures retained: %ls\n", root.c_str());
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "Video packs: %s\n", error.what()); return 1; }
}
