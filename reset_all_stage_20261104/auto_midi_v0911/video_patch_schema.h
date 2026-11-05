#pragma once
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <initializer_list>
#include "patch_slot_bounds.h"
namespace autoeyez {
struct PatchField {const char* key;int low,high;};
static const PatchField patchFields[]={
{"enc0",0,127},
{"enc1",0,127},
{"enc2",0,127},
{"enc3",0,127},
{"enc4",0,127},
{"enc5",0,127},
{"enc6",0,127},
{"enc7",0,127},
{"enc8",0,127},
{"enc9",0,127},
{"enc10",0,127},
{"enc11",0,127},
{"enc12",0,127},
{"enc13",0,127},
{"enc14",0,127},
{"enc15",0,127},
{"enc16",0,127},
{"enc17",0,127},
{"ctrl0",0,1},
{"ctrl1",0,1},
{"ctrl2",0,1},
{"ctrl3",0,1},
{"ctrl4",0,1},
{"ctrl5",0,1},
{"ctrl6",0,1},
{"ctrl7",0,1},
{"func0",0,3},
{"func1",0,3},
{"func2",0,3},
{"func3",0,3},
{"func4",0,3},
{"func5",0,3},
{"func6",0,3},
{"func7",0,3},
{"react",-1,2},
{"lumaKey",0,127},
{"lumaHigh",0,127},
{"lumaLowEn",0,1},
{"lumaHiEn",0,1},
{"renderLumaInvert",0,1},
{"renderMirror",0,1},
{"mix",0,127},
{"chA",0,27},
{"chB",0,27},
{"lumaSrc",0,28},
{"lumaTrack",0,1},
{"mixMode",0,2},
{"clip",0,63},
{"loop",0,1},
{"srcHue",0,127},
{"srcSat",0,127},
{"srcVal",0,127},
{"srcShader",0,127},
{"mosh",0,127},
{"chroma",0,127},
{"crush",0,127},
{"pixel",0,127},
{"bleed",0,127},
{"align",0,127},
{"ghost",0,127},
{"wobble",0,127},
{"overdrive",0,127},
{"hold",0,127},
{"peek",0,127},
{"peekMask",0,1},
{"codecLevel",0,127},
{"codecStatic",0,127},
{"codecLow",0,127},
{"codecMid",0,127},
{"codecHigh",0,127},
{"codecRate",0,127},
{"houseCard",0,1},
{"houseLogo",0,1},
{"mainLogo",0,1},
{"patchVersion",1,7},
{"slot",0,videoPatchSlotCount-1},
};
constexpr size_t patchFieldCount=sizeof(patchFields)/sizeof(patchFields[0]);
struct VideoPatch {int values[patchFieldCount]={};bool seen[patchFieldCount]={};
 int index(const char* key) const {for(size_t i=0;i<patchFieldCount;++i)if(std::strcmp(key,patchFields[i].key)==0)return int(i);return -1;}
 int get(const char* key) const {return values[index(key)];}
 bool put(const char* key,int v) {int i=index(key);if(i<0||v<patchFields[i].low||v>patchFields[i].high||seen[i])return false;values[i]=v;seen[i]=true;return true;}
 bool renderMarkerSeen=false;
 bool line(char* text) {
  char* eq=std::strchr(text,'=');if(!eq||eq==text)return false;*eq++=0;
  errno=0;char* end=nullptr;long n=std::strtol(eq,&end,10);
  if(errno||end==eq||*end||n<INT_MIN||n>INT_MAX)return false;
  // Explicit migration marker from the independently mapped Renderer release.
  if(std::strcmp(text,"renderVersion")==0){if(renderMarkerSeen)return false;renderMarkerSeen=true;return n==1;}
  return put(text,int(n));
 }
 bool finish(int slot) {
  int ver=index("patchVersion"),si=index("slot");
  bool modern=seen[ver];if(modern && (!seen[si]||values[si]!=slot))return false;
  if(!modern){values[ver]=1;seen[ver]=true;values[si]=slot;seen[si]=true;
   for(auto key:{"renderLumaInvert","renderMirror"}){int i=index(key);if(!seen[i]){values[i]=get(std::strcmp(key,"renderMirror")==0?"ctrl3":"ctrl0");seen[i]=true;}}
  }
  // Existing patches predate the five input-processing controls.
  for(auto key:{"bleed","align","ghost","wobble","overdrive"}){int i=index(key);if(!seen[i]){values[i]=0;seen[i]=true;}}
  {int i=index("hold");if(!seen[i]){if(values[ver]>=4)return false;values[i]=0;seen[i]=true;}}
  for(auto key:{"peek","peekMask"}){int i=index(key);if(!seen[i]){if(values[ver]>=5)return false;values[i]=0;seen[i]=true;}}
  for(auto key:{"codecLevel","codecLow","codecMid","codecHigh","codecRate"}){
   int i=index(key);if(!seen[i]){if(values[ver]>=6)return false;
    values[i]=std::strcmp(key,"codecLevel")==0?0:std::strcmp(key,"codecLow")==0?100:std::strcmp(key,"codecRate")==0?64:80;seen[i]=true;}}
  {int i=index("codecStatic");if(!seen[i]){if(values[ver]>=7)return false;values[i]=0;seen[i]=true;}}
  for(size_t i=0;i<patchFieldCount;++i)if(!seen[i])return false;
  return get("lumaKey")<=get("lumaHigh");
 }
};
}
