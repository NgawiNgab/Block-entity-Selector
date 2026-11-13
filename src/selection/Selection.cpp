#include "selection/Selection.hpp"

#include "core/Logging.hpp"

namespace { using bes::core::log::info; }

#include <algorithm>
#include <sstream>

namespace bes::selection {

Selection& Selection::get() {
    static Selection instance;
    return instance;
}

void Selection::set(Target target) {
    std::lock_guard lock(mMutex);
    mTarget = std::move(target);
    if (mTarget.kind != Kind::Entity) mMatchedClasses.clear();
    ++mRevision;

    if (mTarget) {
        info("selection: " + std::string(mTarget.kind == Kind::Block ? "block " : "entity ") +
                  mTarget.id + " (" + mTarget.prettyName + ")");
    } else {
        info("selection: cleared");
    }
}

Target Selection::current() const {
    std::lock_guard lock(mMutex);
    return mTarget;
}

void Selection::clear() {
    set(Target{});
}

bool Selection::active() const {
    std::lock_guard lock(mMutex);
    return static_cast<bool>(mTarget);
}

bool Selection::selectBlock(const std::string& rawIdentifier) {
    auto& index = identifier::ClassIndex::get();
    index.loadDocumentedTable();

    auto entry = identifier::resolve(identifier::Domain::Block, rawIdentifier);
    if (!entry) {
        // Never replace a working selection with a typo.
        info("selection: rejected block identifier \"" + rawIdentifier + "\": " + entry.evidence);
        return false;
    }

    Target next;
    next.kind = Kind::Block;
    next.id = entry.id;
    next.prettyName = entry.id;
    next.confidence = entry.confidence;
    set(std::move(next));
    return true;
}

bool Selection::selectEntity(const std::string& rawIdentifier) {
    auto& index = identifier::ClassIndex::get();
    index.loadDocumentedTable();

    auto entry = identifier::resolve(identifier::Domain::Entity, rawIdentifier);
    if (!entry) {
        info("selection: rejected entity identifier \"" + rawIdentifier + "\": " + entry.evidence);
        return false;
    }

    Target next;
    next.kind = Kind::Entity;
    next.id = entry.id;
    next.prettyName = entry.id;
    next.confidence = entry.confidence;
    next.classes = entry.classes;
    // An unmapped identifier is still selectable: the scanner will find nothing
    // and the diagnostics report will say "no class mapping", which is a much
    // better answer than silently refusing the input.
    set(std::move(next));
    return true;
}

bool Selection::selectEntityFromClass(const std::string& className) {
    if (className.empty()) return false;

    // A live class name is evidence: record it against itself so the index keeps
    // whatever the player picked and the diagnostics dump can show it.
    auto& index = identifier::ClassIndex::get();
    index.loadDocumentedTable();

    Target next;
    next.kind = Kind::Entity;
    // The identifier is the class name as observed; it is not a namespaced id and
    // is not presented as one.
    next.id = className;
    next.prettyName = className;
    next.confidence = identifier::Confidence::ObservedLive;
    next.classes = {className};
    set(std::move(next));
    return true;
}

bool Selection::toggleBlock(const std::string& blockId, const std::string&) {
    const auto canonical = identifier::normalise(blockId, identifier::Domain::Block);
    if (canonical.empty()) return false;
    if (current().kind == Kind::Block && current().id == canonical) {
        clear();
        return false;
    }
    return selectBlock(canonical);
}

bool Selection::toggleEntity(const std::string& entityId, const std::string&) {
    const auto canonical = identifier::normalise(entityId, identifier::Domain::Entity);
    if (canonical.empty()) return false;
    if (current().kind == Kind::Entity && current().id == canonical) {
        clear();
        return false;
    }
    return selectEntity(canonical);
}

void Selection::setMatchedClasses(std::vector<std::string> classes) {
    std::lock_guard lock(mMutex);
    if (mMatchedClasses == classes) return;
    mMatchedClasses = std::move(classes);
    ++mRevision;
}

std::vector<std::string> Selection::matchedClasses() const {
    std::lock_guard lock(mMutex);
    return mMatchedClasses;
}

std::uint64_t Selection::revision() const {
    std::lock_guard lock(mMutex);
    return mRevision;
}

bool entityMatches(const Target& target, const std::string& className) {
    if (target.kind != Kind::Entity || className.empty()) return false;
    if (target.classes.empty() && target.id.empty()) return false;

    // 1:Many: any class bound to the identifier matches. Exact comparison keeps
    // `Zombie` from also matching an unrelated `ZombieHorse`-style subclass
    // unless the caller opted into substring matching.
    for (const auto& mapped : target.classes) {
        if (mapped == className) return true;
    }
    // Backwards compatibility for a target built from a bare class name (the
    // picker path sets both `id` and `classes`, but a saved config may not).
    if (!target.matchBySubstring) {
        return target.classes.empty() && target.id == className;
    }
    for (const auto& mapped : target.classes) {
        if (className.find(mapped) != std::string::npos) return true;
    }
    return target.classes.empty() && className.find(target.id) != std::string::npos;
}

} // namespace bes::selection
