// Built-in tileable textures for the editor (256 px, generated, so nothing needs shipping).
#pragma once
#include <string>
#include <vector>
#include "peg.h"

namespace editor {

struct BuiltinTexture {
  const char* id;
  const char* label;
};
const std::vector<BuiltinTexture>& BuiltinTextures();
mapconv::Image MakeBuiltinTexture(const std::string& id);  // empty image when unknown
mapconv::Image LoadImageFile(const std::string& path);     // PNG / JPG / BMP / TGA, empty on failure

}  // namespace editor
