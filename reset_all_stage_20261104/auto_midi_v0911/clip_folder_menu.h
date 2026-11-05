#pragma once
#include <cstdint>
#include <cstring>

namespace autoeyez {
struct ClipFolderMenu {
  static constexpr int maxClips=64, maxFolders=15;
  char names[maxFolders][13]={{"CLASSICS"},{"FRESH"}};
  uint8_t assigned[maxClips]={};
  int folderCount=2, active=-1, selection=0, scroll=0;
  uint32_t clipCrc=0;

  static int hex(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;return -1;}
  static bool hex32(const char*text,uint32_t&out){if(std::strlen(text)!=8)return false;out=0;for(int i=0;i<8;i++){int h=hex(text[i]);if(h<0)return false;out=(out<<4)|uint32_t(h);}return true;}
  static uint32_t crcByte(uint32_t crc,uint8_t value){crc^=value;for(int i=0;i<8;i++)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));return crc;}
  static uint32_t crcText(const char*text){uint32_t crc=0xffffffffu;for(;*text;text++)crc=crcByte(crc,uint8_t(*text));return crc^0xffffffffu;}
  void reset(uint32_t crc){
    if(clipCrc==crc)return;
    std::memset(assigned,0,sizeof(assigned));std::memset(names,0,sizeof(names));
    std::strcpy(names[0],"CLASSICS");std::strcpy(names[1],"FRESH");
    folderCount=2;clipCrc=crc;active=-1;selection=scroll=0;
  }
  int folderAtRootRow(int row)const{return row==0?1:row==1?0:row;}
  int rows(int clipCount)const{if(active<0)return folderCount;int count=1;for(int i=0;i<clipCount&&i<maxClips;i++)if(assigned[i]==active)count++;return count;}
  int clipAtRow(int row,int clipCount)const{
    if(active<0||row<=0)return -1;
    for(int i=0;i<clipCount&&i<maxClips;i++)if(assigned[i]==active&&--row==0)return i;
    return -1;
  }
  void clamp(int clipCount){
    int count=rows(clipCount);if(selection>=count)selection=count-1;if(selection<0)selection=0;
    if(scroll>selection)scroll=selection;if(selection>=scroll+3)scroll=selection-2;
  }
  void step(int delta,int clipCount){
    int limit=rows(clipCount)-1;selection+=delta;if(selection<0)selection=0;if(selection>limit)selection=limit;clamp(clipCount);
  }
  // Returns a clip index to play; -1 means navigation only.
  int select(int clipCount){
    if(active<0){active=folderAtRootRow(selection);selection=scroll=0;return -1;}
    if(selection==0){int old=active;active=-1;selection=old==1?0:old==0?1:old;scroll=0;clamp(clipCount);return -1;}
    return clipAtRow(selection,clipCount);
  }
  bool receive(const char*line,uint32_t listCrc,int clipCount){
    if(std::strncmp(line,"FOL1:",5)||std::strlen(line)>400||clipCount<0||clipCount>maxClips)return false;
    char copy[401];std::strcpy(copy,line);char*first=copy+5;char*sep1=std::strchr(first,':');if(!sep1)return false;*sep1++=0;
    char*sep2=std::strchr(sep1,':');if(!sep2)return false;*sep2++=0;
    char*sep3=std::strchr(sep2,':');if(!sep3)return false;*sep3++=0;
    uint32_t claimedList,claimedFolder;
    if(!hex32(first,claimedList)||claimedList!=listCrc||!hex32(sep1,claimedFolder))return false;
    // CRC covers the exact folder-name CSV and assignment map.
    *(sep3-1)=':';
    if(crcText(sep2)!=claimedFolder)return false;
    *(sep3-1)=0;
    if(int(std::strlen(sep3))!=clipCount)return false;
    char stagedNames[maxFolders][13]={};int stagedCount=0;
    char*cursor=sep2;
    while(cursor){
      char*comma=std::strchr(cursor,',');if(comma)*comma=0;
      size_t length=std::strlen(cursor);if(stagedCount>=maxFolders||length<1||length>12)return false;
      for(size_t i=0;i<length;i++)if(!((cursor[i]>='A'&&cursor[i]<='Z')||(cursor[i]>='0'&&cursor[i]<='9')||cursor[i]=='_'))return false;
      for(int i=0;i<stagedCount;i++)if(!std::strcmp(stagedNames[i],cursor))return false;
      std::strcpy(stagedNames[stagedCount++],cursor);cursor=comma?comma+1:nullptr;
    }
    if(stagedCount<2||(std::strcmp(stagedNames[0],"CLASSICS")&&std::strcmp(stagedNames[0],"LIBRARY"))||std::strcmp(stagedNames[1],"FRESH"))return false;
    uint8_t staged[maxClips]={};for(int i=0;i<clipCount;i++){int id=hex(sep3[i]);if(id<0||id>=stagedCount)return false;staged[i]=uint8_t(id);}
    std::memcpy(names,stagedNames,sizeof(names));std::memcpy(assigned,staged,sizeof(assigned));
    folderCount=stagedCount;clipCrc=listCrc;if(active>=folderCount)active=-1;clamp(clipCount);return true;
  }
};
}
