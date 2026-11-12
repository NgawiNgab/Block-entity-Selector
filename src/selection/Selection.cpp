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

bool Selection::toggleBlock(const std::string& blockId, const std::string& prettyName) {
    if (blockId.empty()) return false;
    {
        std::lock_guard lock(mMutex);
        if (mTarget.kind == Kind::Block && mTarget.id == blockId) {
            mTarget.clear();
            mMatchedClasses.clear();
            ++mRevision;
            info("selection: cleared (toggled " + blockId + ")");
            return false;
        }
    }
    Target next;
    next.kind = Kind::Block;
    next.id = blockId;
    next.prettyName = prettyName.empty() ? blockId : prettyName;
    set(std::move(next));
    return true;
}

bool Selection::toggleEntity(const std::string& className, const std::string& prettyName) {
    if (className.empty()) return false;
    {
        std::lock_guard lock(mMutex);
        if (mTarget.kind == Kind::Entity && mTarget.id == className) {
            mTarget.clear();
            mMatchedClasses.clear();
            ++mRevision;
            info("selection: cleared (toggled " + className + ")");
            return false;
        }
    }
    Target next;
    next.kind = Kind::Entity;
    next.id = className;
    next.prettyName = prettyName.empty() ? className : prettyName;
    next.matchBySubstring = false;
    set(std::move(next));
    return true;
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
    if (target.kind != Kind::Entity || target.id.empty() || className.empty()) return false;
    if (target.id == className) return true;
    if (!target.matchBySubstring) return false;
    return className.find(target.id) != std::string::npos;
}

} // namespace bes::selection
