#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bes::core {

/// Monotonic milliseconds since an arbitrary epoch.
///
/// Lives here rather than in each translation unit because two subsystems now
/// timestamp the same event: the picker stamps a message and the frame hook
/// decides whether it has expired. Two independent clocks would disagree.
std::uint64_t nowMilliseconds();

/// Thin wrapper over the preloader logger so every subsystem logs under one tag.
namespace log {

void info(std::string_view message);
void warn(std::string_view message);
void error(std::string_view message);
void debug(std::string_view message);

/// Appends to the in-memory diagnostic report that the module dumps on demand.
void report(std::string_view line);

/// Returns the report and clears it.
///
/// Only for callers that own the buffer outright. A diagnostics dump must NOT use
/// this: taking the report empties it, so the second dump would show nothing that
/// happened before the first one and startup lines would be lost exactly when
/// they are needed.
std::string takeReport();

/// Returns the report without clearing it. This is what a diagnostics dump uses,
/// so repeated dumps stay cumulative.
std::string peekReport();

}

}
