// .bbchunk_xbox2 container (loader sub_82119E58): the 968-byte header and the file split into named
// sections. Writing puts every section back at its alignment, pads the end like the stock files and
// rebuilds the header's section directory (0x50..0x148) and the stale-offset words at 848.
#pragma once
#include <map>
#include "be.h"
#include "geometry.h"

namespace mapconv {

struct Section {
  std::string name;
  size_t align = 64;
  Bytes data;
};

struct Chunk {
  Bytes header;                  // 968 bytes
  std::vector<Section> secs;
  Section& Get(const std::string& name);
  const Section& Get(const std::string& name) const;
  uint32_t H(size_t off) const { return U32(header, off); }
  // Offset the section would be written at (sections before it unchanged in size).
  size_t OffsetOf(const std::string& name) const;
};

struct ChunkCounts {
  uint32_t meshes = 0, instances = 0, vbs = 0, c180 = 0;
};

Chunk ReadChunk(const Bytes& d);
Bytes WriteChunk(const Chunk& ch);
ChunkCounts Counts(const Chunk& ch);
// Geometry of a chunk (parsed from the chunk as currently written).
Geometry ChunkGeometry(const Chunk& ch);
// Replaces the geometry section with g (alignment computed for its final position).
void SetGeometry(Chunk& ch, const Geometry& g);
// String table (texture names): list and append.
std::vector<std::string> ChunkStrings(const Chunk& ch);
std::vector<uint32_t> AddStrings(Chunk& ch, const std::vector<std::string>& names);

}  // namespace mapconv
