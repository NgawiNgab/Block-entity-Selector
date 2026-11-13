#include "render/HighlightRenderer.hpp"

#include "core/Logging.hpp"
#include "core/Resolver.hpp"
#include "selection/Scanner.hpp"
#include "render/GlState.hpp"

#include <bes/sdk/AddressSpace.hpp>
#include <bes/sdk/Memory.hpp>
#include <pl/memory/Hook.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace bes::render {
namespace {

using namespace bes::core;

using TessellatorBeginFn = void (*)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount,
                                    int noIndices);
using TessellatorColorFn = void (*)(void* tessellator, float r, float g, float b, float a);
using TessellatorVertexFn = void (*)(void* tessellator, float x, float y, float z);
using RenderMeshFn = void (*)(void* screenContext, void* tessellator, void* material, char* scratch);
using RenderLevelFn = void (*)(void* levelRenderer, void* screenContext, void* a3);

TessellatorBeginFn gTessBegin = nullptr;
TessellatorColorFn gTessColor = nullptr;
TessellatorVertexFn gTessVertex = nullptr;
RenderMeshFn gRenderMesh = nullptr;
RenderLevelFn gRenderLevelOriginal = nullptr;
pl::memory::HookHandle gRenderLevelHook;

/// Bedrock primitive enums (mce::PrimitiveMode): 4 = lines, 5 = triangles.
constexpr int kPrimitiveLines = 4;
constexpr int kPrimitiveTriangles = 5;

/// Scratch passed to renderMeshImmediately. The helper only reads the front of
/// the structure, and the same zeroed block is what the vanilla renderer passes
/// for immediate draws.
constexpr std::size_t kRenderMeshScratchSize = 0x58;

/// FNV-1a, matching mce::HashedString::computeHash.
std::uint64_t hashString(std::string_view text) {
    constexpr std::uint64_t kOffset = 0xCBF29CE484222325ull;
    constexpr std::uint64_t kPrime = 0x100000001B3ull;
    std::uint64_t hash = kOffset;
    for (const char character : text) {
        hash = static_cast<std::uint64_t>(static_cast<unsigned char>(character)) ^ (kPrime * hash);
    }
    return hash;
}

/// Mirrors mce::HashedString: hash, owning string, then a mutable match cache.
struct HashedString {
    std::uint64_t hash = 0;
    std::string text;
    mutable const HashedString* lastMatch = nullptr;

    HashedString() = default;
    explicit HashedString(const char* value) : hash(hashString(value ? value : "")), text(value ? value : "") {}
};

/// Mirrors the shared_ptr-shaped MaterialPtr returned by RenderMaterialGroup.
struct MaterialPtr {
    void* data[2]{nullptr, nullptr};

    explicit operator bool() const { return data[0] != nullptr; }
};

using GetMaterialFn = MaterialPtr (*)(void*, const HashedString*);

void* gMaterialGroup = nullptr;
MaterialPtr gSelectionMaterial;
/// False until a material is acquired. A failed lookup is retried from the next
/// frame (throttled) instead of being cached as a permanent failure: the world
/// may not exist yet the first time renderLevel runs.
bool gMaterialResolved = false;
/// Frames remaining before a failed lookup is attempted again.
int gMaterialRetryCountdown = 0;
constexpr int kMaterialRetryFrames = 180;

