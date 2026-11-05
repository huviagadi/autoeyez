#pragma once
namespace autoeyez {
struct SourceConfigMenu {
 int cursor=0; // Source enable, low value, cut enable, high value.
 bool editing=false;
 void turn(int delta,int& low,int& high) {
  if(!editing){cursor=(cursor+delta)%4;if(cursor<0)cursor+=4;return;}
  if(cursor==1){low+=delta;if(low<0)low=0;if(low>high)low=high;}
  if(cursor==3){high+=delta;if(high<low)high=low;if(high>127)high=127;}
 }
 // 1/2: toggle source/cut; 0: open/close value editor.
 int click() {
  if(editing){editing=false;return 0;}
  if(cursor==0)return 1;
  if(cursor==2)return 2;
  editing=true;return 0;
 }
};
}
