// .peg_xbox2 texture packages (VKEG v3): DXT1 encoding in the Xbox 360 tiled layout and appending
// textures to an existing package (entry template copied from a stock DXT1 texture).
#pragma once
#include "be.h"

namespace mapconv {

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgb;  // w*h*3
};

// Stock layout: base level, then each level of 128 px or more; the small levels share a packed tail
// filled with the average colour (verified in game with 256 px textures).
Bytes EncodeMipChain(const Image& img);
Bytes AppendTextures(const Bytes& peg, const std::vector<std::pair<std::string, Image>>& textures,
                     const std::string& template_name);
// Renames entries whose name contains `from` (replaced by `to`).
Bytes RenamePegEntries(const Bytes& peg, const std::string& from, const std::string& to);
std::vector<std::string> PegNames(const Bytes& peg);
Image Resize(const Image& src, int w, int h);

}  // namespace mapconv
