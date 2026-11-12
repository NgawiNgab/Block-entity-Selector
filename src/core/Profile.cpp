#include "core/Profile.hpp"

#include "core/Logging.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <unordered_map>

namespace bes::core {

namespace {

/// Appends `existing` as fallbacks for candidates this pass did not supply.
/// Duplicates are dropped: loading the same profile twice (or the bundled file
/// plus an override that repeats a value) must not grow the list or make the
/// resolver try one pattern several times.
template <typename Candidate>
void appendFallbacks(std::vector<Candidate>& loaded, const std::vector<Candidate>& existing) {
    for (const auto& candidate : existing) {
        const bool duplicate = std::any_of(loaded.begin(), loaded.end(), [&](const Candidate& kept) {
            if constexpr (std::is_same_v<Candidate, OffCandidate>) {
                return kept.value == candidate.value;
            } else {
                return kept.pattern == candidate.pattern && kept.slot == candidate.slot &&
                       kept.kind == candidate.kind;
            }
        });
        if (!duplicate) loaded.push_back(candidate);
    }
}

} // namespace
namespace {

constexpr const char* kSigNames[] = {
    "render_level",
    "tessellator_begin",
    "tessellator_color",
    "tessellator_vertex",
    "render_mesh_immediately",
    "actor_manager_list",
    "actor_get_name_tag",
    "actor_is_player",
    "level_get_hit_result",
    "hit_result_get_entity",
    "block_source_get_block",
    "render_material_group",
    "client_instance_update",
    "client_instance_get_local_player",
};

constexpr const char* kOffNames[] = {
    "client_instance_level_renderer",
    "level_renderer_player",
    "level_renderer_player_cam_pos",
    "screen_context_tessellator",
    "screen_context_color_holder",
    "actor_state_vector",
    "actor_aabb_component",
    "actor_level",
    "actor_dimension",
    "dimension_block_source",
    "level_actor_manager",
};

static_assert(std::size(kSigNames) == SigCount);
static_assert(std::size(kOffNames) == OffCount);

/// Parses "0x1B8", "440" or 440 into a number.
bool parseNumeric(const nlohmann::json& value, std::size_t& out) {
    if (value.is_number_unsigned()) {
        out = value.get<std::size_t>();
        return true;
    }
    if (value.is_number_integer()) {
        const auto raw = value.get<long long>();
        if (raw < 0) return false;
        out = static_cast<std::size_t>(raw);
        return true;
    }
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (text.empty()) return false;
        try {
            out = static_cast<std::size_t>(std::stoull(text, nullptr, 0));
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

} // namespace

const char* sigName(SigId id) {
    const auto index = static_cast<std::size_t>(id);
    return index < SigCount ? kSigNames[index] : "unknown";
}

const char* offName(OffId id) {
    const auto index = static_cast<std::size_t>(id);
    return index < OffCount ? kOffNames[index] : "unknown";
}

Profile& Profile::get() {
    static Profile instance;
    return instance;
}

void Profile::clear() {
    for (auto& list : mSignatures) list.clear();
    for (auto& list : mOffsets) list.clear();
    mGameVersion.clear();
    mSource.clear();
    mLoaded = false;
}

const std::vector<SigCandidate>& Profile::candidates(SigId id) const {
    static const std::vector<SigCandidate> empty;
    const auto index = static_cast<std::size_t>(id);
    return index < SigCount ? mSignatures[index] : empty;
}

const std::vector<OffCandidate>& Profile::candidates(OffId id) const {
    static const std::vector<OffCandidate> empty;
    const auto index = static_cast<std::size_t>(id);
    return index < OffCount ? mOffsets[index] : empty;
}

bool Profile::load(const std::filesystem::path& bundled, const std::filesystem::path& overridePath) {
    clear();
    bool any = false;
    if (!bundled.empty()) {
        any = loadFromFile(bundled, true) || any;
    }
    if (!overridePath.empty() && std::filesystem::exists(overridePath)) {
        const bool overrode = loadFromFile(overridePath, false);
        if (overrode) {
            log::info("profile override applied from " + overridePath.string());
        } else {
            log::warn("profile override at " + overridePath.string() + " could not be parsed; using bundled defaults");
        }
        any = any || overrode;
    }
    mLoaded = any;
    if (!any) log::warn("no profile could be loaded; the mod will fall back to RTTI discovery only");
    return any;
}

bool Profile::loadFromFile(const std::filesystem::path& path, bool replaceAll) {
    std::ifstream input(path);
    if (!input) {
        if (replaceAll) log::warn("bundled profile missing at " + path.string());
        return false;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    return loadFromText(buffer.str(), path.string(), replaceAll);
}

bool Profile::loadFromText(std::string_view json, std::string_view originLabel, bool replaceAll) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(json.begin(), json.end());
    } catch (const std::exception& error) {
        log::error(std::string("profile JSON parse failure (") + std::string(originLabel) + "): " + error.what());
        return false;
    }
    if (!document.is_object()) {
        log::error("profile root must be a JSON object (" + std::string(originLabel) + ")");
        return false;
    }

    // In merge mode, remember the candidate counts so an override that changed
    // nothing can be reported as a no-op instead of silently "succeeding".
    std::vector<std::size_t> before;
    if (!replaceAll) {
        before.reserve(SigCount + OffCount);
        for (const auto& list : mSignatures) before.push_back(list.size());
        for (const auto& list : mOffsets) before.push_back(list.size());
    }

    const std::string label(originLabel);
    if (document.contains("game_version") && document["game_version"].is_string()) {
        mGameVersion = document["game_version"].get<std::string>();
    }
    mSource = label;

    // ---- signatures ------------------------------------------------------
    if (const auto it = document.find("signatures"); it != document.end() && it->is_object()) {
        for (const auto& [key, entry] : it->items()) {
            std::size_t idIndex = SigCount;
            for (std::size_t index = 0; index < SigCount; ++index) {
                if (key == kSigNames[index]) {
                    idIndex = index;
                    break;
                }
            }
            if (idIndex == SigCount) {
                log::debug("profile: ignoring unknown signature key " + key);
                continue;
            }
            // Candidates from this document are tried first; in merge mode the
            // previously loaded ones stay behind them as fallbacks. That is what
            // makes dropping in a freshly generated profile non-destructive.
            std::vector<SigCandidate> loaded;

            auto pushCandidate = [&](const nlohmann::json& node) -> bool {
                if (node.is_string()) {
                    SigCandidate candidate;
                    candidate.kind = SigCandidate::Kind::Pattern;
                    candidate.pattern = node.get<std::string>();
                    candidate.origin = label;
                    if (candidate.pattern.empty()) return false;
                    loaded.push_back(std::move(candidate));
                    return true;
                }
                if (!node.is_object()) return false;

                SigCandidate candidate;
                candidate.origin = node.contains("note") && node["note"].is_string()
                    ? label + " (" + node["note"].get<std::string>() + ")"
                    : label;

                if (node.contains("pattern") && node["pattern"].is_string()) {
                    candidate.kind = SigCandidate::Kind::Pattern;
                    candidate.pattern = node["pattern"].get<std::string>();
                    if (candidate.pattern.empty()) return false;
                } else if (node.contains("rtti") && node["rtti"].is_string()) {
                    candidate.kind = SigCandidate::Kind::Vtable;
                    candidate.pattern = node["rtti"].get<std::string>();
                    if (candidate.pattern.empty()) return false;
                    if (node.contains("slot")) {
                        std::size_t slot = 0;
                        if (parseNumeric(node["slot"], slot)) candidate.slot = slot;
                    }
                } else {
                    return false;
                }
                loaded.push_back(std::move(candidate));
                return true;
            };

            // Accepted shapes: a bare string, a bare array of strings/objects,
            // or an object with a "candidates" array.
            if (entry.is_array()) {
                for (const auto& node : entry) pushCandidate(node);
            } else if (entry.contains("candidates") && entry["candidates"].is_array()) {
                for (const auto& node : entry["candidates"]) pushCandidate(node);
            } else {
                pushCandidate(entry);
            }

            auto& target = mSignatures[idIndex];
            if (!replaceAll) appendFallbacks(loaded, target);
            target = std::move(loaded);
        }
    }

    // ---- offsets ---------------------------------------------------------
    if (const auto it = document.find("offsets"); it != document.end() && it->is_object()) {
        for (const auto& [key, entry] : it->items()) {
            std::size_t idIndex = OffCount;
            for (std::size_t index = 0; index < OffCount; ++index) {
                if (key == kOffNames[index]) {
                    idIndex = index;
                    break;
                }
            }
            if (idIndex == OffCount) {
                log::debug("profile: ignoring unknown offset key " + key);
                continue;
            }
            std::vector<OffCandidate> loaded;

            auto pushCandidate = [&](const nlohmann::json& node) -> bool {
                OffCandidate candidate;
                candidate.origin = label;
                if (parseNumeric(node, candidate.value)) {
                    loaded.push_back(std::move(candidate));
                    return true;
                }
                if (!node.is_object()) return false;
                if (node.contains("note") && node["note"].is_string()) {
                    candidate.origin = label + " (" + node["note"].get<std::string>() + ")";
                }
                if (!parseNumeric(node.contains("offset") ? node["offset"] : node, candidate.value)) {
                    return false;
                }
                loaded.push_back(std::move(candidate));
                return true;
            };

            if (entry.is_array()) {
                for (const auto& node : entry) pushCandidate(node);
            } else if (entry.contains("candidates") && entry["candidates"].is_array()) {
                for (const auto& node : entry["candidates"]) pushCandidate(node);
            } else {
                pushCandidate(entry);
            }

            auto& target = mOffsets[idIndex];
            if (!replaceAll) appendFallbacks(loaded, target);
            target = std::move(loaded);
        }
    }

    mLoaded = true;

    if (!replaceAll) {
        std::size_t changed = 0;
        std::size_t index = 0;
        for (const auto& list : mSignatures) {
            if (index >= before.size() || before[index] != list.size()) ++changed;
            ++index;
        }
        for (const auto& list : mOffsets) {
            if (index >= before.size() || before[index] != list.size()) ++changed;
            ++index;
        }
        if (changed == 0) {
            log::warn("profile override from " + label + " defined no new candidates (typo in a key name?)");
        } else {
            log::info("profile override from " + label + ": " + std::to_string(changed) + " entr(ies) updated");
        }
    }
    return true;
}

std::string Profile::describe() const {
    std::ostringstream out;
    out << "profile source: " << (mSource.empty() ? "<none>" : mSource) << '\n';
    out << "game version:   " << (mGameVersion.empty() ? "<unset>" : mGameVersion) << '\n';
    for (std::size_t index = 0; index < SigCount; ++index) {
        out << "  sig " << kSigNames[index] << ": " << mSignatures[index].size() << " candidate(s)\n";
    }
    for (std::size_t index = 0; index < OffCount; ++index) {
        out << "  off " << kOffNames[index] << ":";
        for (const auto& candidate : mOffsets[index]) {
            out << " 0x" << std::hex << candidate.value << std::dec;
        }
        if (mOffsets[index].empty()) out << " <none>";
        out << '\n';
    }
    return out.str();
}

} // namespace bes::core