/// Decodes "<reg> = ADRP page; <reg> = ADD reg, #imm" into the final address.
/// Used to recover the RenderMaterialGroup singleton from a helper that
/// references it, which survives game updates as long as the helper matches.
std::uintptr_t resolveAdrpAdd(const std::uint32_t* instructions, std::size_t count, std::uint32_t registerIndex) {
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint32_t instruction = instructions[index];
        if ((instruction & 0x1F) != registerIndex) continue;

        // ADRP
        if ((instruction & 0x9F000000u) == 0x90000000u) {
            const std::uintptr_t page =
                (reinterpret_cast<std::uintptr_t>(&instructions[index]) & ~0xFFFull) +
                ((static_cast<std::int64_t>(static_cast<std::uint64_t>(((instruction >> 3) & 0x1FFFFC) |
                                                                     ((instruction >> 29) & 3))
                                          << 43) >>
                  31));

            for (std::size_t next = index + 1; next < count; ++next) {
                const std::uint32_t candidate = instructions[next];
                // ADD (immediate), 64-bit, source == destination == target register.
                if ((candidate & 0xFF000000u) == 0x91000000u && ((candidate >> 5) & 0x1F) == registerIndex &&
                    (candidate & 0x1F) == registerIndex) {
                    std::uint32_t immediate = (candidate >> 10) & 0xFFF;
                    if (candidate & 0x400000u) immediate <<= 12;
                    return page + immediate;
                }
                if ((candidate & 0x1F) == registerIndex) break;
            }
        }
    }
    return 0;
}

std::uint32_t packColor(const bes::sdk::Color& color) {
    const auto toByte = [](float value) {
        return static_cast<std::uint32_t>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
    };
    return (toByte(color.a) << 24) | (toByte(color.r) << 16) | (toByte(color.g) << 8) | toByte(color.b);
}

void unpackColor(std::uint32_t packed, float& r, float& g, float& b, float& a) {
    r = static_cast<float>((packed >> 16) & 0xFF) / 255.f;
    g = static_cast<float>((packed >> 8) & 0xFF) / 255.f;
    b = static_cast<float>(packed & 0xFF) / 255.f;
    a = static_cast<float>((packed >> 24) & 0xFF) / 255.f;
}

/// Forces the shader colour to white so per-vertex colours pass through
/// unmodulated, restoring the previous value on scope exit.
class ScopedShaderColor {
public:
    explicit ScopedShaderColor(void* screenContext) {
        if (!screenContext) return;
        // Unproven reads as 0 here too. Unlike the tessellator this one only
        // memcpy()s floats in and out, so a wrong offset corrupts an unrelated
        // field instead of faulting -- quieter, not safer. Require the proof.
        auto& resolver = Resolver::get();
        if (!resolver.offsetValidated(OffId::ScreenContextColorHolder)) return;
        const auto offset = resolver.offset(OffId::ScreenContextColorHolder);
        if (!offset) return;
        if (!sdk::AddressSpace::get().readable(screenContext, offset + sizeof(mSaved))) return;
        mHolder = sdk::fieldPointer(screenContext, offset);
        if (!sdk::plausiblePointer(mHolder) ||
            !sdk::AddressSpace::get().readable(mHolder, sizeof(mSaved))) {
            mHolder = nullptr;
            return;
        }
        std::memcpy(mSaved.data(), mHolder, sizeof(mSaved));
        const std::array<float, 4> white{1.f, 1.f, 1.f, 1.f};
        std::memcpy(mHolder, white.data(), sizeof(white));
        markDirty();
    }

    ~ScopedShaderColor() {
        if (!mHolder) return;
        std::memcpy(mHolder, mSaved.data(), sizeof(mSaved));
        markDirty();
    }

    explicit operator bool() const noexcept { return mHolder != nullptr; }

    ScopedShaderColor(const ScopedShaderColor&) = delete;
    ScopedShaderColor& operator=(const ScopedShaderColor&) = delete;

private:
    void markDirty() { static_cast<std::byte*>(mHolder)[0x10] = std::byte{1}; }

    void* mHolder = nullptr;
    std::array<float, 4> mSaved{};
};

