// Saints Reborn Map Converter - big-endian byte helpers shared by the file format code.
#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace mapconv {

using Bytes = std::vector<uint8_t>;

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

inline uint32_t Rd32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint16_t Rd16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline int32_t RdI32(const uint8_t* p) { return int32_t(Rd32(p)); }
inline int16_t RdS16(const uint8_t* p) { return int16_t(Rd16(p)); }
inline float RdF(const uint8_t* p) {
  uint32_t v = Rd32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
inline void Wr16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
inline void WrF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Wr32(p, v);
}
inline size_t Al(size_t c, size_t a) { return (c + a - 1) / a * a; }

// Bounds-checked views into a byte vector.
inline uint32_t U32(const Bytes& b, size_t o) {
  if (o + 4 > b.size()) throw Error("read past end of data");
  return Rd32(&b[o]);
}
inline uint16_t U16(const Bytes& b, size_t o) {
  if (o + 2 > b.size()) throw Error("read past end of data");
  return Rd16(&b[o]);
}
inline int16_t S16(const Bytes& b, size_t o) { return int16_t(U16(b, o)); }
inline int32_t I32(const Bytes& b, size_t o) { return int32_t(U32(b, o)); }
inline float F32(const Bytes& b, size_t o) {
  uint32_t v = U32(b, o);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline Bytes Slice(const Bytes& b, size_t from, size_t to) {
  if (from > to || to > b.size()) throw Error("slice past end of data");
  return Bytes(b.begin() + from, b.begin() + to);
}
inline void Put32(Bytes& b, uint32_t v) {
  size_t n = b.size();
  b.resize(n + 4);
  Wr32(&b[n], v);
}
inline void Put16(Bytes& b, uint16_t v) {
  size_t n = b.size();
  b.resize(n + 2);
  Wr16(&b[n], v);
}
inline void PutF(Bytes& b, float f) {
  size_t n = b.size();
  b.resize(n + 4);
  WrF(&b[n], f);
}
inline void Append(Bytes& b, const Bytes& x) { b.insert(b.end(), x.begin(), x.end()); }
// Pads so that (base + size) is a multiple of a.
inline void PadTo(Bytes& b, size_t a, size_t base = 0) {
  size_t pos = base + b.size();
  b.resize(b.size() + (Al(pos, a) - pos), 0);
}

}  // namespace mapconv
