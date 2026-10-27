#include "runtime/native/user_data_paths.h"
#include "renderer/d3d11/shader_bytecode_cache.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace DarkRecomp::Native;
namespace fs = std::filesystem;
namespace ShaderCache = DarkRecomp::ShaderBytecodeCache;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Action> static void rejected(Action action, const char* message) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, message);
}
struct Fixture {
    fs::path root, game, profile;
    std::vector<fs::path> files;
    Fixture() {
        root = fs::temp_directory_path() / (L"DarkRecomp-user-data-test-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        check(fs::create_directory(root), "fixture directory already exists");
        game = root / L"game"; profile = root / L"private profile";
        check(fs::create_directory(game), "cannot create fake game directory");
    }
    void write(const fs::path& path, const char* text) {
        std::ofstream output(path, std::ios::binary);
        check(bool(output << text), "cannot write owned fixture");
        files.push_back(path);
    }
    ~Fixture() {
        std::error_code ignored;
        for (auto it = files.rbegin(); it != files.rend(); ++it) fs::remove(*it, ignored);
        // Only remove known files and empty directories created by this fixture.
        for (const auto& path : {profile / L"DarkRecomp.settings.ini", profile / L"build_native/run/cache",
             profile / L"build_native/run", profile / L"build_native", profile / L"saves", profile, game, root})
            fs::remove(path, ignored);
    }
};
static void pathsAndValidation(Fixture& fixture) {
    const auto legacy = resolveUserDataPaths(fixture.game);
    check(!legacy.isolated && legacy.settings == fixture.root / L"DarkRecomp.settings.ini" &&
          legacy.saves == fixture.root / L"saves" && legacy.shaderCache.empty(), "omitted flag changed legacy paths");
    check(legacy.guestCache == fixture.root / L"build_native/run/cache" &&
          legacy.menuCache == fixture.root / L"build_native/run/native-menu" &&
          legacy.developerCache == fixture.root / L"build_native/run/developer-weapons", "legacy cache locations changed");
    prepareUserDataDirectory(legacy);
    check(!fs::exists(legacy.saves) && !fs::exists(legacy.guestCache), "omitted flag created new directories");
    const auto selected = resolveUserDataPaths(fixture.game, fixture.profile);
    check(selected.isolated && selected.root == fixture.profile && !fs::exists(fixture.profile),
          "path selection wrote files or lost the private root");
    prepareUserDataDirectory(selected);
    check(fs::is_directory(selected.saves), "private save directory was not prepared");
    fixture.write(selected.settings, "PRIVATE SETTINGS");
    fixture.write(selected.saves / L"probe.tmp", "PRIVATE SAVE");
    fixture.write(fixture.game / L"sentinel.bin", "READ ONLY GAME");
    check(!fs::exists(legacy.settings) && !fs::exists(legacy.saves), "private selection wrote to legacy settings/saves");
    check(userDataCacheDirectory(fixture.game, selected.menuCache, L"build_native/run/native-menu") == selected.menuCache &&
          userDataCacheDirectory(fixture.game, selected.developerCache, L"build_native/run/developer-weapons") == selected.developerCache &&
          userDataCacheDirectory(fixture.game, {}, L"build_native/run/developer-weapons") == legacy.developerCache,
          "explicit/default asset cache output selection changed");
    rejected([] { userDataDirectoryArgument(L""); }, "empty CLI directory accepted");
    rejected([] { userDataDirectoryArgument(L"--fps"); }, "missing CLI directory consumed the next flag");
    rejected([&] { userDataDirectoryArgument((fixture.profile / L"../escape").wstring()); }, "parent segment accepted");
    const auto blocker = fixture.root / L"blocker";
    fixture.write(blocker, "BLOCKER");
    rejected([&] { resolveUserDataPaths(fixture.game, blocker); }, "ordinary file accepted as private root");
    rejected([&] { resolveUserDataPaths(fixture.game, blocker / L"child"); }, "file ancestor accepted as private root");
    fs::remove(selected.settings);
    check(fs::create_directory(selected.settings), "cannot create bad settings fixture");
    rejected([&] { resolveUserDataPaths(fixture.game, fixture.profile); }, "directory accepted as settings destination");
    fs::remove(selected.settings);
    check(!ordinaryUserDataAttributes(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY) &&
          !ordinaryUserDataAttributes(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_NORMAL) &&
          !ordinaryUserDataAttributes(INVALID_FILE_ATTRIBUTES), "reparse/unreadable attribute accepted");
    wchar_t home[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", home, DWORD(std::size(home)));
    bool checkedExistingReparse = false;
    if (length && length < std::size(home)) {
        for (const auto* suffix : {L"Local Settings", L"Application Data", L"AppData/Local/Application Data"}) {
            const auto path = fs::path(home) / suffix;
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                rejected([&] { userDataDirectoryArgument(path.wstring()); }, "existing profile reparse root accepted");
                check(!ordinaryUserDataPath(path / L"child"), "existing reparse ancestor accepted");
                checkedExistingReparse = true; break;
            }
        }
    }
    std::printf("Existing OS reparse fixture checked=%u; no links created.\n", unsigned(checkedExistingReparse));
    std::ifstream sentinel(fixture.game / L"sentinel.bin", std::ios::binary);
    check(std::string(std::istreambuf_iterator<char>(sentinel), std::istreambuf_iterator<char>()) == "READ ONLY GAME",
          "game sentinel changed");
}
static void privateShaderFailure(Fixture& fixture) {
    const auto privateCache = fixture.profile / L"DarkRecompShaderCache";
    ShaderCache::setDefaultDirectoryOverride(privateCache);
    // Simulate a destination becoming unusable after startup validation.
    fixture.write(privateCache, "BLOCKED CACHE");
    const auto& cache = ShaderCache::defaultCache();
    check(cache.path(123).parent_path() == privateCache, "private shader cache used a shared fallback");
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    check(SUCCEEDED(D3DCreateBlob(4, &blob)), "CPU-only byte blob allocation failed");
    check(!cache.save(123, blob.Get()), "private shader cache wrote through a blocked directory");
    check(cache.path(456).parent_path() == privateCache, "failed private cache switched to a shared directory");
    rejected([&] { ShaderCache::setDefaultDirectoryOverride(fixture.root / L"other"); }, "late shader directory change accepted");
}
int main() {
    try {
        Fixture fixture;
        pathsAndValidation(fixture);
        privateShaderFailure(fixture);
        std::puts("User data paths: default/private roots, asset cache routing, invalid/reparse destinations and shader no-fallback passed; CPU only.");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
