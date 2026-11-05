#pragma once
#include <array>
#include <cstdint>

namespace autoeyez {
// CC 110-112 carry low/mid/high. CC 113 commits only a complete frame.
struct MidiFftFrame {
    std::array<std::uint8_t, 3> pending{{0, 0, 0}};
    std::array<std::uint8_t, 3> values{{0, 0, 0}};
    std::uint8_t mask = 0;
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
        if (control != 113) return false;
        const bool complete = mask == 0x07;
        mask = 0;
        if (complete) {
            values = pending;
            sequence = static_cast<std::uint8_t>(value);
            valid = true;
            ++frames;
        }
        return complete;
    }
};
}
