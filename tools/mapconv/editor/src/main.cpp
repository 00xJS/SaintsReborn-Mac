// Saints Reborn Map Editor: window, Direct3D 11 device, Dear ImGui set-up and the main loop.
//
// Command line:
//   SaintsRebornMapEditor.exe [map.srmap]
//   SaintsRebornMapEditor.exe --test-screenshot out.png [map.srmap]   (renders a few frames hidden, saves a PNG)
//   SaintsRebornMapEditor.exe --test-export <maps folder> [map.srmap] (exports without a window, writes a report)
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <string>
#include <vector>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "ImGuizmo.h"
#include "textures.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fs = std::filesystem;
using namespace editor;

namespace {

ComPtr<ID3D11RenderTargetView> g_rtv_srgb;  // the 3D view is lit in linear space; the UI draws to App::rtv (plain)
std::string g_imgui_ini;

void CreateTargets(App& a) {
  a.rtv.Reset();
  g_rtv_srgb.Reset();
  a.dsv.Reset();
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(a.swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
  D3D11_TEXTURE2D_DESC bd;
  back->GetDesc(&bd);
  a.width = int(bd.Width);
  a.height = int(bd.Height);
  a.dev->CreateRenderTargetView(back.Get(), nullptr, &a.rtv);
  D3D11_RENDER_TARGET_VIEW_DESC rd{};
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  a.dev->CreateRenderTargetView(back.Get(), &rd, &g_rtv_srgb);
  D3D11_TEXTURE2D_DESC dd{};
  dd.Width = bd.Width;
  dd.Height = bd.Height;
  dd.MipLevels = 1;
  dd.ArraySize = 1;
  dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  dd.SampleDesc.Count = 1;
  dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ComPtr<ID3D11Texture2D> depth;
  if (SUCCEEDED(a.dev->CreateTexture2D(&dd, nullptr, &depth))) a.dev->CreateDepthStencilView(depth.Get(), nullptr, &a.dsv);
}

void Resize(App& a, UINT w, UINT h) {
  if (!a.swap || w == 0 || h == 0) return;
  a.ctx->OMSetRenderTargets(0, nullptr, nullptr);
  a.rtv.Reset();
  g_rtv_srgb.Reset();
  a.dsv.Reset();
  a.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
  CreateTargets(a);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
  App* a = g_app;
  switch (msg) {
    case WM_SIZE:
      if (a && wp != SIZE_MINIMIZED) Resize(*a, LOWORD(lp), HIWORD(lp));
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize = {800, 500};
      return 0;
    }
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wp);
      wchar_t buf[MAX_PATH * 4];
      if (a && DragQueryFileW(drop, 0, buf, UINT(std::size(buf)))) {
        fs::path p(buf);
        std::wstring ext = p.extension().wstring();
        for (auto& c : ext) c = wchar_t(towlower(c));
        if (ext == L".srmap") {
          if (a->ConfirmDiscard()) a->Open(p);
        } else {
          a->status = "Drop a .srmap file to open it (images go in through the Textures panel)";
        }
      }
      DragFinish(drop);
      return 0;
    }
    case WM_SYSCOMMAND:
      if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // Alt is used for orbiting
      break;
    case WM_CLOSE:
      if (a && !a->ConfirmDiscard()) return 0;
      if (a) {
        a->SaveSettings();
        a->link.Stop();
      }
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

bool CreateDevice(App& a, bool hidden) {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Width = UINT(a.width);
  sd.BufferDesc.Height = UINT(a.height);
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = a.hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  HRESULT hr = E_FAIL;
  for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &a.swap, &a.dev, &got, &a.ctx);
    if (SUCCEEDED(hr)) break;
  }
  (void)hidden;
  if (FAILED(hr)) return false;
  // stop DXGI's own Alt+Enter handling (Alt is an editor key)
  ComPtr<IDXGIFactory> factory;
  if (SUCCEEDED(a.swap->GetParent(IID_PPV_ARGS(&factory)))) factory->MakeWindowAssociation(a.hwnd, DXGI_MWA_NO_ALT_ENTER);
  CreateTargets(a);
  return true;
}

