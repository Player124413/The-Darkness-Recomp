#pragma once
#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace DarkRecomp::Native {
enum class UserDataPathKind { Any, Directory, File };
inline bool ordinaryUserDataAttributes(DWORD attributes) noexcept {
    return attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE));
}
// Inspect each existing ancestor without following reparse entries. Missing
// destinations are permitted; an existing parent must be an ordinary directory.
inline bool ordinaryUserDataPath(const std::filesystem::path& path,
                                UserDataPathKind kind = UserDataPathKind::Any) {
    if (!path.is_absolute()) return false;
    for (const auto& component : path) if (component == L"..") return false;
    for (auto current = path; !current.empty();) {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        } else {
            if (!ordinaryUserDataAttributes(attributes)) return false;
            const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if ((current != path || kind == UserDataPathKind::Directory) && !directory) return false;
            if (current == path && kind == UserDataPathKind::File && directory) return false;
        }
        auto parent = current.parent_path();
        if (parent == current) break;
        current = std::move(parent);
    }
    return true;
}
inline void requireOrdinaryUserDataPath(const std::filesystem::path& path,
                                      UserDataPathKind kind = UserDataPathKind::Any) {
    if (!ordinaryUserDataPath(path, kind))
        throw std::runtime_error("User-data destination or ancestor is not an ordinary path");
}
inline void validateUserDataDirectory(const std::filesystem::path& root) {
    requireOrdinaryUserDataPath(root, UserDataPathKind::Directory);
    const DWORD rootAttributes = GetFileAttributesW(root.c_str());
    if (rootAttributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
        throw std::runtime_error("User-data root cannot be inspected");
    }
    if (!ordinaryUserDataAttributes(rootAttributes) || !(rootAttributes & FILE_ATTRIBUTE_DIRECTORY))
        throw std::runtime_error("User-data root is not an ordinary directory");
    std::vector<std::pair<std::filesystem::path, unsigned>> pending{{root, 0}};
    size_t visited = 0;
    while (!pending.empty()) {
        auto [directory, depth] = std::move(pending.back()); pending.pop_back();
        if (depth > 128) throw std::runtime_error("User-data validation depth exceeded");
        requireOrdinaryUserDataPath(directory, UserDataPathKind::Directory);
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (++visited > 100000) throw std::runtime_error("User-data validation entry bound exceeded");
            const DWORD attributes = GetFileAttributesW(entry.path().c_str());
            if (!ordinaryUserDataAttributes(attributes))
                throw std::runtime_error("User-data directory contains a reparse or unreadable entry");
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) pending.emplace_back(entry.path(), depth + 1);
        }
    }
}
inline std::filesystem::path userDataDirectoryArgument(std::wstring_view value) {
    if (value.empty() || value.starts_with(L"--"))
        throw std::runtime_error("--user-data-dir requires a directory");
    std::filesystem::path requested(value);
    for (const auto& component : requested)
        if (component == L"..") throw std::runtime_error("--user-data-dir cannot contain parent segments");
    auto root = std::filesystem::absolute(requested).lexically_normal();
    validateUserDataDirectory(root);
    return root;
}
struct UserDataPaths {
    bool isolated = false;
    std::filesystem::path root, settings, saves, guestCache, menuCache, developerCache, shaderCache;
};
// Callers retain their existing game spelling: native_main uses absolute(),
// while Memory uses its canonical directory. Omitted-flag behavior is unchanged.
inline UserDataPaths resolveUserDataPaths(const std::filesystem::path& gameDirectory,
                                         const std::filesystem::path& requestedRoot = {}) {
    UserDataPaths result;
    result.isolated = !requestedRoot.empty();
    result.root = result.isolated ? userDataDirectoryArgument(requestedRoot.wstring()) : gameDirectory.parent_path();
    result.settings = result.root / L"DarkRecomp.settings.ini";
    result.saves = result.root / L"saves";
    result.guestCache = result.root / L"build_native/run/cache";
    result.menuCache = result.root / L"build_native/run/native-menu";
    result.developerCache = result.root / L"build_native/run/developer-weapons";
    if (result.isolated) {
        result.shaderCache = result.root / L"DarkRecompShaderCache";
        requireOrdinaryUserDataPath(result.settings, UserDataPathKind::File);
        for (const auto& directory : {result.saves, result.guestCache, result.menuCache, result.developerCache, result.shaderCache})
            requireOrdinaryUserDataPath(directory, UserDataPathKind::Directory);
    }
    return result;
}
inline void prepareUserDataDirectory(const UserDataPaths& paths) {
    if (!paths.isolated) return;
    validateUserDataDirectory(paths.root);
    std::filesystem::create_directories(paths.saves);
    validateUserDataDirectory(paths.root);
}
inline std::filesystem::path userDataCacheDirectory(const std::filesystem::path& gameDirectory,
                                                   const std::filesystem::path& requestedDirectory,
                                                   const std::filesystem::path& legacySuffix) {
    if (requestedDirectory.empty()) return gameDirectory.parent_path() / legacySuffix;
    requireOrdinaryUserDataPath(requestedDirectory, UserDataPathKind::Directory);
    return requestedDirectory;
}
}
