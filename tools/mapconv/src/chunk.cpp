#include "chunk.h"

#include <algorithm>
#include <cstdlib>

namespace mapconv {
namespace {

constexpr uint32_t kMagic = 0xBBCACA12;

struct Walk {
  const Bytes& d;
  size_t c;
  std::vector<std::pair<std::string, std::pair<size_t, size_t>>> marks;  // name, (start, end)
  uint32_t H(size_t o) const { return U32(d, o); }
  void Mark(const std::string& n, size_t s) { marks.push_back({n, {s, c}}); }
  size_t CStrEnd(size_t at) const {
    auto it = std::find(d.begin() + at, d.end(), uint8_t(0));
    if (it == d.end()) throw Error("unterminated string");
    return size_t(it - d.begin()) + 1;
  }
  // 821173C0 (r5 = 2): u32 n, align64, n x 44 B, then sum(+12) x u16.
  std::pair<size_t, uint32_t> Tree(size_t at) const {
    uint32_t n = U32(d, at);
    size_t c2 = at + 4;
    c2 = Al(c2, 64);
    size_t b = c2;
    c2 += 44 * size_t(n);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) sum += U32(d, b + 44 * i + 12);
    c2 += size_t(sum) * 2;
    return {c2, n};
  }
  // 82633DC8: node +0 i32 count (<0 empty, 0 = two 32-byte children).
  size_t KdTree(size_t node, size_t c2, uint32_t esz) const {
    while (I32(d, node) >= 0) {
      int32_t cnt = I32(d, node);
      size_t data = c2;
      if (cnt == 0) {
        c2 += 64;
        c2 = KdTree(data, c2, esz);
        node = data + 32;
        continue;
      }
      c2 += size_t(cnt) * esz;
      c2 = Al(c2, 4);
      break;
    }
    return c2;
  }
};

struct Front {
  size_t strings_at, strings_end, counts_at, a164_at, inst_at, a184_at, blob_at, blob_size, geo_at;
  ChunkCounts n;
};

Front ParseFront(const Bytes& d) {
  if (d.size() < 968 || U32(d, 0) != kMagic) throw Error("not a Saints Row map chunk (.bbchunk_xbox2)");
  if (U32(d, 4) != 85) throw Error("unsupported chunk version");
  Front f;
  size_t c = Al(968, 64);
  f.strings_at = c;
  uint32_t n = U32(d, c);
  c += 4 + 4 * size_t(n);
  for (uint32_t i = 0; i < n; ++i) {
    auto it = std::find(d.begin() + c, d.end(), uint8_t(0));
    if (it == d.end()) throw Error("bad string table");
    c = size_t(it - d.begin()) + 1;
  }
  f.strings_end = c;
  c = Al(c, 64);
  f.counts_at = c;
  f.n.meshes = U32(d, c);
  f.n.instances = U32(d, c + 4);
  f.n.vbs = U32(d, c + 8);
  f.n.c180 = U32(d, c + 12);
  c += 16;
  c = Al(c, 64);
  f.a164_at = c;
  c += 16 * size_t(f.n.meshes);
  c = Al(c, 64);
  f.inst_at = c;
  c += 112 * size_t(f.n.instances);
  c = Al(c, 64);
  f.a184_at = c;
  c += 100 * size_t(f.n.c180);
  c = Al(c, 64);
  c = Al(c, 64);
  f.blob_size = U32(d, c);
  c += 4;
  f.blob_at = c;
  c += f.blob_size;
  c = Al(c, 64);
  f.geo_at = c;
  return f;
}

std::vector<std::pair<std::string, std::pair<size_t, size_t>>> WalkTail(const Bytes& d, size_t geo_end,
                                                                        size_t& end_out) {
  Walk w{d, geo_end, {}};
  size_t& c = w.c;
  auto H = [&](size_t o) { return w.H(o); };
  size_t s;
  c = Al(c, 64);
  c = Al(c, 64);
  {
    uint32_t n = H(868);
    s = c;
    c += 80 * size_t(n);
    for (uint32_t i = 0; i < n; ++i) c = w.CStrEnd(c);
    w.Mark("static_records", s);
  }
  s = c = Al(c, 64); { uint32_t sz = U32(d, c); c += 4 + sz; } w.Mark("rec_blob308", s);
  s = c = Al(c, 64); { uint32_t n = U32(d, c); c += 4 + 4 * size_t(n); } w.Mark("u32_list524", s);
  for (int key : {540, 548}) {
    s = c = Al(c, 64);
    uint32_t n = U32(d, c);
    c += 4;
    size_t b = c;
    c += 80 * size_t(n);
    for (uint32_t i = 0; i < n; ++i) c += size_t(U32(d, b + 80 * i + 72)) * 4;
    w.Mark("rec80_" + std::to_string(key), s);
  }
  s = c = Al(c, 64); { int32_t n = I32(d, 900); c += 84 * size_t(std::max(n, 0)); } w.Mark("spawns900", s);
  s = c = Al(c, 64); c += 48 * size_t(H(872)); w.Mark("rec48_204", s);
  s = c = Al(c, 64); c = w.Tree(c).first; w.Mark("tree236", s);
  s = c = Al(c, 64); c += 2 * size_t(H(936)); w.Mark("u16_936", s);
  s = c = Al(c, 64); c += H(864); w.Mark("skip864", s);
  s = c = Al(c, 64);
  c += 28 * size_t(H(908)) + 24 * size_t(H(912)) + 28 * size_t(H(916)) + 4 * size_t(H(920)) + 12 * size_t(H(924)) +
       8 * size_t(H(928)) + H(932);
  w.Mark("nav908", s);
  s = c = Al(c, 64);
  if (H(896) > 0) throw Error("template chunk has an unsupported section (header 896)");
  w.Mark("mesh896", s);
  {
    s = c = Al(c, 64);
    uint32_t n = H(884);
    size_t b = c;
    c += 136 * size_t(n);
    for (uint32_t i = 0; i < n; ++i) c = w.CStrEnd(c);
    c = Al(c, 64);
    for (uint32_t i = 0; i < n; ++i) {
      int32_t v = I32(d, b + 136 * i + 20);
      if (v > 0) c += 4 * size_t(v);
    }
    c = Al(c, 64);
    for (uint32_t i = 0; i < n; ++i) {
      int32_t v = I32(d, b + 136 * i + 28);
      if (v) c += 2 * size_t(std::abs(v));
    }
    w.Mark("rec136_260", s);
  }
  s = c = Al(c, 4096); c = w.Tree(c).first; w.Mark("tree192", s);
  s = c = Al(c, 64); c += 2 * size_t(H(888)); w.Mark("u16_196", s);
  s = c = Al(c, 64); { uint32_t sz = U32(d, c); c += 4 + sz; } w.Mark("blob_a", s);
  s = c = Al(c, 64); { uint32_t sz = U32(d, c); c += 4; c += sz; } w.Mark("rec40_324", s);
  s = c = Al(c, 64); { uint32_t n = U32(d, c); c += 4 + 80 * size_t(n); } w.Mark("rec80_268", s);
  s = c = Al(c, 64); { uint32_t n = U32(d, c); c += 4 + 200 * size_t(n); c = w.Tree(c).first; } w.Mark("rec200_52+tree60", s);
  s = c = Al(c, 64); { uint32_t sz = U32(d, c + 8); c += sz; } w.Mark("block556", s);
  s = c = Al(c, 64); { uint32_t n = U32(d, c); c += 4 + 52 * size_t(n); } w.Mark("rec52_424", s);
  s = c = Al(c, 64); for (int k = 0; k < 2; ++k) { uint32_t sz = U32(d, c); c += 4 + sz; } w.Mark("blobs76_84", s);
  s = c = Al(c, 64); for (int k = 0; k < 2; ++k) { uint32_t sz = U32(d, c); c += 4 + sz; } w.Mark("blobs92_100", s);
  s = c = Al(c, 64); { uint32_t n = U32(d, c); c += 4 + 100 * size_t(n); } w.Mark("rec100_104", s);
  s = c = Al(c, 64); { uint32_t sz = U32(d, c); c += 4 + sz; } w.Mark("blob_82515210", s);
  s = c = Al(c, 64); { uint32_t n = U32(d, c); c += 4 + 32 * size_t(n); } w.Mark("rec32_348", s);
  {
    s = c = Al(c, 64);
    uint32_t n = U32(d, c);
    c += 4;
    if (n > 0) {
      uint32_t esz = U32(d, c);
      size_t root = c + 12;
      c += 44;
      c = w.KdTree(root, c, esz);
    }
    w.Mark("kdtree356", s);
  }
  {
    s = c = Al(c, 64);
    uint32_t n = U32(d, c);
    c += 4;
    size_t b = c;
    c += 40 * size_t(n);
    for (uint32_t i = 0; i < n; ++i) c += size_t(U32(d, b + 40 * i)) * 12 + size_t(U32(d, b + 40 * i + 8)) * 12;
    w.Mark("rec40_360", s);
  }
  s = c = Al(c, 64); { uint32_t sz = U32(d, c); c += 4 + sz; } w.Mark("blob_822500D8", s);
  uint32_t n372;
  s = c = Al(c, 64); { uint32_t sz = U32(d, c); c += 4; n372 = U32(d, c); c += sz; } w.Mark("list372", s);
  s = c = Al(c, 64); { c += 4; c += H(204) - 4; } w.Mark("skip388", s);
  s = c = Al(c, 64); { uint32_t n = U32(d, c); c += 4 + 76 * size_t(n); } w.Mark("rec76_396", s);
  {
    s = c = Al(c, 64);
    uint32_t n = U32(d, c);
    c += 4;
    if (n > 0) {
      c += 96 * size_t(n);
      uint32_t sz = U32(d, c);
      c += 4 + sz;
    }
    w.Mark("movers404", s);
  }
  for (int k : {440, 444, 448}) {
    s = c = Al(c, 64);
    uint32_t sz = U32(d, c + 8);
    c += sz;
    w.Mark("block" + std::to_string(k), s);
  }
  s = c = Al(c, 64); { uint32_t n = U32(d, c); c += 4 + 60 * size_t(n); } w.Mark("rec60_416", s);
  {
    s = c = Al(c, 64);
    uint32_t n = U32(d, c);
    c += 4;
    for (uint32_t i = 0; i < n; ++i) {
      c = Al(c, 4);
      size_t h = c;
      c += 40;
      c = Al(c, 16);
      c += size_t(U16(d, h + 24)) * 16 + size_t(U16(d, h + 32)) * 6;
    }
    w.Mark("rec40_380", s);
  }
  {
    s = c = Al(c, 64);
    for (uint32_t i = 0; i < n372; ++i) {
      c = Al(c, 4);
      uint32_t sz = U32(d, c);
      c += 4 + sz;
    }
    w.Mark("blobs384", s);
  }
  {
    s = c = Al(c, 64);
    uint32_t n = U32(d, c);
    c += 4;
    if (n > 0) {
      c += 36 * size_t(n);
      uint32_t m = U32(d, c);
      c += 4 + 36 * size_t(m);
      uint32_t sz = U32(d, c);
      c += 4 + sz;
    }
    w.Mark("rec36_432", s);
  }
  s = c = Al(c, 64); { uint32_t n = U32(d, c); c += 4 + 52 * size_t(n); uint32_t sz = U32(d, c); c += 4 + sz; } w.Mark("rec52_452", s);
  {
    s = c = Al(c, 64);
    uint32_t n1 = U32(d, c); c += 4 + 4 * size_t(n1);
    uint32_t n2 = U32(d, c); c += 4 + 88 * size_t(n2);
    uint32_t n3 = U32(d, c); c += 4 + 3 * size_t(n3);
    uint32_t sz = U32(d, c); c += 4 + sz;
    w.Mark("rec_460_468_476", s);
  }
  {
    s = c = Al(c, 64);
    uint32_t n = U32(d, c);
    c += 4;
    if (n) {
      c += 88 * size_t(n);
      for (uint32_t i = 0; i < n; ++i) {
        c = Al(c, 4);
        uint32_t a = U32(d, c), b2 = U32(d, c + 4);
        c += 16 + size_t(a) * 16 + size_t(b2) * 6;
      }
    }
    w.Mark("rec88_484", s);
  }
  {
    s = c = Al(c, 64);
    int32_t n = I32(d, c);
    if (n > 32767 || n < 0) {  // some stock files store this count little-endian
      n = int32_t(uint32_t(d[c]) | (uint32_t(d[c + 1]) << 8) | (uint32_t(d[c + 2]) << 16) | (uint32_t(d[c + 3]) << 24));
    }
    c += 4 + 44 * size_t(n);
    w.Mark("rec44_492", s);
  }
  s = c = Al(c, 64); { uint32_t n = U32(d, c); c += 4 + 8 * size_t(n); } w.Mark("rec8_504", s);
  s = c = Al(c, 64); { uint32_t n = U32(d, c); c += 4 + 72 * size_t(n); } w.Mark("rec72_512", s);
  s = c = Al(c, 64); { uint32_t n = U32(d, c); c += 4 + 4 * size_t(n); } w.Mark("u32_532", s);
  for (int i = 0; i < 32; ++i) {
    c = Al(c, 64);
    c += 4;
  }
  c = Al(c, 2048);
  end_out = c;
  return w.marks;
}

// Header section directory: (offset, first section, last section, rule).
struct DirRule {
  size_t off;
  const char* first;
  const char* last;
  int rule;  // 0 exact, 1 end aligned to 64, 2 from 968 to the start of the section after `last`
};
const DirRule kDir[] = {
    {0x50, "blob_a", "blob_a", 1}, {0x58, "rec40_324", "rec40_324", 1}, {0x60, "rec80_268", "rec80_268", 0},
    {0x70, "@968", "mesh896", 2}, {0x78, "rec200_52+tree60", "rec200_52+tree60", 0},
    {0x80, "rec136_260", "u16_196", 0}, {0x88, "blobs76_84", "blobs76_84", 0}, {0x90, "blobs92_100", "blobs92_100", 0},
    {0x98, "rec100_104", "rec100_104", 0}, {0xa0, "blob_82515210", "blob_82515210", 0},
    {0xa8, "rec32_348", "rec32_348", 0}, {0xb0, "kdtree356", "kdtree356", 0},
    {0xb8, "rec40_360", "blob_822500D8", 0}, {0xc0, "list372", "list372", 0}, {0xc8, "skip388", "skip388", 0},
    {0xd0, "rec76_396", "rec76_396", 1}, {0xd8, "movers404", "movers404", 1}, {0xe0, "block440", "block440", 0},
    {0xe8, "block444", "block448", 0}, {0xf0, "rec60_416", "rec60_416", 0}, {0xf8, "rec40_380", "rec40_380", 0},
    {0x100, "blobs384", "blobs384", 0}, {0x108, "rec36_432", "rec36_432", 0}, {0x110, "rec52_452", "rec52_452", 1},
    {0x118, "rec_460_468_476", "rec_460_468_476", 1}, {0x120, "rec88_484", "rec88_484", 1},
    {0x128, "rec44_492", "rec44_492", 1}, {0x130, "rec8_504", "rec8_504", 1}, {0x138, "rec72_512", "rec72_512", 0},
    {0x140, "u32_532", "u32_532", 0}};

}  // namespace

