#pragma once
#include <array>
#include <cstdint>

namespace autoeyez {
// CC 110-112 keep the legacy 7-bit bands. CC 114-119 carry three 14-bit
// unbounded band magnitudes (0..32). CC 113 commits only a complete frame.
struct MidiFftFrame {
    std::array<std::uint8_t, 3> pending{{0, 0, 0}};
    std::array<std::uint8_t, 3> values{{0, 0, 0}};
    std::uint8_t mask = 0;
    std::array<std::uint8_t, 6> pendingExtended{{0, 0, 0, 0, 0, 0}};
    std::array<std::uint16_t, 3> extendedValues{{0, 0, 0}};
    std::uint8_t extendedMask = 0;
    bool hasExtended = false;
    std::uint8_t sequence = 0;
    bool valid = false;
    std::uint64_t frames = 0;

    bool accept(unsigned control, unsigned value) {
        if (value > 127) return false;
        if (control >= 110 && control <= 112) {
            const unsigned index = control - 110;
            pending[index] = static_cast<std::uint8_t>(value);
            mask |= static_cast<std::uint8_t>(1u << index);
            return false;
        }
        if (control >= 114 && control <= 119) {
            const unsigned index = control - 114;
            pendingExtended[index] = static_cast<std::uint8_t>(value);
            extendedMask |= static_cast<std::uint8_t>(1u << index);
            return false;
        }
        if (control != 113) return false;
        const bool complete = mask == 0x07;
        mask = 0;
        if (complete) {
            values = pending;
            hasExtended = extendedMask == 0x3f;
            if (hasExtended)
                for (unsigned i = 0; i < 3; ++i)
                    extendedValues[i] = static_cast<std::uint16_t>(
                        (pendingExtended[i * 2] << 7) | pendingExtended[i * 2 + 1]);
            sequence = static_cast<std::uint8_t>(value);
            valid = true;
            ++frames;
        }
        extendedMask = 0;
        return complete;
    }
};
}
