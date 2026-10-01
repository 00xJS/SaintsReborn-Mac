// Direct3D 11 renderer for the editor viewport: textured, lit meshes and coloured lines.
#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <vector>

#include "peg.h"

namespace editor {

using Microsoft::WRL::ComPtr;

struct MeshVertex {
  float pos[3], nrm[3], uv[2];
};
struct LineVertex {
  float pos[3];
  uint32_t rgba;
};

class Renderer {
 public:
  bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx);
  // Uploads an image as a mip-mapped texture; returns an index for DrawMesh.
  int CreateTexture(const mapconv::Image& img);
  void UpdateTexture(int id, const mapconv::Image& img);
  void ReleaseTexture(int id);
  // A texture that can be rendered into (previews); returns its id for TextureView.
  int CreateTargetTexture(int w, int h, ComPtr<ID3D11RenderTargetView>& rtv);
  ID3D11ShaderResourceView* TextureView(int id) const { return id >= 0 && size_t(id) < textures_.size() ? textures_[size_t(id)].Get() : nullptr; }
  struct Mesh {
    ComPtr<ID3D11Buffer> vb;
    UINT count = 0;
  };
  Mesh CreateMesh(const std::vector<MeshVertex>& v);

  void Begin(const DirectX::XMMATRIX& view_proj, const float eye[3]);
  void DrawMesh(const Mesh& m, int texture, const float tint[4], bool textured = true);
  // Lines are collected and drawn at End (depth tested unless on_top).
  void Line(const float a[3], const float b[3], uint32_t rgba, bool on_top = false);
  void Box(const float mn[3], const float mx[3], uint32_t rgba, bool on_top = false);
  void End();

 private:
  ID3D11Device* dev_ = nullptr;
  ID3D11DeviceContext* ctx_ = nullptr;
  ComPtr<ID3D11VertexShader> mesh_vs_, line_vs_;
  ComPtr<ID3D11PixelShader> mesh_ps_, line_ps_;
  ComPtr<ID3D11InputLayout> mesh_il_, line_il_;
  ComPtr<ID3D11Buffer> cb_, line_vb_;
  ComPtr<ID3D11SamplerState> sampler_;
  ComPtr<ID3D11RasterizerState> raster_, raster_lines_;
  ComPtr<ID3D11DepthStencilState> depth_, depth_off_;
  ComPtr<ID3D11BlendState> blend_;
  std::vector<ComPtr<ID3D11ShaderResourceView>> textures_;
  std::vector<int> free_textures_;
  int NewSlot();
  std::vector<LineVertex> lines_, lines_top_;
  UINT line_capacity_ = 0;
  DirectX::XMFLOAT4X4 view_proj_{};
  float eye_[3]{};
  void SetConstants(const float tint[4], bool textured);
  void FlushLines(std::vector<LineVertex>& lines, bool on_top);
};

}  // namespace editor