Section& Chunk::Get(const std::string& name) {
  for (auto& s : secs)
    if (s.name == name) return s;
  throw Error("chunk has no section " + name);
}
const Section& Chunk::Get(const std::string& name) const {
  for (auto& s : secs)
    if (s.name == name) return s;
  throw Error("chunk has no section " + name);
}
size_t Chunk::OffsetOf(const std::string& name) const {
  size_t c = header.size();
  for (const auto& s : secs) {
    c = Al(c, s.align);
    if (s.name == name) return c;
    c += s.data.size();
  }
  throw Error("chunk has no section " + name);
}

Chunk ReadChunk(const Bytes& d) {
  Front f = ParseFront(d);
  uint32_t nm_count = f.n.meshes;
  Geometry g = ReadGeometry(d, f.geo_at, f.n.vbs, nm_count, f.a164_at);
  size_t end = 0;
  auto marks = WalkTail(d, g.end, end);
  if (end != d.size()) throw Error("chunk layout not understood (size mismatch)");
  Chunk ch;
  ch.header = Slice(d, 0, 968);
  ch.secs.push_back({"strings", 64, Slice(d, f.strings_at, f.strings_end)});
  ch.secs.push_back({"counts", 64, Slice(d, f.counts_at, f.counts_at + 16)});
  ch.secs.push_back({"a164", 64, Slice(d, f.a164_at, f.a164_at + 16 * size_t(f.n.meshes))});
  ch.secs.push_back({"instances", 64, Slice(d, f.inst_at, f.inst_at + 112 * size_t(f.n.instances))});
  ch.secs.push_back({"a184", 64, Slice(d, f.a184_at, f.a184_at + 100 * size_t(f.n.c180))});
  ch.secs.push_back({"blob64", 64, Slice(d, f.blob_at - 4, f.blob_at + f.blob_size)});
  ch.secs.push_back({"geometry", 64, Slice(d, f.geo_at, marks[0].second.first)});
  for (auto& m : marks) {
    size_t a = m.first == "tree192" ? 4096 : 64;
    ch.secs.push_back({m.first, a, Slice(d, m.second.first, m.second.second)});
  }
  return ch;
}

