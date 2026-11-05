#pragma once

#include <cmath>

namespace autoeyez {

// Preserve the existing response through the default setting (100).
// The remaining 27 encoder steps provide up to 8x FFT sensitivity for
// unusually quiet line-level feeds, about 16 dB above the old maximum.
inline float lineInGain(int value) {
  if (value <= 0) return 0.0f;
  if (value > 127) value = 127;
  const float legacyGain = (value * (32.0f / 127.0f)) / 25.0f;
  if (value <= 100) return legacyGain;
  return legacyGain * std::pow(6.25f, (value - 100) / 27.0f);
}

}  // namespace autoeyez
