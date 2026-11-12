#pragma once

/// Host-side stand-in for the preloader logger.
///
/// Only used by the tests target, which compiles a small subset of the mod on a
/// desktop toolchain where the preloader SDK is unavailable. The real header
/// (fmt-based, Android log) is used for actual builds.

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace pl::log {

class Logger {
public:
    explicit Logger(std::string name) : mLoggerName(std::move(name)) {}

    static Logger& getOrCreate(std::string name) {
        static std::mutex mutex;
        static std::unordered_map<std::string, std::unique_ptr<Logger>> loggers;
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = loggers.find(name); it != loggers.end()) return *it->second;
        auto logger = std::make_unique<Logger>(std::move(name));
        auto& reference = *logger;
        loggers.emplace(reference.mLoggerName, std::move(logger));
        return reference;
    }

    // The stub ignores format arguments; tests only care that logging is a
    // no-op that never throws.
    template <typename... Args>
    void info(std::string_view format, Args&&...) const {
        emit("I", format);
    }

    template <typename... Args>
    void warn(std::string_view format, Args&&...) const {
        emit("W", format);
    }

    template <typename... Args>
    void error(std::string_view format, Args&&...) const {
        emit("E", format);
    }

    template <typename... Args>
    void debug(std::string_view format, Args&&...) const {
        emit("D", format);
    }

private:
    void emit(const char* level, std::string_view message) const {
        std::fprintf(stderr, "[%s] %s: %.*s\n", level, mLoggerName.c_str(), static_cast<int>(message.size()),
                     message.data());
    }

    std::string mLoggerName;
};

} // namespace pl::log

inline auto& preloaderLogger = pl::log::Logger::getOrCreate("Preloader");
