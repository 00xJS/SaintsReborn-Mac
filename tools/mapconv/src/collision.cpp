#include "collision.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace mapconv {

std::vector<Triangle> MeshTriangles(const Geometry& g, size_t k) {
  std::vector<Triangle> out;
  for (const auto& S : g.meshes[k].subs) {
    const VertexBuffer& V = g.vbs[U32(S.s16, 8)];
    size_t st = U16(V.desc, 16);
    for (uint32_t b = 0; b < U16(S.s16, 2); ++b) {
      uint32_t first = U32(S.b16, 8 * b);
      uint32_t cnt = U16(S.b16, 8 * b + 4);
      for (uint32_t p = 0; p + 2 < cnt; ++p) {
        uint16_t t[3] = {U16(V.ib, 2 * (first + p)), U16(V.ib, 2 * (first + p + 1)), U16(V.ib, 2 * (first + p + 2))};
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
        Triangle tr;
        tr.key = (b << 16) | (first + p);
        for (int j = 0; j < 3; ++j)
          for (int a = 0; a < 3; ++a) tr.p[j][a] = F32(V.vd, st * t[j] + 4 * a);
        out.push_back(tr);
      }
    }
  }
  return out;
}

namespace {

Bytes Term(uint32_t k) {
  if (k < 32) return {uint8_t(0x30 + k)};
  if (k < 256) return {0x50, uint8_t(k)};
  if (k < 65536) return {0x51, uint8_t(k >> 8), uint8_t(k)};
  if (k >= (1u << 24)) throw Error("too many collision triangles in one mesh");
  return {0x52, uint8_t(k >> 16), uint8_t(k >> 8), uint8_t(k)};
}

struct Q {
  uint32_t key;
  int mn[3], mx[3];
};

Bytes Node(std::vector<Q>& L, size_t lo, size_t hi) {
  if (hi - lo == 1) return Term(L[lo].key);
  int ext[3];
  for (int a = 0; a < 3; ++a) {
    int mx = 0, mn = 255;
    for (size_t i = lo; i < hi; ++i) {
      mx = std::max(mx, L[i].mx[a]);
      mn = std::min(mn, L[i].mn[a]);
    }
    ext[a] = mx - mn;
  }
  int ax = 0;
  for (int a = 1; a < 3; ++a)
    if (ext[a] > ext[ax]) ax = a;
  std::sort(L.begin() + lo, L.begin() + hi, [&](const Q& x, const Q& y) {
    int sx = x.mn[ax] + x.mx[ax], sy = y.mn[ax] + y.mx[ax];
    return sx != sy ? sx < sy : x.key < y.key;
  });
  size_t h = lo + (hi - lo) / 2;
  int A = 0, B = 255;
  for (size_t i = lo; i < h; ++i) A = std::max(A, L[i].mx[ax]);
  for (size_t i = h; i < hi; ++i) B = std::min(B, L[i].mn[ax]);
  Bytes lc = Node(L, lo, h);
  Bytes rc = Node(L, h, hi);
  Bytes out;
  if (lc.size() <= 255) {
    out = {uint8_t(0x10 + ax), uint8_t(A), uint8_t(B), uint8_t(lc.size())};
  } else {
    if (lc.size() > 65535) throw Error("collision tree too large");
    out = {uint8_t(0x23 + ax), uint8_t(A), uint8_t(B), 0, 0, uint8_t(lc.size() >> 8), uint8_t(lc.size())};
  }
  Append(out, lc);
  Append(out, rc);
  return out;
}

}  // namespace

