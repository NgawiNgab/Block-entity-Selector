#include "core/Rtti.hpp"

#include <cxxabi.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bes::core::rtti {
namespace {

/// Itanium C++ ABI vtable layout, low address to high:
///
///   vtable[-2]  offset-to-top        (0 for a primary vtable)
///   vtable[-1]  address of typeinfo  <- this is the one
///   vtable[0]   first virtual function
///
/// This was -2, which reads offset-to-top. That is zero for every primary
/// vtable, so `rawName()` bailed out at the null check and `className()`
/// returned "" for every object in the game. Nothing failed loudly: every RTTI
/// gate in World.cpp simply never passed, which is why `offsets proven` sat at
/// 0/11 with the whole chain blocked and no error anywhere to explain it.
constexpr std::ptrdiff_t kTypeInfoSlot = -1;
/// std::type_info::__type_name in libc++ (a const char*).
constexpr std::size_t kTypeInfoNameOffset = 8;

/// Reads one length-prefixed identifier, e.g. "6Zombie" -> "Zombie".
bool readIdentifier(const char* text, std::size_t& pos, std::string& out) {
    std::size_t length = 0;
    std::size_t digits = 0;
    while (std::isdigit(static_cast<unsigned char>(text[pos]))) {
        length = length * 10 + static_cast<std::size_t>(text[pos] - '0');
        ++pos;
        if (++digits > 6) return false;
    }
    if (digits == 0 || length == 0) return false;
    if (std::strlen(text + pos) < length) return false;
    out.append(text + pos, length);
    pos += length;
    return true;
}

/// Skips a balanced <...> group; `pos` must point at '<'.
bool skipTemplate(const char* text, std::size_t& pos) {
    if (text[pos] != '<') return false;
    int depth = 0;
    while (text[pos]) {
        if (text[pos] == '<') {
            ++depth;
        } else if (text[pos] == '>') {
            if (--depth == 0) {
                ++pos;
                return true;
            }
        }
        ++pos;
    }
    return false;
}

/// Walks a nested-name mangling and keeps the last identifier seen, which is the
/// innermost class name: "N6bedrock5Actor6ZombieE" -> "Zombie".
std::string parseNested(const char* text, std::size_t& pos) {
    std::string result;
    while (text[pos] != '\0' && text[pos] != 'E') {
        if (text[pos] == 'N') {
            ++pos;
            const auto nested = parseNested(text, pos);
            if (!nested.empty()) result = nested;
            continue;
        }
        if (text[pos] == 'I') {
            if (!skipTemplate(text, pos)) break;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(text[pos]))) {
            std::string identifier;
            if (!readIdentifier(text, pos, identifier)) break;
            result = identifier;
            continue;
        }
        ++pos; // Stray substitution / qualifier character: skip defensively.
    }
    return result;
}

/// Best-effort read of a typeinfo name. Typeinfo objects for classes are plain
/// length-prefixed names, which is all this mod ever has to handle; nested and
/// templated forms are parsed without needing a full demangler, since
/// __cxa_demangle rejects bare typeinfo names ("12X" is not an unambiguous
/// mangled symbol).
std::string parseTypeinfoName(std::string_view mangled) {
    if (mangled.empty()) return {};

    // Accept both "-ZTS<name>" and the bare name libc++ stores in type_info.
    if (mangled.starts_with("_ZTS")) mangled.remove_prefix(4);
    else if (mangled.starts_with("_Z")) mangled.remove_prefix(2);

    // A leading '*' marks a type with internal linkage (applied by the compiler
    // to keep the name from being merged across translation units), so
    // "N12_GLOBAL__N_112rtti_fixture8TriangleE" arrives as
    // "*N12_GLOBAL__N_112rtti_fixture8TriangleE". Without this the first character
    // is neither a digit nor 'N', every parse fails, and the class reports as
    // nameless -- which then reads as "no RTTI" rather than "unparsed".
    if (mangled.starts_with("*")) mangled.remove_prefix(1);

    const std::string text(mangled);
    std::size_t position = 0;
    if (text.empty()) return {};

    if (text[0] == 'N') {
        ++position;
        auto nested = parseNested(text.c_str(), position);
        if (!nested.empty()) return nested;
    }

    std::string identifier;
    if (readIdentifier(text.c_str(), position, identifier)) {
        // A template argument list may follow the identifier; the name is enough.
        return identifier;
    }
    return {};
}