const bes::sdk::Vec3 kBoxCorners[8] = {
    {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {1.f, 0.f, 1.f}, {0.f, 0.f, 1.f},
    {0.f, 1.f, 0.f}, {1.f, 1.f, 0.f}, {1.f, 1.f, 1.f}, {0.f, 1.f, 1.f},
};

void appendBoxEdges(const bes::sdk::AABB& box, bes::render::HighlightRenderer::Geometry& geometry,
                    std::uint32_t color) {
    static constexpr int kEdges[24] = {0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4, 0, 4, 1, 5, 2, 6, 3, 7};
    const auto extent = box.extent();

    for (const int cornerIndex : kEdges) {
        const auto& unit = kBoxCorners[cornerIndex];
        geometry.lineVertices.push_back({box.min.x + unit.x * extent.x, box.min.y + unit.y * extent.y,
                                         box.min.z + unit.z * extent.z});
    }
    geometry.lineColors.resize(geometry.lineVertices.size() / 2, color);
}

void appendBoxFaces(const bes::sdk::AABB& box, bes::render::HighlightRenderer::Geometry& geometry,
                    std::uint32_t color) {
    static constexpr int kQuads[6][4] = {
        {0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {3, 0, 4, 7},
    };
    const auto extent = box.extent();

    const auto corner = [&](int index) {
        const auto& unit = kBoxCorners[index];
        return bes::sdk::Vec3{box.min.x + unit.x * extent.x, box.min.y + unit.y * extent.y,
                              box.min.z + unit.z * extent.z};
    };

    for (const auto& quad : kQuads) {
        geometry.triangleVertices.push_back(corner(quad[0]));
        geometry.triangleVertices.push_back(corner(quad[1]));
        geometry.triangleVertices.push_back(corner(quad[2]));
        geometry.triangleVertices.push_back(corner(quad[0]));
        geometry.triangleVertices.push_back(corner(quad[2]));
        geometry.triangleVertices.push_back(corner(quad[3]));
    }
    geometry.triangleColors.resize(geometry.triangleVertices.size() / 3, color);
}

} // namespace

HighlightRenderer& HighlightRenderer::get() {
    static HighlightRenderer instance;
    return instance;
}

void HighlightRenderer::setStyle(const Style& style) {
    std::lock_guard lock(mStyleMutex);
    mStyle = style;
}

Style HighlightRenderer::style() const {
    std::lock_guard lock(mStyleMutex);
    return mStyle;
}

bool HighlightRenderer::install() {
    if (mInstalled) return true;

    auto& resolver = Resolver::get();
    gTessBegin = reinterpret_cast<TessellatorBeginFn>(resolver.address(SigId::TessellatorBegin));
    gTessColor = reinterpret_cast<TessellatorColorFn>(resolver.address(SigId::TessellatorColor));
    gTessVertex = reinterpret_cast<TessellatorVertexFn>(resolver.address(SigId::TessellatorVertex));
    gRenderMesh = reinterpret_cast<RenderMeshFn>(resolver.address(SigId::RenderMeshImmediately));

    if (!gTessBegin || !gTessColor || !gTessVertex || !gRenderMesh) {
        mLastError = "tessellator signatures unresolved";
        core::log::error(std::string("renderer: ") + mLastError);
        return false;
    }

    const auto renderLevel = resolver.address(SigId::RenderLevel);
    if (!renderLevel) {
        mLastError = "renderLevel signature unresolved";
        core::log::error(std::string("renderer: ") + mLastError);
        return false;
    }

    auto* target = reinterpret_cast<void*>(renderLevel);
    auto* detour = reinterpret_cast<void*>(&HighlightRenderer::renderLevelDetour);
    if (pl::memory::hook(target, detour, reinterpret_cast<void**>(&gRenderLevelOriginal)) != 0) {
        mLastError = "renderLevel hook rejected";
        core::log::error(std::string("renderer: ") + mLastError);
        return false;
    }
    // Own the handle so uninstall() can undo the hook cleanly.
    gRenderLevelHook = pl::memory::HookHandle(target, detour, reinterpret_cast<void**>(&gRenderLevelOriginal));

    mInstalled = true;
    mLastError = "";
    core::log::info("renderer: hooked LevelRenderer::renderLevel");
    return true;
}

void HighlightRenderer::uninstall() {
    if (!mInstalled) return;
    gRenderLevelHook.reset();
    gRenderLevelOriginal = nullptr;
    mInstalled = false;
}

void HighlightRenderer::renderLevelDetour(void* levelRenderer, void* screenContext, void* a3) {
    if (gRenderLevelOriginal) gRenderLevelOriginal(levelRenderer, screenContext, a3);
    HighlightRenderer::get().draw(levelRenderer, screenContext);
}

bool HighlightRenderer::ensureMaterial() {
    if (gMaterialResolved) return true;
    if (gMaterialRetryCountdown > 0) {
        --gMaterialRetryCountdown;
        return false;
    }
    gMaterialRetryCountdown = kMaterialRetryFrames;

    auto& resolver = Resolver::get();

    if (!gMaterialGroup) {
        const auto groupAddress = resolver.address(SigId::RenderMaterialGroup);
        if (groupAddress) {
            const auto group = resolveAdrpAdd(reinterpret_cast<const std::uint32_t*>(groupAddress), 8, 0);
            if (group) gMaterialGroup = reinterpret_cast<void*>(group);
        }
    }

    if (!gMaterialGroup) {
        core::log::warn("renderer: render_material_group signature unresolved; add it from Zaphkiel");
        return false;
    }

    if (!sdk::plausiblePointer(gMaterialGroup)) {
        gMaterialGroup = nullptr;
        core::log::warn("renderer: render_material_group resolved to an implausible address");
        return false;
    }

    auto** vtable = *reinterpret_cast<void***>(gMaterialGroup);
    if (!vtable || !sdk::plausiblePointer(vtable)) {
        core::log::warn("renderer: material group has no vtable");
        return false;
    }
    const auto lookup = vtable[2];
    if (!sdk::plausiblePointer(lookup)) {
        core::log::warn("renderer: material group has no lookup slot");
        return false;
    }

    HashedString name("selection_box");
    gSelectionMaterial = reinterpret_cast<GetMaterialFn>(lookup)(gMaterialGroup, &name);
    if (!gSelectionMaterial) {
        core::log::warn("renderer: selection_box material not found in group");
        return false;
    }
    gMaterialResolved = true;
    gMaterialRetryCountdown = 0;
    core::log::debug("renderer: selection_box material acquired");
    return true;
}

void HighlightRenderer::emitLines(void* screenContext, void* tessellator, void* material, const Geometry& geometry,
                                  const bes::sdk::Vec3& camera) {
    if (geometry.lineVertices.empty()) return;

    ScopedShaderColor shaderColor(screenContext);
    if (!shaderColor) return;

    gTessBegin(tessellator, nullptr, kPrimitiveLines, static_cast<int>(geometry.lineVertices.size()), 0);

    std::size_t segment = 0;
    for (std::size_t index = 0; index < geometry.lineVertices.size(); ++index) {
        if (index % 2 == 0) {
            segment = index / 2;
            const std::uint32_t packed =
                segment < geometry.lineColors.size() ? geometry.lineColors[segment] : geometry.lineColors.back();
            float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
            unpackColor(packed, r, g, b, a);
            gTessColor(tessellator, r, g, b, a);
        }
        const auto& vertex = geometry.lineVertices[index];
        gTessVertex(tessellator, vertex.x - camera.x, vertex.y - camera.y, vertex.z - camera.z);
    }

    std::array<char, kRenderMeshScratchSize> scratch{};
    gRenderMesh(screenContext, tessellator, material, scratch.data());
    mLastVertices += geometry.lineVertices.size();
}

void HighlightRenderer::emitTriangles(void* screenContext, void* tessellator, void* material,
                                      const Geometry& geometry, const bes::sdk::Vec3& camera) {
    if (geometry.triangleVertices.empty()) return;

    ScopedShaderColor shaderColor(screenContext);
    if (!shaderColor) return;

    gTessBegin(tessellator, nullptr, kPrimitiveTriangles, static_cast<int>(geometry.triangleVertices.size()), 0);

    std::size_t triangle = 0;
    for (std::size_t index = 0; index < geometry.triangleVertices.size(); ++index) {
        if (index % 3 == 0) {
            triangle = index / 3;
            const std::uint32_t packed = triangle < geometry.triangleColors.size() ? geometry.triangleColors[triangle]
                                                                                  : geometry.triangleColors.back();
            float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
            unpackColor(packed, r, g, b, a);
            gTessColor(tessellator, r, g, b, a);
        }
        const auto& vertex = geometry.triangleVertices[index];
        gTessVertex(tessellator, vertex.x - camera.x, vertex.y - camera.y, vertex.z - camera.z);
    }

    std::array<char, kRenderMeshScratchSize> scratch{};
    gRenderMesh(screenContext, tessellator, material, scratch.data());
    mLastVertices += geometry.triangleVertices.size();
}

void HighlightRenderer::draw(void* levelRenderer, void* screenContext) {
    if (!mInstalled) return;
    if (!levelRenderer || !sdk::plausiblePointer(screenContext)) return;

    const auto targets = selection::Scanner::get().targets();
    if (targets.blocks.empty() && targets.entityBoxes.empty()) return;

    auto& resolver = Resolver::get();

    // Unproven reads as 0, and offset 0 on the ScreenContext is its vptr -- a
    // mapped pointer that would sail through plausiblePointer() and then be used
    // as a Tessellator. Require the proof instead of the heuristic.
    if (!resolver.offsetValidated(OffId::ScreenContextTessellator)) return;
    const auto tessellatorPointer = sdk::fieldPointer(screenContext, resolver.offset(OffId::ScreenContextTessellator));
    if (!sdk::plausiblePointer(tessellatorPointer)) return;
    void* tessellator = tessellatorPointer;

    // Camera position is kept fresh by the tick path (it reads the same field
    // the renderer uses), so no per-frame game lookups are needed here.
    const auto camera = world::World::get().snapshot().cameraPosition;
    if (!camera.finite()) return;

    if (!ensureMaterial()) return;
    void* material = &gSelectionMaterial;

    const Style style = this->style();

    Geometry geometry;

    const auto blockColor = packColor(style.blockColor);
    const auto entityColor = packColor(style.entityColor);
    const auto playerColor = packColor(style.entityPlayerColor);

    for (const auto& pos : targets.blocks) {
        // Slight inset/outset keeps the outline visible on the block surface
        // without z-fighting with the vanilla selection box.
        const bes::sdk::AABB box{{static_cast<float>(pos.x) - style.inflate, static_cast<float>(pos.y) - style.inflate,
                                  static_cast<float>(pos.z) - style.inflate},
                                 {static_cast<float>(pos.x) + 1.f + style.inflate,
                                  static_cast<float>(pos.y) + 1.f + style.inflate,
                                  static_cast<float>(pos.z) + 1.f + style.inflate}};
        appendBoxEdges(box, geometry, blockColor);
        if (style.drawFill && !style.outlineOnly) {
            appendBoxFaces(box, geometry, packColor(style.blockColor.withAlpha(style.fillAlpha)));
        }
    }

    for (std::size_t index = 0; index < targets.entityBoxes.size(); ++index) {
        const auto& box = targets.entityBoxes[index];
        const auto expanded = box.expanded(style.inflate);
        const bool isPlayer = index < targets.entityIsPlayer.size() && targets.entityIsPlayer[index] != 0;
        const auto color = isPlayer ? playerColor : entityColor;
        appendBoxEdges(expanded, geometry, color);
        if (style.drawFill && !style.outlineOnly) {
            appendBoxFaces(expanded, geometry, packColor(bes::sdk::Color::fromArgb(color).withAlpha(style.fillAlpha)));
        }
    }

    if (geometry.lineVertices.empty() && geometry.triangleVertices.empty()) return;

    if (style.seeThrough) {
        // Through-wall pass: everything is drawn with depth testing off, which
        // is exactly the x-ray outline the module advertises.
        gl::ScopedDepthTestDisabled depthDisabled;
        emitLines(screenContext, tessellator, material, geometry, camera);
        emitTriangles(screenContext, tessellator, material, geometry, camera);
    } else {
        emitLines(screenContext, tessellator, material, geometry, camera);
        emitTriangles(screenContext, tessellator, material, geometry, camera);
    }

    ++mFrames;
}

} // namespace bes::render
