#pragma once

#include <bes/sdk/Types.hpp>

#include <cstdint>
#include <string>

namespace bes::diag {

/// Turns the Zaphkiel-confirmed `BlockType` layout into a runtime measurement.
///
/// Static RE established (CONFIRMED, runs 14-15):
///   - `BlockType` is polymorphic with a 134-slot vtable at RVA 0x13ded6f0;
///   - `+0x138..+0x144` is returned by slot 130 as four floats in s0..s3;
///   - `+0x200` is read as a byte at four sites and as a pointer at eight;
///   - `+0x1e8` is read nine times inside class methods and written none;
///   - no vtable slot and no scanned member reads a std::string.
///
/// What static analysis could NOT establish is where the block identifier lives.
/// This probe answers that from a live object instead of from the binary.
///
/// It is deliberately read-only: it never calls a virtual function, so no ABI
/// mismatch can take the game down. It only reads memory that the game itself
/// has already dereferenced.
class BlockTypeProbe {
public:
    static BlockTypeProbe& get();

    /// Dumps a live `BlockType` to `blocktype_probe.txt` in the data directory.
    ///
    /// `blockType` must be a pointer the game has already dereferenced; the
    /// caller is responsible for having obtained it from a real block.
    /// Returns false when nothing was written.
    bool dump(void* blockType, const bes::sdk::BlockPos& origin);

    /// Convenience entry point used by the diagnostics button: takes the block
    /// the local player is standing in.
    bool dumpNearPlayer();

    /// Path of the last written report.
    const std::string& lastReportPath() const noexcept { return mLastPath; }

    /// Number of times `dump` produced output; surfaced in diagnostics.
    std::uint32_t dumpCount() const noexcept { return mDumpCount; }

private:
    BlockTypeProbe() = default;

    std::string mLastPath;
    std::uint32_t mDumpCount = 0;
};

} // namespace bes::diag
