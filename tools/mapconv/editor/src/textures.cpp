#include "textures.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

#include "stb_image.h"

namespace editor {
namespace {

constexpr int N = 256;

struct Noise {
  std::vector<float> v;
  int n;
  // tileable value noise: random lattice (period n / cell) with smooth interpolation, summed octaves
  Noise(int size, int seed, int cells, int octaves, float persist) : v(size_t(size) * size, 0), n(size) {
    std::mt19937 rng{uint32_t(seed)};
    std::uniform_real_distribution<float> U(-1, 1);
    float amp = 1, total = 0;
    for (int o = 0; o < octaves; ++o, cells *= 2, amp *= persist) {
      std::vector<float> g(size_t(cells) * cells);
      for (auto& x : g) x = U(rng);
      for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
          float fx = float(x) * cells / n, fy = float(y) * cells / n;
          int x0 = int(fx), y0 = int(fy);
          float tx = fx - x0, ty = fy - y0;
          tx = tx * tx * (3 - 2 * tx);
          ty = ty * ty * (3 - 2 * ty);
          auto at = [&](int a, int b) { return g[size_t((b % cells) * cells + (a % cells))]; };
          float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
          float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
          v[size_t(y) * n + x] += amp * (a + (b - a) * ty);
        }
      total += amp;
    }
    for (auto& x : v) x /= total;
  }
  float operator()(int x, int y) const { return v[size_t((y % n + n) % n) * n + (x % n + n) % n]; }
};

template <class F>
mapconv::Image Make(F fn) {
  mapconv::Image im;
  im.w = im.h = N;
  im.rgb.resize(size_t(N) * N * 3);
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
      float c[3];
      fn(x, y, c);
      for (int k = 0; k < 3; ++k) im.rgb[(size_t(y) * N + x) * 3 + k] = uint8_t(std::clamp(c[k], 0.0f, 255.0f));
    }
  return im;
}

void Set(float* c, float r, float g, float b, float k = 1) {
  c[0] = r * k;
  c[1] = g * k;
  c[2] = b * k;
}

}  // namespace

const std::vector<BuiltinTexture>& BuiltinTextures() {
  static const std::vector<BuiltinTexture> t = {
      {"concrete", "Concrete"}, {"grid", "Grid floor"},  {"brick", "Brick"},     {"asphalt", "Asphalt"},
      {"planks", "Wood planks"}, {"metal", "Metal plate"}, {"tiles", "Tiles"},     {"grass", "Grass"},
      {"hazard", "Hazard stripes"}, {"sand", "Sand"},       {"white", "White"},     {"grey", "Grey"},
      {"black", "Black"},       {"red", "Saints purple"}, {"blue", "Blue"},       {"green", "Green"},
  };
  return t;
}

