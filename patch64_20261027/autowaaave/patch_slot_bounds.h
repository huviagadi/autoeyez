#pragma once
namespace autoeyez {
constexpr int videoPatchSlotCount = 64;
constexpr int audioPatchSlotCount = 64;
constexpr bool validStoredAudioSlot(int slot) {
    return slot >= 1 && slot <= audioPatchSlotCount;
}
constexpr bool validControllerAudioSlot(int slot) {
    return slot >= 0 && slot < audioPatchSlotCount;
}
constexpr bool validControllerVideoSlot(int slot) {
    return slot >= 0 && slot < videoPatchSlotCount;
}
} // namespace autoeyez
