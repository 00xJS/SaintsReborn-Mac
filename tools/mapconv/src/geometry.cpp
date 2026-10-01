#include "geometry.h"

#include <algorithm>

namespace mapconv {

Geometry ReadGeometry(const Bytes& d, size_t at, uint32_t nv, uint32_t nmesh, size_t a164_at) {
  Geometry g;
  size_t c = at;
  uint32_t n = U32(d, c);
  g.coll = Slice(d, c + 4, c + 4 + n);
  c += 4 + n;
  g.vec24 = Slice(d, c, c + 24);
  c += 24;
  c = Al(c, 4);
  g.vbptr = Slice(d, c, c + 4 * nv);
  c += 4 * nv;
  for (uint32_t i = 0; i < nv; ++i) g.auxflag.push_back(U32(d, c + 4 * i));
  c += 4 * nv;
  c = Al(c, 64);
  g.vbs.resize(nv);
  for (uint32_t i = 0; i < nv; ++i) {
    VertexBuffer& V = g.vbs[i];
    c = Al(c, 16);
    V.desc = Slice(d, c, c + 88);
    c += 88;
    if (g.auxflag[i]) {
      V.has_aux = true;
      c = Al(c, 16);
      V.aux_header = Slice(d, c, c + 16);
      c += 16;
      if (U32(V.aux_header, 0)) {
        c = Al(c, 32);
        size_t k = size_t(U16(V.aux_header, 8)) * 2;
        V.aux_a = Slice(d, c, c + k);
        c += k;
      }
      if (U32(V.aux_header, 4)) {
        c = Al(c, 32);
        size_t k = size_t(U16(V.aux_header, 10)) * 2;
        V.aux_b = Slice(d, c, c + k);
        c += k;
      }
    }
  }
  c = Al(c, 4096);
  for (auto& V : g.vbs) {
    if (U32(V.desc, 8)) {
      c = Al(c, 32);
      size_t k = size_t(U16(V.desc, 14)) * 2;
      V.ib = Slice(d, c, c + k);
      c += k;
    }
    if (U32(V.desc, 4)) {
      c = Al(c, 32);
      size_t k = size_t(U16(V.desc, 16)) * U16(V.desc, 12);
      V.vd = Slice(d, c, c + k);
      c += k;
    }
  }
  c = Al(c, 4096);
  c = Al(c, 16);
  g.mathdr = Slice(d, c, c + 12);
  c += 12;
  uint32_t nm = U16(g.mathdr, 0), n2 = U16(g.mathdr, 2);
  if (nm) {
    c = Al(c, 16);
    size_t mb = c;
    c += size_t(nm) * 56;
    c = Al(c, 16);
    for (uint32_t i = 0; i < nm; ++i) {
      Material M;
      M.m = Slice(d, mb + 56 * i, mb + 56 * i + 56);
      auto take = [&](size_t align, size_t k) {
        c = Al(c, align);
        M.parts.push_back(Slice(d, c, c + k));
        c += k;
      };
      take(4, size_t(U16(M.m, 10)) * 12);
      take(4, size_t(std::max<int>(S16(M.m, 20), 0)) * 8);
      for (size_t off : {28, 40}) {
        take(16, size_t(U16(M.m, off)) * 24);
        take(16, size_t(U16(M.m, off + 2)) * 2);
      }
      take(4, size_t(U16(M.m, 8)) * 6);
      g.mats.push_back(std::move(M));
    }
  }
  if (n2) {
    c = Al(c, 4);
    size_t eb = c;
    c += size_t(n2) * 16;
    for (uint32_t i = 0; i < n2; ++i) {
      MaterialEntry E;
      E.e = Slice(d, eb + 16 * i, eb + 16 * i + 16);
      c = Al(c, 4);
      size_t k = size_t(U16(E.e, 8)) * 4;
      E.x = Slice(d, c, c + k);
      c += k;
      g.ents.push_back(std::move(E));
    }
  }
  for (uint32_t i = 0; i < nmesh; ++i) {
    Mesh M;
    size_t e = a164_at + 16 * i;
    if (U32(d, e + 4)) {
      size_t m = c;
      size_t k = 48 + size_t(U32(d, m + 36)) * 12 + size_t(U32(d, m + 28)) * 4 + size_t(U32(d, m + 44)) * 2;
      M.has48 = true;
      M.blk48 = Slice(d, c, c + k);
      c += k;
    }
    c = Al(c, 4);
    M.hdr = Slice(d, c, c + 40);
    c += 40;
    c = Al(c, 16);
    c = Al(c, 16);
    int k = S16(M.hdr, 28);
    if (k < 0) throw Error("bad submesh count");
    size_t s32 = c;
    c += 16 * size_t(k);
    bool has36 = U32(M.hdr, 36) != 0;
    size_t s36 = 0;
    if (has36) {
      c = Al(c, 16);
      s36 = c;
      c += 12 * size_t(k);
    }
    for (int j = 0; j < k; ++j) {
      SubMesh S;
      S.s16 = Slice(d, s32 + 16 * j, s32 + 16 * j + 16);
      if (U16(S.s16, 2)) {
        c = Al(c, 16);
        size_t n8 = size_t(U16(S.s16, 2)) * 8;
        S.b16 = Slice(d, c, c + n8);
        c += n8;
      }
      if (has36) {
        S.has12 = true;
        S.s12 = Slice(d, s36 + 12 * j, s36 + 12 * j + 12);
        if (U16(S.s12, 2)) {
          c = Al(c, 16);
          size_t n8 = size_t(U16(S.s12, 2)) * 8;
          S.b12 = Slice(d, c, c + n8);
          c += n8;
        }
      }
      M.subs.push_back(std::move(S));
    }
    g.meshes.push_back(std::move(M));
  }
  g.start = at;
  g.end = c;
  return g;
}

Bytes WriteGeometry(const Geometry& g, size_t start) {
  Bytes out;
  auto A = [&](size_t n) { PadTo(out, n, start); };
  Put32(out, uint32_t(g.coll.size()));
  Append(out, g.coll);
  Append(out, g.vec24);
  A(4);
  Append(out, g.vbptr);
  for (uint32_t x : g.auxflag) Put32(out, x);
  A(64);
  for (size_t i = 0; i < g.vbs.size(); ++i) {
    const VertexBuffer& V = g.vbs[i];
    A(16);
    Append(out, V.desc);
    if (g.auxflag[i]) {
      A(16);
      Append(out, V.aux_header);
      if (U32(V.aux_header, 0)) {
        A(32);
        Append(out, V.aux_a);
      }
      if (U32(V.aux_header, 4)) {
        A(32);
        Append(out, V.aux_b);
      }
    }
  }
  A(4096);
  for (const auto& V : g.vbs) {
    if (U32(V.desc, 8)) {
      A(32);
      Append(out, V.ib);
    }
    if (U32(V.desc, 4)) {
      A(32);
      Append(out, V.vd);
    }
  }
  A(4096);
  A(16);
  Append(out, g.mathdr);
  if (!g.mats.empty()) {
    A(16);
    for (const auto& M : g.mats) Append(out, M.m);
    A(16);
    for (const auto& M : g.mats) {
      const auto& P = M.parts;
      A(4);
      Append(out, P[0]);
      A(4);
      Append(out, P[1]);
      A(16);
      Append(out, P[2]);
      A(16);
      Append(out, P[3]);
      A(16);
      Append(out, P[4]);
      A(16);
      Append(out, P[5]);
      A(4);
      Append(out, P[6]);
    }
  }
  if (!g.ents.empty()) {
    A(4);
    for (const auto& E : g.ents) Append(out, E.e);
    for (const auto& E : g.ents) {
      A(4);
      Append(out, E.x);
    }
  }
  for (const auto& M : g.meshes) {
    if (M.has48) Append(out, M.blk48);
    A(4);
    Append(out, M.hdr);
    A(16);
    A(16);
    for (const auto& S : M.subs) Append(out, S.s16);
    if (U32(M.hdr, 36)) {
      A(16);
      for (const auto& S : M.subs) Append(out, S.s12);
    }
    for (const auto& S : M.subs) {
      if (U16(S.s16, 2)) {
        A(16);
        Append(out, S.b16);
      }
      if (U32(M.hdr, 36) && U16(S.s12, 2)) {
        A(16);
        Append(out, S.b12);
      }
    }
  }
  return out;
}

}  // namespace mapconv
