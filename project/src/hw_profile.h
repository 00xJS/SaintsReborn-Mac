// Hardware profile (hw_profile.cpp): what the PC has, read once at start-up,
// so defaults can fit weak PCs (old laptops) without changing anything on
// strong ones. Only defaults: files the player set (res_scale.txt etc.) win.
#pragma once
#include <cstdint>
#include <string>

namespace sr {

struct HwProfile {
  // Direct3D 12 capable hardware adapter the game should render on (index in
  // DXGI's EnumAdapters1 order, -1 = none found / leave it to the SDK).
  int adapter_index = -1;
  int adapter_count = 0;       // D3D12-capable hardware adapters seen
  std::string adapter_name;
  uint32_t vendor_id = 0;
  uint64_t dedicated_vram_mb = 0;
  uint64_t shared_mem_mb = 0;
  bool integrated = false;     // little or no dedicated VRAM (iGPU / APU)
  uint32_t logical_cpus = 0;
  uint32_t physical_cores = 0;
  uint64_t total_ram_mb = 0;
  // Memory the texture cache can use without spilling over into system RAM.
  uint64_t gpu_budget_mb() const;
  // "Weak" GPU: default to native (1x) internal resolution.
  bool weak_gpu() const;
};

// Detected once (cheap after the first call).
const HwProfile& GetHwProfile();

}  // namespace sr
