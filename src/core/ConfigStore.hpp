#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace bes::core {

/// Debounced JSON persistence for the mod's config.
///
/// The launcher can write config values at a high rate (sliders), so writes are
/// coalesced on a worker thread exactly like BedrockTools does it.
class ConfigStore {
public:
    static ConfigStore& get();

    void setPath(std::filesystem::path path);
    const std::filesystem::path& path() const noexcept { return mPath; }

    /// Loads the file if it exists and hands the document to the callback.
    void load(const std::function<void(const nlohmann::json&)>& apply);

    /// Queues a write of the given document (coalesced, ~150 ms debounce).
    void save(const nlohmann::json& document);

    /// Blocks until the pending write has landed.
    void flush();

private:
    ConfigStore() = default;
    ~ConfigStore();

    void ensureWorker();

    std::mutex mMutex;
    std::condition_variable mCondition;
    std::thread mWorker;
    std::filesystem::path mPath;
    std::string mPendingPayload;
    bool mHasPending = false;
    bool mStopping = false;
    std::uint64_t mQueuedGeneration = 0;
    std::uint64_t mCompletedGeneration = 0;
    std::chrono::steady_clock::time_point mDeadline{};
};

}