Bytes BuildMoppBlock(const std::vector<CollisionItem>& items) {
  if (items.empty()) throw Error("empty collision block");
  Vec3 mn, mx;
  for (int a = 0; a < 3; ++a) {
    float lo = 1e30f, hi = -1e30f;
    for (auto& it : items) {
      lo = std::min(lo, it.mn[a]);
      hi = std::max(hi, it.mx[a]);
    }
    mn[a] = lo - 0.05f;
    mx[a] = hi + 0.05f;
  }
  double ext = std::max({double(mx[0]) - mn[0], double(mx[1]) - mn[1], double(mx[2]) - mn[2]});
  float scale = float(double(1 << 24) / ext * 0.999);
  std::vector<Q> L;
  L.reserve(items.size());
  for (auto& it : items) {
    Q q;
    q.key = it.key;
    for (int a = 0; a < 3; ++a) {
      long long lo = (long long)((double(it.mn[a]) - mn[a]) * scale) >> 16;
      long long hi = ((long long)((double(it.mx[a]) - mn[a]) * scale) >> 16) + 1;
      q.mn[a] = int(std::clamp<long long>(lo, 0, 255));
      q.mx[a] = int(std::clamp<long long>(hi, 0, 255));
    }
    L.push_back(q);
  }
  Bytes code = Node(L, 0, L.size());
  Bytes out = {'P', 'P', 'O', 'M'};
  Put32(out, 300);
  PutF(out, mn[0]);
  PutF(out, mn[1]);
  PutF(out, mn[2]);
  PutF(out, scale);
  Put32(out, uint32_t(code.size()));
  Append(out, code);
  return out;
}

std::vector<MoppBlock> ParseMoppBlocks(const Bytes& d) {
  std::vector<MoppBlock> B;
  size_t c = 0;
  while (c < d.size()) {
    if (c + 28 > d.size() || d[c] != 'P' || d[c + 1] != 'P' || d[c + 2] != 'O' || d[c + 3] != 'M')
      throw Error("collision blob not understood");
    uint32_t n = U32(d, c + 24);
    B.push_back({c, Slice(d, c, c + 24), Slice(d, c + 28, c + 28 + n)});
    c = Al(c + 28 + n, 64);
  }
  return B;
}

std::vector<uint32_t> MoppKeys(const Bytes& code) {
  std::vector<uint32_t> out;
  std::function<void(size_t, uint32_t, int)> run = [&](size_t i, uint32_t off, int depth) {
    if (depth > 400) throw Error("collision code too deep");
    for (;;) {
      if (i >= code.size()) throw Error("collision code runs past its end");
      uint8_t op = code[i];
      if (op >= 0x30 && op <= 0x4F) { out.push_back(off + op - 0x30); return; }
      if (op == 0x50) { out.push_back(off + code[i + 1]); return; }
      if (op == 0x51) { out.push_back(off + ((code[i + 1] << 8) | code[i + 2])); return; }
      if (op == 0x52) { out.push_back(off + ((code[i + 1] << 16) | (code[i + 2] << 8) | code[i + 3])); return; }
      if (op == 0x53) { out.push_back(off + Rd32(&code[i + 1])); return; }
      if (op == 0x05) { i += 2 + code[i + 1]; continue; }
      if (op == 0x06) { i += 3 + ((code[i + 1] << 8) | code[i + 2]); continue; }
      if (op == 0x09) { off += code[i + 1]; i += 2; continue; }
      if (op == 0x0A) { off += (code[i + 1] << 8) | code[i + 2]; i += 3; continue; }
      if (op == 0x0B) { off = Rd32(&code[i + 1]); i += 5; continue; }
      if (op >= 0x01 && op <= 0x04) { i += 4; continue; }
      if (op >= 0x20 && op <= 0x22) { i += 3; continue; }
      if (op >= 0x26 && op <= 0x28) { i += 3; continue; }
      if (op >= 0x29 && op <= 0x2B) { i += 7; continue; }
      if (op >= 0x10 && op <= 0x1C) { run(i + 4, off, depth + 1); i = i + 4 + code[i + 3]; continue; }
      if (op >= 0x23 && op <= 0x25) {
        size_t j1 = (code[i + 3] << 8) | code[i + 4], j2 = (code[i + 5] << 8) | code[i + 6];
        run(i + 7 + j1, off, depth + 1);
        i = i + 7 + j2;
        continue;
      }
      throw Error("unexpected collision opcode");
    }
  };
  if (!code.empty()) run(0, 0, 0);
  return out;
}

