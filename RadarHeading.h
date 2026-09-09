#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

// Radar-only policy and the exact native call contract verified in the local
// Definitive Edition executable. No game camera or weapon values are written.
namespace RadarHeading {
    struct Direction { float x, y, z; };
    static_assert(sizeof(Direction) == 12);

    constexpr uint32_t ImageSize = 0x5CA4400;
    constexpr uint32_t ImageTimestamp = 0x66FC70B2;
    constexpr uintptr_t CallRva = 0x11C1E27;
    constexpr uintptr_t RendererRva = 0xB83A30;
    constexpr uintptr_t SourceRva = 0x11C1D08;
    constexpr uintptr_t ConsumerRva = 0xB83ACA;
    constexpr std::array<uint8_t, 5> OriginalCall{ 0xE8, 0x04, 0x1C, 0x9C, 0xFF };
    constexpr std::array<uint8_t, 18> OriginalRendererEntry{
        0x4C,0x8B,0xDC,0x55, 0x49,0x8D,0xAB,0x88,0xF3,0xFF,0xFF,
        0x48,0x81,0xEC,0x70,0x0D,0x00,0x00
    };
    // Active GTA camera Front -> the radar call's fourth argument, not a camera write.
    constexpr std::array<uint8_t, 27> OriginalSource{
        0xF3,0x0F,0x10,0x84,0x31,0x68,0x26,0x3E,0x05,
        0xF3,0x0F,0x10,0xAC,0x31,0x6C,0x26,0x3E,0x05,
        0xF3,0x0F,0x10,0xB4,0x31,0x70,0x26,0x3E,0x05
    };
    // The renderer reads that argument's X/Y and calculates its own radar yaw.
    constexpr std::array<uint8_t, 16> OriginalConsumer{
        0xF3,0x41,0x0F,0x10,0x0E, 0xF3,0x41,0x0F,0x10,0x46,0x04,
        0xE8,0x66,0xF2,0xDC,0x00
    };

    constexpr uint64_t DisabledSample = (std::numeric_limits<uint64_t>::max)();
    constexpr uint32_t MaxSampleAgeMs = 250;

    // Publish yaw and time as one atomic value: a reader never combines two frames.
    inline uint64_t MakeSample(float nativeCameraX, float nativeCameraY, uint32_t now) {
        if (!std::isfinite(nativeCameraX) || !std::isfinite(nativeCameraY)
            || std::hypot(nativeCameraX, nativeCameraY) < 0.0001f)
            return DisabledSample;
        const float yaw = std::atan2(-nativeCameraY, nativeCameraX); // GTA -> Unreal Y sign
        uint32_t bits = 0;
        std::memcpy(&bits, &yaw, sizeof(bits));
        return (uint64_t(now) << 32) | bits;
    }

    inline bool ApplySample(uint64_t sample, uint32_t now,
        const Direction& nativeDirection, Direction& radarDirection) {
        if (sample == DisabledSample
            || uint32_t(now - uint32_t(sample >> 32)) > MaxSampleAgeMs)
            return false;
        const uint32_t bits = uint32_t(sample);
        float yaw = 0.0f;
        std::memcpy(&yaw, &bits, sizeof(yaw));
        if (!std::isfinite(yaw))
            return false;
        radarDirection = nativeDirection;
        radarDirection.x = std::cos(yaw);
        radarDirection.y = std::sin(yaw);
        return true;
    }
}
