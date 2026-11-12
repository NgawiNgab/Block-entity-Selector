#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bes::sdk {

/// Reads a typed field out of a raw object pointer plus byte offset.
template <class T>
inline T& field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(object) + offset);
}

template <class T>
inline const T& field(const void* object, std::size_t offset) {
    return *reinterpret_cast<const T*>(reinterpret_cast<std::uintptr_t>(object) + offset);
}

inline void* fieldPointer(const void* object, std::size_t offset) {
    return field<void*>(object, offset);
}

/// A pointer is only ever dereferenced when it looks like a mapped user address.
inline bool plausiblePointer(const void* pointer) {
    const auto value = reinterpret_cast<std::uintptr_t>(pointer);
    return value >= 0x1000 && (value & 0x7) == 0;
}

/// Calls a virtual function by vtable slot index.
template <class Return, class... Args>
inline Return virtualCall(void* instance, std::size_t index, Args&&... args) {
    if (!instance) return Return{};
    auto** table = *reinterpret_cast<void***>(instance);
    if (!table || !table[index]) return Return{};
    auto function = reinterpret_cast<Return (*)(void*, Args...)>(table[index]);
    return function(instance, args...);
}

}
