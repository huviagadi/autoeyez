#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
namespace autoeyez {
struct ClipListReceiver {
 char staged[64][20]={},identity[17]={},completed[17]={};uint8_t lengths[64]={};uint64_t seen=0;
 uint32_t expected=0,completedCrc=0,updated=0;unsigned size=0;bool active=false,published=false;
 char latestSession[9]={};uint32_t latestSequence=0;
 static int nibble(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
 static bool hex(const char*s,unsigned n,uint32_t&value){if(std::strlen(s)!=n)return false;value=0;for(unsigned i=0;i<n;i++){int v=nibble(s[i]);if(v<0)return false;value=(value<<4)|unsigned(v);}return true;}
 static bool id(const char*s){if(std::strlen(s)!=16)return false;for(unsigned i=0;i<16;i++)if(nibble(s[i])<0)return false;return true;}
 static bool number(const char*s,unsigned maximum,unsigned&value){if(!*s)return false;value=0;for(;*s;s++){if(*s<'0'||*s>'9')return false;value=value*10+unsigned(*s-'0');if(value>maximum)return false;}return true;}
 static uint32_t byte(uint32_t crc,uint8_t value){crc^=value;for(int i=0;i<8;i++)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));return crc;}
 uint32_t checksum()const{uint32_t crc=byte(0xffffffffu,uint8_t(size));for(unsigned i=0;i<size;i++){crc=byte(crc,lengths[i]);for(unsigned j=0;j<lengths[i];j++)crc=byte(crc,uint8_t(staged[i][j]));}return crc^0xffffffffu;}
 bool handle(const char*line,uint32_t now,char (&live)[64][20],int&count,char*ack,unsigned ackSize){
  published=false;if(ackSize)ack[0]=0;
  if(std::strncmp(line,"CLB1:",5)&&std::strncmp(line,"CLI1:",5)&&std::strncmp(line,"CLE1:",5))return false;
  if(active&&uint32_t(now-updated)>3000){active=false;seen=0;}
  if(std::strlen(line)>=80)return true;
  char buffer[80];std::strcpy(buffer,line);char*parts[4]={buffer};unsigned n=1;
  for(char*p=buffer;*p;p++)if(*p==':'){if(n==4)return true;*p=0;parts[n++]=p+1;}
  if(n<2||!id(parts[1]))return true;
  if(!std::strcmp(parts[0],"CLB1")){
   unsigned length;uint32_t crc,sequence;
   if(n!=4||!number(parts[2],64,length)||!hex(parts[3],8,crc)||!hex(parts[1]+8,8,sequence))return true;
   if(!std::strcmp(completed,parts[1]))return true;
   if(!std::strcmp(identity,parts[1])){if(size==length&&expected==crc){if(!active)seen=0;active=true;updated=now;}return true;}
   if(!std::strncmp(latestSession,parts[1],8)&&sequence<=latestSequence)return true;
   std::memcpy(latestSession,parts[1],8);latestSession[8]=0;latestSequence=sequence;
   std::strcpy(identity,parts[1]);size=length;expected=crc;seen=0;active=true;updated=now;return true;
  }
  if(!std::strcmp(parts[0],"CLE1")&&n==2&&!std::strcmp(completed,parts[1])){std::snprintf(ack,ackSize,"CLA1:%s:%08lx",completed,(unsigned long)completedCrc);return true;}
  if(!active||std::strcmp(identity,parts[1]))return true;
  if(!std::strcmp(parts[0],"CLI1")){
   unsigned index;const unsigned bytes=n==4?unsigned(std::strlen(parts[3])):0;
   if(n!=4||!number(parts[2],63,index)||index>=size||bytes>38||bytes%2)return true;
   char name[20]={};for(unsigned i=0;i<bytes/2;i++){int a=nibble(parts[3][i*2]),b=nibble(parts[3][i*2+1]);if(a<0||b<0||!(a||b))return true;name[i]=char(a*16+b);}
   uint64_t bit=uint64_t(1)<<index;
   if(seen&bit){if(lengths[index]!=bytes/2||std::memcmp(staged[index],name,20)){active=false;seen=0;}return true;}
   std::memcpy(staged[index],name,20);lengths[index]=uint8_t(bytes/2);seen|=bit;updated=now;return true;
  }
  if(std::strcmp(parts[0],"CLE1")||n!=2)return true;
  uint64_t required=size==64?UINT64_MAX:((uint64_t(1)<<size)-1);
  if(seen!=required||checksum()!=expected)return true;
  std::memcpy(live,staged,size*20);count=int(size);published=true;active=false;
  std::strcpy(completed,identity);completedCrc=expected;
  std::snprintf(ack,ackSize,"CLA1:%s:%08lx",completed,(unsigned long)completedCrc);return true;
 }
};
struct SerialLineBuffer {
 char data[1024]={};unsigned used=0;bool overflow=false;
 template<class Handle>void feed(char c,Handle handle){
  if(c=='\n'){if(!overflow&&used){data[used]=0;handle(data,used);}used=0;overflow=false;}
  else if(c=='\0'){overflow=true;used=0;}
  else if(!overflow){if(used<sizeof(data)-1)data[used++]=c;else{overflow=true;used=0;}}
 }
};
}
