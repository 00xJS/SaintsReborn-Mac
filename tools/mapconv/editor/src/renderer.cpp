#include "renderer.h"

#include <d3dcompiler.h>

#include <cstring>

namespace editor {
using namespace DirectX;

namespace {

const char* kShaders = R"(
cbuffer C : register(b0) {
  row_major float4x4 view_proj;
  float4 tint;
  float4 light;    // xyz = direction towards the light, w = textured
  float4 eye;
};
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct MI { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD; };
struct MO { float4 pos : SV_Position; float3 nrm : NORMAL; float2 uv : TEXCOORD; float3 world : WORLD; };
MO mesh_vs(MI i) { MO o; o.pos = mul(float4(i.pos, 1), view_proj); o.nrm = i.nrm; o.uv = i.uv; o.world = i.pos; return o; }
float4 mesh_ps(MO i) : SV_Target {
  float3 n = normalize(i.nrm);
  float sun = saturate(dot(n, normalize(light.xyz)));
  float sky = 0.5 + 0.5 * n.y;
  float3 lit = 0.32 + 0.18 * sky + 0.62 * sun;
  float3 base = light.w > 0.5 ? tex.Sample(smp, i.uv).rgb : float3(1, 1, 1);
  float fog = saturate((distance(i.world, eye.xyz) - 120) / 400);
  float3 c = base * tint.rgb * lit;
  return float4(lerp(c, float3(0.42, 0.47, 0.55), fog), tint.a);
}
struct LI { float3 pos : POSITION; float4 col : COLOR; };
struct LO { float4 pos : SV_Position; float4 col : COLOR; };
LO line_vs(LI i) { LO o; o.pos = mul(float4(i.pos, 1), view_proj); o.col = i.col; return o; }
float4 line_ps(LO i) : SV_Target { return i.col; }
)";

struct Constants {
  XMFLOAT4X4 view_proj;
  float tint[4];
  float light[4];
  float eye[4];
};

ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
  ComPtr<ID3DBlob> code, err;
  D3DCompile(kShaders, std::strlen(kShaders), "editor", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
             &code, &err);
  if (err) OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
  return code;
}

}  // namespace