void SetupStyle(float scale) {
  ImGuiStyle& s = ImGui::GetStyle();
  ImGui::StyleColorsDark(&s);
  s.WindowRounding = 6;
  s.FrameRounding = 4;
  s.GrabRounding = 4;
  s.TabRounding = 4;
  s.PopupRounding = 4;
  s.ScrollbarRounding = 6;
  s.WindowBorderSize = 1;
  s.FramePadding = ImVec2(8, 5);
  s.ItemSpacing = ImVec2(8, 6);
  ImVec4* c = s.Colors;
  const ImVec4 purple(0.56f, 0.25f, 0.85f, 1.0f), purple_hi(0.68f, 0.38f, 0.95f, 1.0f), purple_lo(0.36f, 0.16f, 0.55f, 1.0f);
  c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.08f, 0.11f, 0.96f);
  c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.09f, 0.13f, 0.98f);
  c[ImGuiCol_MenuBarBg] = ImVec4(0.12f, 0.10f, 0.15f, 1.0f);
  c[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.10f, 0.15f, 1.0f);
  c[ImGuiCol_TitleBgActive] = purple_lo;
  c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.15f, 0.21f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.20f, 0.32f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.22f, 0.40f, 1.0f);
  c[ImGuiCol_Button] = purple_lo;
  c[ImGuiCol_ButtonHovered] = purple;
  c[ImGuiCol_ButtonActive] = purple_hi;
  c[ImGuiCol_Header] = purple_lo;
  c[ImGuiCol_HeaderHovered] = purple;
  c[ImGuiCol_HeaderActive] = purple_hi;
  c[ImGuiCol_CheckMark] = purple_hi;
  c[ImGuiCol_SliderGrab] = purple;
  c[ImGuiCol_SliderGrabActive] = purple_hi;
  c[ImGuiCol_Tab] = purple_lo;
  c[ImGuiCol_TabHovered] = purple;
  c[ImGuiCol_TabSelected] = purple;
  c[ImGuiCol_SeparatorHovered] = purple;
  c[ImGuiCol_ResizeGrip] = purple_lo;
  c[ImGuiCol_ResizeGripHovered] = purple;
  c[ImGuiCol_TextSelectedBg] = ImVec4(0.56f, 0.25f, 0.85f, 0.45f);
  c[ImGuiCol_NavHighlight] = purple_hi;
  s.ScaleAllSizes(scale);
}

void LoadFonts(float scale) {
  ImGuiIO& io = ImGui::GetIO();
  wchar_t windir[MAX_PATH];
  GetWindowsDirectoryW(windir, MAX_PATH);
  fs::path font = fs::path(windir) / "Fonts" / "segoeui.ttf";
  ImFont* f = nullptr;
  if (fs::exists(font)) f = io.Fonts->AddFontFromFileTTF(font.u8string().c_str(), std::floor(17.0f * scale));
  if (!f) {
    ImFontConfig cfg;
    cfg.SizePixels = std::floor(13.0f * scale);
    io.Fonts->AddFontDefault(&cfg);
  }
}

