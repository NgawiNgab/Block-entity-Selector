#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bes::core::rtti {

/// Cheap sanity check that a pointer looks like a real vtable before reading
/// its RTTI slots.
bool plausibleVtable(void* const* vtable);

/// Optional mapping predicate, called before the typeinfo and the name pointer
/// are dereferenced.
///
/// The mod installs one backed by AddressSpace so that a garbage vptr produces
/// an empty name instead of a fault; the host tests leave it null and work on
/// real C++ objects. This is a hook rather than a direct AddressSpace call so
/// Rtti.cpp keeps no dependency on the loader.
using TypeInfoReadable = bool (*)(const void* address, std::size_t size);

extern TypeInfoReadable gTypeInfoReadable;

/// Returns the mangled typeinfo name ("12ZombieActor") stored in an object's
/// vtable, or an empty string when the object has no usable RTTI.
std::string mangledName(const void* object);

/// Demangles an Itanium-ABI name. Returns the input when demangling fails.
std::string demangle(std::string_view mangled);

/// Best-effort readable class name:
///  - nested names are reduced to the innermost component
///  - the leading length prefix is stripped when demangling is unavailable
std::string className(const void* object);

/// Same as className, but from an already-mangled name.
std::string classNameFromMangled(std::string_view mangled);

/// Heuristic actor categorisation, used to keep the type filter meaningful.
enum class ActorKind : std::uint8_t {
    Unknown,
    Mob,
    Player,
    Item,
    Projectile,
    Other,
};

ActorKind classifyActor(const std::string& className);

const char* kindName(ActorKind kind);

/// Enumerates RTTI typeinfo names reachable from an object's class hierarchy.
/// Used by the diagnostics dump to show what the mod is matching against.
std::string describeHierarchy(const void* object);

}
