// Hardware profile, see hw_profile.h.
//
// The adapter choice matters on laptops with two GPUs: the SDK takes the FIRST
// Direct3D 12 adapter DXGI lists, and on hybrid laptops (Intel/AMD iGPU +
// NVIDIA/AMD dGPU) that is usually the integrated one. The NvOptimusEnablement
// / AmdPowerXpressRequestHighPerformance exports only count in the EXE (the
// SDK has them in the GPU plugin DLL, where drivers ignore them), so they are
// also exported from main.cpp, and the adapter with the most dedicated VRAM is
// picked here.

#include "hw_profile.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d12.h>
#endif

namespace sr {

uint64_t HwProfile::gpu_budget_mb() const {
  // Integrated GPUs report a small "dedicated" carve-out and use system RAM.
  if (integrated) return std::min<uint64_t>(shared_mem_mb / 2, 2048);
  return dedicated_vram_mb;
}

bool HwProfile::weak_gpu() const {
  // Test aid: a file named "force_weak_gpu" next to the exe.
  static const bool forced = [] {
    FILE* f = std::fopen("force_weak_gpu", "rb");
    if (f) std::fclose(f);
    return f != nullptr;
  }();
  if (forced) return true;
  if (adapter_index < 0) return false;  // unknown: keep the old default
  return integrated || dedicated_vram_mb < 3000;
}

namespace {

HwProfile Detect() {
  HwProfile p;
#ifdef _WIN32
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  p.logical_cpus = si.dwNumberOfProcessors;
  {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len) {
      std::string buf(len, '\0');
      auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
      if (GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len)) {
        for (DWORD off = 0; off < len;) {
          auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
          if (e->Relationship == RelationProcessorCore) ++p.physical_cores;
          if (!e->Size) break;
          off += e->Size;
        }
      }
    }
  }
  MEMORYSTATUSEX ram{};
  ram.dwLength = sizeof(ram);
  if (GlobalMemoryStatusEx(&ram)) p.total_ram_mb = ram.ullTotalPhys >> 20;

  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
  auto create_factory = dxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
  auto create_device = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
  IDXGIFactory1* factory = nullptr;
  if (create_factory && create_device &&
      SUCCEEDED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    uint64_t best_vram = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
      DXGI_ADAPTER_DESC1 d{};
      if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
          SUCCEEDED(create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
        ++p.adapter_count;
        const uint64_t vram = uint64_t(d.DedicatedVideoMemory) >> 20;
        // Most dedicated VRAM wins (the discrete GPU on hybrid laptops);
        // ties keep DXGI's order.
        if (p.adapter_index < 0 || vram > best_vram) {
          best_vram = vram;
          p.adapter_index = int(i);
          p.vendor_id = d.VendorId;
          p.dedicated_vram_mb = vram;
          p.shared_mem_mb = uint64_t(d.SharedSystemMemory) >> 20;
          char name[256] = {};
          WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
          p.adapter_name = name;
        }
      }
      adapter->Release();
      adapter = nullptr;
    }
    factory->Release();
  }
  // "Dedicated" memory under 512 MB = a carve-out of system RAM (iGPU / APU).
  p.integrated = p.adapter_index >= 0 && p.dedicated_vram_mb < 512;
#endif
  return p;
}

}  // namespace

const HwProfile& GetHwProfile() {
  static HwProfile profile;
  static std::once_flag once;
  std::call_once(once, [] { profile = Detect(); });
  return profile;
}

}  // namespace sr
