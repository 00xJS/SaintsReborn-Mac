#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

// Native NPC create payload: LE position[3], quaternion[4], model name + NUL,
// animation byte count, animation bytes. See 823A7AB0 and 8238B0B8.
namespace coop {
constexpr unsigned kNativeCapacity = 1024;
#pragma pack(push, 1)
struct NativePacket {
  char magic[4] = {'W','N','P','3'};
  uint32_t sequence = 0;
  uint32_t host_handle = 0;
  uint32_t replication_type = 6;
  uint32_t payload_size = 0;
  uint32_t time = 0; // sender clock, ms
  uint8_t payload[kNativeCapacity]{};
};
#pragma pack(pop)
static_assert(sizeof(NativePacket) < 1200);
inline bool ValidateNpc(const NativePacket& p, size_t bytes) {
  if (bytes != sizeof(p) || std::memcmp(p.magic,"WNP3",4) ||
      !p.host_handle || p.replication_type != 6 ||
      p.payload_size < 31 || p.payload_size > kNativeCapacity) return false;
  float v[7];
  std::memcpy(v,p.payload,sizeof(v));
  for (float x : v) if (!std::isfinite(x)) return false;
  for (unsigned i=0;i<3;++i) if (std::abs(v[i])>100000) return false;
  const float norm=v[3]*v[3]+v[4]*v[4]+v[5]*v[5]+v[6]*v[6];
  if (norm<0.5f || norm>1.5f) return false;
  unsigned end=28;
  while (end<p.payload_size && p.payload[end]) {
    if (p.payload[end]<32 || p.payload[end]>126 || end-28>=255) return false;
    ++end;
  }
  if (end==28 || end+1>=p.payload_size) return false;
  return end+2+p.payload[end+1]==p.payload_size;
}
}
