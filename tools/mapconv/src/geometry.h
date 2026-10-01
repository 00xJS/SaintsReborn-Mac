// Geometry section of a .bbchunk_xbox2 (loader sub_82117468) as an editable structure.
// WriteGeometry mirrors the loader's alignment rules exactly (byte-identical round trip).
#pragma once
#include "be.h"

namespace mapconv {

struct VertexBuffer {
  Bytes desc;       // 88-byte descriptor: +0 u16 triangle count, +4 has vertices, +8 has indices,
                    // +12 u16 vertex count, +14 u16 index count, +16 u16 stride
  bool has_aux = false;
  Bytes aux_header;  // 16 bytes: +0/+4 list present, +8/+10 u16 counts
  Bytes aux_a, aux_b;
  Bytes ib, vd;      // big-endian u16 strip indices, vertices
};

struct SubMesh {
  Bytes s16;         // 16 bytes: +2 u16 batch count, +8 u32 vertex buffer index
  Bytes b16;         // batches, 8 bytes each: u32 first index, u16 index count, u16 material
  bool has12 = false;
  Bytes s12, b12;
};

struct Mesh {
  bool has48 = false;
  Bytes blk48;
  Bytes hdr;         // 40 bytes: +0 bbox min, +12 bbox max, +28 s16 submesh count, +36 u32 shadow lists flag
  std::vector<SubMesh> subs;
};

struct Material {
  Bytes m;                 // 56 bytes: +0 shader hash, +4 name hash, +8 u16, +10 u16 texture refs, +20 s16, +28/+40 constant blocks
  std::vector<Bytes> parts;  // 7 variable parts in file order
};

struct MaterialEntry {
  Bytes e, x;
};

struct Geometry {
  Bytes coll;              // Havok MOPP collision blob
  Bytes vec24;             // chunk bounds (min, max)
  Bytes vbptr;
  std::vector<uint32_t> auxflag;
  std::vector<VertexBuffer> vbs;
  Bytes mathdr;            // 12 bytes: +0 u16 material count, +2 u16 entry count
  std::vector<Material> mats;
  std::vector<MaterialEntry> ents;
  std::vector<Mesh> meshes;
  size_t start = 0, end = 0;
};

// d = whole chunk file; at = geometry start; counts from the front of the chunk.
Geometry ReadGeometry(const Bytes& d, size_t at, uint32_t vb_count, uint32_t mesh_count, size_t a164_at);
// start = file offset the section will be written at (alignment is absolute).
Bytes WriteGeometry(const Geometry& g, size_t start);

}  // namespace mapconv