void InstanceAabb(const Bytes& inst, size_t i, const Vec3& bmin, const Vec3& bmax, Vec3& mn, Vec3& mx) {
  size_t ia = 112 * i;
  float pos[3], R[9];
  for (int k = 0; k < 3; ++k) pos[k] = F32(inst, ia + 4 + 4 * k);
  for (int k = 0; k < 9; ++k) R[k] = F32(inst, ia + 16 + 4 * k);
  for (int j = 0; j < 3; ++j) {
    mn[j] = 1e30f;
    mx[j] = -1e30f;
  }
  for (int c = 0; c < 8; ++c) {
    float v[3] = {(c & 1) ? bmax[0] : bmin[0], (c & 2) ? bmax[1] : bmin[1], (c & 4) ? bmax[2] : bmin[2]};
    for (int j = 0; j < 3; ++j) {
      float w = pos[j];
      for (int k = 0; k < 3; ++k) w += v[k] * R[3 * k + j];
      mn[j] = std::min(mn[j], w);
      mx[j] = std::max(mx[j], w);
    }
  }
}

size_t RebuildCollision(Geometry& g, const Bytes& inst, const std::vector<bool>& has_coll) {
  auto stock = ParseMoppBlocks(g.coll);
  if (stock.size() != g.meshes.size() + 1) throw Error("collision block count does not match the meshes");
  std::vector<Bytes> B;
  for (size_t k = 0; k < g.meshes.size(); ++k) {
    if (!has_coll[k]) {
      Bytes b = stock[k].header24;
      b.resize(28, 0);  // code size 0 = no collision
      B.push_back(b);
      continue;
    }
    std::vector<CollisionItem> items;
    for (const auto& t : MeshTriangles(g, k)) {
      CollisionItem it;
      it.key = t.key;
      for (int a = 0; a < 3; ++a) {
        it.mn[a] = std::min({t.p[0][a], t.p[1][a], t.p[2][a]});
        it.mx[a] = std::max({t.p[0][a], t.p[1][a], t.p[2][a]});
      }
      items.push_back(it);
    }
    B.push_back(BuildMoppBlock(items));
  }
  // meshes sharing a vertex buffer share collision: the first mesh using it decides
  std::vector<int64_t> vbk;
  for (auto& M : g.meshes) vbk.push_back(M.subs.empty() ? -1 : int64_t(U32(M.subs[0].s16, 8)));
  std::vector<CollisionItem> items;
  for (size_t i = 0; i < inst.size() / 112; ++i) {
    uint32_t flags = U32(inst, 112 * i + 108), me = U32(inst, 112 * i + 104);
    size_t canon = size_t(std::find(vbk.begin(), vbk.end(), vbk[me]) - vbk.begin());
    if ((flags & 0x4C7) || !has_coll[canon]) continue;
    const Bytes& h = g.meshes[me].hdr;
    Vec3 bmin = {F32(h, 0), F32(h, 4), F32(h, 8)}, bmax = {F32(h, 12), F32(h, 16), F32(h, 20)};
    CollisionItem it;
    it.key = uint32_t(items.size());
    InstanceAabb(inst, i, bmin, bmax, it.mn, it.mx);
    items.push_back(it);
  }
  if (items.empty()) throw Error("nothing to collide with");
  B.push_back(BuildMoppBlock(items));
  Bytes blob;
  for (size_t i = 0; i < B.size(); ++i) {
    Append(blob, B[i]);
    if (i + 1 < B.size()) PadTo(blob, 64);
  }
  g.coll = std::move(blob);
  return items.size();
}

}  // namespace mapconv
