#pragma once
#include <cstdint>
namespace autoeyez {
inline int32_t encoderDifference(int32_t previous,int32_t current){return int32_t(uint32_t(previous)-uint32_t(current));}
inline int takeSteps(long& remainder,int threshold){int steps=int(remainder/threshold);remainder-=long(steps)*threshold;return steps;}
inline int boundedValue(int value,int64_t delta,int low,int high){int64_t result=int64_t(value)+delta;return result<low?low:result>high?high:int(result);}
inline int wrappedValue(int value,int delta,int count){int64_t result=(int64_t(value)+delta)%count;return int(result<0?result+count:result);}
struct OledBudget {uint16_t dirty=0;unsigned cursor=0;uint32_t last=0;bool sent=false;
 void mark(unsigned index){if(index<9)dirty|=uint16_t(1u<<index);}
 int next(uint32_t now){if(!dirty||(sent&&uint32_t(now-last)<2000))return -1;for(unsigned j=0;j<9;j++){unsigned i=(cursor+j)%9;if(dirty&(1u<<i)){dirty&=uint16_t(~(1u<<i));cursor=(i+1)%9;last=now;sent=true;return int(i);}}return -1;}
};
struct FftSnapshot {uint16_t bins[128]={};uint32_t generation=0;void capture(const uint16_t* input){for(unsigned i=0;i<128;i++)bins[i]=input[i];generation++;}float read(unsigned bin)const{return bin<128?float(bins[bin])*(1.f/16384.f):0.f;}};
}
