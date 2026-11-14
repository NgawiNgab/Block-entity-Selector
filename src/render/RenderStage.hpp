#pragma once

#include <cstddef>
#include <cstdint>

namespace bes::render {

/// Where a draw attempt stopped.
///
/// This lives in its own header with no game dependencies so the host tests can
/// check the names. The distinction it exists for is the one a single counter
/// cannot make: `frames: 0` used to mean any of seven different failures, and
/// "rendering is broken" could not be told apart from "there was nothing to
/// draw".
enum class RenderStage : std::uint8_t {
    NotYetDrawn,   ///< no frame has reached draw() yet -- says nothing about the hook
    NotInstalled,  ///< draw() ran but the hook is no longer installed
    BadArguments,  ///< no LevelRenderer, or an implausible ScreenContext
    NoTargets,     ///< the scanner has nothing to draw -- not a render fault
    NoTessellator, ///< ScreenContext::mTessellator unproven or unreadable
    NoCamera,      ///< LevelRendererPlayer::mCamPos unproven or non-finite
    NoMaterial,    ///< RenderMaterialGroup or the selection_box lookup failed
    EmptyGeometry, ///< boxes produced no vertices (should not happen)
    Drew,          ///< tessellator accepted the geometry
};

/// Number of RenderStage values; `mStageCounts` is sized from this.
inline constexpr std::size_t kRenderStageCount = 9;

inline const char* renderStageName(RenderStage stage) {
    switch (stage) {
    case RenderStage::NotYetDrawn: return "no frame drawn yet";
    case RenderStage::NotInstalled: return "not installed";
    case RenderStage::BadArguments: return "bad arguments";
    case RenderStage::NoTargets: return "no targets";
    case RenderStage::NoTessellator: return "no tessellator";
    case RenderStage::NoCamera: return "no camera";
    case RenderStage::NoMaterial: return "no material";
    case RenderStage::EmptyGeometry: return "empty geometry";
    case RenderStage::Drew: return "drew";
    }
    return "unknown";
}

} // namespace bes::render
