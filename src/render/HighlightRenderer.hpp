#pragma once

#include "selection/Selection.hpp"
#include "world/World.hpp"

#include <bes/sdk/Types.hpp>

#include <cstdint>
#include <mutex>
#include <vector>

namespace bes::render {

/// Appearance/behaviour settings mirrored from the module config.
struct Style {
    bes::sdk::Color blockColor = bes::sdk::Color::fromArgb(0xFF2ED573);
    bes::sdk::Color entityColor = bes::sdk::Color::fromArgb(0xFFFFA502);
    bes::sdk::Color entityPlayerColor = bes::sdk::Color::fromArgb(0xFF3B82F6);
    float fillAlpha = 0.18f;
    bool drawFill = false;
    bool seeThrough = true;
    bool outlineOnly = true;

    /// Expands boxes slightly so coplanar faces do not z-fight with the block
    /// the outline is hugging.
    float inflate = 0.002f;
};

/// Draws the highlight geometry inside the level render pass.
///
/// Deliberately game-agnostic: it takes a camera position and a list of boxes
/// and emits batched geometry through the tessellator, so nothing here depends
/// on the camera matrix format.
class HighlightRenderer {
public:
    static HighlightRenderer& get();

    void setStyle(const Style& style);
    Style style() const;

    /// Hooks LevelRenderer::renderLevel. Returns false when required signatures
    /// are missing so the caller can report a useful error.
    bool install();
    void uninstall();
    bool installed() const noexcept { return mInstalled; }

    std::uint64_t frames() const noexcept { return mFrames; }
    std::uint64_t lastDrawVertices() const noexcept { return mLastVertices; }
    const char* lastError() const noexcept { return mLastError; }

    /// Batched geometry for one frame.
    struct Geometry {
        std::vector<bes::sdk::Vec3> lineVertices;
        std::vector<std::uint32_t> lineColors;      ///< one entry per line segment
        std::vector<bes::sdk::Vec3> triangleVertices;
        std::vector<std::uint32_t> triangleColors;  ///< one entry per triangle
    };

private:
    HighlightRenderer() = default;

    static void renderLevelDetour(void* levelRenderer, void* screenContext, void* a3);

    void draw(void* levelRenderer, void* screenContext);
    bool ensureMaterial();
    void emitLines(void* screenContext, void* tessellator, void* material, const Geometry& geometry,
                   const bes::sdk::Vec3& camera);
    void emitTriangles(void* screenContext, void* tessellator, void* material, const Geometry& geometry,
                       const bes::sdk::Vec3& camera);

    mutable std::mutex mStyleMutex;
    Style mStyle{};
    bool mInstalled = false;
    std::uint64_t mFrames = 0;
    std::uint64_t mLastVertices = 0;
    const char* mLastError = "";
};

}
