#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

namespace autoeyez {
struct EnvelopeConfig {
    bool shaped = false;
    std::array<int,6> values{{38,64,102,102,96,64}};
    bool operator==(const EnvelopeConfig& b) const {return shaped==b.shaped && values==b.values;}
};
inline bool validEnvelope(const EnvelopeConfig& c) {
    for(int v:c.values) if(v<0 || v>127) return false;
    return true;
}
inline bool parseEnvelope(const std::string& text, EnvelopeConfig& c, std::string& id) {
    std::istringstream in(text);int version,mode;std::string token,extra;EnvelopeConfig candidate;
    if(!(in>>version>>token>>mode) || version!=2 || (mode!=0 && mode!=1) || token.empty() || token.size()>96) return false;
    for(char ch:token) if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'))return false;
    for(int& v:candidate.values) if(!(in>>v)) return false;
    if(in>>extra || !validEnvelope(candidate)) return false;
    candidate.shaped=mode==1;c=candidate;id=token;return true;
}
inline std::string serializeEnvelope(const EnvelopeConfig& c,const std::string& id) {
    std::ostringstream out;out<<"2 "<<id<<" "<<(c.shaped?1:0);for(int v:c.values)out<<" "<<v;out<<"\n";return out.str();
}
inline double legacyDecayTau(){return -1.0/(30.0*std::log(.65));}
inline double releaseTau(int value) {
    if(value==127)return 0;
    return legacyDecayTau()*std::exp2((102-value)/24.0);
}
inline double riseTau(int value) {
    if(value==127)return 0;
    // 0: 1 second, 64: 5 ms, 127: immediate. Higher means faster.
    return .005*std::exp2((64-value)*std::log2(200.0)/64.0);
}
inline float approach(float current,float target,double dt,double tau) {
    if(tau<=0)return target;
    if(dt<=0)return current;
    return current+(target-current)*static_cast<float>(-std::expm1(-dt/tau));
}
class FftEnvelope {
public:
    EnvelopeConfig config;
    struct Band {float value=0,lastTarget=0,peak=0;double attackRemaining=0;};
    std::array<Band,3> bands;
    std::array<float,3> update(const std::array<float,3>& input,double dt,float maxInput=1.0f) {
        if(!std::isfinite(dt) || dt<0)dt=0;
        std::array<float,3> result;
        const double smooth=std::exp2((config.values[0]-38)/44.5);
        const double slide=riseTau(config.values[5])*smooth;
        const double attack=riseTau(config.values[1])*smooth;
        for(size_t i=0;i<3;++i){
            auto& b=bands[i];const float raw=std::isfinite(input[i])?input[i]:0;
            const float target=std::max(0.f,std::min(maxInput,raw));
            if(!config.shaped){
                b.attackRemaining=0;
                b.value=target>=b.value?target:approach(b.value,target,dt,legacyDecayTau());
            }else{
                if(target>b.value){
                    // A step in the raw signal triggers Hit once, not once per rendered frame.
                    if(target-b.lastTarget>=.05f){
                        b.value+=(target-b.value)*(config.values[4]/127.f);
                        b.attackRemaining=.05; // initial 50 ms, then Slide takes over
                    }
                    const double initial=std::min(dt,b.attackRemaining);
                    if(b.attackRemaining>0)b.value=approach(b.value,target,initial,attack);
                    b.attackRemaining-=initial;
                    if(dt>initial)b.value=approach(b.value,target,dt-initial,slide);
                }else{
                    b.attackRemaining=0;
                    const float split=b.peak*.25f;
                    const double decay=releaseTau(config.values[2])*smooth;
                    const double tail=releaseTau(config.values[3])*smooth;
                    if(target<.01f && target<split){
                        if(b.value<=split)b.value=approach(b.value,target,dt,tail);
                        else {
                            const double crossing=decay<=0?0:decay*std::log((b.value-target)/(split-target));
                            b.value=crossing<dt?approach(split,target,dt-crossing,tail):approach(b.value,target,dt,decay);
                        }
                    }else b.value=approach(b.value,target,dt,decay);
                }
            }
            if(target>b.lastTarget)b.peak=std::max(target,b.value);
            if(b.value<.00001f && target<.00001f)b.peak=0;
            // Keep the peak while releasing; it selects the B-Decay tail.
            result[i]=b.value;b.lastTarget=target;
        }
        return result;
    }
};
}

namespace autoeyez {
inline void writeEnvelopePatch(std::ostream& out,const EnvelopeConfig& config) {
    out<<"env_version=2\nenv_mode="<<(config.shaped?1:0)<<"\n";
    const char* names[]={"smooth","attack","decay","bdecay","hit","slide"};
    for(size_t i=0;i<6;++i)out<<"env_"<<names[i]<<"="<<config.values[i]<<"\n";
}
inline bool readEnvelopePatch(std::istream& in,EnvelopeConfig& config) {
    EnvelopeConfig candidate;bool seen[8]={false};bool any=false;std::string line;
    const char* keys[]={"env_version","env_mode","env_smooth","env_attack","env_decay","env_bdecay","env_hit","env_slide"};
    while(std::getline(in,line)) {
        if(line.compare(0,4,"env_")!=0)continue;
        any=true;auto eq=line.find('=');if(eq==std::string::npos)return false;
        const auto key=line.substr(0,eq);int index=-1;
        for(int i=0;i<8;++i)if(key==keys[i])index=i;
        if(index<0 || seen[index])return false;
        std::istringstream value(line.substr(eq+1));int v;std::string extra;
        if(!(value>>v) || value>>extra)return false;
        if(index==0){if(v!=2)return false;}
        else if(index==1){if(v!=0&&v!=1)return false;candidate.shaped=v==1;}
        else candidate.values[index-2]=v;
        seen[index]=true;
    }
    if(any)for(bool field:seen)if(!field)return false;
    if(!validEnvelope(candidate))return false;
    config=candidate;return true;
}
}