Bytes WriteChunk(const Chunk& ch) {
  Bytes out = ch.header;
  std::map<std::string, std::pair<size_t, size_t>> pos;
  std::vector<std::string> order;
  for (const auto& s : ch.secs) {
    PadTo(out, s.align);
    size_t a = out.size();
    Append(out, s.data);
    pos[s.name] = {a, out.size()};
    order.push_back(s.name);
  }
  for (int i = 0; i < 32; ++i) {
    PadTo(out, 64);
    out.resize(out.size() + 4, 0);
  }
  PadTo(out, 2048);
  for (const auto& r : kDir) {
    size_t start, end;
    if (std::string(r.first) == "@968") {
      auto it = std::find(order.begin(), order.end(), std::string(r.last));
      start = 968;
      end = pos[*(it + 1)].first;
    } else {
      start = pos[r.first].first;
      end = pos[r.last].second;
      if (r.rule == 1) end = Al(end, 64);
    }
    Wr32(&out[r.off], uint32_t(start));
    Wr32(&out[r.off + 4], uint32_t(end - start));
  }
  // 848..860: offsets and sizes of the two navigation blobs (stale in a few stock files; always rewritten)
  size_t b0 = pos["blobs76_84"].first;
  uint32_t s0 = U32(out, b0);
  size_t b1 = b0 + 4 + s0;
  uint32_t s1 = U32(out, b1);
  Wr32(&out[848], uint32_t(b0));
  Wr32(&out[852], s0);
  Wr32(&out[856], uint32_t(b1));
  Wr32(&out[860], s1);
  return out;
}

