#pragma once
#include "fft_envelope.h"
#include "fft_band_patch.h"
#include <set>
#include <cmath>
namespace autoeyez {
struct AudioPatch {
 float coefficients[3][25]{};int reactive=0;EnvelopeConfig envelope;FftBands bands=FftBands::Legacy;
};
inline bool readAudioPatch(const std::string& text,int slot,AudioPatch& result){
 if(text.size()>16384||slot<1||slot>32)return false;
 std::istringstream normalize(text);std::string row,clean;while(std::getline(normalize,row)){if(!row.empty()&&row.back()=='\r')row.pop_back();clean+=row+"\n";}
 AudioPatch p;std::istringstream env(clean),bands(clean);
 if(!readEnvelopePatch(env,p.envelope)||!readBandPatch(bands,p.bands))return false;
 std::istringstream input(clean);std::string line;std::set<std::string> seen;bool coefficients[3][25]{};
 bool modern=false,identity=false,reactive=false;
 while(std::getline(input,line)){
  if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty())continue;
  auto eq=line.find('=');if(eq==std::string::npos||eq==0)return false;
  const std::string key=line.substr(0,eq),raw=line.substr(eq+1);
  if(!seen.insert(key).second)return false;
  if(key.compare(0,4,"env_")==0||key=="teensy_bands_version"||key=="teensy_bands_profile")continue;
  std::istringstream value(raw);double number;std::string extra;
  if(!(value>>number)||value>>extra||!std::isfinite(number))return false;
  if(key=="audio_version"){if(number!=2)return false;modern=true;continue;}
  if(key=="slot"){if(number!=slot)return false;identity=true;continue;}
  if(key=="react"){if(number<0||number>3||std::floor(number)!=number)return false;p.reactive=int(number);reactive=true;continue;}
  const char* prefixes[]={"low","mid","high"};int band=-1,index=-1;
  for(int i=0;i<3;++i){const std::string prefix=prefixes[i];if(key.compare(0,prefix.size(),prefix)==0){for(int j=1;j<=25;++j)if(key==prefix+std::to_string(j)){band=i;index=j-1;}}}
  // Legacy multipliers can exceed unity (up to ~8.13); preserve that range.
  if(band<0||std::abs(number)>16)return false;
  p.coefficients[band][index]=float(number);coefficients[band][index]=true;
 }
 if(!reactive||(modern&&!identity)||(!modern&&identity))return false;
 if(modern)for(const char* field:{"env_version","env_mode","env_smooth","env_attack","env_decay","env_bdecay","env_hit","env_slide","teensy_bands_version","teensy_bands_profile"})if(!seen.count(field))return false;
 for(int b=0;b<3;++b)for(int j=0;j<16;++j)if(!coefficients[b][j])return false;
 bool added=false;for(int b=0;b<3;++b)for(int j=16;j<20;++j)added=added||coefficients[b][j];
 if(added||modern)for(int b=0;b<3;++b)for(int j=16;j<20;++j)if(!coefficients[b][j])return false;
 bool inputProcessingAdded=false;for(int b=0;b<3;++b)for(int j=20;j<25;++j)inputProcessingAdded=inputProcessingAdded||coefficients[b][j];
 if(inputProcessingAdded)for(int b=0;b<3;++b)for(int j=20;j<25;++j)if(!coefficients[b][j])return false;
 result=p;return true;
}
}
