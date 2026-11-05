#pragma once
#include <cmath>
#include <cstdint>
namespace autoeyez {
// Positive FFT256 bins 1..127, exactly once. Finer spacing below 4 kHz.
// Eight columns each of widths 1, 2, 4, then 8 (last width 7).
inline unsigned spectrumBegin(unsigned column) {
 if(column<8)return 1+column;
 if(column<16)return 9+(column-8)*2;
 if(column<24)return 25+(column-16)*4;
 return 57+(column-24)*8;
}
inline unsigned spectrumEnd(unsigned column) {
 return column==31 ? 128 : spectrumBegin(column+1);
}
struct FftSpectrum {
 float levels[32]={};float peaks[32]={};std::uint32_t holds[32]={};
 std::uint32_t lastTime=0;bool initialized=false;
 template<class Read> void update(Read read,float gain,std::uint32_t now) {
  const float elapsed=initialized ? float(std::uint32_t(now-lastTime))*.001f : .02322f;
  lastTime=now;initialized=true;
  const float attack=1.f-std::exp(-elapsed/.01443f);
  const float decay=1.f-std::exp(-elapsed/.2462f);
  for(unsigned i=0;i<32;++i) {
   float magnitude=0.f;
   // Peak aggregation preserves narrow tones independently of bucket width.
   for(unsigned bin=spectrumBegin(i);bin<spectrumEnd(i);++bin) {
    const float value=read(bin);
    if(std::isfinite(value) && value>magnitude)magnitude=value;
   }
   magnitude*=gain;
   // Fixed logarithmic scale: -60 to -6 dB relative to FFT magnitude.
   // This is a display scale, not AGC or a change to the reactive FFT values.
   float target=magnitude>.001f ? (20.f*std::log10(magnitude)+60.f)/54.f : 0.f;
   if(!std::isfinite(target) || target<0.f)target=0.f;
   if(target>1.f)target=1.f;
   levels[i]+=(target-levels[i])*(target>levels[i]?attack:decay);
   if(levels[i]>=peaks[i]){peaks[i]=levels[i];holds[i]=now;}
   else {
    const std::uint32_t age=now-holds[i];
    // Decay only the elapsed portion after the one-second hold expires.
    const float release=age>1000 ? std::fmin(elapsed,float(age-1000)*.001f) : 0.f;
    peaks[i]=std::fmax(levels[i],peaks[i]-.4f*release);
   }
  }
 }
};
}
