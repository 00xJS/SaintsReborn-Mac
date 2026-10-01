#include "gltf_import.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace mapconv {
namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

// "weapon_ak47.001" -> "weapon_ak47" (Blender duplicate suffix), trims spaces.
std::string CleanName(std::string s) {
  s = Lower(s);
  size_t dot = s.find('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  while (!s.empty() && (s.back() == ' ' || s.back() == '_')) s.pop_back();
  return s;
}

bool Starts(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }

// Minimal string value lookup in a JSON object: "key": "value".
std::string JsonString(const std::string& json, const std::string& key) {
  size_t k = json.find("\"" + key + "\"");
  if (k == std::string::npos) return {};
  size_t c = json.find(':', k);
  if (c == std::string::npos) return {};
  size_t q1 = json.find('"', c);
  size_t close = json.find_first_of(",}", c);
  if (q1 == std::string::npos || (close != std::string::npos && close < q1)) return {};
  size_t q2 = json.find('"', q1 + 1);
  if (q2 == std::string::npos) return {};
  return json.substr(q1 + 1, q2 - q1 - 1);
}

struct Importer {
  std::filesystem::path path;
  cgltf_data* data = nullptr;
  MapInput out;
  std::map<const cgltf_material*, int> mat_index;

  Image DecodeImage(const cgltf_image* img) {
    Image im;
    std::vector<uint8_t> bytes;
    if (img->buffer_view) {
      const cgltf_buffer_view* bv = img->buffer_view;
      const uint8_t* p = static_cast<const uint8_t*>(bv->buffer->data) + bv->offset;
      bytes.assign(p, p + bv->size);
    } else if (img->uri) {
      std::string uri = img->uri;
      if (Starts(uri, "data:")) {
        size_t comma = uri.find(',');
        void* dec = nullptr;
        std::string b64 = uri.substr(comma + 1);
        cgltf_options opt{};
        size_t size = b64.size() * 3 / 4;
        while (!b64.empty() && b64.back() == '=') { b64.pop_back(); --size; }
        if (cgltf_load_buffer_base64(&opt, size, uri.c_str() + comma + 1, &dec) == cgltf_result_success) {
          bytes.assign(static_cast<uint8_t*>(dec), static_cast<uint8_t*>(dec) + size);
          free(dec);
        }
      } else {
        std::vector<char> u(uri.begin(), uri.end());
        u.push_back(0);
        cgltf_decode_uri(u.data());
        std::ifstream f(path.parent_path() / std::filesystem::u8path(u.data()), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(f), {});
      }
    }
    if (bytes.empty()) return im;
    int w, h, n;
    uint8_t* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 3);
    if (!px) {
      out.warnings.push_back(std::string("could not read texture ") + (img->name ? img->name : img->uri ? img->uri : "?"));
      return im;
    }
    im.w = w;
    im.h = h;
    im.rgb.assign(px, px + size_t(w) * h * 3);
    stbi_image_free(px);
    return im;
  }

  int Material(const cgltf_material* m) {
    auto it = mat_index.find(m);
    if (it != mat_index.end()) return it->second;
    InMaterial im;
    float f[4] = {0.7f, 0.7f, 0.7f, 1.0f};
    const cgltf_texture* tex = nullptr;
    if (m) {
      im.name = m->name ? m->name : "material";
      if (m->has_pbr_metallic_roughness) {
        std::copy(m->pbr_metallic_roughness.base_color_factor, m->pbr_metallic_roughness.base_color_factor + 4, f);
        tex = m->pbr_metallic_roughness.base_color_texture.texture;
      } else if (m->has_pbr_specular_glossiness) {
        std::copy(m->pbr_specular_glossiness.diffuse_factor, m->pbr_specular_glossiness.diffuse_factor + 4, f);
        tex = m->pbr_specular_glossiness.diffuse_texture.texture;
      }
    } else {
      im.name = "default";
    }
    if (tex && tex->image) im.image = DecodeImage(tex->image);
    if (im.image.w == 0) {
      im.image.w = im.image.h = 8;
      im.image.rgb.assign(8 * 8 * 3, 255);
    }
    // colour factor (glTF factors are linear; the textures are sRGB)
    for (int c = 0; c < 3; ++c) {
      float k = std::pow(std::clamp(f[c], 0.0f, 1.0f), 1.0f / 2.2f);
      if (k < 0.999f)
        for (size_t i = c; i < im.image.rgb.size(); i += 3) im.image.rgb[i] = uint8_t(std::lround(im.image.rgb[i] * k));
    }
    int idx = int(out.materials.size());
    out.materials.push_back(std::move(im));
    mat_index[m] = idx;
    return idx;
  }

  bool TryMarker(const cgltf_node* node, const float* M) {
    std::string name = CleanName(node->name ? node->name : "");
    std::string type, value;
    if (node->extras.data) {
      std::string ex = node->extras.data;
      type = Lower(JsonString(ex, "sr_type"));
      value = JsonString(ex, "sr_value");
    }
    Marker mk;
    mk.source = node->name ? node->name : "";
    auto rest = [&](const std::string& prefix) { return name.substr(prefix.size()); };
    if (type == "spawn" || type == "team1_spawn" || type == "team2_spawn") {
      mk.kind = MarkerKind::Spawn;
      mk.team = type == "team1_spawn" ? 1 : type == "team2_spawn" ? 2 : 0;
    } else if (type == "player_start") {
      mk.kind = MarkerKind::PlayerStart;
    } else if (type == "weapon") {
      mk.kind = MarkerKind::Weapon;
      mk.type = value;
    } else if (type == "vehicle") {
      mk.kind = MarkerKind::Vehicle;
      mk.type = value;
    } else if (type == "chains_dropoff") {
      mk.kind = MarkerKind::ChainsDropOff;
    } else if (!type.empty()) {
      out.warnings.push_back("unknown sr_type '" + type + "' on " + mk.source);
      return false;
    } else if (Starts(name, "player_start") || Starts(name, "playerstart") || Starts(name, "sr_player_start")) {
      mk.kind = MarkerKind::PlayerStart;
    } else if (Starts(name, "team1_spawn") || Starts(name, "red_spawn") || Starts(name, "spawn_team1") || Starts(name, "sr_team1_spawn")) {
      mk.kind = MarkerKind::Spawn;
      mk.team = 1;
    } else if (Starts(name, "team2_spawn") || Starts(name, "blue_spawn") || Starts(name, "spawn_team2") || Starts(name, "sr_team2_spawn")) {
      mk.kind = MarkerKind::Spawn;
      mk.team = 2;
    } else if (Starts(name, "spawn") || Starts(name, "respawn") || Starts(name, "sr_spawn")) {
      mk.kind = MarkerKind::Spawn;
    } else if (Starts(name, "weapon_") || Starts(name, "sr_weapon_")) {
      mk.kind = MarkerKind::Weapon;
      mk.type = rest(Starts(name, "sr_") ? "sr_weapon_" : "weapon_");
    } else if (Starts(name, "vehicle_") || Starts(name, "sr_vehicle_")) {
      mk.kind = MarkerKind::Vehicle;
      mk.type = rest(Starts(name, "sr_") ? "sr_vehicle_" : "vehicle_");
    } else if (Starts(name, "chains_dropoff") || Starts(name, "dropoff") || Starts(name, "sr_chains_dropoff")) {
      mk.kind = MarkerKind::ChainsDropOff;
    } else {
      return false;
    }
    // position (mirror x into game space) and facing (the node's +Z axis)
    mk.pos = {-M[12], M[13], M[14]};
    // facing = the node's +Z axis (Blender: -Y, the way characters face; Unity: the blue Z arrow),
    // or its -Y axis when +Z points up or down (an object tilted to lie flat)
    float fx = -M[8], fz = M[10], len = std::sqrt(M[8] * M[8] + M[9] * M[9] + M[10] * M[10]);
    if (std::sqrt(fx * fx + fz * fz) < 0.3f * len) { fx = M[4]; fz = -M[6]; }
    mk.yaw = (std::abs(fx) + std::abs(fz) > 1e-6f) ? std::atan2(fx, fz) : 0.0f;
    if ((mk.kind == MarkerKind::Weapon || mk.kind == MarkerKind::Vehicle) && mk.type.empty()) {
      out.warnings.push_back(mk.source + ": no " + std::string(mk.kind == MarkerKind::Weapon ? "weapon" : "vehicle") + " type given");
      return true;
    }
    out.markers.push_back(mk);
    return true;
  }

  void AddPrimitive(const cgltf_primitive& prim, const float* M) {
    if (prim.type != cgltf_primitive_type_triangles && prim.type != cgltf_primitive_type_triangle_strip &&
        prim.type != cgltf_primitive_type_triangle_fan)
      return;
    const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr;
    for (size_t i = 0; i < prim.attributes_count; ++i) {
      const auto& a = prim.attributes[i];
      if (a.type == cgltf_attribute_type_position) pos = a.data;
      else if (a.type == cgltf_attribute_type_normal) nrm = a.data;
      else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uv = a.data;
    }
    if (!pos) return;
    int mat = Material(prim.material);
    size_t count = prim.indices ? prim.indices->count : pos->count;
    auto idx = [&](size_t i) { return prim.indices ? cgltf_accessor_read_index(prim.indices, i) : i; };
    std::vector<std::array<size_t, 3>> tris;
    if (prim.type == cgltf_primitive_type_triangles) {
      for (size_t i = 0; i + 2 < count; i += 3) tris.push_back({idx(i), idx(i + 1), idx(i + 2)});
    } else if (prim.type == cgltf_primitive_type_triangle_strip) {
      for (size_t i = 0; i + 2 < count; ++i)
        tris.push_back(i % 2 ? std::array<size_t, 3>{idx(i + 1), idx(i), idx(i + 2)} : std::array<size_t, 3>{idx(i), idx(i + 1), idx(i + 2)});
    } else {
      for (size_t i = 1; i + 1 < count; ++i) tris.push_back({idx(0), idx(i), idx(i + 1)});
    }
    // normal matrix = inverse transpose of the upper 3x3 (column-major M)
    double a[9] = {M[0], M[4], M[8], M[1], M[5], M[9], M[2], M[6], M[10]};  // row-major 3x3
    double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
    double inv[9] = {(a[4] * a[8] - a[5] * a[7]) / det, (a[2] * a[7] - a[1] * a[8]) / det, (a[1] * a[5] - a[2] * a[4]) / det,
                     (a[5] * a[6] - a[3] * a[8]) / det, (a[0] * a[8] - a[2] * a[6]) / det, (a[2] * a[3] - a[0] * a[5]) / det,
                     (a[3] * a[7] - a[4] * a[6]) / det, (a[1] * a[6] - a[0] * a[7]) / det, (a[0] * a[4] - a[1] * a[3]) / det};
    for (auto& t : tris) {
      InTriangle tr;
      tr.material = mat;
      bool ok = true;
      for (int j = 0; j < 3; ++j) {
        float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, u[2] = {0, 0};
        if (t[j] >= pos->count) { ok = false; break; }
        cgltf_accessor_read_float(pos, t[j], p, 3);
        float w[3];
        for (int r = 0; r < 3; ++r) w[r] = M[r] * p[0] + M[4 + r] * p[1] + M[8 + r] * p[2] + M[12 + r];
        tr.p[j] = {-w[0], w[1], w[2]};
        if (nrm && t[j] < nrm->count) {
          cgltf_accessor_read_float(nrm, t[j], n, 3);
          double m[3];
          for (int r = 0; r < 3; ++r) m[r] = inv[0 + r] * n[0] + inv[3 + r] * n[1] + inv[6 + r] * n[2];  // inv^T * n
          double l = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
          if (l > 1e-12) tr.n[j] = {float(-m[0] / l), float(m[1] / l), float(m[2] / l)};
          else tr.n[j] = {0, 0, 0};
        } else {
          tr.n[j] = {0, 0, 0};
        }
        if (uv && t[j] < uv->count) cgltf_accessor_read_float(uv, t[j], u, 2);
        tr.uv[j][0] = u[0];
        tr.uv[j][1] = u[1];
      }
      if (!ok) continue;
      // mirroring x (and a mirrored node transform) reverses the winding: restore it
      bool flip = true;
      if (det < 0) flip = !flip;
      if (flip) {
        std::swap(tr.p[1], tr.p[2]);
        std::swap(tr.n[1], tr.n[2]);
        std::swap(tr.uv[1][0], tr.uv[2][0]);
        std::swap(tr.uv[1][1], tr.uv[2][1]);
      }
      out.tris.push_back(tr);
    }
  }

  void Visit(const cgltf_node* node) {
    float M[16];
    cgltf_node_transform_world(node, M);
    bool marker = TryMarker(node, M);
    if (node->mesh && !marker)
      for (size_t i = 0; i < node->mesh->primitives_count; ++i) AddPrimitive(node->mesh->primitives[i], M);
    for (size_t i = 0; i < node->children_count; ++i) Visit(node->children[i]);
  }
};

}  // namespace

MapInput ImportGltf(const std::filesystem::path& path) {
  Importer im;
  im.path = path;
  cgltf_options opt{};
  std::string p = path.u8string();
  if (cgltf_parse_file(&opt, p.c_str(), &im.data) != cgltf_result_success)
    throw Error("could not read " + path.filename().u8string() + " (not a valid .glb / .gltf file)");
  struct Free { cgltf_data* d; ~Free() { cgltf_free(d); } } guard{im.data};
  if (cgltf_load_buffers(&opt, im.data, p.c_str()) != cgltf_result_success)
    throw Error("could not load the data of " + path.filename().u8string() + " (missing .bin file?)");
  const cgltf_scene* scene = im.data->scene ? im.data->scene : (im.data->scenes_count ? &im.data->scenes[0] : nullptr);
  if (scene) {
    for (size_t i = 0; i < scene->nodes_count; ++i) im.Visit(scene->nodes[i]);
  } else {
    for (size_t i = 0; i < im.data->nodes_count; ++i)
      if (!im.data->nodes[i].parent) im.Visit(&im.data->nodes[i]);
  }
  return std::move(im.out);
}

}  // namespace mapconv
