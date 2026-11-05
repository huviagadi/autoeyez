#pragma once
#include <stdint.h>
#include "controller_work.h"

namespace autoeyez {
// A beat-grid oscillator. Onsets estimate and steer the grid; they never
// advance the beat counter directly. MIDI supplies exact phase when present.
struct BeatClock {
  enum Source : uint8_t { None, Audio, Midi };
  Source source = None;
  uint8_t step = 0;                 // 1..4, zero while unsynchronized
  uint32_t lastBeatMs = 0;
  uint32_t nextBeatMs = 0;
  uint32_t lastMidiMs = 0;
  uint8_t lastMidiStep = 0;
  uint32_t lastAudioActivityMs = 0;
  uint32_t lastAlignedOnsetMs = 0;
  uint32_t lastCandidateMs = 0;
  uint32_t periodMs = 0;
  uint32_t fftGeneration = 0;
  float previous[20] = {};
  float fluxAverage = 0;
  float energyAverage = 0;
  uint32_t candidates[32] = {};
  float candidateStrength[32] = {};
  uint8_t candidateCount = 0;
  uint32_t lastTempoEstimateMs = 0;
  uint32_t lastTapMs = 0;
  uint32_t lastTapIntervalMs = 0;
  int32_t phaseErrorSum = 0;
  uint8_t phaseErrorCount = 0;

  static uint32_t distance(uint32_t a, uint32_t b) {
    return a > b ? a - b : b - a;
  }

  void midiBeat(uint8_t beat, uint32_t now) {
    if (beat < 1 || beat > 4) return;
    if (source != Midi) {
      phaseErrorSum = 0;
      phaseErrorCount = 0;
    }
    if (lastMidiMs && beat == (lastMidiStep == 4 ? 1 : lastMidiStep + 1)) {
      uint32_t interval = uint32_t(now - lastMidiMs);
      if (interval >= 280 && interval <= 1100)
        periodMs = periodMs ? (periodMs * 3 + interval) / 4 : interval;
    }
    bool alreadyPulsed = periodMs && step == beat &&
                         uint32_t(now - lastBeatMs) < 100;
    source = Midi;
    step = beat;
    lastMidiMs = now;
    lastMidiStep = beat;
    if (!alreadyPulsed) lastBeatMs = now;
    if (periodMs) nextBeatMs = now + periodMs;
  }

  void midiStop() {
    // Retain the last MIDI period and bar phase for the audio takeover.
    source = periodMs ? Audio : None;
    lastMidiMs = 0;
  }

  void tap(uint32_t now) {
    // Incoming MIDI beats own the tempo and phase while the clock is live.
    if (source == Midi && lastMidiMs &&
        uint32_t(now - lastMidiMs) <= (periodMs ? periodMs + 100 : 1100)) return;

    const uint32_t interval = lastTapMs ? uint32_t(now - lastTapMs) : 0;
    if (lastTapMs && interval < 180) return;
    if (interval >= 280 && interval <= 1100) {
      uint32_t target = interval;
      if (lastTapIntervalMs && distance(interval, lastTapIntervalMs) <= interval / 4)
        target = (interval + lastTapIntervalMs) / 2;
      periodMs = periodMs ? (periodMs + 3 * target) / 4 : target;
      lastTapIntervalMs = interval;
    } else {
      lastTapIntervalMs = 0;
      if (!periodMs) periodMs = 500;  // Provisional grid until the second tap.
    }
    lastTapMs = now;
    source = Audio;
    step = step == 4 ? 1 : step + 1;
    lastBeatMs = now;
    nextBeatMs = now + periodMs;
    lastAlignedOnsetMs = now;
    lastAudioActivityMs = now;
    recordOnset(now, 8.0f);
  }

  uint32_t estimateTempo(uint32_t now) const {
    if (candidateCount < 32 || uint32_t(now - candidates[candidateCount - 1]) < 6500)
      return 0;
    // Adjacent onsets may span eighths, triplets, quarters, and multiple
    // beats. Dotted-quarter and five-eighth gaps are important: otherwise
    // their strong attacks can masquerade as a slower quarter note.
    float bestScore = 0;
    uint32_t bestPeriod = 0;
    for (uint32_t period = 330; period <= 900; period += 5) {
      float score = 0;
      for (unsigned i = 0; i + 1 < candidateCount; ++i) {
        uint32_t gap = uint32_t(candidates[i] - candidates[i + 1]);
        const uint32_t targets[] = {period, period / 2, period / 3,
                                    period * 3 / 2, period * 2,
                                    period * 5 / 2};
        const float weights[] = {1.0f, 0.7f, 0.45f, 0.65f, 0.5f, 0.3f};
        for (unsigned k = 0; k < 6; ++k) {
          uint32_t error = distance(gap, targets[k]);
          if (error < 20)
            score += candidateStrength[i] * weights[k] * (1.0f - float(error) / 20.0f);
        }
      }
      if (score > bestScore) { bestScore = score; bestPeriod = period; }
    }
    return bestScore > 8.0f ? bestPeriod : 0;
  }

