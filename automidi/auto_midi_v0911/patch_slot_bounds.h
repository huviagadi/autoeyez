#pragma once
namespace autoeyez {
constexpr int audioPatchSlotCount = 32;
constexpr bool validStoredAudioSlot(int slot) {
    return slot >= 1 && slot <= audioPatchSlotCount;
}
constexpr bool validControllerAudioSlot(int slot) {
    return slot >= 0 && slot < audioPatchSlotCount;
}
} // namespace autoeyez
