#pragma once
#include <cstring>
namespace autoeyez {
enum class FftBands {Legacy, Contiguous};
inline const char* bandProfileName(FftBands p){return p==FftBands::Contiguous?"CONTIGUOUS":"LEGACY";}
inline bool parseBandProfile(const char* name,FftBands& p){
    if(std::strcmp(name,"LEGACY")==0){p=FftBands::Legacy;return true;}
    if(std::strcmp(name,"CONTIGUOUS")==0){p=FftBands::Contiguous;return true;}
    return false;
}
struct BandEnergy{float low,mid,high;};
template<class Reader> BandEnergy readFftBands(Reader read,float gain,FftBands profile){
    BandEnergy bands{0,0,0};
    if(profile==FftBands::Legacy){
        for(int i=0;i<4;++i)bands.low+=read((i*4)+1);
        for(int i=4;i<12;++i)bands.mid+=read((i*4)+1);
        for(int i=12;i<32;++i)bands.high+=read((i*4)+1);
        bands.low*=gain*6.0f;bands.mid*=gain*4.0f;bands.high*=gain*3.0f;
    }else{
        // FFT256 at 44100 Hz: bin centers at k*172.265625 Hz.
        // DC excluded; [0,700), [700,2100), [2100,5500) Hz.
        for(int i=1;i<=4;++i)bands.low+=read(i);
        for(int i=5;i<=12;++i)bands.mid+=read(i);
        for(int i=13;i<=31;++i)bands.high+=read(i);
        bands.low*=gain*6.0f;bands.mid*=gain*4.0f;
        // Preserve the legacy expected level for spectrally flat noise:
        // 20 old high-band samples, 19 new samples. No automatic AGC.
        bands.high*=gain*(3.0f*20.0f/19.0f);
    }
    return bands;
}
}