ChunkCounts Counts(const Chunk& ch) {
  const Bytes& c = ch.Get("counts").data;
  return {U32(c, 0), U32(c, 4), U32(c, 8), U32(c, 12)};
}

Geometry ChunkGeometry(const Chunk& ch) {
  // Re-read from a written copy so the geometry's absolute alignment is right.
  Bytes d = WriteChunk(ch);
  ChunkCounts n = Counts(ch);
  size_t geo = ch.OffsetOf("geometry");
  size_t a164 = ch.OffsetOf("a164");
  return ReadGeometry(d, geo, n.vbs, n.meshes, a164);
}

void SetGeometry(Chunk& ch, const Geometry& g) {
  size_t at = ch.OffsetOf("geometry");
  ch.Get("geometry").data = WriteGeometry(g, at);
}

std::vector<std::string> ChunkStrings(const Chunk& ch) {
  const Bytes& b = ch.Get("strings").data;
  uint32_t n = U32(b, 0);
  size_t c = 4 + 4 * size_t(n);
  std::vector<std::string> out;
  for (uint32_t i = 0; i < n; ++i) {
    size_t e = c;
    while (e < b.size() && b[e]) ++e;
    out.emplace_back(reinterpret_cast<const char*>(&b[c]), e - c);
    c = e + 1;
  }
  return out;
}

std::vector<uint32_t> AddStrings(Chunk& ch, const std::vector<std::string>& names) {
  Bytes& b = ch.Get("strings").data;
  uint32_t n = U32(b, 0);
  Bytes out;
  Put32(out, n + uint32_t(names.size()));
  out.insert(out.end(), b.begin() + 4, b.begin() + 4 + 4 * size_t(n));
  out.resize(out.size() + 4 * names.size(), 0);
  out.insert(out.end(), b.begin() + 4 + 4 * size_t(n), b.end());
  std::vector<uint32_t> idx;
  for (size_t i = 0; i < names.size(); ++i) {
    out.insert(out.end(), names[i].begin(), names[i].end());
    out.push_back(0);
    idx.push_back(n + uint32_t(i));
  }
  b = std::move(out);
  return idx;
}

}  // namespace mapconv
