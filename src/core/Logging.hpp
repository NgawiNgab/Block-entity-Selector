#pragma once

#include <string>
#include <string_view>

namespace bes::core {

/// Thin wrapper over the preloader logger so every subsystem logs under one tag.
namespace log {

void info(std::string_view message);
void warn(std::string_view message);
void error(std::string_view message);
void debug(std::string_view message);

/// Appends to the in-memory diagnostic report that the module dumps on demand.
void report(std::string_view line);
std::string takeReport();

}

}
