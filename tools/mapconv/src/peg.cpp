#include "peg.h"

#include <algorithm>
#include <cmath>

#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

namespace mapconv {
namespace {

uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t width, uint32_t log_bpp) {
  uint32_t aw = (width + 31) & ~31u;
  uint32_t macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (log_bpp + 7);
  uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log_bpp;
  uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4);
  return (((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> log_bpp;
}

Bytes EncodeDxt1(const Image& img) {
  int bw = img.w / 4, bh = img.h / 4;
  Bytes out(size_t(std::max(bw, 32)) * std::max(bh, 32) * 8, 0);
  uint8_t block[64];
  for (int by = 0; by < bh; ++by) {
    for (int bx = 0; bx < bw; ++bx) {
      for (int py = 0; py < 4; ++py)
        for (int px = 0; px < 4; ++px) {
          const uint8_t* s = &img.rgb[(size_t(by * 4 + py) * img.w + bx * 4 + px) * 3];
          uint8_t* d = &block[(py * 4 + px) * 4];
          d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        }
      uint8_t pc[8];
      stb_compress_dxt_block(pc, block, 0, STB_DXT_HIGHQUAL);
      size_t o = size_t(TiledOffset(bx, by, bw, 3)) * 8;
      // PC blocks are little-endian 16-bit words; the 360 stores them big-endian
      for (int i = 0; i < 8; i += 2) {
        out[o + i] = pc[i + 1];
        out[o + i + 1] = pc[i];
      }
    }
  }
  return out;
}

Image Half(const Image& a) {
  Image b;
  b.w = a.w / 2;
  b.h = a.h / 2;
  b.rgb.resize(size_t(b.w) * b.h * 3);
  for (int y = 0; y < b.h; ++y)
    for (int x = 0; x < b.w; ++x)
      for (int c = 0; c < 3; ++c) {
        int s = a.rgb[(size_t(2 * y) * a.w + 2 * x) * 3 + c] + a.rgb[(size_t(2 * y) * a.w + 2 * x + 1) * 3 + c] +
                a.rgb[(size_t(2 * y + 1) * a.w + 2 * x) * 3 + c] + a.rgb[(size_t(2 * y + 1) * a.w + 2 * x + 1) * 3 + c];
        b.rgb[(size_t(y) * b.w + x) * 3 + c] = uint8_t((s + 2) / 4);
      }
  return b;
}

}  // namespace

Image Resize(const Image& src, int w, int h) {
  Image out;
  out.w = w;
  out.h = h;
  out.rgb.resize(size_t(w) * h * 3);
  // area average when shrinking, bilinear when growing
  for (int y = 0; y < h; ++y) {
    double y0 = double(y) * src.h / h, y1 = double(y + 1) * src.h / h;
    for (int x = 0; x < w; ++x) {
      double x0 = double(x) * src.w / w, x1 = double(x + 1) * src.w / w;
      double acc[3] = {0, 0, 0}, wsum = 0;
      if (x1 - x0 <= 1.0 && y1 - y0 <= 1.0) {
        double fx = (x + 0.5) * src.w / w - 0.5, fy = (y + 0.5) * src.h / h - 0.5;
        int ix = int(std::floor(fx)), iy = int(std::floor(fy));
        double tx = fx - ix, ty = fy - iy;
        for (int j = 0; j < 2; ++j)
          for (int i = 0; i < 2; ++i) {
            int sx = std::clamp(ix + i, 0, src.w - 1), sy = std::clamp(iy + j, 0, src.h - 1);
            double wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
            for (int c = 0; c < 3; ++c) acc[c] += wgt * src.rgb[(size_t(sy) * src.w + sx) * 3 + c];
            wsum += wgt;
          }
      } else {
        for (int sy = int(y0); sy < int(std::ceil(y1)) && sy < src.h; ++sy)
          for (int sx = int(x0); sx < int(std::ceil(x1)) && sx < src.w; ++sx) {
            double wy = std::min(y1, sy + 1.0) - std::max(y0, double(sy));
            double wx = std::min(x1, sx + 1.0) - std::max(x0, double(sx));
            double wgt = std::max(0.0, wx) * std::max(0.0, wy);
            for (int c = 0; c < 3; ++c) acc[c] += wgt * src.rgb[(size_t(sy) * src.w + sx) * 3 + c];
            wsum += wgt;
          }
      }
      for (int c = 0; c < 3; ++c) out.rgb[(size_t(y) * w + x) * 3 + c] = uint8_t(std::clamp(acc[c] / wsum + 0.5, 0.0, 255.0));
    }
  }
  return out;
}

Bytes EncodeMipChain(const Image& img) {
  Bytes out = EncodeDxt1(img);
  Image cur = img;
  while (cur.w >= 128 && cur.h >= 128) {
    cur = Half(cur);
    Append(out, EncodeDxt1(cur));
  }
  double avg[3] = {0, 0, 0};
  for (size_t i = 0; i < img.rgb.size(); i += 3)
    for (int c = 0; c < 3; ++c) avg[c] += img.rgb[i + c];
  size_t n = img.rgb.size() / 3;
  int r = int(std::lround(avg[0] / n)), g = int(std::lround(avg[1] / n)), b = int(std::lround(avg[2] / n));
  uint16_t c565 = uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  for (int i = 0; i < (16384 + 32768) / 8; ++i) {
    Put16(out, c565);
    Put16(out, c565);
    Put32(out, 0);
  }
  return out;
}

std::vector<std::string> PegNames(const Bytes& peg) {
  std::vector<std::string> out;
  uint16_t n = U16(peg, 0x14);
  for (uint16_t i = 0; i < n; ++i) {
    size_t e = 0x18 + 0x48 * size_t(i);
    std::string s(reinterpret_cast<const char*>(&peg[e + 0x17]), 0x46 - 0x17);
    out.push_back(s.substr(0, s.find('\0')));
  }
  return out;
}

Bytes AppendTextures(const Bytes& peg, const std::vector<std::pair<std::string, Image>>& textures,
                     const std::string& template_name) {
  Bytes d = peg;
  uint16_t n = U16(d, 0x14);
  uint32_t first = 0xFFFFFFFF;
  for (uint16_t i = 0; i < n; ++i) first = std::min(first, U32(d, 0x18 + 0x48 * size_t(i)));
  auto names = PegNames(d);
  Bytes tmpl;
  for (uint16_t i = 0; i < n; ++i) {
    std::string lower = names[i];
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == template_name) tmpl = Slice(d, 0x18 + 0x48 * size_t(i), 0x60 + 0x48 * size_t(i));
  }
  if (tmpl.empty()) throw Error("texture template " + template_name + " not found in the template package");
  if (0x18 + 0x48 * (size_t(n) + textures.size()) > first)
    throw Error("too many textures for the template package (" + std::to_string(textures.size()) + ")");
  Bytes head = Slice(d, 0, first), data = Slice(d, first, d.size());
  std::vector<Bytes> entries;
  for (const auto& [name, img] : textures) {
    if (name.size() > 0x46 - 0x17 - 1) throw Error("texture name too long: " + name);
    Bytes blob = EncodeMipChain(img);
    size_t off = first + data.size();
    off = Al(off, 4096);
    data.resize(off - first, 0);
    Append(data, blob);
    Bytes e = tmpl;
    Wr32(&e[0], uint32_t(off));
    Wr16(&e[4], uint16_t(img.w));
    Wr16(&e[6], uint16_t(img.h));
    Wr16(&e[8], 0x190);  // DXT1
    int mips = int(std::log2(std::max(img.w, img.h) / 4)) + 1;
    e[0x16] = uint8_t(mips);
    e[0x14] = std::max(img.w, img.h) >= 256 ? 0x20 : 0x10;
    std::fill(e.begin() + 0x17, e.begin() + 0x46, 0);
    std::copy(name.begin(), name.end(), e.begin() + 0x17);
    entries.push_back(e);
  }
  data.resize(Al(data.size(), 4096), 0);
  for (size_t k = 0; k < entries.size(); ++k)
    std::copy(entries[k].begin(), entries[k].end(), head.begin() + 0x18 + 0x48 * (n + k));
  Wr32(&head[8], uint32_t(0x18 + 0x48 * (n + entries.size())));
  Wr32(&head[12], uint32_t(data.size()));
  Wr16(&head[0x10], uint16_t(U16(head, 0x10) + entries.size()));
  Wr16(&head[0x14], uint16_t(n + entries.size()));
  Append(head, data);
  return head;
}

Bytes RenamePegEntries(const Bytes& peg, const std::string& from, const std::string& to) {
  Bytes d = peg;
  auto names = PegNames(d);
  for (size_t i = 0; i < names.size(); ++i) {
    std::string nm = names[i];
    size_t p = nm.find(from);
    if (p == std::string::npos) continue;
    nm.replace(p, from.size(), to);
    if (nm.size() > 0x46 - 0x17 - 1) throw Error("texture name too long: " + nm);
    size_t e = 0x18 + 0x48 * i;
    std::fill(d.begin() + e + 0x17, d.begin() + e + 0x46, 0);
    std::copy(nm.begin(), nm.end(), d.begin() + e + 0x17);
  }
  return d;
}

}  // namespace mapconv