  void advance(uint32_t now) {
    if (!periodMs || source == None) return;
    if (source == Midi && uint32_t(now - lastMidiMs) > periodMs + 100)
      source = Audio;
    if (source == Audio && uint32_t(now - lastAudioActivityMs) > 1800) {
      source = None;
      step = 0;
      return;
    }
    // Keep time through missing kick drums. Never skip a numbered beat on a
    // loud offbeat; only the predicted quarter-note boundary advances it.
    for (unsigned i = 0; i < 4 && int32_t(now - nextBeatMs) >= 0; ++i) {
      step = step == 4 ? 1 : step + 1;
      lastBeatMs = nextBeatMs;
      nextBeatMs += periodMs;
    }
  }

  void recordOnset(uint32_t now, float strength = 1.0f) {
    if (lastCandidateMs && uint32_t(now - lastCandidateMs) < 130) return;
    if (lastCandidateMs && uint32_t(now - lastCandidateMs) > 2000)
      candidateCount = 0;
    lastCandidateMs = now;
    for (unsigned i = 31; i > 0; --i) {
      candidates[i] = candidates[i - 1];
      candidateStrength[i] = candidateStrength[i - 1];
    }
    candidates[0] = now;
    candidateStrength[0] = strength > 8.0f ? 8.0f : strength;
    if (candidateCount < 32) ++candidateCount;

    if (source == Midi) return;
    if (source == Audio && periodMs) {
      uint32_t target = distance(now, lastBeatMs) < distance(now, nextBeatMs)
                        ? lastBeatMs : nextBeatMs;
      int32_t error = int32_t(now - target);
      uint32_t window = periodMs / 5;
      if (window < 65) window = 65;
      if (window > 110) window = 110;
      if (error >= -int32_t(window) && error <= int32_t(window) &&
          (!lastAlignedOnsetMs || uint32_t(now - lastAlignedOnsetMs) > periodMs / 2)) {
        lastAlignedOnsetMs = now;
        // Individual onsets can be syncopated. They may nudge phase, but
        // cannot change the period inferred from the full onset pattern.
        phaseErrorSum += error;
        if (++phaseErrorCount >= 4) {
          int32_t correction = phaseErrorSum / 16;
          if (correction > 8) correction = 8;
          if (correction < -8) correction = -8;
          nextBeatMs = uint32_t(int32_t(nextBeatMs) + correction);
          phaseErrorSum = 0;
          phaseErrorCount = 0;
        }
      }
      return;
    }

    // A cold start waits for several seconds of evidence, then chooses the
    // strongest repeating onset phase. Beat 1 remains arbitrary without MIDI.
    if (uint32_t(now - lastTempoEstimateMs) < 500) return;
    lastTempoEstimateMs = now;
    uint32_t estimate = estimateTempo(now);
    if (!estimate) return;
    uint32_t bestAnchor = 0;
    float bestScore = 0;
    for (unsigned anchor = 0; anchor < candidateCount; ++anchor) {
      float score = 0;
      for (unsigned j = 0; j < candidateCount; ++j) {
        uint32_t delta = distance(candidates[anchor], candidates[j]);
        uint32_t remainder = delta % estimate;
        if (remainder <= 35 || estimate - remainder <= 35)
          score += candidateStrength[j];
      }
      if (score > bestScore) {
        bestScore = score;
        bestAnchor = candidates[anchor];
      }
    }
    if (bestScore > 0) {
      periodMs = estimate;
      step = 1;
      source = Audio;
      lastBeatMs = bestAnchor + uint32_t(now - bestAnchor) / periodMs * periodMs;
      nextBeatMs = lastBeatMs + periodMs;
      lastAlignedOnsetMs = lastBeatMs;
    }
  }

  void feed(const FftSnapshot& fft, uint32_t now) {
    advance(now);
    if (fftGeneration == fft.generation) return;
    fftGeneration = fft.generation;
    float flux = 0, energy = 0;
    for (unsigned i = 0; i < 20; ++i) {
      float weight = i < 5 ? 1.0f : (i < 12 ? 0.45f : 0.2f);
      float value = fft.read(i + 1);
      float rise = value - previous[i];
      if (rise > 0) flux += rise * weight;
      energy += value * weight;
      previous[i] = value;
    }
    float oldFlux = fluxAverage;
    float oldEnergy = energyAverage;
    fluxAverage = fluxAverage * 0.94f + flux * 0.06f;
    energyAverage = energyAverage * 0.97f + energy * 0.03f;
    if (energy > 0.003f) lastAudioActivityMs = now;
    if (energy > 0.003f && flux > 0.002f &&
        flux > oldFlux * 1.8f && energy > oldEnergy * 1.06f)
      recordOnset(now, flux / (oldFlux + 0.002f));
  }

  bool pulse(uint32_t now) const {
    return source != None && step && uint32_t(now - lastBeatMs) < 110;
  }
};
}
