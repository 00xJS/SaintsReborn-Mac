#include "build.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include "chunk.h"
#include "collision.h"
#include "vpp.h"

namespace mapconv {
namespace fs = std::filesystem;
namespace {

constexpr const char* kTemplate = "mp_crib";
constexpr uint32_t kBaseShader = 0x100742ad;  // base colour + normal + specular, no constants

uint32_t Crc32(const std::string& s) {
  uint32_t c = 0xFFFFFFFF;
  for (unsigned char ch : s) {
    c ^= ch;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

std::u16string Utf16(const std::string& s) {
  std::u16string out;
  for (size_t i = 0; i < s.size();) {
    uint32_t c = uint8_t(s[i]);
    int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (n > 1) c &= (0xFF >> (n + 1));
    for (int k = 1; k < n && i + k < s.size(); ++k) c = (c << 6) | (uint8_t(s[i + k]) & 0x3F);
    i += n;
    if (c >= 0x10000) {
      c -= 0x10000;
      out.push_back(char16_t(0xD800 + (c >> 10)));
      out.push_back(char16_t(0xDC00 + (c & 0x3FF)));
    } else {
      out.push_back(char16_t(c));
    }
  }
  return out;
}

void WriteFile(const fs::path& p, const Bytes& b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) throw Error("cannot write " + p.u8string());
  f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
  if (!f) throw Error("cannot write " + p.u8string());
}
void WriteText(const fs::path& p, const std::string& text) {
  std::string crlf;
  for (char c : text) {
    if (c == '\n') crlf += '\r';
    crlf += c;
  }
  WriteFile(p, Bytes(crlf.begin(), crlf.end()));
}

uint32_t PackNormal(V3 v) {
  uint32_t out = 0;
  for (int i = 0; i < 3; ++i) {
    int q = int(std::lround(std::clamp(v[i], -1.0f, 1.0f) * 511)) & 1023;
    out |= uint32_t(q) << (10 * i);
  }
  return out;
}
V3 Sub(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 Cross(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float Dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 Norm(V3 v, V3 fallback = {0, 1, 0}) {
  float l = std::sqrt(Dot(v, v));
  if (l < 1e-12f) return fallback;
  return {v[0] / l, v[1] / l, v[2] / l};
}

// One triangle strip per draw batch. New triangles either continue the strip (sharing the last edge
// with the right winding for their position) or are joined with degenerate triangles so that each
// real triangle lands on an even position (odd positions have the opposite winding).
struct Strip {
  std::vector<uint16_t> s;
  void Add(uint16_t a, uint16_t b, uint16_t c) {
    if (s.empty()) {
      s = {a, b, c};
      return;
    }
    size_t n = s.size();
    uint16_t x = s[n - 2], y = s[n - 1];
    bool even = ((n - 2) % 2) == 0;
    const uint16_t rots[3][3] = {{a, b, c}, {b, c, a}, {c, a, b}};
    for (auto& r : rots) {
      if ((even && r[0] == x && r[1] == y) || (!even && r[0] == y && r[1] == x)) {
        s.push_back(r[2]);
        return;
      }
    }
    uint16_t last = s.back();
    s.push_back(last);
    if (s.size() % 2 == 0) s.push_back(last);
    s.insert(s.end(), {a, a, b, c});
  }
};

struct VBOut {
  Bytes vd;
  std::vector<uint16_t> ib;
  std::unordered_map<std::string, uint16_t> lookup;
  struct Batch { uint32_t first, count; uint16_t mat; };
  std::vector<Batch> batches;
  Strip strip;
  int strip_mat = -1;
  size_t tris = 0;
  V3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
  size_t Verts() const { return vd.size() / 32; }
  void Flush() {
    if (strip.s.empty()) return;
    batches.push_back({uint32_t(ib.size()), uint32_t(strip.s.size()), uint16_t(strip_mat)});
    ib.insert(ib.end(), strip.s.begin(), strip.s.end());
    strip.s.clear();
  }
  uint16_t Vertex(const Bytes& v, V3 p) {
    std::string key(v.begin(), v.end());
    auto it = lookup.find(key);
    if (it != lookup.end()) return it->second;
    uint16_t idx = uint16_t(Verts());
    Append(vd, v);
    lookup.emplace(std::move(key), idx);
    for (int a = 0; a < 3; ++a) {
      mn[a] = std::min(mn[a], p[a]);
      mx[a] = std::max(mx[a], p[a]);
    }
    return idx;
  }
};

int16_t UvS16(float x, bool& clipped) {
  double v = std::round(double(x) * 1024.0);
  if (v > 32767 || v < -32768) clipped = true;
  return int16_t(std::clamp(v, -32768.0, 32767.0));
}

std::string Fmt(float v) {
  char b[32];
  std::snprintf(b, sizeof b, "%f", v);
  return b;
}

const std::set<std::string> kWeapons = {"ak47", "desert eagle", "m16", "mac10", "molotov", "pipe_bomb",
                                        "pump_action_shotgun", "rpg_launcher", "sniper_rifle", "spas12", "tec9",
                                        "baseball_bat", "beretta", "grenade", "shotgun", "uzi", "k6"};

}  // namespace

BuildReport BuildPack(const MapInput& in, const MapSettings& st, const fs::path& packfiles, const fs::path& maps_dir,
                      const Log& log) {
  BuildReport rep;
  rep.warnings = in.warnings;
  const std::string id = st.map_id;
  if (in.tris.empty()) throw Error("the file has no map geometry (only markers or nothing at all)");

  // ---- template files from the player's own game ----
  log("Reading the template map from your game files...");
  Packfile stream(packfiles / "mp_city_stream.vpp_xbox2");
  Packfile misc(packfiles / "misc.vpp_xbox2");
  Packfile pegs(packfiles / "pegfiles.vpp_xbox2");
  const std::string T = kTemplate;
  Bytes chunk_b = stream.Read(T + ".bbchunk_xbox2"), peg_b = stream.Read(T + ".peg_xbox2");
  Bytes city_b = stream.Read(T + ".bbcity_xbox2"), pvsv_b = stream.Read(T + ".pvsv_xbox2");
  Bytes hmap_b = misc.Read(T + ".hmap_xbox2"), amb_b = misc.Read("amb_" + T + ".cts");
  Bytes cts_b = misc.Read(T + ".cts"), ld_b = pegs.Read("ld-" + T + ".peg_xbox2");

  Chunk ch = ReadChunk(chunk_b);
  Geometry g = ChunkGeometry(ch);

  // ---- textures and materials ----
  std::vector<int> used(in.materials.size(), 0);
  for (auto& t : in.tris) used[size_t(t.material)] = 1;
  std::vector<std::pair<std::string, Image>> textures;
  std::map<std::vector<uint8_t>, size_t> image_index;
  std::vector<int> mat_to_tex(in.materials.size(), -1);
  int S = st.texture_size;
  for (size_t i = 0; i < in.materials.size(); ++i) {
    if (!used[i]) continue;
    Image img = Resize(in.materials[i].image, S, S);
    auto it = image_index.find(img.rgb);
    if (it != image_index.end()) {
      mat_to_tex[i] = int(it->second);
      continue;
    }
    char name[64];
    std::snprintf(name, sizeof name, "%s_t%02zu.tga", id.c_str(), textures.size());
    image_index[img.rgb] = textures.size();
    mat_to_tex[i] = int(textures.size());
    textures.push_back({name, std::move(img)});
  }
  std::vector<std::string> tex_names;
  for (auto& t : textures) tex_names.push_back(t.first);
  auto str_idx = AddStrings(ch, tex_names);
  int base_mat = -1;
  for (size_t i = 0; i < g.mats.size(); ++i)
    if (U32(g.mats[i].m, 0) == kBaseShader) { base_mat = int(i); break; }
  if (base_mat < 0) throw Error("template map has no plain material to copy");
  std::vector<int> mat_to_game(in.materials.size(), -1);
  for (size_t i = 0; i < in.materials.size(); ++i) {
    if (!used[i]) continue;
    Material m = g.mats[size_t(base_mat)];
    Wr32(&m.m[4], Crc32(id + "/" + std::to_string(i)));
    if (m.parts[0].size() < 12) throw Error("template material has no texture slot");
    Wr32(&m.parts[0][4], str_idx[size_t(mat_to_tex[i])]);  // slot 0 = base colour
    mat_to_game[i] = int(g.mats.size());
    g.mats.push_back(std::move(m));
  }
  Wr16(&g.mathdr[0], uint16_t(g.mats.size()));
  rep.textures = textures.size();

  // ---- vertices, strips, vertex buffers ----
  log("Building geometry...");
  std::vector<size_t> order(in.tris.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return in.tris[a].material < in.tris[b].material; });
  std::vector<VBOut> vbs(1);
  bool clipped = false;
  size_t degenerate = 0;
  for (size_t oi : order) {
    const InTriangle& t = in.tris[oi];
    V3 fn = Cross(Sub(t.p[1], t.p[0]), Sub(t.p[2], t.p[0]));
    if (Dot(fn, fn) < 1e-14f) { ++degenerate; continue; }
    fn = Norm(fn);
    int gm = mat_to_game[size_t(t.material)];
    VBOut* V = &vbs.back();
    if (V->Verts() + 3 > 65535 || V->ib.size() + V->strip.s.size() + 8 > 65535) {
      V->Flush();
      vbs.emplace_back();
      V = &vbs.back();
    }
    if (V->strip_mat != gm) {
      V->Flush();
      V->strip_mat = gm;
    }
    // shift the triangle's UVs by whole repeats so they fit the 16-bit texture coordinates
    float su = std::floor((t.uv[0][0] + t.uv[1][0] + t.uv[2][0]) / 3), sv = std::floor((t.uv[0][1] + t.uv[1][1] + t.uv[2][1]) / 3);
    // tangent from the UV directions
    V3 e1 = Sub(t.p[1], t.p[0]), e2 = Sub(t.p[2], t.p[0]);
    float du1 = t.uv[1][0] - t.uv[0][0], dv1 = t.uv[1][1] - t.uv[0][1], du2 = t.uv[2][0] - t.uv[0][0], dv2 = t.uv[2][1] - t.uv[0][1];
    float r = du1 * dv2 - du2 * dv1;
    V3 tan = std::abs(r) > 1e-12f ? V3{(e1[0] * dv2 - e2[0] * dv1) / r, (e1[1] * dv2 - e2[1] * dv1) / r, (e1[2] * dv2 - e2[2] * dv1) / r}
                                  : e1;
    uint16_t idx[3];
    for (int j = 0; j < 3; ++j) {
      V3 n = Dot(t.n[j], t.n[j]) > 0.25f ? Norm(t.n[j]) : fn;
      V3 tj = Sub(tan, V3{n[0] * Dot(n, tan), n[1] * Dot(n, tan), n[2] * Dot(n, tan)});
      tj = Norm(tj, Norm(Cross(n, std::abs(n[1]) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0})));
      Bytes v;
      for (int a = 0; a < 3; ++a) PutF(v, t.p[j][a]);
      Put32(v, PackNormal(n));
      Put32(v, PackNormal(tj));
      Put16(v, uint16_t(UvS16(t.uv[j][0] - su, clipped)));
      Put16(v, uint16_t(UvS16(t.uv[j][1] - sv, clipped)));
      Put32(v, 0);
      Put32(v, 0);
      idx[j] = V->Vertex(v, t.p[j]);
    }
    V->strip.Add(idx[0], idx[1], idx[2]);
    V->tris++;
    rep.triangles++;
  }
  vbs.back().Flush();
  if (vbs.back().tris == 0) vbs.pop_back();
  if (clipped) rep.warnings.push_back("some faces repeat their texture more than 32 times; their texture coordinates were clipped (split big faces or use a larger texture scale)");
  if (degenerate) rep.warnings.push_back(std::to_string(degenerate) + " zero-area triangles were skipped");
  for (auto& V : vbs) rep.vertices += V.Verts();

  // ---- mesh slots of the template ----
  Bytes& inst = ch.Get("instances").data;
  Bytes& recs = ch.Get("static_records").data;
  uint32_t nrec = ch.H(868), ninst = uint32_t(inst.size() / 112);
  std::vector<int> inst_use(ninst, 0);
  for (uint32_t r = 0; r < nrec; ++r) {
    int32_t ii = I32(recs, 80 * r + 60);
    if (ii >= 0 && uint32_t(ii) < ninst) inst_use[size_t(ii)]++;
  }
  std::vector<uint32_t> free_recs;
  for (uint32_t r = 0; r < nrec; ++r) {
    int32_t ii = I32(recs, 80 * r + 60);
    if (ii >= 0 && uint32_t(ii) < ninst && inst_use[size_t(ii)] == 1 && (U32(recs, 80 * r + 32) & 0x10000000)) free_recs.push_back(r);
  }
  std::vector<size_t> free_meshes;
  for (size_t k = 0; k < g.meshes.size(); ++k)
    if (!g.meshes[k].has48 && !g.meshes[k].subs.empty()) free_meshes.push_back(k);
  int desc_vb = -1;
  for (size_t v = 0; v < g.vbs.size(); ++v)
    if (U16(g.vbs[v].desc, 16) == 32 && U32(g.vbs[v].desc, 4) && U32(g.vbs[v].desc, 8)) { desc_vb = int(v); break; }
  if (desc_vb < 0) throw Error("template map has no usable vertex format");
  size_t capacity = std::min({free_recs.size(), free_meshes.size() - 1, g.vbs.size() - 1});
  if (vbs.size() > capacity) {
    char msg[200];
    std::snprintf(msg, sizeof msg, "the map is too big: it needs %zu vertex buffers (about 20,000 triangles each), the template has room for %zu. Simplify the map or split it.", vbs.size(), capacity);
    throw Error(msg);
  }
  Bytes desc_tmpl = g.vbs[size_t(desc_vb)].desc;
  Bytes s16_tmpl = g.meshes[free_meshes[0]].subs[0].s16;
  size_t empty_mesh = free_meshes.back();
  std::set<size_t> our_vbs;
  for (size_t j = 0; j < vbs.size(); ++j) our_vbs.insert(j);
  size_t trash_vb = vbs.size();  // first vertex buffer we do not use
  std::vector<size_t> our_meshes;
  std::vector<bool> has_coll(g.meshes.size(), false);
  V3 map_mn{1e30f, 1e30f, 1e30f}, map_mx{-1e30f, -1e30f, -1e30f};
  for (size_t j = 0; j < vbs.size(); ++j) {
    VBOut& O = vbs[j];
    VertexBuffer& V = g.vbs[j];
    V.desc = desc_tmpl;
    Wr16(&V.desc[0], uint16_t(std::min<size_t>(O.tris, 65535)));
    Wr16(&V.desc[12], uint16_t(O.Verts()));
    Wr16(&V.desc[14], uint16_t(O.ib.size()));
    Wr16(&V.desc[16], 32);
    V.has_aux = false;
    V.aux_header.clear(); V.aux_a.clear(); V.aux_b.clear();
    g.auxflag[j] = 0;
    V.vd = O.vd;
    V.ib.clear();
    for (uint16_t x : O.ib) Put16(V.ib, x);
    size_t k = free_meshes[j];
    Mesh& M = g.meshes[k];
    for (int a = 0; a < 3; ++a) {
      WrF(&M.hdr[4 * a], O.mn[a]);
      WrF(&M.hdr[12 + 4 * a], O.mx[a]);
      map_mn[a] = std::min(map_mn[a], O.mn[a]);
      map_mx[a] = std::max(map_mx[a], O.mx[a]);
    }
    Wr16(&M.hdr[28], 1);
    Wr32(&M.hdr[36], 0);
    SubMesh sm;
    sm.s16 = s16_tmpl;
    Wr16(&sm.s16[2], uint16_t(O.batches.size()));
    Wr32(&sm.s16[8], uint32_t(j));
    for (auto& b : O.batches) {
      Put32(sm.b16, b.first);
      Put16(sm.b16, uint16_t(b.count));
      Put16(sm.b16, b.mat);
    }
    M.subs = {sm};
    our_meshes.push_back(k);
    has_coll[k] = true;
  }
  std::set<size_t> ours(our_meshes.begin(), our_meshes.end());
  for (size_t k = 0; k < g.meshes.size(); ++k) {
    if (ours.count(k)) continue;
    for (auto& S2 : g.meshes[k].subs) {
      Wr16(&S2.s16[2], 0);
      S2.b16.clear();
      Wr32(&S2.s16[8], uint32_t(trash_vb));
      if (S2.has12) {
        Wr16(&S2.s12[2], 0);
        S2.b12.clear();
      }
    }
  }
  std::set<uint32_t> our_inst;
  for (size_t j = 0; j < vbs.size(); ++j) {
    uint32_t r = free_recs[j];
    uint32_t ii = uint32_t(I32(recs, 80 * r + 60));
    our_inst.insert(ii);
    size_t ia = 112 * size_t(ii);
    Wr32(&inst[ia + 104], uint32_t(our_meshes[j]));
    for (int a = 0; a < 3; ++a) WrF(&inst[ia + 4 + 4 * a], 0.0f);
    const float I9[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int a = 0; a < 9; ++a) WrF(&inst[ia + 16 + 4 * a], I9[a]);
    for (int a = 0; a < 3; ++a) {
      WrF(&recs[80 * r + 4 * a], vbs[j].mn[a]);
      WrF(&recs[80 * r + 16 + 4 * a], vbs[j].mx[a]);
    }
  }
  for (uint32_t i = 0; i < ninst; ++i)
    if (!our_inst.count(i)) Wr32(&inst[112 * size_t(i) + 104], uint32_t(empty_mesh));
  rep.meshes = vbs.size();

  // template lights off (their colour to black) unless the map is an interior on purpose
  {
    Bytes& L = ch.Get("rec136_260").data;
    uint32_t nl = ch.H(884);
    for (uint32_t i = 0; i < nl; ++i)
      for (int a = 0; a < 3; ++a) WrF(&L[136 * i + 8 + 4 * a], 0.0f);
  }

  // bounds (plus air above for jumping / flying) and the chunk's own name
  V3 bmn{map_mn[0] - 1, map_mn[1] - 2, map_mn[2] - 1}, bmx{map_mx[0] + 1, map_mx[1] + 60, map_mx[2] + 1};
  g.vec24.clear();
  for (int a = 0; a < 3; ++a) PutF(g.vec24, bmn[a]);
  for (int a = 0; a < 3; ++a) PutF(g.vec24, bmx[a]);
  for (int a = 0; a < 3; ++a) {
    WrF(&ch.header[940 + 4 * a], bmn[a]);
    WrF(&ch.header[952 + 4 * a], bmx[a]);
  }
  WrF(&ch.header[964], bmn[1]);
  {
    std::string hn = id + ".tga";
    std::fill(ch.header.begin() + 0x10, ch.header.begin() + 0x50, 0);
    std::copy(hn.begin(), hn.begin() + std::min<size_t>(hn.size(), 0x3F), ch.header.begin() + 0x10);
  }

  // Spatial trees over the static records (tree236) and lights (tree192): 44-byte nodes, +0 min,
  // +12 count, +16 max. Their boxes still describe the template's objects; stretched to the whole map
  // so nothing is culled by them (every record is still tested against its own box).
  for (const char* tree : {"tree236", "tree192"}) {
    Bytes& t = ch.Get(tree).data;
    uint32_t nn = U32(t, 0);
    size_t at = Al(4, 64);
    for (uint32_t i = 0; i < nn; ++i) {
      size_t e = at + 44 * size_t(i);
      for (int a = 0; a < 3; ++a) {
        WrF(&t[e + 4 * a], bmn[a]);
        WrF(&t[e + 16 + 4 * a], bmx[a]);
      }
    }
  }

  // collision from the render triangles
  log("Building collision...");
  RebuildCollision(g, inst, has_coll);
  {
    auto blocks = ParseMoppBlocks(g.coll);
    for (size_t k : our_meshes) {
      auto keys = MoppKeys(blocks[k].code);
      if (keys.size() != MeshTriangles(g, k).size()) throw Error("internal error: collision does not cover every triangle");
    }
  }
  SetGeometry(ch, g);
  Bytes out_chunk = WriteChunk(ch);
  {
    Chunk check = ReadChunk(out_chunk);  // throws if the written file does not parse back
    Geometry g2 = ChunkGeometry(check);
    if (g2.meshes.size() != g.meshes.size()) throw Error("internal error: written map does not read back");
  }

  // ---- textures ----
  log("Compressing " + std::to_string(textures.size()) + " texture(s)...");
  std::string tmpl_tex = "bedroom_wo.tga";
  Bytes out_peg = AppendTextures(peg_b, textures, tmpl_tex);

  // ---- city file: chunk name, play area, player start ----
  Bytes city = city_b;
  if (city.size() != 960) throw Error("unexpected template city file");
  WrF(&city[0x78], bmn[0]);
  WrF(&city[0x7C], bmn[2]);
  WrF(&city[0x80], bmx[0]);
  WrF(&city[0x84], bmx[2]);
  std::fill(city.begin() + 0xB8, city.begin() + 0x1C0, 0);
  if (id.size() + 1 + 12 > 0x1C0 - 0xB8) throw Error("map id too long");
  Wr32(&city[0xB8], uint32_t(id.size() + 1));
  std::copy(id.begin(), id.end(), city.begin() + 0xBC);
  Wr32(&city[Al(0xBC + id.size() + 1, 4)], 1);
  const Marker* start = nullptr;
  for (auto& m : in.markers) if (m.kind == MarkerKind::PlayerStart) { start = &m; break; }
  if (!start) for (auto& m : in.markers) if (m.kind == MarkerKind::Spawn) { start = &m; break; }
  V3 sp = start ? start->pos : V3{(map_mn[0] + map_mx[0]) / 2, map_mx[1] + 1, (map_mn[2] + map_mx[2]) / 2};
  float syaw = start ? start->yaw : 0;
  for (int a = 0; a < 3; ++a) WrF(&city[0x1E0 + 4 * a], sp[a]);
  const float R[9] = {std::cos(syaw), 0, -std::sin(syaw), 0, 1, 0, std::sin(syaw), 0, std::cos(syaw)};
  for (int a = 0; a < 9; ++a) WrF(&city[0x1EC + 4 * a], R[a]);

  // ---- gameplay layout (.cts) ----
  std::ostringstream nav, items, vehicles, respawns, triggers;
  size_t ns = 0, nw = 0, nv = 0, nd = 0;
  auto navpt = [&](const std::string& n, V3 p, float yaw) {
    nav << "$Navpoint:\t\"" << n << "\"\n$Type:\t\t\"ground\"\n$Pos:\t\t\t<" << Fmt(p[0]) << " " << Fmt(p[1]) << " " << Fmt(p[2])
        << ">\n$Orient:\t\t[" << Fmt(yaw) << "]\n+Chunk:\t\t\"" << id << "\"\n\n";
  };
  bool teams = false;
  for (auto& m : in.markers) {
    char n[96];
    switch (m.kind) {
      case MarkerKind::Spawn: {
        std::snprintf(n, sizeof n, "%s__$respawn%03zu", id.c_str(), ns);
        navpt(n, m.pos, m.yaw);
        respawns << "$Respawn:\t\t\"" << id << "__respawn" << (ns < 10 ? "00" : ns < 100 ? "0" : "") << ns << "\"\n$Start nav:\t\t\"" << n
                 << "\"\n$Box size:\t\t-2.000000 0.000000 -2.000000 2.000000 2.000000 2.000000\n";
        if (m.team) { respawns << "+MPTeam" << m.team << "\n"; teams = true; }
        respawns << "\n";
        ++ns;
        break;
      }
      case MarkerKind::Weapon: {
        std::snprintf(n, sizeof n, "%s_$i_%03zu", id.c_str(), nw);
        navpt(n, m.pos, m.yaw);
        std::string type = m.type;
        std::replace(type.begin(), type.end(), '-', ' ');
        if (!kWeapons.count(type)) rep.warnings.push_back("weapon type '" + type + "' (" + m.source + ") is not one the stock maps use; it may not spawn");
        items << "$Item:\t\t\"" << n << "\"\n$Item type:\t\"" << type << "\"\n$Start nav:\t\"" << n << "\"\n\n";
        ++nw;
        break;
      }
      case MarkerKind::Vehicle: {
        std::snprintf(n, sizeof n, "%s_$v%03zu", id.c_str(), nv);
        navpt(n, m.pos, m.yaw);
        vehicles << "$Vehicle:\t\t\"" << n << "\"\n$Vehicle type:\t\"" << m.type << "\"\n$Start nav:\t\t\"" << n
                 << "\"\n$Stream Distance:\t\t\t50.000\n\n";
        ++nv;
        break;
      }
      case MarkerKind::ChainsDropOff: {
        std::snprintf(n, sizeof n, "%s__$big ass chains drop off%03zu", id.c_str(), nd);
        navpt(n, m.pos, m.yaw);
        triggers << "$Trigger:\t\t\t\t\"" << n << "\"\n$Trigger type:\t\t\t\"sphere\"\n$Trigger action:\t\t\"chains drop off\"\n"
                 << "$Trigger max fires:\t0\n$Trigger delay:\t\t10000\n$Start nav:\t\t\t\t\"" << n << "\"\n+Disabled\n$Sphere radius:\t\t0.700000\n\n";
        ++nd;
        break;
      }
      case MarkerKind::PlayerStart:
        break;
    }
  }
  if (ns == 0) throw Error("the map has no spawn points: add empties / objects named spawn_1, spawn_2, ... (at least 2, better 8+)");
  if (ns < 4) rep.warnings.push_back("only " + std::to_string(ns) + " spawn point(s); 8 or more play much better");
  rep.spawns = ns; rep.weapons = nw; rep.vehicles = nv; rep.dropoffs = nd;
  std::string cts(cts_b.begin(), cts_b.end());
  cts.erase(std::remove(cts.begin(), cts.end(), '\r'), cts.end());
  for (size_t p; (p = cts.find(T)) != std::string::npos;) cts.replace(p, T.size(), id);
  auto section = [&](const std::string& head, const std::string& next, const std::string& body) {
    std::string h = head + "\n// -------\n";
    size_t a = cts.find(h);
    size_t b = cts.find("// -------\n" + next, a == std::string::npos ? 0 : a);
    if (a == std::string::npos || b == std::string::npos) throw Error("unexpected template layout file (" + head + ")");
    a += h.size();
    cts.replace(a, b - a, "\n" + body);
  };
  section("#Navpoints", "#Cameras", nav.str());
  section("#Items", "#Triggers", items.str());
  section("#Triggers", "#Vehicles", triggers.str());
  section("#Vehicles", "#Respawns", vehicles.str());
  section("#Respawns", "#Humans", respawns.str());

  // ---- level list, name, bundle manifest ----
  std::vector<std::pair<std::string, std::string>> modes = {{"Gangsta Brawl", "GB"}, {"Team Gangsta Brawl", "TGB"}};
  if (nd) { modes.push_back({"Big Ass Chains", "BAC"}); modes.push_back({"Team Big Ass Chains", "TBAC"}); }
  if (!teams) rep.warnings.push_back("no team spawns (team1_spawn / team2_spawn): team modes use the shared spawn points");
  std::string key = "MULTI_LEVEL_";
  for (char c : id) key += char(std::toupper((unsigned char)c));
  uint32_t base_id = 1000 + (Crc32(id) % 7000) * 4;  // stored as 16 bits by the game
  std::ostringstream xt;
  xt << "<root>\n<Table>\n";
  int mi = 0;
  for (auto& [mode, abbr] : modes) {
    xt << "\t<MultiplayerLevel>\n\t\t<Name>" << id << " " << abbr << "</Name>\n\t\t<Mode>" << mode << "</Mode>\n\t\t<Map>" << id
       << "</Map>\n\t\t<CTSFile>" << id << "</CTSFile>\n\t\t<_Editor>\n\t\t\t<Category>Entries</Category>\n\t\t\t</_Editor>\n"
       << "\t\t<DisplayName>" << key << "</DisplayName>\n\t\t<ID>" << base_id + mi++ << "</ID>\n\t\t<Flags>\n"
       << "\t\t\t<Flag>AlwaysClearWeather</Flag>\n\t\t\t</Flags>\n\t\t<MinimapZoom>1.5</MinimapZoom>\n"
       << "\t\t<LevelSelectionWeight>500</LevelSelectionWeight>\n\t\t<Optimal_Players_Min>2</Optimal_Players_Min>\n"
       << "\t\t<Optimal_Players_Max>12</Optimal_Players_Max>\n\t\t<num_customizable_weapons>8</num_customizable_weapons>\n"
       << "\t\t<num_customizable_vehicles>" << (nv ? 4 : 0) << "</num_customizable_vehicles>\n\t\t</MultiplayerLevel>\n";
    rep.modes.push_back(mode);
  }
  xt << "\t</Table>\n</root>\n";
  std::u16string txt = u"BINTEXT\r\n\r\n" + Utf16(key) + u"=" + Utf16(st.display_name) + u"\r\n" + Utf16("             1") + u"\r\n";
  Bytes strings = {0xFF, 0xFE};
  for (char16_t c : txt) { strings.push_back(uint8_t(c & 0xFF)); strings.push_back(uint8_t(c >> 8)); }

  // ---- write the pack ----
  fs::path folder = maps_dir / fs::u8path(st.folder), levels = folder / "levels";
  fs::create_directories(levels);
  for (auto& e : fs::directory_iterator(levels)) {   // old output of an earlier conversion
    std::string ext = e.path().extension().u8string();
    if (e.is_regular_file() && (ext == ".bbchunk_xbox2" || ext == ".peg_xbox2" || ext == ".bbcity_xbox2" || ext == ".pvsv_xbox2" ||
                                ext == ".hmap_xbox2" || ext == ".cts" || ext == ".xtbl" || ext == ".txt" || ext == ".pvs_xbox2"))
      fs::remove(e.path());
  }
  WriteFile(levels / (id + ".bbchunk_xbox2"), out_chunk);
  WriteFile(levels / (id + ".peg_xbox2"), out_peg);
  WriteFile(levels / (id + ".bbcity_xbox2"), city);
  WriteFile(levels / (id + ".pvsv_xbox2"), pvsv_b);
  WriteFile(levels / (id + ".hmap_xbox2"), hmap_b);
  WriteFile(levels / ("amb_" + id + ".cts"), amb_b);
  WriteFile(levels / ("ld-" + id + ".peg_xbox2"), RenamePegEntries(ld_b, T, id));
  WriteText(levels / (id + ".cts"), cts);
  WriteText(levels / "multiplayer_levels_extra.xtbl", xt.str());
  WriteFile(levels / "US_strings.txt", strings);
  WriteText(folder / "bundle_info.xtbl",
            "<root>\n<Table>\n\t<BundleInfo>\n\t\t<BundleInfoFlags>\n\t\t\t<Flag>BUNDLE_TYPE_LEVEL</Flag>\n\t\t\t</BundleInfoFlags>\n\t\t</BundleInfo>\n\t</Table>\n</root>\n");
  rep.folder = folder;
  return rep;
}

}  // namespace mapconv
