#pragma once

#include <cmath>
#include <cstdint>

namespace bes::sdk {

struct Vec2 {
    float x = 0.f;
    float y = 0.f;
};

struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;

    Vec3() = default;
    Vec3(float xValue, float yValue, float zValue) : x(xValue), y(yValue), z(zValue) {}

    Vec3 operator+(const Vec3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    Vec3 operator-(const Vec3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    Vec3 operator*(float scalar) const { return {x * scalar, y * scalar, z * scalar}; }

    float lengthSquared() const { return x * x + y * y + z * z; }
    float length() const { return std::sqrt(lengthSquared()); }

    bool finite() const {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }
};

struct BlockPos {
    int x = 0;
    int y = 0;
    int z = 0;

    BlockPos() = default;
    BlockPos(int xValue, int yValue, int zValue) : x(xValue), y(yValue), z(zValue) {}

    bool operator==(const BlockPos& other) const {
        return x == other.x && y == other.y && z == other.z;
    }

    static BlockPos fromCorner(const Vec3& corner) {
        return {static_cast<int>(std::floor(corner.x)),
                static_cast<int>(std::floor(corner.y)),
                static_cast<int>(std::floor(corner.z))};
    }
};

struct BlockPosHash {
    std::size_t operator()(const BlockPos& pos) const noexcept {
        // Same packing Mojang uses for block position hashing, cheap and well spread.
        std::uint64_t value = static_cast<std::uint64_t>(static_cast<std::uint32_t>(pos.x)) * 0x9E3779B97F4A7C15ull;
        value ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(pos.y)) * 0xC2B2AE3D27D4EB4Full;
        value ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(pos.z)) * 0x165667B19E3779F9ull;
        value ^= value >> 29;
        return static_cast<std::size_t>(value);
    }
};

struct AABB {
    Vec3 min{};
    Vec3 max{};

    Vec3 center() const {
        return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
    }

    Vec3 extent() const { return max - min; }

    bool valid() const {
        if (!min.finite() || !max.finite()) return false;
        return max.x > min.x && max.y > min.y && max.z > min.z;
    }

    AABB expanded(float amount) const {
        AABB result = *this;
        result.min = {min.x - amount, min.y - amount, min.z - amount};
        result.max = {max.x + amount, max.y + amount, max.z + amount};
        return result;
    }

    /// Squared distance from a point to this box (0 when inside).
    float distanceSquaredTo(const Vec3& point) const {
        float dx = 0.f;
        if (point.x < min.x) dx = min.x - point.x;
        else if (point.x > max.x) dx = point.x - max.x;
        float dy = 0.f;
        if (point.y < min.y) dy = min.y - point.y;
        else if (point.y > max.y) dy = point.y - max.y;
        float dz = 0.f;
        if (point.z < min.z) dz = min.z - point.z;
        else if (point.z > max.z) dz = point.z - max.z;
        return dx * dx + dy * dy + dz * dz;
    }

    bool intersectsRay(const Vec3& origin, const Vec3& direction, float maxDistance) const;
};

struct Color {
    float r = 1.f;
    float g = 1.f;
    float b = 1.f;
    float a = 1.f;

    /// Accepts 0xAARRGGBB, the same layout LeviLauncher's Color config entry uses.
    static Color fromArgb(std::uint32_t argb) {
        return {static_cast<float>((argb >> 16) & 0xFF) / 255.f,
                static_cast<float>((argb >> 8) & 0xFF) / 255.f,
                static_cast<float>(argb & 0xFF) / 255.f,
                static_cast<float>((argb >> 24) & 0xFF) / 255.f};
    }

    Color withAlpha(float alpha) const { return {r, g, b, alpha}; }
};

inline bool AABB::intersectsRay(const Vec3& origin, const Vec3& direction, float maxDistance) const {
    float tMin = 0.f;
    float tMax = maxDistance;
    const float o[3] = {origin.x, origin.y, origin.z};
    const float d[3] = {direction.x, direction.y, direction.z};
    const float lo[3] = {min.x, min.y, min.z};
    const float hi[3] = {max.x, max.y, max.z};

    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(d[axis]) < 1e-6f) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) return false;
            continue;
        }
        const float inverse = 1.f / d[axis];
        float near = (lo[axis] - o[axis]) * inverse;
        float far = (hi[axis] - o[axis]) * inverse;
        if (near > far) {
            const float swap = near;
            near = far;
            far = swap;
        }
        if (near > tMin) tMin = near;
        if (far < tMax) tMax = far;
        if (tMin > tMax) return false;
    }
    return true;
}

}
