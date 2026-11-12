#include "core/ConfigStore.hpp"

#include "core/Logging.hpp"

#include <fstream>
#include <sstream>

namespace bes::core {
namespace {

constexpr auto kDebounce = std::chrono::milliseconds(150);

void writeAtomically(const std::filesystem::path& path, const std::string& payload) {
    std::error_code error;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
    }

    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return;
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.flush();
        if (!output) return;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error) std::filesystem::remove(temporary, error);
    }
}

} // namespace

ConfigStore& ConfigStore::get() {
    static ConfigStore instance;
    return instance;
}

ConfigStore::~ConfigStore() {
    flush();
    {
        std::lock_guard lock(mMutex);
        mStopping = true;
    }
    mCondition.notify_all();
    if (mWorker.joinable()) mWorker.join();
}

void ConfigStore::setPath(std::filesystem::path path) {
    std::lock_guard lock(mMutex);
    mPath = std::move(path);
}

void ConfigStore::load(const std::function<void(const nlohmann::json&)>& apply) {
    std::filesystem::path path;
    {
        std::lock_guard lock(mMutex);
        path = mPath;
    }
    if (path.empty() || !std::filesystem::exists(path)) {
        log::info("config: no existing file at " + path.string());
        return;
    }

    try {
        std::ifstream input(path);
        if (!input) return;
        nlohmann::json document;
        input >> document;
        if (apply) apply(document);
        log::info("config: loaded " + path.string());
    } catch (const std::exception& error) {
        log::warn(std::string("config: failed to read ") + path.string() + ": " + error.what());
    }
}

void ConfigStore::ensureWorker() {
    if (!mWorker.joinable()) mWorker = std::thread([this] {
        std::unique_lock lock(mMutex);
        while (!mStopping) {
            mCondition.wait(lock, [this] { return mStopping || mHasPending; });
            if (mStopping && !mHasPending) break;

            const auto generation = mQueuedGeneration;
            const auto deadline = mDeadline;
            if (!mStopping &&
                mCondition.wait_until(lock, deadline,
                                      [&] { return mStopping || mQueuedGeneration != generation; })) {
                continue; // A newer value arrived; keep batching.
            }

            const std::string payload = std::move(mPendingPayload);
            mPendingPayload.clear();
            const auto path = mPath;
            mHasPending = false;
            lock.unlock();
            if (!path.empty()) writeAtomically(path, payload);
            lock.lock();
            if (mCompletedGeneration < generation) mCompletedGeneration = generation;
            mCondition.notify_all();
        }
    });
}

void ConfigStore::save(const nlohmann::json& document) {
    const std::string payload = document.dump(2);
    {
        std::lock_guard lock(mMutex);
        if (mStopping) return;
        ensureWorker();
        mPendingPayload = payload;
        mDeadline = std::chrono::steady_clock::now() + kDebounce;
        mHasPending = true;
        ++mQueuedGeneration;
    }
    mCondition.notify_all();
}

void ConfigStore::flush() {
    std::unique_lock lock(mMutex);
    if (!mWorker.joinable() || !mHasPending) return;
    const auto target = mQueuedGeneration;
    mDeadline = std::chrono::steady_clock::now();
    mCondition.notify_all();
    mCondition.wait(lock, [&] { return mCompletedGeneration >= target || mStopping; });
}

} // namespace bes::core
