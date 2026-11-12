#include "core/Resolver.hpp"

#include "core/Logging.hpp"

#include <pl/memory/Signature.hpp>
#include <pl/memory/Vtable.hpp>

#include <array>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace bes::core {

Resolver& Resolver::get() {
    static Resolver instance;
    return instance;
}

bool Resolver::resolveAll(std::string_view libraryName) {
    if (mResolved) return true;

    const auto& profile = Profile::get();
    if (!profile.loaded()) {
        log::warn("resolver: no profile loaded, every signature will be unresolved");
    }

    std::vector<std::string> patterns;
    std::vector<SigId> patternOwners;
    for (std::size_t index = 0; index < SigCount; ++index) {
        const auto id = static_cast<SigId>(index);
        for (const auto& candidate : profile.candidates(id)) {
            if (candidate.kind != SigCandidate::Kind::Pattern) continue;
            if (candidate.pattern.empty()) continue;
            patterns.push_back(candidate.pattern);
            patternOwners.push_back(id);
        }
    }

    std::unordered_map<std::string, std::uintptr_t> matches;
    if (!patterns.empty()) {
        try {
            matches = pl::memory::resolveSignatures(patterns, libraryName);
        } catch (...) {
            log::warn("resolver: signature scan threw, falling back to per-candidate resolution");
        }
    }

    std::size_t resolvedCount = 0;
    for (std::size_t index = 0; index < SigCount; ++index) {
        const auto id = static_cast<SigId>(index);
        mAddresses[index] = 0;
        mOrigins[index].clear();

        // Pattern candidates are resolved in profile order: first hit wins.
        for (const auto& candidate : profile.candidates(id)) {
            if (candidate.kind != SigCandidate::Kind::Pattern) continue;
            const auto it = matches.find(candidate.pattern);
            if (it != matches.end() && it->second) {
                mAddresses[index] = it->second;
                mOrigins[index] = candidate.origin + " [pattern]";
                break;
            }
        }
        if (mAddresses[index]) {
            ++resolvedCount;
            continue;
        }

        // Vtable candidates are resolved directly through RTTI.
        for (const auto& candidate : profile.candidates(id)) {
            if (candidate.kind != SigCandidate::Kind::Vtable) continue;
            const auto address = resolveVtableCandidate(candidate, libraryName);
            if (address) {
                mAddresses[index] = address;
                mOrigins[index] = candidate.origin + " [vtable]";
                break;
            }
        }
        if (mAddresses[index]) ++resolvedCount;
    }

    for (std::size_t index = 0; index < OffCount; ++index) {
        const auto id = static_cast<OffId>(index);
        const auto& candidates = profile.candidates(id);
        mOffsets[index] = candidates.empty() ? 0 : candidates.front().value;
        mOffOrigins[index] = candidates.empty() ? std::string{} : candidates.front().origin;
    }

    mResolved = true;
    refreshSummary();

    std::ostringstream message;
    message << "resolver: " << resolvedCount << '/' << SigCount << " signatures resolved";
    if (resolvedCount == 0) message << " (no profile match for this game build)";
    log::info(message.str());
    return resolvedCount > 0;
}

std::uintptr_t Resolver::resolveVtableCandidate(const SigCandidate& candidate, std::string_view libraryName) {
    const std::string library(libraryName);

    // Accept "12ZombieActor", "_ZTS12ZombieActor", "ZombieActor" and fully
    // nested mangled forms; pl::memory::resolveVtableFunction handles the rest.
    std::array<std::string, 3> forms{candidate.pattern, candidate.pattern, candidate.pattern};
    if (candidate.pattern.rfind("_ZTS", 0) != 0) {
        forms[1] = "_ZTS" + candidate.pattern;
    }
    if (!candidate.pattern.empty() && std::isdigit(static_cast<unsigned char>(candidate.pattern.front()))) {
        forms[2] = candidate.pattern;
    } else if (const auto scope = candidate.pattern.rfind("::"); scope != std::string::npos) {
        const auto leaf = candidate.pattern.substr(scope + 2);
        forms[2] = std::to_string(leaf.size()) + leaf;
    } else {
        forms[2] = std::to_string(candidate.pattern.size()) + candidate.pattern;
    }

    for (const auto& form : forms) {
        if (form.empty()) continue;
        try {
            const auto address = pl::memory::resolveVtableFunction(form, candidate.slot, library);
            if (address) return address;
        } catch (...) {
            // Ignore and try the next spelling.
        }
    }
    return 0;
}

std::uintptr_t Resolver::address(SigId id) const {
    const auto index = static_cast<std::size_t>(id);
    return index < SigCount ? mAddresses[index] : 0;
}

std::size_t Resolver::offset(OffId id) const {
    const auto index = static_cast<std::size_t>(id);
    return index < OffCount ? mOffsets[index] : 0;
}

bool Resolver::hasOffsetCandidates(OffId id) const {
    return !Profile::get().candidates(id).empty();
}

void Resolver::confirmOffset(OffId id, std::size_t value, std::string_view evidence) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= OffCount) return;
    if (mOffsets[index] == value) return;

    std::ostringstream message;
    message << "resolver: offset " << offName(id) << " -> 0x" << std::hex << value << std::dec
            << " (evidence: " << evidence << ')';
    log::info(message.str());

    mOffsets[index] = value;
    mOffOrigins[index] = std::string(evidence);
    refreshSummary();
}

void Resolver::refreshSummary() {
    std::ostringstream out;
    for (std::size_t index = 0; index < SigCount; ++index) {
        const auto id = static_cast<SigId>(index);
        out << "  fn " << sigName(id) << " = 0x" << std::hex << mAddresses[index] << std::dec;
        if (!mOrigins[index].empty()) out << "  <- " << mOrigins[index];
        out << '\n';
    }
    for (std::size_t index = 0; index < OffCount; ++index) {
        const auto id = static_cast<OffId>(index);
        out << "  off " << offName(id) << " = 0x" << std::hex << mOffsets[index] << std::dec;
        if (!mOffOrigins[index].empty()) out << "  <- " << mOffOrigins[index];
        out << '\n';
    }
    mSummary = out.str();
}

} // namespace bes::core
