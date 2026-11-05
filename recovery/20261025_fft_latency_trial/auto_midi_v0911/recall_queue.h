#pragma once
#include <array>
namespace autoeyez {
struct Recall {bool audio=false;int slot=0;};
class RecallQueue {
 std::array<Recall,4> values{};unsigned head=0,count=0;
public:
 bool push(bool audio,int slot){if(slot<0||slot>=32||count==values.size())return false;values[(head+count)%values.size()]={audio,slot};++count;return true;}
 bool pop(Recall& out){if(!count)return false;out=values[head];head=(head+1)%values.size();--count;return true;}
 unsigned size() const {return count;}
};
}
