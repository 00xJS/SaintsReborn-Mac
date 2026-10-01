// Havok MOPP collision for chunk meshes ("PPOM" blocks: one per mesh, then one for the chunk's instances).
#pragma once
#include <array>
#include "be.h"
#include "geometry.h"

namespace mapconv {

using Vec3 = std::array<float, 3>;

struct CollisionItem {
  uint32_t key;
  Vec3 mn, mx;
};

struct Triangle {
  uint32_t key;  // (batch << 16) | strip position
  Vec3 p[3];     // strip order (odd strip positions have the opposite winding)
};

// Every non-degenerate triangle of a mesh's draw batches.
std::vector<Triangle> MeshTriangles(const Geometry& g, size_t mesh);
Bytes BuildMoppBlock(const std::vector<CollisionItem>& items);
// Header of each block (offset in the blob, code size).
struct MoppBlock {
  size_t at;
  Bytes header24;
  Bytes code;
};
std::vector<MoppBlock> ParseMoppBlocks(const Bytes& coll);
// All keys the code can report (full traversal), for self-checks.
std::vector<uint32_t> MoppKeys(const Bytes& code);
// World box of instance i (112-byte records: +4 position, +16 3x3 rotation, row vector times matrix).
void InstanceAabb(const Bytes& inst, size_t i, const Vec3& bmin, const Vec3& bmax, Vec3& mn, Vec3& mx);
// Rebuilds the collision blob: has_coll[k] = mesh k gets a tree of its triangles; the last block is the
// chunk tree over instances whose mesh has collision. Returns the number of instances in the chunk tree.
size_t RebuildCollision(Geometry& g, const Bytes& inst, const std::vector<bool>& has_coll);

}  // namespace mapconv
