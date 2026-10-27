#pragma once
#include <windows.h>
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace DarkRecomp::Native {
class VideoModReadLease;
// A read pin prevents payload writes/deletes. Guest read sharing is tracked
// separately; each guest open still receives an independent file position.
class VideoModFile {
public:
    ~VideoModFile();
    VideoModFile(const VideoModFile&) = delete;
    VideoModFile& operator=(const VideoModFile&) = delete;
    const std::filesystem::path path;
    const uint64_t bytes;
    bool firstOpen() const { return !opened_.test_and_set(std::memory_order_relaxed); }
private:
    friend class VideoModPack;
    friend class VideoModReadLease;
    VideoModFile(std::filesystem::path value, uint64_t size, HANDLE pin)
        : path(std::move(value)), bytes(size), pin_(pin) {}
    HANDLE pin_ = INVALID_HANDLE_VALUE;
    mutable std::atomic_flag opened_ = ATOMIC_FLAG_INIT;
    mutable std::mutex sharingMutex_;
    mutable size_t readers_ = 0, denyReaders_ = 0;
};

class VideoModReadLease {
public:
    static std::shared_ptr<const VideoModReadLease> acquire(
        std::shared_ptr<const VideoModFile> file, uint32_t access, uint32_t sharing);
    ~VideoModReadLease();
    VideoModReadLease(const VideoModReadLease&) = delete;
    VideoModReadLease& operator=(const VideoModReadLease&) = delete;
private:
    explicit VideoModReadLease(std::shared_ptr<const VideoModFile> file) : file_(std::move(file)) {}
    std::shared_ptr<const VideoModFile> file_;
    bool active_ = false, deniesRead_ = false;
};

class VideoModPack {
public:
    // An empty directory disables replacement. Invalid packs throw before any
    // entry is published; Memory catches the error and retains stock media.
    static std::shared_ptr<const VideoModPack> load(
        const std::filesystem::path& gameDirectory,
        const std::filesystem::path& packDirectory);
    std::shared_ptr<const VideoModFile> find(const std::filesystem::path& original) const;
    size_t size() const { return entries_.size(); }
private:
    std::filesystem::path gameDirectory_;
    std::map<std::string, std::shared_ptr<const VideoModFile>> entries_;
};
}
