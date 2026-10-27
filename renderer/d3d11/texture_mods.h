#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DarkRecomp {
struct ColorImage;
namespace TextureMods {
struct Config {
    std::filesystem::path packDirectory, dumpDirectory;
    size_t sourceQueueBytes = 32u * 1024 * 1024;
    size_t gpuBytes = 128u * 1024 * 1024;
    size_t dumpBytes = 128u * 1024 * 1024;
    unsigned queueEntries = 8, cacheEntries = 256;
};
struct Replacement {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    uint32_t width = 0, height = 0, mipLevels = 0;
    size_t bytes = 0;
};
struct Stats {
    size_t queuedSourceBytes = 0, gpuBytes = 0, dumpedBytes = 0;
    unsigned entries = 0, pending = 0;
    uint64_t finished = 0, ready = 0, rejected = 0;
};
// Content identity includes a version domain, dimensions, codec, authored mip
// count and every owned RGBA byte. Guest pointers and texture IDs are absent.
bool eligibleSource(const ColorImage&) noexcept;
std::string contentKey(const ColorImage&);
std::vector<uint8_t> sourceDds(const ColorImage&);

class Worker {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Worker(ID3D11Device*, const Config&);
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    // Returns the original-path miss until a complete immutable GPU view is
    // ready. Only bounded queue/cache work occurs on this calling thread.
    Replacement request(const std::shared_ptr<const ColorImage>&) noexcept;
    Stats stats() const;
    void stop() noexcept;
};
// Empty configuration allocates nothing, starts no worker and touches no file.
std::unique_ptr<Worker> makeWorker(ID3D11Device*, const Config&);
} // namespace TextureMods
} // namespace DarkRecomp
