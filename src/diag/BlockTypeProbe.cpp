#include "diag/BlockTypeProbe.hpp"

#include "core/Logging.hpp"
#include "core/Profile.hpp"
#include "core/Runtime.hpp"
#include "world/World.hpp"

#include <bes/sdk/AddressSpace.hpp>
#include <bes/sdk/LibcxxString.hpp>
#include <bes/sdk/Memory.hpp>

#include <cmath>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace bes::diag {
namespace {

using bes::sdk::plausiblePointer;

/// True when `value` is inside a mapped readable region, asked politely.
///
/// A heuristic ("non-null and 8-byte aligned") is not safe here: a garbage field
/// can satisfy it and point at an unmapped page, and a probe that faults would
/// take the game down. This consults the real map instead.
bool readablePointer(std::uintptr_t value, std::size_t size = 1) {
    if ((value & 0x3) != 0) return false;
    return sdk::AddressSpace::get().readable(value, size);
}

/// Reads a libc++ `std::string` that lives inline at `object + offset`.
///
/// Only the inline form is considered. Treating an arbitrary field as a pointer
/// to a string elsewhere produces false positives on every ordinary pointer
/// member, and the inline form already covers both the small and heap-backed
/// representations because the 24-byte object is in place either way.
std::string readStringAt(const void* object, std::size_t offset) {
    const auto* const address = static_cast<const std::byte*>(object) + offset;
    return sdk::readLibcxxString(address, 96, [](std::uintptr_t value) { return readablePointer(value); });
}

/// The namespace-qualified pattern every Bedrock identifier follows.
bool looksLikeIdentifier(const std::string& text) {
    if (text.size() < 3 || text.size() > 128) return false;
    const auto colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) return false;
    for (const char character : text) {
        const auto value = static_cast<unsigned char>(character);
        if (!(std::islower(value) || std::isdigit(value) || character == ':' || character == '_' ||
              character == '.' || character == '/')) {
            return false;
        }
    }
    return true;
}

std::string hex(std::uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string hexOffset(std::size_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%zx", value);
    return buffer;
}

/// Reads four floats at `object+offset` and renders them as a box when finite.
bool readBox(const void* object, std::size_t offset, bes::sdk::AABB& out) {
    float values[4]{};
    std::memcpy(values, static_cast<const std::byte*>(object) + offset, sizeof(values));
    for (const float value : values) {
        if (!(value > -3.0e7f && value < 3.0e7f)) return false;
    }
    out.min = {values[0], values[1], values[2]};
    out.max = {values[0] + 1.f, values[1] + 1.f, values[2] + 1.f};
    return true;
}

} // namespace

BlockTypeProbe& BlockTypeProbe::get() {
    static BlockTypeProbe instance;
    return instance;
}