bool Renderer::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  dev_ = dev;
  ctx_ = ctx;
  auto mvs = Compile("mesh_vs", "vs_5_0"), mps = Compile("mesh_ps", "ps_5_0");
  auto lvs = Compile("line_vs", "vs_5_0"), lps = Compile("line_ps", "ps_5_0");
  if (!mvs || !mps || !lvs || !lps) return false;
  dev->CreateVertexShader(mvs->GetBufferPointer(), mvs->GetBufferSize(), nullptr, &mesh_vs_);
  dev->CreatePixelShader(mps->GetBufferPointer(), mps->GetBufferSize(), nullptr, &mesh_ps_);
  dev->CreateVertexShader(lvs->GetBufferPointer(), lvs->GetBufferSize(), nullptr, &line_vs_);
  dev->CreatePixelShader(lps->GetBufferPointer(), lps->GetBufferSize(), nullptr, &line_ps_);
  D3D11_INPUT_ELEMENT_DESC mil[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  dev->CreateInputLayout(mil, 3, mvs->GetBufferPointer(), mvs->GetBufferSize(), &mesh_il_);
  D3D11_INPUT_ELEMENT_DESC lil[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  dev->CreateInputLayout(lil, 2, lvs->GetBufferPointer(), lvs->GetBufferSize(), &line_il_);
  D3D11_BUFFER_DESC cb{sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  dev->CreateBuffer(&cb, nullptr, &cb_);
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_ANISOTROPIC;
  sd.MaxAnisotropy = 8;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  dev->CreateSamplerState(&sd, &sampler_);
  // Game front faces: cross(p1 - p0, p2 - p0) points out of the face = clockwise on screen in this
  // left-handed view (D3D's default front face), so back faces are culled like in the game.
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_BACK;
  rd.DepthClipEnable = TRUE;
  dev->CreateRasterizerState(&rd, &raster_);
  rd.CullMode = D3D11_CULL_NONE;
  rd.AntialiasedLineEnable = TRUE;
  dev->CreateRasterizerState(&rd, &raster_lines_);
  D3D11_DEPTH_STENCIL_DESC dd{};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  dev->CreateDepthStencilState(&dd, &depth_);
  dd.DepthEnable = FALSE;
  dev->CreateDepthStencilState(&dd, &depth_off_);
  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  dev->CreateBlendState(&bd, &blend_);
  return mesh_il_ && line_il_;
}

int Renderer::NewSlot() {
  if (!free_textures_.empty()) {
    int id = free_textures_.back();
    free_textures_.pop_back();
    return id;
  }
  textures_.emplace_back();
  return int(textures_.size() - 1);
}

int Renderer::CreateTexture(const mapconv::Image& img) {
  int id = NewSlot();
  UpdateTexture(id, img);
  return id;
}

void Renderer::ReleaseTexture(int id) {
  if (id < 0 || size_t(id) >= textures_.size() || !textures_[size_t(id)]) return;
  textures_[size_t(id)].Reset();
  free_textures_.push_back(id);
}

int Renderer::CreateTargetTexture(int w, int h, ComPtr<ID3D11RenderTargetView>& rtv) {
  if (!dev_) return -1;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = UINT(w);
  td.Height = UINT(h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> tex;
  if (FAILED(dev_->CreateTexture2D(&td, nullptr, &tex))) return -1;
  ComPtr<ID3D11ShaderResourceView> srv;
  if (FAILED(dev_->CreateShaderResourceView(tex.Get(), nullptr, &srv)) || FAILED(dev_->CreateRenderTargetView(tex.Get(), nullptr, &rtv)))
    return -1;
  int id = NewSlot();
  textures_[size_t(id)] = srv;
  return id;
}

void Renderer::UpdateTexture(int id, const mapconv::Image& img) {
  if (!dev_ || img.w <= 0 || img.h <= 0) return;  // no device in the command-line export
  std::vector<uint32_t> rgba(size_t(img.w) * img.h);
  for (size_t i = 0; i < rgba.size(); ++i)
    rgba[i] = img.rgb[i * 3] | (img.rgb[i * 3 + 1] << 8) | (img.rgb[i * 3 + 2] << 16) | 0xFF000000u;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = UINT(img.w);
  td.Height = UINT(img.h);
  td.MipLevels = 0;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  ComPtr<ID3D11Texture2D> tex;
  if (FAILED(dev_->CreateTexture2D(&td, nullptr, &tex))) return;
  ctx_->UpdateSubresource(tex.Get(), 0, nullptr, rgba.data(), UINT(img.w * 4), 0);
  ComPtr<ID3D11ShaderResourceView> srv;
  dev_->CreateShaderResourceView(tex.Get(), nullptr, &srv);
  ctx_->GenerateMips(srv.Get());
  textures_[size_t(id)] = srv;
}

Renderer::Mesh Renderer::CreateMesh(const std::vector<MeshVertex>& v) {
  Mesh m;
  if (!dev_ || v.empty()) return m;
  D3D11_BUFFER_DESC bd{UINT(v.size() * sizeof(MeshVertex)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA sd{v.data(), 0, 0};
  dev_->CreateBuffer(&bd, &sd, &m.vb);
  m.count = UINT(v.size());
  return m;
}

void Renderer::Begin(const XMMATRIX& view_proj, const float eye[3]) {
  XMStoreFloat4x4(&view_proj_, view_proj);
  std::memcpy(eye_, eye, sizeof eye_);
  lines_.clear();
  lines_top_.clear();
  ctx_->RSSetState(raster_.Get());
  ctx_->OMSetDepthStencilState(depth_.Get(), 0);
  float bf[4] = {0, 0, 0, 0};
  ctx_->OMSetBlendState(blend_.Get(), bf, 0xFFFFFFFF);
  ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
  ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
}

void Renderer::SetConstants(const float tint[4], bool textured) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
  Constants c;
  c.view_proj = view_proj_;
  std::memcpy(c.tint, tint, sizeof c.tint);
  c.light[0] = 0.45f;
  c.light[1] = 0.8f;
  c.light[2] = -0.35f;
  c.light[3] = textured ? 1.0f : 0.0f;
  c.eye[0] = eye_[0];
  c.eye[1] = eye_[1];
  c.eye[2] = eye_[2];
  c.eye[3] = 0;
  std::memcpy(m.pData, &c, sizeof c);
  ctx_->Unmap(cb_.Get(), 0);
}

void Renderer::DrawMesh(const Mesh& mesh, int texture, const float tint[4], bool textured) {
  if (!mesh.vb) return;
  auto* srv = TextureView(texture);
  SetConstants(tint, textured && srv);
  UINT stride = sizeof(MeshVertex), off = 0;
  ctx_->IASetInputLayout(mesh_il_.Get());
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx_->IASetVertexBuffers(0, 1, mesh.vb.GetAddressOf(), &stride, &off);
  ctx_->VSSetShader(mesh_vs_.Get(), nullptr, 0);
  ctx_->PSSetShader(mesh_ps_.Get(), nullptr, 0);
  ctx_->PSSetShaderResources(0, 1, &srv);
  ctx_->Draw(mesh.count, 0);
}

void Renderer::Line(const float a[3], const float b[3], uint32_t rgba, bool on_top) {
  auto& L = on_top ? lines_top_ : lines_;
  L.push_back({{a[0], a[1], a[2]}, rgba});
  L.push_back({{b[0], b[1], b[2]}, rgba});
}

void Renderer::Box(const float mn[3], const float mx[3], uint32_t rgba, bool on_top) {
  float c[8][3];
  for (int i = 0; i < 8; ++i) {
    c[i][0] = (i & 1) ? mx[0] : mn[0];
    c[i][1] = (i & 2) ? mx[1] : mn[1];
    c[i][2] = (i & 4) ? mx[2] : mn[2];
  }
  const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (auto& x : e) Line(c[x[0]], c[x[1]], rgba, on_top);
}

void Renderer::FlushLines(std::vector<LineVertex>& lines, bool on_top) {
  if (lines.empty()) return;
  if (lines.size() > line_capacity_) {
    line_capacity_ = UINT(lines.size() * 2);
    D3D11_BUFFER_DESC bd{UINT(line_capacity_ * sizeof(LineVertex)), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER,
                         D3D11_CPU_ACCESS_WRITE, 0, 0};
    line_vb_.Reset();
    dev_->CreateBuffer(&bd, nullptr, &line_vb_);
  }
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx_->Map(line_vb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
  std::memcpy(m.pData, lines.data(), lines.size() * sizeof(LineVertex));
  ctx_->Unmap(line_vb_.Get(), 0);
  float white[4] = {1, 1, 1, 1};
  SetConstants(white, false);
  UINT stride = sizeof(LineVertex), off = 0;
  ctx_->RSSetState(raster_lines_.Get());
  ctx_->OMSetDepthStencilState(on_top ? depth_off_.Get() : depth_.Get(), 0);
  ctx_->IASetInputLayout(line_il_.Get());
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
  ctx_->IASetVertexBuffers(0, 1, line_vb_.GetAddressOf(), &stride, &off);
  ctx_->VSSetShader(line_vs_.Get(), nullptr, 0);
  ctx_->PSSetShader(line_ps_.Get(), nullptr, 0);
  ctx_->Draw(UINT(lines.size()), 0);
}

void Renderer::End() {
  FlushLines(lines_, false);
  FlushLines(lines_top_, true);
  ctx_->RSSetState(raster_.Get());
  ctx_->OMSetDepthStencilState(depth_.Get(), 0);
}

}  // namespace editor