std::string_view rawName(const void* object) {
    if (!object) return {};

    // A polymorphic object stores its vptr at offset 0, so the object itself is
    // always 8-byte aligned. Without this check a one-byte or int-only object --
    // or any interior pointer -- is read as eight bytes of "vptr" that may happen
    // to look aligned, and then vtable[-1] is read from an address nowhere near a
    // vtable. That is an out-of-bounds read, not a failed guess.
    if ((reinterpret_cast<std::uintptr_t>(object) & 0x7) != 0) return {};

    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    if (!plausibleVtable(vtable)) return {};

    const auto* typeInfo = reinterpret_cast<const std::uintptr_t*>(vtable[kTypeInfoSlot]);
    if (!typeInfo) return {};

    // A game object can be reached with a plausible vptr whose typeinfo slot
    // holds something that is not a typeinfo at all -- a wrong object, a
    // partially initialised one, or an unrelated struct that happens to start
    // with a code pointer. Both the typeinfo and its name field are dereferenced
    // below, so both are checked first; an unchecked read here faults the game
    // on data we chose to trust. When no predicate is installed (host tests) the
    // check is skipped and the caller is operating on real C++ objects.
    const auto readable = [](const void* address, std::size_t size) {
        if (gTypeInfoReadable && !gTypeInfoReadable(address, size)) return false;
        return true;
    };
    if (!readable(typeInfo, kTypeInfoNameOffset + sizeof(void*))) return {};

    const auto nameOffset = reinterpret_cast<const std::byte*>(typeInfo) + kTypeInfoNameOffset;
    if (!readable(nameOffset, sizeof(void*))) return {};
    const auto namePointer = *reinterpret_cast<const char* const*>(nameOffset);
    if (!namePointer) return {};
    // One byte is all that decides between "" and a readable name, and a bad
    // pointer is far more likely than an empty typeinfo name.
    if (!readable(namePointer, 1)) return {};
    return std::string_view(namePointer);
}

} // namespace

/// Zero unless the runtime installs the address-space-backed predicate.
///
/// Defined here, at namespace scope, rather than inside the anonymous namespace
/// above: internal linkage there would leave the extern declaration in the header
/// permanently unresolved. The link error is the only symptom, and it appears at
/// the very end of a build rather than at the point of the mistake.
TypeInfoReadable gTypeInfoReadable = nullptr;

bool plausibleVtable(void* const* vtable) {
    const auto value = reinterpret_cast<std::uintptr_t>(vtable);
    return value >= 0x1000 && (value & 0x7) == 0;
}

std::string mangledName(const void* object) {
    const auto name = rawName(object);
    return name.empty() ? std::string{} : std::string(name);
}

std::string demangle(std::string_view mangled) {
    if (mangled.empty()) return {};
    // Typeinfo names are not always prefixed with _Z; libc++ stores the bare
    // mangled form, so re-add the prefix when it is missing.
    std::string symbol;
    if (mangled.rfind("_Z", 0) == 0) symbol.assign(mangled);
    else symbol = "_Z" + std::string(mangled);

    int status = 0;
    char* demangled = abi::__cxa_demangle(symbol.c_str(), nullptr, nullptr, &status);
    if (status != 0 || !demangled) {
        std::free(demangled);
        return std::string(mangled);
    }
    std::string result(demangled);
    std::free(demangled);
    return result;
}

std::string classNameFromMangled(std::string_view mangled) {
    std::string readable = parseTypeinfoName(mangled);
    if (readable.empty()) {
        // Fall back to the real demangler for shapes the small parser refuses.
        readable = demangle(mangled);
        if (readable == mangled) return {};
    }

    // Drop any template arguments the demangler kept, and namespace qualifiers.
    if (const auto templateStart = readable.find('<'); templateStart != std::string::npos) {
        readable.resize(templateStart);
    }
    if (const auto marker = readable.find("(anonymous namespace)::"); marker != std::string::npos) {
        readable.erase(marker, std::strlen("(anonymous namespace)::"));
    }
    while (!readable.empty() && (readable.front() == ' ' || readable.front() == ':')) readable.erase(readable.begin());
    while (!readable.empty() && (readable.back() == ' ' || readable.back() == ':')) readable.pop_back();
    if (const auto scope = readable.rfind("::"); scope != std::string::npos && scope + 2 < readable.size()) {
        readable = readable.substr(scope + 2);
    }
    return readable;
}

std::string className(const void* object) {
    return classNameFromMangled(mangledName(object));
}

ActorKind classifyActor(const std::string& className) {
    if (className.empty()) return ActorKind::Unknown;
    if (className == "Player" || className == "LocalPlayer" || className == "ServerPlayer" ||
        className == "RemotePlayer") {
        return ActorKind::Player;
    }
    if (className.find("ItemActor") != std::string::npos || className == "Item" ||
        className.find("ExperienceOrb") != std::string::npos) {
        return ActorKind::Item;
    }
    if (className.find("Arrow") != std::string::npos || className.find("Snowball") != std::string::npos ||
        className.find("Egg") != std::string::npos || className.find("Fireball") != std::string::npos ||
        className.find("Projectile") != std::string::npos || className.find("Thrown") != std::string::npos ||
        className.find("FishingHook") != std::string::npos || className.find("Firework") != std::string::npos ||
        className.find("Boat") != std::string::npos || className.find("Minecart") != std::string::npos) {
        return ActorKind::Projectile;
    }
    // The Mob / Monster / Animal families cover every living entity in Bedrock.
    if (className.find("Mob") != std::string::npos || className.find("Monster") != std::string::npos ||
        className.find("Animal") != std::string::npos) {
        return ActorKind::Mob;
    }
    return ActorKind::Other;
}

const char* kindName(ActorKind kind) {
    switch (kind) {
        case ActorKind::Mob: return "mob";
        case ActorKind::Player: return "player";
        case ActorKind::Item: return "item";
        case ActorKind::Projectile: return "projectile";
        case ActorKind::Other: return "other";
        case ActorKind::Unknown: break;
    }
    return "unknown";
}

std::string describeHierarchy(const void* object) {
    const auto mangled = mangledName(object);
    if (mangled.empty()) return "<no rtti>";
    return demangle(mangled);
}

} // namespace bes::core::rtti