bool BlockTypeProbe::dump(void* blockType, const bes::sdk::BlockPos& origin) {
    if (!blockType || !plausiblePointer(blockType)) {
        core::log::warn("probe: refusing to dump a null or implausible BlockType");
        return false;
    }

    // Load/refresh the readable-region table before any dereference. If it
    // cannot be read, every check below fails closed and we write nothing.
    if (!sdk::AddressSpace::get().refresh()) {
        core::log::warn("probe: /proc/self/maps unreadable; refusing to dereference");
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(blockType);
    std::ostringstream report;
    report << "BlockType probe\n";
    report << "origin block: " << origin.x << ' ' << origin.y << ' ' << origin.z << '\n';
    report << "BlockType: " << hex(base) << '\n';

    // Every read below walks 0x400 bytes of this object; confirm the whole
    // window is mapped once, up front.
    if (!readablePointer(base, 0x400)) {
        core::log::warn("probe: BlockType window is not fully readable");
        return false;
    }
    report << "mapped regions: " << sdk::AddressSpace::get().regionCount() << '\n';

    // The vtable pointer is the first thing a polymorphic object stores. Reading
    // it back is how we confirm this really is a BlockType and not a stale read.
    std::uintptr_t vtable = 0;
    std::memcpy(&vtable, reinterpret_cast<const void*>(base), sizeof(vtable));
    report << "vtable[0]: " << hex(vtable) << '\n';
    if (readablePointer(vtable, 16)) {
        report << "vtable[1]: " << hex(*reinterpret_cast<const std::uintptr_t*>(vtable + 8)) << '\n';
    }

    // Slot 130 is CONFIRMED to return four floats from +0x138.
    bes::sdk::AABB box{};
    if (readBox(blockType, 0x138, box)) {
        report << "box(+0x138): " << box.min.x << ' ' << box.min.y << ' ' << box.min.z << '\n';
    } else {
        report << "box(+0x138): not finite\n";
    }

    // The members static analysis flagged as interesting, dumped raw so the
    // runtime value can settle what they are.
    for (const std::size_t offset : {std::size_t{0x1e8}, std::size_t{0x200}}) {
        std::uintptr_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(base + offset), sizeof(value));
        report << "qword(+0x" << std::hex << offset << std::dec << "): " << hex(value);
        if (readablePointer(value, sizeof(std::uintptr_t))) {
            std::uintptr_t target = 0;
            std::memcpy(&target, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(value)),
                        sizeof(target));
            report << "  ->[" << hex(target) << ']';
        }
        report << '\n';
    }

    // The actual question: does any offset in the object look like a std::string
    // holding a namespaced identifier? Walk every 8-byte slot, because the
    // identifier's offset is exactly what is unknown.
    report << "\n=== std::string scan ===\n";
    std::size_t identifierHits = 0;
    for (std::size_t offset = 0; offset < 0x400; offset += 8) {
        const auto text = readStringAt(blockType, offset);
        if (text.empty()) continue;
        const bool named = looksLikeIdentifier(text);
        report << "  +" << hexOffset(offset) << " = \"" << text << '"'
               << (named ? "   <== identifier-shaped" : "") << '\n';
        if (named) ++identifierHits;
    }
    report << "\nidentifier-shaped strings: " << identifierHits << '\n';

    // A BlockType normally points at a BlockDefinition-shaped owner; record any
    // pointer whose target begins with its own printable identifier.
    report << "\n=== pointer scan ===\n";
    for (std::size_t offset = 0; offset < 0x400; offset += 8) {
        std::uintptr_t pointer = 0;
        std::memcpy(&pointer, reinterpret_cast<const void*>(base + offset), sizeof(pointer));
        if (!readablePointer(pointer, sizeof(std::uintptr_t))) continue;
        if (pointer == vtable) continue;
        std::string probe;
        std::uintptr_t first = 0;
        std::memcpy(&first, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(pointer)),
                    sizeof(first));
        if (readablePointer(first, 24)) {
            probe = sdk::readLibcxxString(reinterpret_cast<const void*>(first), 64,
                                          [](std::uintptr_t value) { return readablePointer(value); });
        }
        report << "  +" << hexOffset(offset) << " -> " << hex(pointer) << "  [0]=" << hex(first);
        if (!probe.empty()) report << " \"" << probe << '"';
        report << '\n';
    }

    const auto& dataDirectory = core::Runtime::get().dataDirectory();
    std::error_code error;
    std::filesystem::create_directories(dataDirectory, error);
    const auto path = dataDirectory / "blocktype_probe.txt";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        core::log::warn("probe: could not write " + path.string());
        return false;
    }
    const auto text = report.str();
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();

    mLastPath = path.string();
    ++mDumpCount;
    core::log::info("probe: BlockType dumped to " + mLastPath);
    return true;
}

bool BlockTypeProbe::dumpNearPlayer() {
    auto& world = world::World::get();
    void* blockSource = world.currentBlockSource();
    if (!blockSource) {
        core::log::warn("probe: no block source yet, cannot locate a live block");
        return false;
    }

    const auto snapshot = world.snapshot();
    if (!snapshot.valid) {
        core::log::warn("probe: world snapshot is not valid yet, enter a world first");
        return false;
    }

    // The feet block is frequently air, which has no identifier worth reading.
    // Walk down a couple of blocks first so the probe lands on solid ground.
    const auto feetX = static_cast<int>(std::floor(snapshot.playerPosition.x));
    const auto feetY = static_cast<int>(std::floor(snapshot.playerPosition.y));
    const auto feetZ = static_cast<int>(std::floor(snapshot.playerPosition.z));

    void* block = nullptr;
    bes::sdk::BlockPos pos{feetX, feetY, feetZ};
    for (int drop = 0; drop <= 2 && !block; ++drop) {
        pos = {feetX, feetY - drop, feetZ};
        block = world.blockAt(blockSource, pos);
    }

    if (!block) {
        core::log::warn("probe: block lookup returned null for the blocks under the player");
        return false;
    }

    void* blockType = world.blockTypeOf(block);
    if (!blockType) {
        // Not fatal to the investigation: the report says which step failed, and
        // the name the mod already derives tells us whether the block was real.
        core::log::warn("probe: block at " + std::to_string(pos.x) + "," + std::to_string(pos.y) + "," +
                        std::to_string(pos.z) + " has no reachable BlockType");
        return false;
    }

    const auto name = world.blockName(blockSource, pos);
    core::log::info("probe: probing block at " + std::to_string(pos.x) + "," + std::to_string(pos.y) + "," +
                    std::to_string(pos.z) + (name.empty() ? std::string(" (name unresolved)")
                                                          : " (" + name + ")"));
    return dump(blockType, pos);
}

} // namespace bes::diag
