#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "video_patch_schema.h"
namespace autoeyez {
inline std::uint32_t patchChecksum(const void* bytes,size_t size,std::uint32_t hash=2166136261u){const auto* p=static_cast<const unsigned char*>(bytes);for(size_t i=0;i<size;++i)hash=(hash^p[i])*16777619u;return hash;}
struct PendingSave {
 std::uint32_t magic=0x50545832u;
 char transaction[17]{};char operation[6]{};int slot=-1;
 bool hadPrevious=false;std::uint32_t previousChecksum=0,newChecksum=0;
 int values[patchFieldCount]{};std::uint32_t checksum=0;
 void seal(){checksum=patchChecksum(this,offsetof(PendingSave,checksum));}
 bool valid() const {return magic==0x50545832u&&slot>=0&&slot<(operation[0]=='V'?videoPatchSlotCount:audioPatchSlotCount)&&transaction[16]==0&&operation[5]==0&&std::strlen(transaction)==16&&(std::strcmp(operation,"VSAVE")==0||std::strcmp(operation,"ASAVE")==0)&&checksum==patchChecksum(this,offsetof(PendingSave,checksum));}
};
}
