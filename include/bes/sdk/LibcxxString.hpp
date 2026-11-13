#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace bes::sdk {

/// Reads a libc++ `std::string` out of raw memory.
///
/// The mod has to read strings that the game owns, so it cannot use
/// `std::string`'s own accessors: it must know the object layout. libc++ is 24
/// bytes and selects between two forms with the low bit of byte 0:
///
///   small: `(byte0 & 1) == 0`; size is `byte0 >> 1`; bytes are inline at `+1`
///   long : `(byte0 & 1) == 1`; the pair at `+8` is {size, data pointer}
///
/// The long-form pair is what run 11 of the RE log recorded at `0x10d6b16c`
/// (`ldrb w8,[x1]`, `ldp x10,x9,[x1,#0x8]`, `tst w8,#0x1`) and the small form
/// is the `add x10,x0,#0x8` / `csel` idiom in the same window.
///
/// Nothing here is assumed: every field is validated before it is trusted, so a
/// wrong guess returns an empty string rather than reading wild memory.
///
/// `readable` lets the caller supply its own pointer test. The default accepts
/// any aligned, non-null, plausibly-user-space address, which is deliberately
/// permissive: these checks are a sanity filter, not a guarantee of mapping.
inline bool defaultReadable(std::uintptr_t value) {
    return value >= 0x1000 && value < 0x0000'8000'0000'0000ull && (value & 0x7) == 0;
}

/// Returns the string stored at `address`, or an empty string when the bytes
/// there do not form a self-consistent libc++ string of at most `limit` bytes.
template <class Readable>
std::string readLibcxxString(const void* address, std::size_t limit, Readable&& readable) {
    const auto base = reinterpret_cast<std::uintptr_t>(address);
    if (!readable(base)) return {};

    const auto* const raw = static_cast<const unsigned char*>(address);
    const auto header = static_cast<unsigned>(raw[0]);

    if ((header & 0x1u) == 0) {
        // Small-string form.
        const auto size = static_cast<std::size_t>(header >> 1);
        if (size < 2 || size > limit || size > 22) return {};
        return std::string(reinterpret_cast<const char*>(raw + 1), size);
    }

    // Long form: {size, data} as one pair at +8.
    std::uint64_t size = 0;
    std::uint64_t data = 0;
    std::memcpy(&size, raw + 8, sizeof(size));
    std::memcpy(&data, raw + 16, sizeof(data));
    if (size < 2 || size > limit) return {};
    if (!readable(static_cast<std::uintptr_t>(data))) return {};

    const auto* const bytes = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(data));
    return std::string(bytes, static_cast<std::size_t>(size));
}

/// Name the custom-predicate entry point explicitly: a two-argument overload set
/// would make `readLibcxxString(p, 64)` bind the integer to `Readable`.
template <class Readable>
std::string readLibcxxStringWith(const void* address, std::size_t limit, Readable&& readable) {
    return readLibcxxString(address, limit, static_cast<Readable&&>(readable));
}

inline std::string readLibcxxString(const void* address, std::size_t limit = 128) {
    return readLibcxxString(address, limit, &defaultReadable);
}

} // namespace bes::sdk
