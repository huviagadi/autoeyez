#pragma once
#include <cstdint>
void serviceControllerOled();
namespace autoeyez {
struct ControllerPerf {
 uint32_t loops=0,workMax=0,gapMax=0,oledCount=0,oledTotal=0,oledMax=0,oledPerLoopMax=0,fftReads=0,loopOled=0,lastStart=0;bool started=false;
 uint32_t buckets[10]={};
 static uint32_t bound(unsigned i){const uint32_t b[]={50,100,250,500,1000,2000,5000,10000,20000,0xffffffffu};return b[i];}
 void begin(uint32_t now){if(started){uint32_t d=now-lastStart;if(d>gapMax)gapMax=d;}lastStart=now;started=true;loopOled=0;}
 void end(uint32_t elapsed){loops++;if(elapsed>workMax)workMax=elapsed;unsigned i=0;while(i<9&&elapsed>bound(i))i++;buckets[i]++;if(loopOled>oledPerLoopMax)oledPerLoopMax=loopOled;}
 void display(uint32_t elapsed){oledCount++;loopOled++;oledTotal+=elapsed;if(elapsed>oledMax)oledMax=elapsed;}
 uint32_t p95()const{uint32_t n=0,target=(loops*95+99)/100;for(unsigned i=0;i<10;i++){n+=buckets[i];if(n>=target)return bound(i);}return 0;}
 void reset(){const uint32_t start=lastStart;const bool seen=started;*this=ControllerPerf{};lastStart=start;started=seen;}
};
struct ControllerLoopSample {ControllerPerf& perf;uint32_t start;ControllerLoopSample(ControllerPerf&p,uint32_t now):perf(p),start(now){perf.begin(now);}~ControllerLoopSample(){serviceControllerOled();perf.end(micros()-start);}};
}