mapconv::Image MakeBuiltinTexture(const std::string& id) {
  if (id == "concrete") {
    Noise a(N, 1, 8, 4, 0.55f), b(N, 2, 2, 2, 0.5f);
    return Make([&](int x, int y, float* c) {
      float k = 0.82f + 0.12f * a(x, y) + 0.08f * b(x, y);
      if (x % 128 < 2 || y % 128 < 2) k *= 0.72f;
      Set(c, 142, 140, 134, k);
    });
  }
  if (id == "grid") {
    Noise a(N, 3, 8, 3, 0.5f);
    return Make([&](int x, int y, float* c) {
      if (x % 64 < 2 || y % 64 < 2) return Set(c, 205, 172, 58);
      Set(c, 82, 88, 96, 0.92f + 0.12f * a(x, y));
    });
  }
  if (id == "brick") {
    Noise a(N, 4, 16, 3, 0.5f), b(N, 5, 4, 2, 0.5f);
    return Make([&](int x, int y, float* c) {
      int row = y / 32, bx = (x + (row % 2) * 32) % 64;
      if (y % 32 < 3 || bx < 3) return Set(c, 172, 166, 152, 0.95f + 0.1f * a(x, y));
      float k = 0.88f + 0.12f * a(x, y) + 0.1f * b(x / 64 * 64 + row, y);
      Set(c, 152, 70, 48, k);
    });
  }
  if (id == "asphalt") {
    Noise a(N, 6, 32, 3, 0.6f), b(N, 7, 2, 2, 0.5f);
    return Make([&](int x, int y, float* c) { Set(c, 58, 58, 60, 0.9f + 0.18f * a(x, y) + 0.08f * b(x, y)); });
  }
  if (id == "planks") {
    Noise a(N, 8, 4, 4, 0.5f);
    return Make([&](int x, int y, float* c) {
      int plank = x / 32;
      float grain = std::sin((y + plank * 37) * 0.19f + 6 * a(x / 4, y)) * 0.06f;
      float k = 0.85f + grain + 0.06f * ((plank * 7) % 5) / 4.0f;
      if (x % 32 < 2 || (y + plank * 83) % 256 < 2) k *= 0.55f;
      Set(c, 150, 104, 62, k);
    });
  }
  if (id == "metal") {
    Noise a(N, 9, 16, 3, 0.5f);
    return Make([&](int x, int y, float* c) {
      int dx = x % 128, dy = y % 128;
      float k = 0.9f + 0.06f * a(x, y);
      if (dx < 2 || dy < 2) k *= 0.6f;
      bool rivet = (std::abs(dx - 8) < 3 || std::abs(dx - 120) < 3) && (std::abs(dy - 8) < 3 || std::abs(dy - 120) < 3);
      if (rivet) k *= 1.25f;
      if (((x + 2 * y) / 6) % 4 == 0) k *= 0.97f;
      Set(c, 128, 132, 138, k);
    });
  }
  if (id == "tiles") {
    Noise a(N, 10, 8, 2, 0.5f);
    return Make([&](int x, int y, float* c) {
      if (x % 64 < 3 || y % 64 < 3) return Set(c, 120, 118, 112);
      Set(c, 214, 210, 200, 0.93f + 0.07f * a(x, y) + 0.04f * (((x / 64 + y / 64) % 2) ? 1 : -1));
    });
  }
  if (id == "grass") {
    Noise a(N, 11, 32, 3, 0.6f), b(N, 12, 4, 2, 0.5f);
    return Make([&](int x, int y, float* c) {
      float k = 0.85f + 0.2f * a(x, y);
      float t = 0.5f + 0.5f * b(x, y);
      Set(c, 70 + 30 * t, 110 + 20 * t, 45, k);
    });
  }
  if (id == "hazard") {
    return Make([&](int x, int y, float* c) {
      if (((x + y) / 32) % 2 == 0) Set(c, 235, 185, 25); else Set(c, 30, 30, 30);
    });
  }
  if (id == "sand") {
    Noise a(N, 13, 64, 2, 0.5f), b(N, 14, 4, 2, 0.5f);
    return Make([&](int x, int y, float* c) { Set(c, 198, 176, 128, 0.92f + 0.1f * a(x, y) + 0.06f * b(x, y)); });
  }
  struct Flat { const char* id; float r, g, b; };
  static const Flat flats[] = {{"white", 225, 225, 225}, {"grey", 128, 128, 128}, {"black", 30, 30, 32},
                               {"red", 110, 40, 150}, {"blue", 50, 90, 190}, {"green", 60, 150, 70}};
  for (auto& f : flats)
    if (id == f.id) {
      Noise a(N, 15, 8, 2, 0.5f);
      return Make([&](int x, int y, float* c) { Set(c, f.r, f.g, f.b, 0.97f + 0.04f * a(x, y)); });
    }
  return {};
}

mapconv::Image LoadImageFile(const std::string& path) {
  mapconv::Image im;
  int w, h, n;
  uint8_t* px = stbi_load(path.c_str(), &w, &h, &n, 3);
  if (!px) return im;
  im.w = w;
  im.h = h;
  im.rgb.assign(px, px + size_t(w) * h * 3);
  stbi_image_free(px);
  return im;
}

}  // namespace editor
