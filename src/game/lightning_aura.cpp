#include "lightning_aura.h"

#include <algorithm>
#include <cmath>

namespace game::lightning_aura {
namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kHalfWidth = 1.35f;
constexpr float kHalfLength = 2.25f;
constexpr float kHeight = 1.15f;

float Hash01(unsigned value) noexcept {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return float(value & 0x00ffffffu) / float(0x01000000u);
}
}  // namespace

float IntensityForSpeed(float speed_kmh) noexcept {
    if (!std::isfinite(speed_kmh) || speed_kmh < 150.0f) return 0.0f;
    return std::clamp((speed_kmh - 150.0f) / 350.0f, 0.0f, 1.0f);
}

std::size_t BuildVertices(float intensity, float time_seconds, Vertex* output,
                          std::size_t capacity) noexcept {
    if (!output || capacity < kVertexCount || !std::isfinite(intensity) ||
        !std::isfinite(time_seconds) || time_seconds < 0.0f || intensity <= 0.0f) return 0;

    intensity = std::clamp(intensity, 0.0f, 1.0f);
    const float phase = time_seconds * (5.0f + 4.0f * intensity);
    std::size_t at = 0;

    for (unsigned bolt = 0; bolt < kBoltCount; ++bolt) {
        const float angle = kTwoPi * (float(bolt) / float(kBoltCount));
        const float cs = std::cos(angle), sn = std::sin(angle);
        const float seed = float(bolt * 17u + 3u);
        float previous_x = cs * (kHalfWidth + 0.12f);
        float previous_y = sn * (kHalfLength + 0.12f);
        float previous_z = 0.18f + 0.16f * std::sin(phase + seed);

        for (unsigned segment = 0; segment < kSegmentsPerBolt; ++segment) {
            const float t = float(segment + 1) / float(kSegmentsPerBolt);
            const float jitter = (Hash01(bolt * 101u + segment * 13u +
                                         unsigned(time_seconds * 12.0f)) - 0.5f) *
                                 (0.22f + 0.18f * intensity);
            const float radius = 1.0f + 0.12f * std::sin(phase + seed + t * 7.0f);
            const float next_x = cs * (kHalfWidth * radius + jitter) -
                                 sn * (jitter * 0.7f);
            const float next_y = sn * (kHalfLength * radius + jitter) +
                                 cs * (jitter * 0.7f);
            const float next_z = kHeight * t + 0.22f * std::sin(phase + seed + t * 9.0f);

            const float flicker = 0.65f + 0.35f * std::sin(phase * 1.7f + seed + t * 5.0f);
            const float brightness = intensity * flicker;
            output[at++] = {previous_x, previous_y, previous_z, brightness};
            output[at++] = {next_x, next_y, next_z, brightness};

            previous_x = next_x;
            previous_y = next_y;
            previous_z = next_z;
        }
    }
    return at;
}

}  // namespace game::lightning_aura
