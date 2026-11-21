#pragma once

#include <cstddef>
#include <cstdint>

namespace game::lightning_aura {

// Speed is in km/h. The effect is off below 150 and reaches full intensity at 500.
float IntensityForSpeed(float speed_kmh) noexcept;

struct Vertex {
    float x, y, z;       // car-local coordinates; transformed by the car body matrix by the renderer
    float intensity;     // 0..1, for the eventual additive yellow/gold pass
};

// A deterministic, animated bolt mesh in car-local space. Each pair is one line segment.
inline constexpr std::size_t kBoltCount = 12;
inline constexpr std::size_t kSegmentsPerBolt = 8;
inline constexpr std::size_t kVertexCount = kBoltCount * kSegmentsPerBolt * 2;

std::size_t BuildVertices(float intensity, float time_seconds, Vertex* output,
                          std::size_t capacity) noexcept;

}  // namespace game::lightning_aura