void Frame(App& a, bool interactive) {
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  a.PumpAssets();
  a.ProcessThumbnails();
  if (interactive) a.HandleInput();
  // 3D view (linear lighting into the sRGB view of the back buffer)
  const float sky[4] = {0.42f, 0.47f, 0.55f, 1.0f};
  a.ctx->ClearRenderTargetView(g_rtv_srgb.Get(), sky);
  a.ctx->ClearDepthStencilView(a.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
  D3D11_VIEWPORT vp{0, 0, float(a.width), float(a.height), 0, 1};
  a.ctx->RSSetViewports(1, &vp);
  a.ctx->OMSetRenderTargets(1, g_rtv_srgb.GetAddressOf(), a.dsv.Get());
  a.RenderScene();
  // UI
  a.DrawUi();
  a.UpdateTitle();
  ImGui::Render();
  a.ctx->OMSetRenderTargets(1, a.rtv.GetAddressOf(), nullptr);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

bool SaveBackBuffer(App& a, const fs::path& out) {
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(a.swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
  D3D11_TEXTURE2D_DESC d;
  back->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(a.dev->CreateTexture2D(&d, nullptr, &staging))) return false;
  a.ctx->CopyResource(staging.Get(), back.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(a.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
  std::vector<unsigned char> px(size_t(d.Width) * d.Height * 4);
  for (UINT y = 0; y < d.Height; ++y) {
    std::memcpy(&px[size_t(y) * d.Width * 4], static_cast<unsigned char*>(m.pData) + size_t(y) * m.RowPitch, size_t(d.Width) * 4);
    for (UINT x = 0; x < d.Width; ++x) px[(size_t(y) * d.Width + x) * 4 + 3] = 255;
  }
  a.ctx->Unmap(staging.Get(), 0);
  return stbi_write_png(out.u8string().c_str(), int(d.Width), int(d.Height), 4, px.data(), int(d.Width) * 4) != 0;
}

fs::path ExeDir() {
  wchar_t buf[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return fs::path(std::wstring(buf, n)).parent_path();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> args(argv + 1, argv + argc);
  LocalFree(argv);

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // folder picker
  App app;
  g_app = &app;
  app.exe_dir = ExeDir();
  app.LoadSettings();
  SetModelSource([](const std::string& asset) -> const mapconv::MapInput* {
    ModelData* md = g_app ? g_app->Model(asset, false) : nullptr;
    return md && md->loaded ? &md->in : nullptr;
  });

  // ---- headless export test: no window, no device
  if (!args.empty() && args[0] == L"--test-export") {
    fs::path report = app.exe_dir / "SaintsRebornMapEditor_test.txt";
    std::ofstream rep(report, std::ios::trunc);
    if (args.size() < 2) {
      rep << "usage: --test-export <maps folder> [map.srmap]\n";
      return 2;
    }
    app.maps_dir = fs::path(args[1]);
    app.NewDocument();
    app.StartAssets();
    if (args.size() >= 3 && !app.Open(fs::path(args[2]))) {
      rep << "could not open the map\n";
      return 3;
    }
    std::string log = app.Export();
    rep << "game folder: " << app.game_dir.u8string() << "\n" << log;
    app.link.Stop();
    return log.find("Map pack written") != std::string::npos ? 0 : 1;
  }

  bool shot = !args.empty() && args[0] == L"--test-screenshot";
  fs::path shot_path, open_path;
  if (shot) {
    if (args.size() >= 2) shot_path = fs::path(args[1]);
    if (args.size() >= 3) open_path = fs::path(args[2]);
    if (shot_path.empty()) return 2;
  } else if (!args.empty()) {
    open_path = fs::path(args[0]);
  }

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  WNDCLASSEXW wc{sizeof wc};
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  wc.lpszClassName = L"SaintsRebornMapEditor";
  RegisterClassExW(&wc);

  float scale = 1.0f;
  {
    POINT origin{0, 0};
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = GetDpiForSystem();
    scale = shot ? 1.0f : float(dpi) / 96.0f;
    int sw = mi.rcWork.right - mi.rcWork.left, sh = mi.rcWork.bottom - mi.rcWork.top;
    app.width = shot ? 1600 : std::min(int(1600 * scale), sw - 40);
    app.height = shot ? 900 : std::min(int(940 * scale), sh - 40);
  }
  RECT wr{0, 0, app.width, app.height};
  AdjustWindowRectExForDpi(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0, shot ? 96 : GetDpiForSystem());
  app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Saints Reborn Map Editor", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, inst, nullptr);
  if (!app.hwnd) return 1;
  if (shot) {
    // the client area must match the requested size even though the window stays hidden
    SetWindowPos(app.hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (!CreateDevice(app, shot)) {
    MessageBoxW(nullptr, L"Direct3D 11 could not be started on this PC.", L"Saints Reborn Map Editor", MB_ICONERROR);
    return 1;
  }
  if (!app.r.Init(app.dev.Get(), app.ctx.Get())) {
    MessageBoxW(nullptr, L"The viewport shaders could not be compiled (d3dcompiler_47.dll missing?).", L"Saints Reborn Map Editor", MB_ICONERROR);
    return 1;
  }
  DragAcceptFiles(app.hwnd, TRUE);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // No keyboard navigation: it keeps the keyboard for the panels after a click, so Delete / W / E / R
  // would stop reaching the view. A single click on a number field types into it.
  io.ConfigDragClickToInputText = true;
  g_imgui_ini = (app.exe_dir / "SaintsRebornMapEditor_layout.ini").u8string();
  io.IniFilename = shot ? nullptr : g_imgui_ini.c_str();
  SetupStyle(scale);
  LoadFonts(scale);
  ImGui_ImplWin32_Init(app.hwnd);
  ImGui_ImplDX11_Init(app.dev.Get(), app.ctx.Get());

  for (auto& b : BuiltinTextures()) app.builtin_thumbs.push_back(app.r.CreateTexture(MakeBuiltinTexture(b.id)));
  app.NewDocument();
  app.StartAssets();
  if (!open_path.empty()) app.Open(open_path);
  if (!shot) {
    // the game folder must be chosen (and confirmed once) before anything else
    if (!app.game_dir_confirmed) app.show_folder_dialog = true;
    app.status = "Ready. Right mouse + WASD to fly, click to select, W/E/R move/rotate/scale";
  }

  int exit_code = 0;
  if (shot) {
    app.dirty = false;
    app.SyncGpu();
    if (!app.map.objects.empty()) app.sel.Set({SelItem::Obj, 0});
    app.FocusSelection();
    app.sel.Clear();
    if (app.map.objects.size() > 1) app.sel.Set({SelItem::Obj, int(app.map.objects.size()) - 1});
    // optional: SR_EDITOR_TAB = asset browser tab to show; wait for the game assets it needs
    if (std::getenv("SR_EDITOR_FOLDER")) app.show_folder_dialog = true;
    if (const char* tab = std::getenv("SR_EDITOR_TAB")) app.asset_tab_request = std::atoi(tab);
    if (const char* sel = std::getenv("SR_EDITOR_SELECT")) {
      app.sel.Clear();
      for (const char* c = sel; *c;) {
        app.sel.Add({SelItem::Obj, std::atoi(c)});
        while (*c && *c != ',') ++c;
        if (*c) ++c;
      }
    }
    DWORD start = GetTickCount();
    for (int i = 0;; ++i) {
      Frame(app, false);
      bool models_done = true;
      for (auto& [n, md] : app.models)
        if ((!md.loaded && !md.failed) || (md.loaded && md.thumb_wanted && md.thumb < 0)) models_done = false;
      bool waiting = app.link.state() == AssetLink::State::Starting || app.link.Busy() > 0 || !models_done;
      if (i >= 4 && (!waiting || GetTickCount() - start > 90000)) break;
      app.swap->Present(0, 0);
      if (waiting) Sleep(30);
    }
    exit_code = SaveBackBuffer(app, shot_path) ? 0 : 1;
    app.swap->Present(0, 0);
    app.link.Stop();
  } else {
    ShowWindow(app.hwnd, show == SW_HIDE ? SW_SHOWDEFAULT : show);
    UpdateWindow(app.hwnd);
    bool running = true;
    while (running) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) running = false;
      }
      if (!running) break;
      if (IsIconic(app.hwnd)) {
        Sleep(30);
        continue;
      }
      Frame(app, true);
      app.swap->Present(1, 0);
    }
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  g_app = nullptr;
  return exit_code;
}
