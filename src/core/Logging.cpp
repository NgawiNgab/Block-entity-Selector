#include "core/Logging.hpp"

#include <bes/Version.hpp>

#include <pl/Logger.hpp>

#include <mutex>
#include <string>

namespace bes::core::log {
namespace {

std::mutex reportMutex;
std::string reportBuffer;

} // namespace

void info(std::string_view message) {
    pl::log::Logger::getOrCreate(std::string(bes::ShortName)).info("{}", message);
    report(message);
}

void warn(std::string_view message) {
    pl::log::Logger::getOrCreate(std::string(bes::ShortName)).warn("{}", message);
    report(std::string("[warn] ") + std::string(message));
}

void error(std::string_view message) {
    pl::log::Logger::getOrCreate(std::string(bes::ShortName)).error("{}", message);
    report(std::string("[error] ") + std::string(message));
}

void debug(std::string_view message) {
    pl::log::Logger::getOrCreate(std::string(bes::ShortName)).debug("{}", message);
}

void report(std::string_view line) {
    std::lock_guard lock(reportMutex);
    if (!reportBuffer.empty()) reportBuffer.push_back('\n');
    reportBuffer.append(line.data(), line.size());
    // Keep the buffer bounded; the interesting part is always the tail.
    constexpr std::size_t kMaxReportBytes = 64 * 1024;
    if (reportBuffer.size() > kMaxReportBytes) {
        reportBuffer.erase(0, reportBuffer.size() - kMaxReportBytes);
    }
}

std::string takeReport() {
    std::lock_guard lock(reportMutex);
    std::string copy = reportBuffer;
    reportBuffer.clear();
    return copy;
}

} // namespace bes::core::log
