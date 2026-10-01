#include "world_studio_bridge.h"

#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <windows.h>

#include <rex/logging.h>
#include <rex/audio/audio_system.h>
#include <rex/chrono/clock.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include "wml/packfile.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace sr {
extern std::atomic<bool> g_studio_menu_active;  // kbm.cpp
}

namespace sr::world_studio {
namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\SaintsReborn.WorldStudio.v1";
constexpr uint32_t kPlayerPointer = 0x8309ABECu;
constexpr uint32_t kObjectTable = 0x830866C8u;
constexpr uint32_t kObjectSlots = 4096;
constexpr uint32_t kCamera = 0x827D9778u;
constexpr uint32_t kHudVisible = 0x827AEDF2u;
constexpr uint32_t kCutscenePlaying = 0x8370D991u;
constexpr uint32_t kScriptedCutscenePlaying = 0x8370D990u;

struct Vec3 { float x = 0, y = 0, z = 0; };
struct Command {
  enum class Kind { kSetTransform, kSetCamera } kind{};
  uint32_t address = 0;
  uint32_t handle = 0;
  Vec3 position{};
  float fov = 0;
};

std::once_flag g_start_once;
std::atomic<bool> g_client_connected{false};
std::atomic<bool> g_paused{false};
std::atomic<unsigned> g_steps{0};
std::atomic<uint64_t> g_frame{0};
std::mutex g_commands_mutex;
std::vector<Command> g_commands;
std::mutex g_snapshot_mutex;
std::string g_snapshot = R"({"type":"snapshot","protocol":1,"ready":false})";
bool g_last_cutscene = false;
std::chrono::steady_clock::time_point g_cutscene_started{};
bool g_editor_camera_initialized = false;
bool g_editor_free_camera = true;
bool g_f1_down = false;
Vec3 g_editor_camera_position{};
float g_editor_camera_yaw = 0;
float g_editor_camera_pitch = 0;
std::chrono::steady_clock::time_point g_editor_camera_tick{};
enum class Drag { kNone, kLook, kPan, kOrbit, kZoom };
Drag g_drag = Drag::kNone;
POINT g_drag_anchor{};
float g_fly_speed = 12.0f;
float g_orbit_distance = 10.0f;
std::atomic<float> g_wheel{0.0f};
// Selection and move gizmo (drawn by the overlay, see GetGizmoDraw).
uint32_t g_sel_address = 0, g_sel_handle = 0;
int g_axis_hot = -1, g_axis_drag = -1;
bool g_lmb_was = false, g_pause_key_was = false;
POINT g_last_cursor{};
std::mutex g_gizmo_mutex;
GizmoDraw g_gizmo_draw{};
std::chrono::steady_clock::time_point g_gizmo_time{};
std::atomic<uint32_t> g_pending_select_address{0}, g_pending_select_handle{0};
std::atomic<bool> g_pending_select{false};
// Cutscene requests from the Studio (pipe thread -> game thread).
struct CutsceneRequest { int kind = 0; std::string name, archive; bool edited = false; };  // 1 play, 2 stop, 3 revert
std::mutex g_cutscene_mutex;
std::vector<CutsceneRequest> g_cutscene_requests;
std::string g_cutscene_now;      // name of the last cutscene the game started (game thread)
std::mutex g_cutscene_name_mutex;
// Guest context of the game thread during OnPresent, for calling game code.
PPCContext* g_ctx = nullptr;
Vec3 g_cam_forward{0, 0, 1}, g_cam_right{1, 0, 0}, g_cam_up{0, 1, 0};
Vec3 g_gizmo_eye{};                    // camera position the gizmo projects from
std::atomic<bool> g_play_mode{false};  // hold V: play as the player (Studio camera off)
std::atomic<uint32_t> g_fullscreen_seq{0};  // double-clicks in the game view
double g_cutscene_time = 0;            // game time since the cutscene started (s)
bool g_cutscene_was_active = false;
bool g_camera_owned = false;
// Clipboard (Ctrl+C / Ctrl+V / Ctrl+D) and key edges.
struct Clip { uint32_t type = 0; int32_t vehicle_type = -1; std::string model; Vec3 pos{}; float rows[9]{}; };
Clip g_clip;
bool g_key_was[256] = {};

bool Enabled() {
  // World Studio is a developer-only, opt-in facility. The test launcher sets
  // this before starting the game, leaving normal SaintsReborn launches with
  // no pipe thread or per-frame editor work.
  static const bool enabled = [] {
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(
        L"SAINTSREBORN_WORLD_STUDIO", value, DWORD(sizeof(value) / sizeof(value[0])));
    return length != 0 && !(length == 1 && value[0] == L'0');
  }();
  return enabled;
}

bool HostEnvironmentEnabled() {
  static const bool enabled = [] {
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(
        L"SAINTSREBORN_EDITOR_HOST", value, DWORD(sizeof(value) / sizeof(value[0])));
    return length != 0 && !(length == 1 && value[0] == L'0');
  }();
  return enabled;
}

uint8_t* GuestPtr(uint8_t* base, uint32_t address) {
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
}

bool Readable(uint8_t* base, uint32_t address, size_t length) {
  if (!address || uint64_t(address) + length > UINT32_MAX) return false;
  uintptr_t current = reinterpret_cast<uintptr_t>(GuestPtr(base, address));
  const uintptr_t end = current + length;
  while (current < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(current), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (next <= current) return false;
    current = next;
  }
  return true;
}

float ReadF32(uint8_t* base, uint32_t address) {
  const uint32_t bits = PPC_LOAD_U32(address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void WriteF32(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  PPC_STORE_U32(address, bits);
}

Vec3 ReadVec(uint8_t* base, uint32_t address) {
  return {ReadF32(base, address), ReadF32(base, address + 4), ReadF32(base, address + 8)};
}

void WriteVec(uint8_t* base, uint32_t address, const Vec3& value) {
  WriteF32(base, address, value.x);
  WriteF32(base, address + 4, value.y);
  WriteF32(base, address + 8, value.z);
}

bool Finite(const Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
         std::abs(value.x) < 1000000 && std::abs(value.y) < 1000000 &&
         std::abs(value.z) < 1000000;
}

void AppendVec(std::ostringstream& out, const Vec3& value) {
  out << '[' << value.x << ',' << value.y << ',' << value.z << ']';
}

bool ParseBool(const std::string& line, const char* key, bool& value) {
  const auto at = line.find(key);
  if (at == std::string::npos) return false;
  const auto colon = line.find(':', at);
  if (colon == std::string::npos) return false;
  const auto token = line.find_first_not_of(" \t", colon + 1);
  if (token == std::string::npos) return false;
  if (line.compare(token, 4, "true") == 0) { value = true; return true; }
  if (line.compare(token, 5, "false") == 0) { value = false; return true; }
  return false;
}

bool ParseNumber(const std::string& line, const char* key, double& value) {
  const auto at = line.find(key);
  if (at == std::string::npos) return false;
  const auto colon = line.find(':', at);
  if (colon == std::string::npos) return false;
  const char* begin = line.data() + colon + 1;
  char* end = nullptr;
  value = std::strtod(begin, &end);
  return end != begin && std::isfinite(value);
}

bool ParseVec(const std::string& line, const char* key, Vec3& value) {
  const auto at = line.find(key);
  if (at == std::string::npos) return false;
  const auto open = line.find('[', at);
  if (open == std::string::npos) return false;
  const char* cursor = line.data() + open + 1;
  char* end = nullptr;
  value.x = float(std::strtod(cursor, &end)); if (end == cursor) return false;
  cursor = end + 1;
  value.y = float(std::strtod(cursor, &end)); if (end == cursor) return false;
  cursor = end + 1;
  value.z = float(std::strtod(cursor, &end)); if (end == cursor) return false;
  return Finite(value);
}

bool ParseString(const std::string& line, const char* key, std::string& out) {
  const size_t k = line.find(key);
  if (k == std::string::npos) return false;
  const size_t colon = line.find(':', k + std::strlen(key));
  const size_t q = line.find('"', colon);
  const size_t e = q == std::string::npos ? q : line.find('"', q + 1);
  if (colon == std::string::npos || q == std::string::npos || e == std::string::npos) return false;
  out = line.substr(q + 1, e - q - 1);
  return true;
}

void QueueLine(const std::string& line) {
  for (const auto& [text, kind] : {std::pair{R"("command":"cutscene_play")", 1}, std::pair{R"("command":"cutscene_stop")", 2},
                                   std::pair{R"("command":"cutscene_revert")", 3}}) {
    if (line.find(text) == std::string::npos) continue;
    CutsceneRequest request;
    request.kind = kind;
    ParseString(line, R"("name")", request.name);
    ParseString(line, R"("archive")", request.archive);
    ParseBool(line, R"("edited")", request.edited);
    for (const char ch : request.name + request.archive)
      if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.' || ch == '-')) return;
    std::lock_guard lock(g_cutscene_mutex);
    g_cutscene_requests.push_back(request);
    return;
  }
  if (line.find(R"("command":"pause")") != std::string::npos) {
    bool value = false;
    if (ParseBool(line, R"("value")", value)) g_paused.store(value);
    return;
  }
  if (line.find(R"("command":"select")") != std::string::npos) {
    double address = 0, handle = 0;
    if (!ParseNumber(line, R"("address")", address) || !ParseNumber(line, R"("handle")", handle)) return;
    g_pending_select_address.store(uint32_t(address));
    g_pending_select_handle.store(uint32_t(handle));
    g_pending_select.store(true);
    return;
  }
  if (line.find(R"("command":"step")") != std::string::npos) {
    g_paused.store(true);
    g_steps.fetch_add(1);
    return;
  }

  Command command;
  if (line.find(R"("command":"set_transform")") != std::string::npos) {
    command.kind = Command::Kind::kSetTransform;
    double address = 0, handle = 0;
    if (!ParseNumber(line, R"("address")", address) ||
        !ParseNumber(line, R"("handle")", handle) ||
        !ParseVec(line, R"("position")", command.position)) return;
    command.address = uint32_t(address);
    command.handle = uint32_t(handle);
  } else if (line.find(R"("command":"set_camera")") != std::string::npos) {
    command.kind = Command::Kind::kSetCamera;
    double fov = 0;
    if (!ParseVec(line, R"("position")", command.position) ||
        !ParseNumber(line, R"("fov")", fov)) return;
    command.fov = float(fov);
  } else {
    return;
  }
  std::lock_guard lock(g_commands_mutex);
  g_commands.push_back(command);
}

void PipeThread() {
  for (;;) {
    HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 1 << 20, 1 << 16, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
      CloseHandle(pipe); continue;
    }
    g_client_connected.store(true, std::memory_order_release);
    std::string incoming;
    for (;;) {
      DWORD available = 0;
      if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
      if (available) {
        char buffer[4096];
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr)) break;
        incoming.append(buffer, read);
        for (size_t newline; (newline = incoming.find('\n')) != std::string::npos;) {
          QueueLine(incoming.substr(0, newline));
          incoming.erase(0, newline + 1);
        }
      }
      std::string snapshot;
      {
        std::lock_guard lock(g_snapshot_mutex);
        snapshot = g_snapshot;
      }
      snapshot.push_back('\n');
      DWORD written = 0;
      if (!WriteFile(pipe, snapshot.data(), DWORD(snapshot.size()), &written, nullptr)) break;
      Sleep(50);
    }
    g_client_connected.store(false, std::memory_order_release);
    g_paused.store(false, std::memory_order_relaxed);
    g_steps.store(0, std::memory_order_relaxed);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}

void Start() {
  std::call_once(g_start_once, [] { std::thread(PipeThread).detach(); });
}

bool IsCurrentObject(uint8_t* base, uint32_t address, uint32_t handle) {
  const uint32_t slot = handle & 0xffffu;
  if (slot >= kObjectSlots) return false;
  return PPC_LOAD_U32(kObjectTable + 12 + slot * 16) == address &&
         Readable(base, address, 80) && PPC_LOAD_U32(address + 68) == handle;
}

// ---------------------------------------------------------------------------
// Map props. Loaded city chunks: list 0x829A97E8 (+0 count, +16 pointers).
// Chunk +8 slot, +212 static piece count, +216 static records (80 bytes: box
// min +0, max +16, flags +32, mesh +36, instance +60), +404 mover count, +408
// mover records (96 bytes: +8 id, +12 position, +24 3x3 rotation, +60 scale,
// +88 flags), +412 mover runtime (20 bytes: +0 physics object index into the
// pointer table 0x8315FD48, count 0x837100A8). Movers are the props physics
// can knock around (benches, bins, lamp posts...); static pieces are the
// chunk's baked geometry.
// ---------------------------------------------------------------------------
constexpr uint32_t kChunkList = 0x829A97E8u;
constexpr uint32_t kMoverObjects = 0x8315FD48u, kMoverObjectCount = 0x837100A8u;

struct PropSel { int kind = 0; uint32_t chunk = 0; uint32_t index = 0; };  // 1 mover, 2 static
PropSel g_prop;

bool ChunkLoaded(uint8_t* base, uint32_t chunk) {
  const uint32_t count = PPC_LOAD_U32(kChunkList);
  if (count > 512) return false;
  for (uint32_t i = 0; i < count; ++i)
    if (PPC_LOAD_U32(kChunkList + 16 + i * 4) == chunk) return true;
  return false;
}

bool PropValid(uint8_t* base, const PropSel& p) {
  if (!p.kind || !ChunkLoaded(base, p.chunk) || !Readable(base, p.chunk, 416)) return false;
  if (p.kind == 1) return p.index < PPC_LOAD_U32(p.chunk + 404) && Readable(base, PPC_LOAD_U32(p.chunk + 408) + p.index * 96, 96);
  return p.index < PPC_LOAD_U32(p.chunk + 212) && Readable(base, PPC_LOAD_U32(p.chunk + 216) + p.index * 80, 80);
}

uint32_t PropRecord(uint8_t* base, const PropSel& p) {
  return p.kind == 1 ? PPC_LOAD_U32(p.chunk + 408) + p.index * 96 : PPC_LOAD_U32(p.chunk + 216) + p.index * 80;
}

Vec3 PropPosition(uint8_t* base, const PropSel& p) {
  const uint32_t rec = PropRecord(base, p);
  if (p.kind == 1) return ReadVec(base, rec + 12);
  const Vec3 lo = ReadVec(base, rec), hi = ReadVec(base, rec + 16);
  return {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
}

void LogWords(const char* label, uint8_t* base, uint32_t address, uint32_t bytes) {
  if (!Readable(base, address, bytes)) { REXLOG_INFO("World Studio: {} @{:08X} not readable", label, address); return; }
  for (uint32_t off = 0; off < bytes; off += 32) {
    std::string hex, flt;
    char item[40];
    for (uint32_t i = off; i < std::min(bytes, off + 32); i += 4) {
      const uint32_t w = PPC_LOAD_U32(address + i);
      std::snprintf(item, sizeof(item), "%08X ", w); hex += item;
      float f; std::memcpy(&f, &w, 4);
      if (std::isfinite(f) && std::abs(f) < 1e6f && (std::abs(f) > 1e-4f || f == 0)) std::snprintf(item, sizeof(item), "%.3f ", f);
      else std::snprintf(item, sizeof(item), "- ");
      flt += item;
    }
    REXLOG_INFO("World Studio: {} +{:03X}: {}| {}", label, off, hex, flt);
  }
}

std::string ChunkName(uint8_t* base, uint32_t chunk) {
  // Not known yet: log the slot and address.
  char text[48];
  std::snprintf(text, sizeof(text), "chunk %08X slot %u", chunk, PPC_LOAD_U8(chunk + 8));
  return text;
}

void LogProp(uint8_t* base, const PropSel& p) {
  const uint32_t rec = PropRecord(base, p);
  const Vec3 pos = PropPosition(base, p);
  REXLOG_INFO("World Studio: selected {} {} of {} ({}) at {:.1f} {:.1f} {:.1f}", p.kind == 1 ? "mover" : "static piece",
              p.index, PPC_LOAD_U32(p.chunk + (p.kind == 1 ? 404 : 212)), ChunkName(base, p.chunk), pos.x, pos.y, pos.z);
  static int dumps = 0;
  if (dumps++ >= 40) return;
  LogWords(p.kind == 1 ? "mover record" : "static record", base, rec, p.kind == 1 ? 96 : 80);
  if (p.kind == 1) {
    const uint32_t runtime = PPC_LOAD_U32(p.chunk + 412) + p.index * 20;
    LogWords("mover runtime", base, runtime, 20);
    const int32_t object_index = int32_t(PPC_LOAD_U32(runtime));
    if (object_index >= 0 && object_index < int32_t(PPC_LOAD_U32(kMoverObjectCount))) {
      const uint32_t object = PPC_LOAD_U32(kMoverObjects + uint32_t(object_index) * 4);
      REXLOG_INFO("World Studio: mover physics object {} = {:08X}", object_index, object);
      LogWords("mover object", base, object, 352);
    }
  } else {
    const uint32_t instance = PPC_LOAD_U32(rec + 60);
    LogWords("static instance", base, instance, 96);
    LogWords("static mesh", base, PPC_LOAD_U32(rec + 36), 64);
  }
}

// Ray (from the camera through the cursor) against an axis-aligned box.
bool RayBox(const Vec3& o, const Vec3& d, const Vec3& lo, const Vec3& hi, float& t_hit) {
  float t0 = 0.0f, t1 = 1e9f;
  const float os[3] = {o.x, o.y, o.z}, ds[3] = {d.x, d.y, d.z}, los[3] = {lo.x, lo.y, lo.z}, his[3] = {hi.x, hi.y, hi.z};
  for (int a = 0; a < 3; ++a) {
    if (std::abs(ds[a]) < 1e-8f) { if (os[a] < los[a] || os[a] > his[a]) return false; continue; }
    float ta = (los[a] - os[a]) / ds[a], tb = (his[a] - os[a]) / ds[a];
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta); t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  t_hit = t0;
  return true;
}

// Picks a mover (screen distance to its position) or, failing that, the
// smallest static piece whose box the cursor ray passes through (big boxes
// are whole blocks; small ones are single props).
PropSel PickProp(uint8_t* base, const Vec3& eye, const Vec3& ray, float mx, float my,
                 const std::function<bool(const Vec3&, float&, float&)>& project, float focal, const Vec3& forward,
                 float& out_score) {
  PropSel best_sel;
  float best = 1e9f;
  const uint32_t count = PPC_LOAD_U32(kChunkList);
  if (count > 512) return best_sel;
  for (uint32_t c = 0; c < count; ++c) {
    const uint32_t chunk = PPC_LOAD_U32(kChunkList + 16 + c * 4);
    if (!Readable(base, chunk, 416)) continue;
    const uint32_t movers = PPC_LOAD_U32(chunk + 404), mover_records = PPC_LOAD_U32(chunk + 408);
    if (movers && movers < 8192 && Readable(base, mover_records, movers * 96)) {
      for (uint32_t i = 0; i < movers; ++i) {
        const Vec3 p = ReadVec(base, mover_records + i * 96 + 12);
        if (!Finite(p)) continue;
        float sx, sy;
        if (!project(p, sx, sy)) continue;
        const Vec3 d{p.x - eye.x, p.y - eye.y, p.z - eye.z};
        const float depth = std::max(0.1f, d.x * forward.x + d.y * forward.y + d.z * forward.z);
        if (depth > 300.0f) continue;
        const float screen_radius = std::max(16.0f, 0.8f * focal / depth);
        const float dist = std::sqrt((sx - mx) * (sx - mx) + (sy - my) * (sy - my));
        if (dist > screen_radius) continue;
        const float score = dist / screen_radius + depth * 0.002f;
        if (score < best) { best = score; best_sel = {1, chunk, i}; }
      }
    }
  }
  if (best_sel.kind) { out_score = best; return best_sel; }
  float best_volume = 1e18f;
  for (uint32_t c = 0; c < count; ++c) {
    const uint32_t chunk = PPC_LOAD_U32(kChunkList + 16 + c * 4);
    if (!Readable(base, chunk, 416)) continue;
    const uint32_t pieces = PPC_LOAD_U32(chunk + 212), records = PPC_LOAD_U32(chunk + 216);
    if (!pieces || pieces > 65536 || !Readable(base, records, pieces * 80)) continue;
    for (uint32_t i = 0; i < pieces; ++i) {
      const Vec3 lo = ReadVec(base, records + i * 80), hi = ReadVec(base, records + i * 80 + 16);
      if (!Finite(lo) || !Finite(hi) || hi.x < lo.x || hi.y < lo.y || hi.z < lo.z) continue;
      float t;
      if (!RayBox(eye, ray, lo, hi, t) || t > 400.0f) continue;
      const float volume = (hi.x - lo.x + 0.1f) * (hi.y - lo.y + 0.1f) * (hi.z - lo.z + 0.1f);
      if (volume < best_volume) { best_volume = volume; best_sel = {2, chunk, i}; }
    }
  }
  out_score = best_sel.kind ? 2.0f : 1e9f;
  return best_sel;
}

// How many static records of the chunk use this record's instance (the low
// detail city pieces share one identity instance; props have their own).
uint32_t InstanceUsers(uint8_t* base, const PropSel& p) {
  const uint32_t records = PPC_LOAD_U32(p.chunk + 216), count = PPC_LOAD_U32(p.chunk + 212);
  const uint32_t instance = PPC_LOAD_U32(records + p.index * 80 + 60);
  uint32_t users = 0;
  for (uint32_t i = 0; i < count && i < 65536; ++i) users += PPC_LOAD_U32(records + i * 80 + 60) == instance;
  return users;
}

// Static piece: the drawn mesh uses its instance (+4 position, +16 3x3
// rotation); its record box (+0/+16) is used for visibility, moved along.
void MoveProp(uint8_t* base, const PropSel& p, const Vec3& to) {
  const uint32_t rec = PropRecord(base, p);
  if (p.kind == 1) {
    WriteVec(base, rec + 12, to);
    return;
  }
  const Vec3 c = PropPosition(base, p);
  const Vec3 d{to.x - c.x, to.y - c.y, to.z - c.z};
  Vec3 lo = ReadVec(base, rec), hi = ReadVec(base, rec + 16);
  WriteVec(base, rec, {lo.x + d.x, lo.y + d.y, lo.z + d.z});
  WriteVec(base, rec + 16, {hi.x + d.x, hi.y + d.y, hi.z + d.z});
  const uint32_t instance = PPC_LOAD_U32(rec + 60);
  if (!Readable(base, instance, 96)) return;
  static uint32_t warned = 0;
  if (warned != instance) {
    warned = instance;
    const uint32_t users = InstanceUsers(base, p);
    if (users > 1) REXLOG_INFO("World Studio: this piece shares its transform with {} others (they move too)", users - 1);
  }
  const Vec3 ip = ReadVec(base, instance + 4);
  WriteVec(base, instance + 4, {ip.x + d.x, ip.y + d.y, ip.z + d.z});
}

// Delete for a static piece: its rotation is scaled to nothing (invisible;
// collision stays for now). Kept so the same session could restore it.
void HideProp(uint8_t* base, const PropSel& p) {
  const uint32_t rec = PropRecord(base, p);
  if (p.kind == 1) {  // movers: drop far below the map
    const Vec3 q = ReadVec(base, rec + 12);
    WriteVec(base, rec + 12, {q.x, q.y - 2000.0f, q.z});
    return;
  }
  const uint32_t instance = PPC_LOAD_U32(rec + 60);
  if (!Readable(base, instance, 96) || InstanceUsers(base, p) > 1) {
    REXLOG_INFO("World Studio: piece not deleted (shared transform)");
    return;
  }
  for (int i = 0; i < 9; ++i) WriteF32(base, instance + 16 + i * 4, 0.0f);
  WriteVec(base, rec, {0, -5000, 0});
  WriteVec(base, rec + 16, {0, -5000, 0});
}

// ---------------------------------------------------------------------------
// Game-object editing through the game's own functions (same calls the co-op
// mod uses): humans are moved with the character teleport 8243E6F8 (position,
// controller, nav node), vehicles with 82535C68 (body and physics), other
// objects with a physics body index at +784 through the physics setters
// 822679F8 (matrix) / 822678F0 (position). Anything else gets +20 written.
// ---------------------------------------------------------------------------
constexpr uint32_t kStreamTable = 0x83E876B0u;  // streaming groups, 204 bytes
constexpr uint32_t kVehicleInfo = 0x83E8ABD8u, kVehicleInfoCount = 0x8371023Cu, kVehicleInfoSize = 1196;
constexpr uint32_t kPhysicsBodies = 0x839AB550u, kPhysicsBodySize = 5968;

}  // namespace
}  // namespace sr::world_studio

extern "C" {
void sub_8243E6F8(PPCContext& ctx, uint8_t* base);  // teleport human (obj, pos, 0, nav node / -1)
void sub_82535C68(PPCContext& ctx, uint8_t* base);  // place vehicle (veh, pos, matrix, 1)
void sub_822679F8(PPCContext& ctx, uint8_t* base);  // physics body set matrix (index, matrix)
void sub_822678F0(PPCContext& ctx, uint8_t* base);  // physics body set position (index, pos)
void sub_823ACF50(PPCContext& ctx, uint8_t* base);  // destroy object (handle)
void sub_82569700(PPCContext& ctx, uint8_t* base);  // create vehicle
void sub_8250C750(PPCContext& ctx, uint8_t* base);  // streaming request
void sub_824C2CF8(PPCContext& ctx, uint8_t* base);  // clear a spot of people
void sub_82479AB0(PPCContext& ctx, uint8_t* base);  // create human
void sub_82114928(PPCContext& ctx, uint8_t* base);  // character definition by name
void sub_8243E948(PPCContext& ctx, uint8_t* base);  // character controller velocity (obj, vec)
void sub_8243F260(PPCContext& ctx, uint8_t* base);  // character un-hide helper (obj, pos, 1)
}
namespace wml {
using HostFunction = void (*)(PPCContext& ctx, uint8_t* base);
HostFunction FindFunction(uint32_t guest_address);
}

namespace sr::world_studio {
namespace {

// A copy of the game thread's context with its own stack frame 8 KB below
// the current one; the space between is scratch guest memory for arguments.
struct Guest {
  PPCContext c;
  uint8_t* base;
  uint32_t data;
  explicit Guest(uint8_t* b) : c(*g_ctx), base(b) {
    const uint32_t frame = (g_ctx->r1.u32 - 0x2000u) & ~0xFu;
    c.r1.u64 = frame;
    data = frame + 0x400;  // 0x400..0x1F00: ours (callee frames go below `frame`)
  }
};

int32_t VehicleType(uint8_t* base, uint32_t vehicle) {
  const uint32_t info = PPC_LOAD_U32(vehicle + 50324);
  const uint32_t count = PPC_LOAD_U32(kVehicleInfoCount);
  if (info < kVehicleInfo || (info - kVehicleInfo) % kVehicleInfoSize || count > 512) return -1;
  const uint32_t index = (info - kVehicleInfo) / kVehicleInfoSize;
  return index < count ? int32_t(index) : -1;
}

uint32_t PhysicsIndex(uint8_t* base, uint32_t obj) {
  if (!Readable(base, obj, 788)) return 0xFFFFFFFFu;
  const uint32_t physics = PPC_LOAD_U32(obj + 784);
  if (physics >= 4096 || !Readable(base, kPhysicsBodies + physics * kPhysicsBodySize, kPhysicsBodySize))
    return 0xFFFFFFFFu;
  return physics;
}

const char* TypeName(uint32_t type) {
  switch (type) { case 1: return "person"; case 5: return "vehicle"; default: return "object"; }
}

// Moves an object to `p` (its rotation from `rows`, or kept when null).
bool PlaceObject(uint8_t* base, uint32_t obj, const Vec3& p, const float* rows = nullptr) {
  const uint32_t type = PPC_LOAD_U32(obj + 72);
  if (!g_ctx) { WriteVec(base, obj + 20, p); return false; }
  Guest g(base);
  const uint32_t pos = g.data, mat = g.data + 16;
  WriteVec(base, pos, p);
  WriteF32(base, pos + 12, 0);
  for (int i = 0; i < 9; ++i) WriteF32(base, mat + i * 4, rows ? rows[i] : ReadF32(base, obj + 32 + i * 4));
  if (type == 1 && Readable(base, obj, 4252) && (PPC_LOAD_U32(obj + 216) & 0x4000)) {
    if (rows) for (int i = 0; i < 9; ++i) WriteF32(base, obj + 32 + i * 4, rows[i]);
    g.c.r3.u64 = obj; g.c.r4.u64 = pos; g.c.r5.u64 = 0; g.c.r6.u64 = ~0ull;
    sub_8243E6F8(g.c, base);
    return true;
  }
  if (type == 5 && Readable(base, obj, 50328)) {
    g.c.r3.u64 = obj; g.c.r4.u64 = pos; g.c.r5.u64 = mat; g.c.r6.u64 = 1;
    sub_82535C68(g.c, base);
    return true;
  }
  const uint32_t physics = PhysicsIndex(base, obj);
  if (physics != 0xFFFFFFFFu) {
    g.c.r3.u64 = physics; g.c.r4.u64 = mat;
    sub_822679F8(g.c, base);
    g.c.r3.u64 = physics; g.c.r4.u64 = pos;
    sub_822678F0(g.c, base);
    WriteVec(base, obj + 20, p);
    return true;
  }
  WriteVec(base, obj + 20, p);
  if (rows) for (int i = 0; i < 9; ++i) WriteF32(base, obj + 32 + i * 4, rows[i]);
  return false;
}

void DestroyObject(uint8_t* base, uint32_t handle) {
  if (!g_ctx) return;
  Guest g(base);
  g.c.r3.u64 = handle;
  sub_823ACF50(g.c, base);
}

std::string HumanModel(uint8_t* base, uint32_t obj) {
  const uint32_t desc = PPC_LOAD_U32(obj + 228);
  if (!Readable(base, desc, 12)) return {};
  const uint32_t name = PPC_LOAD_U32(desc + 8);
  if (!Readable(base, name, 64)) return {};
  std::string out;
  for (uint32_t i = 0; i < 63; ++i) {
    const uint8_t ch = PPC_LOAD_U8(name + i);
    if (!ch) break;
    out.push_back(char(ch));
  }
  return out;
}

void StreamReference(Guest& g, uint32_t id) {
  g.c.r3.u64 = kStreamTable + (id >> 24) * 204; g.c.r4.u64 = id;
  g.c.r5.u64 = 5; g.c.r6.u64 = 0; g.c.r7.u64 = 1;
  sub_8250C750(g.c, g.base);
}

// Makes a new object like the clipboard one at `p`; returns its handle.
uint32_t CreateLike(uint8_t* base, const Clip& clip, const Vec3& p) {
  if (!g_ctx) return 0;
  Guest g(base);
  const uint32_t pos = g.data, mat = g.data + 16, text = g.data + 64;
  WriteVec(base, pos, p);
  WriteF32(base, pos + 12, 0);
  for (int i = 0; i < 9; ++i) WriteF32(base, mat + i * 4, clip.rows[i]);
  const uint32_t sp = g.c.r1.u32;
  if (clip.type == 5 && clip.vehicle_type >= 0) {
    PPC_STORE_U32(sp + 84, 0); PPC_STORE_U8(sp + 95, 0); PPC_STORE_U32(sp + 100, 0);
    g.c.r3.u64 = 3; g.c.r4.u64 = 2; g.c.r5.u64 = uint32_t(clip.vehicle_type);
    g.c.r6.u64 = pos; g.c.r7.u64 = mat; g.c.r8.u64 = 0; g.c.r9.u64 = 0; g.c.r10.u64 = 0;
    g.c.f1.f64 = 0.0;
    sub_82569700(g.c, base);
    const uint32_t handle = g.c.r3.u32;
    const uint32_t obj = PPC_LOAD_U32(kObjectTable + 12 + (handle & 0xffffu) * 16);
    if ((handle & 0xffffu) >= kObjectSlots || !Readable(base, obj, 80) || PPC_LOAD_U32(obj + 68) != handle) return 0;
    Guest s(base);
    s.c.r1.u64 = sp;
    StreamReference(s, PPC_LOAD_U32(kVehicleInfo + uint32_t(clip.vehicle_type) * kVehicleInfoSize));
    PlaceObject(base, obj, p, clip.rows);
    return handle;
  }
  if (clip.type == 1 && !clip.model.empty()) {
    std::memcpy(GuestPtr(base, text), clip.model.c_str(), clip.model.size() + 1);
    g.c.r3.u64 = text; g.c.r4.u64 = 0;
    sub_82114928(g.c, base);
    const uint32_t definition = g.c.r3.u32;
    if (!Readable(base, definition, 612)) return 0;
    g.c.r3.u64 = pos;
    sub_824C2CF8(g.c, base);
    PPC_STORE_U32(sp + 84, 0); PPC_STORE_U16(sp + 94, 8); PPC_STORE_U8(sp + 103, 0);
    // Kind 0: a scripted character, which the game doesn't remove by itself.
    g.c.r1.u64 = sp;
    g.c.r3.u64 = 0; g.c.r4.u64 = 2; g.c.r5.u64 = definition; g.c.r6.u64 = pos; g.c.r7.u64 = mat;
    g.c.r8.u64 = 255; g.c.r9.u64 = 0; g.c.r10.u64 = 0;
    sub_82479AB0(g.c, base);
    const uint32_t obj = g.c.r3.u32;
    if (!Readable(base, obj, 4252) || obj == PPC_LOAD_U32(kPlayerPointer)) return 0;
    const uint32_t handle = PPC_LOAD_U32(obj + 68);
    const uint32_t desc = PPC_LOAD_U32(obj + 228);
    if (Readable(base, desc, 612)) StreamReference(g, PPC_LOAD_U32(desc + 608));
    return handle;
  }
  return 0;
}

bool KeyEdge(int vk) {
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool edge = down && !g_key_was[vk & 0xFF];
  g_key_was[vk & 0xFF] = down;
  return edge;
}

// The game streams the world (high-detail chunks and textures) and spawns
// people and traffic around the PLAYER. While the Studio camera is in charge
// the player is hidden and carried along with the camera, and put back where
// it stood when the Studio camera lets go (F1, menus, cutscenes). F2 toggles.
bool g_follow_enabled = true;
bool g_following = false;
Vec3 g_follow_home{};
float g_follow_home_rows[9]{};
uint32_t g_follow_player = 0;
std::chrono::steady_clock::time_point g_follow_node_time{};

void SetHidden(uint8_t* base, uint32_t obj, bool hide) {
  const uint32_t flags = PPC_LOAD_U32(obj + 120);
  if (hide == ((flags & 0x08000000u) != 0) || !g_ctx) return;
  Guest g(base);
  if (!hide) { g.c.r3.u64 = obj; g.c.r4.u64 = obj + 20; g.c.r5.u64 = 1; sub_8243F260(g.c, base); }
  PPC_STORE_U32(obj + 120, hide ? (flags | 0x08000000u) : (flags & ~0x08000000u));
  if (flags & 0x04000000u) {
    const uint32_t vtable = PPC_LOAD_U32(obj);
    if (Readable(base, vtable + 28, 4))
      if (auto fn = wml::FindFunction(PPC_LOAD_U32(vtable + 28))) {
        g.c.r3.u64 = obj; g.c.r4.u64 = hide ? 0 : 1; g.c.r5.u64 = 1;
        fn(g.c, base);
      }
  }
}

void TeleportPlayer(uint8_t* base, uint32_t player, const Vec3& p, bool find_node, const float* rows) {
  Guest g(base);
  const uint32_t pos = g.data, velocity = g.data + 16;
  WriteVec(base, pos, p); WriteF32(base, pos + 12, 0);
  WriteVec(base, velocity, {}); WriteF32(base, velocity + 12, 0);
  if (rows) for (int i = 0; i < 9; ++i) WriteF32(base, player + 32 + i * 4, rows[i]);
  const uint32_t node = PPC_LOAD_U32(player + 392);
  g.c.r3.u64 = player; g.c.r4.u64 = pos; g.c.r5.u64 = 0;
  g.c.r6.u64 = (find_node || node == 0xFFFFFFFFu) ? ~0ull : node;
  sub_8243E6F8(g.c, base);
  g.c.r3.u64 = player; g.c.r4.u64 = velocity;
  sub_8243E948(g.c, base);
}

void UpdatePlayerFollow(uint8_t* base) {
  if (KeyEdge(VK_F2)) {
    g_follow_enabled = !g_follow_enabled;
    REXLOG_INFO("World Studio: player follows the camera {}", g_follow_enabled ? "on" : "off");
  }
  const uint32_t player = PPC_LOAD_U32(kPlayerPointer);
  const bool player_ok = Readable(base, player, 4252) && PPC_LOAD_U32(player + 72) == 1 &&
                         (PPC_LOAD_U32(player + 216) & 0x4000) && PPC_LOAD_U32(player + 2496) == 0;
  // Start only in play: the player has been set up for 5 s (never during
  // loading - moving it then sent the loader elsewhere and loading hung) and
  // the camera has been flown more than 5 m away from it.
  static std::chrono::steady_clock::time_point ready_since{};
  static uint32_t ready_player = 0;
  const auto t = std::chrono::steady_clock::now();
  if (!player_ok || player != ready_player) { ready_player = player_ok ? player : 0; ready_since = t; }
  const bool settled = player_ok && t - ready_since > std::chrono::seconds(5);
  bool want = g_follow_enabled && g_camera_owned && settled && g_ctx;
  if (want && !g_following) {
    const Vec3 pp = ReadVec(base, player + 20);
    const float dx = g_editor_camera_position.x - pp.x, dy = g_editor_camera_position.y - pp.y,
                dz = g_editor_camera_position.z - pp.z;
    if (dx * dx + dy * dy + dz * dz < 25.0f) want = false;
  }
  if (g_following && (!want || player != g_follow_player)) {
    // Let go: back where the player stood, visible again.
    if (Readable(base, g_follow_player, 4252) && PPC_LOAD_U32(g_follow_player + 72) == 1) {
      // A cutscene places the player itself: then only show it again.
      if (!PPC_LOAD_U8(kCutscenePlaying) && !PPC_LOAD_U8(kScriptedCutscenePlaying))
        TeleportPlayer(base, g_follow_player, g_follow_home, true, g_follow_home_rows);
      SetHidden(base, g_follow_player, false);
    }
    g_following = false;
    REXLOG_INFO("World Studio: player put back at {:.1f} {:.1f} {:.1f}", g_follow_home.x, g_follow_home.y, g_follow_home.z);
  }
  if (!want) return;
  const auto now = std::chrono::steady_clock::now();
  if (!g_following) {
    g_follow_player = player;
    g_follow_home = ReadVec(base, player + 20);
    for (int i = 0; i < 9; ++i) g_follow_home_rows[i] = ReadF32(base, player + 32 + i * 4);
    SetHidden(base, player, true);
    g_following = true;
    g_follow_node_time = {};
    REXLOG_INFO("World Studio: player follows the camera (was at {:.1f} {:.1f} {:.1f})",
                g_follow_home.x, g_follow_home.y, g_follow_home.z);
  }
  // Every frame, so it never falls; a new nav node twice a second.
  const bool find_node = now - g_follow_node_time > std::chrono::milliseconds(500);
  if (find_node) g_follow_node_time = now;
  TeleportPlayer(base, player, g_editor_camera_position, find_node, nullptr);
}

// Delete / Ctrl+C / Ctrl+V / Ctrl+D on the selection (mouse over the game).
void EditKeys(uint8_t* base, bool hovered) {
  const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool del = KeyEdge(VK_DELETE), c = KeyEdge('C'), v = KeyEdge('V'), d = KeyEdge('D');
  if (!hovered) return;
  const uint32_t player = PPC_LOAD_U32(kPlayerPointer);
  if (del && g_prop.kind) {
    REXLOG_INFO("World Studio: {} {} deleted", g_prop.kind == 1 ? "mover" : "static piece", g_prop.index);
    HideProp(base, g_prop);
    g_prop = {};
    g_axis_drag = -1;
    return;
  }
  if (del && g_sel_address && g_sel_address != player) {
    REXLOG_INFO("World Studio: {} {:08X} deleted", TypeName(PPC_LOAD_U32(g_sel_address + 72)), g_sel_handle);
    DestroyObject(base, g_sel_handle);
    g_sel_address = g_sel_handle = 0;
    g_axis_drag = -1;
    return;
  }
  if (!ctrl) return;
  if ((c || d) && g_sel_address) {
    Clip clip;
    clip.type = PPC_LOAD_U32(g_sel_address + 72);
    clip.pos = ReadVec(base, g_sel_address + 20);
    for (int i = 0; i < 9; ++i) clip.rows[i] = ReadF32(base, g_sel_address + 32 + i * 4);
    if (clip.type == 5) clip.vehicle_type = VehicleType(base, g_sel_address);
    if (clip.type == 1) clip.model = HumanModel(base, g_sel_address);
    const bool can = (clip.type == 5 && clip.vehicle_type >= 0) || (clip.type == 1 && !clip.model.empty());
    REXLOG_INFO("World Studio: copied {} (type {}, {}){}", TypeName(clip.type), clip.type,
                clip.type == 5 ? std::to_string(clip.vehicle_type) : clip.model,
                can ? "" : " - this kind can't be pasted yet");
    if (can) g_clip = clip;
  }
  if ((v || d) && g_clip.type) {
    // Next to where it was copied, 2.5 m to the camera's right per paste.
    static int pastes = 0;
    const float offset = 2.5f * float(1 + (pastes++ % 6));
    Vec3 p{g_clip.pos.x + g_cam_right.x * offset, g_clip.pos.y, g_clip.pos.z + g_cam_right.z * offset};
    const uint32_t handle = CreateLike(base, g_clip, p);
    REXLOG_INFO("World Studio: paste {} -> {:08X}", TypeName(g_clip.type), handle);
    if (handle) {
      g_sel_handle = handle;
      g_sel_address = PPC_LOAD_U32(kObjectTable + 12 + (handle & 0xffffu) * 16);
    }
  }
}

void ApplyCommands(uint8_t* base) {
  std::vector<Command> commands;
  {
    std::lock_guard lock(g_commands_mutex);
    commands.swap(g_commands);
  }
  for (const auto& command : commands) {
    if (command.kind == Command::Kind::kSetCamera) {
      if (Finite(command.position) && command.fov >= 10 && command.fov <= 170) {
        WriteVec(base, kCamera + 44, command.position);
        WriteVec(base, kCamera + 56, command.position);
        WriteF32(base, kCamera + 188, command.fov);
      }
      continue;
    }
    if (!IsCurrentObject(base, command.address, command.handle)) continue;
    PlaceObject(base, command.address, command.position);
  }
}

// ---------------------------------------------------------------------------
// Cutscenes. Each is a packfile game/packfiles/cs_<x>.vpp_xbox2 holding the
// camera track (<name>.csc_xbox2), actors and start positions (<name>.cts,
// text), the shot script (<name>.lua, text), one animation per actor per shot
// and the actors' models. 821F9098(name, 1, hash(""), 0, 0) starts one (the
// same call the script function cutscene_play makes, 824CB790); 821F9700 ends
// the playing one at once. Edited files from the Studio
// (tools/WorldStudio/cutscene_edits/<name>/) are packed into a copy of the
// cutscene's pack in mods/WorldStudio/files/packfiles, which the mod loader
// lays over the game folder.
// ---------------------------------------------------------------------------
}  // namespace
}  // namespace sr::world_studio
extern "C" {
void sub_821F9098(PPCContext& ctx, uint8_t* base);  // start cutscene
void sub_821F9700(PPCContext& ctx, uint8_t* base);  // end cutscene now
void sub_82637B58(PPCContext& ctx, uint8_t* base);  // string hash
}
namespace sr::world_studio {
namespace {

std::filesystem::path ExeFolder() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Returns true when an edited pack is in place for `request`.
bool BuildEditedPack(const CutsceneRequest& request) {
  namespace fs = std::filesystem;
  const fs::path exe = ExeFolder();
  const fs::path original = exe / "game" / "packfiles" / request.archive;
  const fs::path out = exe / "mods" / "WorldStudio" / "files" / "packfiles" / request.archive;
  const fs::path edits = exe / "tools" / "WorldStudio" / "cutscene_edits" / request.name;
  std::error_code ec;
  if (!request.edited || !fs::is_directory(edits, ec)) {
    if (fs::remove(out, ec)) REXLOG_INFO("World Studio: cutscene {} back to the original pack", request.name);
    return false;
  }
  wml::Packfile pack;
  std::string error;
  if (!pack.Load(original, &error)) {
    REXLOG_INFO("World Studio: can't read {}: {}", original.string(), error);
    return false;
  }
  std::map<std::string, std::string> names;
  for (const auto& name : pack.Names()) names[Lower(name)] = name;
  std::map<std::string, std::string> replacements;
  for (const auto& entry : fs::directory_iterator(edits, ec)) {
    if (!entry.is_regular_file()) continue;
    const auto found = names.find(Lower(entry.path().filename().string()));
    if (found == names.end()) {
      REXLOG_INFO("World Studio: {} is not in {} (skipped)", entry.path().filename().string(), request.archive);
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    replacements[found->second] = std::string(std::istreambuf_iterator<char>(in), {});
  }
  if (replacements.empty()) { fs::remove(out, ec); return false; }
  fs::create_directories(out.parent_path(), ec);
  if (!pack.Save(out, replacements, &error)) {
    REXLOG_INFO("World Studio: can't write the edited pack {}: {}", out.string(), error);
    return false;
  }
  REXLOG_INFO("World Studio: edited pack for {} written ({} files replaced)", request.name, replacements.size());
  return true;
}

bool CutsceneActive(uint8_t* base) {
  return PPC_LOAD_U8(kCutscenePlaying) || PPC_LOAD_U8(kScriptedCutscenePlaying);
}


// Diagnostic: finds the cutscene clock. 1 s and 2 s after a cutscene starts,
// compares cutscene-related memory and logs every word that grew like real
// time (float seconds, int milliseconds, float/int frames at 30 or 60).
void FindCutsceneClock(uint8_t* base) {
  struct Region { uint32_t start, size; };
  static std::vector<uint8_t> snapshot;
  static std::vector<Region> regions;
  static int stage = 0;  // 0 idle, 1 waiting for snapshot A, 2 waiting for B, 3 done
  static std::chrono::steady_clock::time_point started, snap_time;
  const bool active = CutsceneActive(base);
  if (!active) { stage = 0; return; }
  const auto now = std::chrono::steady_clock::now();
  if (stage == 0) { stage = 1; started = now; return; }
  if (stage == 3) return;
  if (now - started < std::chrono::seconds(stage)) return;
  const uint32_t record = PPC_LOAD_U32(0x8370D994u);
  std::vector<Region> want = {{0x8370D000u, 0x1000}, {0x827D5000u, 0x2000}, {0x82B2E000u, 0x2000},
                              {0x836FD000u, 0x1800}, {0x827AA600u, 0x200}};
  if (Readable(base, record, 1892)) want.push_back({record, 1892});
  std::vector<uint8_t> data;
  for (const auto& r : want) {
    if (!Readable(base, r.start, r.size)) continue;
    const uint8_t* p = GuestPtr(base, r.start);
    data.insert(data.end(), p, p + r.size);
  }
  if (stage == 1) { snapshot = std::move(data); regions = want; snap_time = now; stage = 2; return; }
  stage = 3;
  const double dt = std::chrono::duration<double>(now - snap_time).count();
  size_t offset = 0;
  int found = 0;
  for (const auto& r : regions) {
    if (!Readable(base, r.start, r.size)) continue;
    for (uint32_t i = 0; i + 4 <= r.size && offset + i + 4 <= snapshot.size() && offset + i + 4 <= data.size(); i += 4) {
      uint32_t a, b;
      std::memcpy(&a, &snapshot[offset + i], 4); std::memcpy(&b, &data[offset + i], 4);
      a = _byteswap_ulong(a); b = _byteswap_ulong(b);
      if (a == b) continue;
      float fa, fb; std::memcpy(&fa, &a, 4); std::memcpy(&fb, &b, 4);
      const double df = double(fb) - double(fa), di = double(int32_t(b)) - double(int32_t(a));
      const char* kind = nullptr;
      if (std::isfinite(fa) && std::isfinite(fb) && std::abs(df - dt) < 0.25 * dt) kind = "float seconds";
      else if (std::isfinite(fa) && std::isfinite(fb) && (std::abs(df - dt * 30) < 0.25 * dt * 30 || std::abs(df - dt * 60) < 0.25 * dt * 60)) kind = "float frames";
      else if (std::abs(di - dt * 1000) < 0.25 * dt * 1000) kind = "int ms";
      else if (std::abs(di - dt * 30) < 0.3 * dt * 30 || std::abs(di - dt * 60) < 0.3 * dt * 60) kind = "int frames";
      if (!kind) continue;
      if (found++ < 60)
        REXLOG_INFO("World Studio: cutscene clock candidate {:08X} {}: {} -> {} (float {} -> {}) over {:.3f} s",
                    r.start + i, kind, int32_t(a), int32_t(b), fa, fb, dt);
    }
    offset += r.size;
  }
  REXLOG_INFO("World Studio: cutscene clock search done, {} candidates (record {:08X})", found, record);
}

void ProcessCutscenes(uint8_t* base) {
  static CutsceneRequest waiting;  // play after the current one has ended
  static std::chrono::steady_clock::time_point waiting_since{};
  std::vector<CutsceneRequest> requests;
  {
    std::lock_guard lock(g_cutscene_mutex);
    requests.swap(g_cutscene_requests);
  }
  for (const auto& request : requests) {
    if (request.kind == 3) {
      CutsceneRequest revert = request; revert.edited = false;
      BuildEditedPack(revert);
      continue;
    }
    if (CutsceneActive(base) && g_ctx) {
      Guest g(base);
      sub_821F9700(g.c, base);
      REXLOG_INFO("World Studio: cutscene stopped");
    }
    if (request.kind == 1) {
      const bool edited = BuildEditedPack(request);
      waiting = request;
      waiting.edited = edited;
      waiting_since = std::chrono::steady_clock::now();
    }
  }
  if (waiting.kind != 1 || !g_ctx) return;
  if (CutsceneActive(base)) {
    if (std::chrono::steady_clock::now() - waiting_since > std::chrono::seconds(5)) {
      REXLOG_INFO("World Studio: cutscene {} not started (another one didn't end)", waiting.name);
      waiting = {};
    }
    return;
  }
  Guest g(base);
  const uint32_t name = g.data + 0x100, empty = g.data + 0x180;
  std::memcpy(GuestPtr(base, name), waiting.name.c_str(), waiting.name.size() + 1);
  PPC_STORE_U8(empty, 0);
  g.c.r3.u64 = empty; g.c.r4.u64 = 0;
  sub_82637B58(g.c, base);
  const uint64_t hash = g.c.r3.u64;
  g.c.r3.u64 = name; g.c.r4.u64 = 1; g.c.r5.u64 = hash; g.c.r6.u64 = 0; g.c.r7.u64 = 0;
  sub_821F9098(g.c, base);
  REXLOG_INFO("World Studio: cutscene {} requested ({})", waiting.name, waiting.edited ? "edited" : "original");
  waiting = {};
}

// Screen position of a world point in the game view (client pixels).
struct Projector {
  Vec3 eye, right, up, forward;
  float focal = 1, cx = 0, cy = 0;
  bool Project(const Vec3& p, float& sx, float& sy) const {
    const Vec3 d{p.x - eye.x, p.y - eye.y, p.z - eye.z};
    const float z = d.x * forward.x + d.y * forward.y + d.z * forward.z;
    if (z < 0.05f) return false;
    const float x = d.x * right.x + d.y * right.y + d.z * right.z;
    const float y = d.x * up.x + d.y * up.y + d.z * up.z;
    sx = cx + x / z * focal;
    sy = cy - y / z * focal;
    return true;
  }
};

float SegmentDistance(float px, float py, float ax, float ay, float bx, float by) {
  const float vx = bx - ax, vy = by - ay;
  const float len2 = vx * vx + vy * vy;
  float t = len2 > 0 ? ((px - ax) * vx + (py - ay) * vy) / len2 : 0;
  t = std::clamp(t, 0.0f, 1.0f);
  const float dx = ax + vx * t - px, dy = ay + vy * t - py;
  return std::sqrt(dx * dx + dy * dy);
}

// Unity-style selection and move gizmo in the game view: left click picks the
// object nearest the cursor (player, people, vehicles, props in the object
// table), dragging an axis arrow moves it along X (red), Y (green, up) or Z
// (blue). Positions are written at object +20 (physics may still move it
// while the world runs; pause the world to place things).
void UpdateGizmo(uint8_t* base, POINT cursor, bool hovered, bool alt) {
  if (g_pending_select.exchange(false)) {
    g_sel_address = g_pending_select_address.load();
    g_sel_handle = g_pending_select_handle.load();
    g_axis_drag = -1;
  }
  EditKeys(base, hovered);
  static HWND view = nullptr;
  if (hovered) {
    HWND under = WindowFromPoint(cursor);
    if (under) view = under;
  }
  GizmoDraw draw{};
  draw.paused = g_paused.load();
  RECT client{};
  if (!view || !IsWindow(view) || !GetClientRect(view, &client) || client.right < 8 || client.bottom < 8) {
    std::lock_guard lock(g_gizmo_mutex);
    g_gizmo_draw = draw;
    return;
  }
  POINT local = cursor;
  ScreenToClient(view, &local);
  float fov = ReadF32(base, kCamera + 188);
  if (!(fov >= 10.0f && fov <= 170.0f)) fov = 60.0f;
  Projector projector;
  projector.eye = g_gizmo_eye;
  projector.forward = ReadVec(base, kCamera + 104);
  projector.right = ReadVec(base, kCamera + 80);
  projector.up = ReadVec(base, kCamera + 92);
  projector.cx = client.right * 0.5f;
  projector.cy = client.bottom * 0.5f;
  // FOV taken as vertical.
  projector.focal = (client.bottom * 0.5f) / std::tan(fov * 3.14159265f / 360.0f);

  if (g_sel_address && !IsCurrentObject(base, g_sel_address, g_sel_handle)) {
    g_sel_address = g_sel_handle = 0;
    g_axis_drag = -1;
  }
  if (g_prop.kind && !PropValid(base, g_prop)) { g_prop = {}; g_axis_drag = -1; }

  const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
  const bool pressed = lmb && !g_lmb_was;
  {
    // Double-click in the game view (not on the gizmo): the Studio toggles a
    // full-screen game view.
    static auto last_click = std::chrono::steady_clock::time_point{};
    static POINT last_point{};
    if (pressed && hovered && !alt && g_axis_hot < 0) {
      const auto now = std::chrono::steady_clock::now();
      if (now - last_click < std::chrono::milliseconds(350) && std::abs(cursor.x - last_point.x) < 6 &&
          std::abs(cursor.y - last_point.y) < 6) {
        g_fullscreen_seq.fetch_add(1);
        last_click = {};
      } else {
        last_click = now;
        last_point = cursor;
      }
    }
  }
  g_lmb_was = lmb;
  const float mx = float(local.x), my = float(local.y);

  // Gizmo geometry for the selected object.
  Vec3 origin{};
  float ox = 0, oy = 0, ex[3] = {}, ey[3] = {};
  bool shown = false, axis_ok[3] = {};
  float length = 1.0f;
  if (g_sel_address || g_prop.kind) {
    origin = g_prop.kind ? PropPosition(base, g_prop) : ReadVec(base, g_sel_address + 20);
    const float dx = origin.x - projector.eye.x, dy = origin.y - projector.eye.y, dz = origin.z - projector.eye.z;
    length = std::max(0.25f, std::sqrt(dx * dx + dy * dy + dz * dz) * 0.15f);
    if (projector.Project(origin, ox, oy)) {
      shown = true;
      const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
      for (int a = 0; a < 3; ++a) {
        const Vec3 tip{origin.x + axes[a].x * length, origin.y + axes[a].y * length,
                       origin.z + axes[a].z * length};
        axis_ok[a] = projector.Project(tip, ex[a], ey[a]);
      }
    }
  }

  // Hover and drag.
  if (g_axis_drag >= 0 && !lmb) g_axis_drag = -1;
  g_axis_hot = -1;
  if (shown && g_axis_drag < 0 && hovered && !alt) {
    float best = 10.0f;
    for (int a = 0; a < 3; ++a) {
      if (!axis_ok[a]) continue;
      const float d = SegmentDistance(mx, my, ox, oy, ex[a], ey[a]);
      if (d < best) { best = d; g_axis_hot = a; }
    }
  }
  if (pressed && hovered && !alt && g_drag == Drag::kNone) {
    if (g_axis_hot >= 0) {
      g_axis_drag = g_axis_hot;
    }
    if (g_axis_drag < 0) {
      // Pick the object under the cursor: every live object in the object
      // table (people, vehicles, props, items). Each counts as a sphere around
      // its position (vehicles 2.5 m, others 0.8 m); the one whose screen
      // circle the cursor is deepest inside wins, nearer ones on a tie.
      uint32_t best_address = 0, best_handle = 0;
      float best = 1e9f;
      const uint32_t player = PPC_LOAD_U32(kPlayerPointer);
      for (uint32_t i = 0; i < kObjectSlots; ++i) {
        const uint32_t object = PPC_LOAD_U32(kObjectTable + 12 + i * 16);
        if (!object || !Readable(base, object, 80)) continue;
        const uint32_t handle = PPC_LOAD_U32(object + 68);
        const uint32_t type = PPC_LOAD_U32(object + 72);
        if (!handle || (handle & 0xffffu) != i || type == 0 || type > 64) continue;
        if (g_following && object == player) continue;
        Vec3 p = ReadVec(base, object + 20);
        if (!Finite(p)) continue;
        const float radius = type == 5 ? 2.5f : 0.8f;
        if (type == 1) p.y += 0.9f;  // people: position is at the feet
        float sx, sy;
        if (!projector.Project(p, sx, sy)) continue;
        const Vec3 d{p.x - projector.eye.x, p.y - projector.eye.y, p.z - projector.eye.z};
        const float depth = std::max(0.1f, d.x * projector.forward.x + d.y * projector.forward.y + d.z * projector.forward.z);
        const float screen_radius = std::max(18.0f, radius * projector.focal / depth);
        const float dist = std::sqrt((sx - mx) * (sx - mx) + (sy - my) * (sy - my));
        if (dist > screen_radius) continue;
        const float score = dist / screen_radius + depth * 0.002f + (object == player ? 0.5f : 0.0f);
        if (score < best) { best = score; best_address = object; best_handle = handle; }
      }
      g_sel_address = best_address;
      g_sel_handle = best_handle;
      g_prop = {};
      if (!best_address) {
        // Nothing live under the cursor: map props (movers, then static pieces).
        const float nx = (mx - projector.cx) / projector.focal, ny = (projector.cy - my) / projector.focal;
        const Vec3 ray{projector.forward.x + projector.right.x * nx + projector.up.x * ny,
                       projector.forward.y + projector.right.y * nx + projector.up.y * ny,
                       projector.forward.z + projector.right.z * nx + projector.up.z * ny};
        float score = 0;
        g_prop = PickProp(base, projector.eye, ray, mx, my,
                          [&](const Vec3& q, float& sx, float& sy) { return projector.Project(q, sx, sy); },
                          projector.focal, projector.forward, score);
        if (g_prop.kind) LogProp(base, g_prop);
      }
      static int lines = 0;
      if (lines++ < 100)
        REXLOG_INFO("World Studio: click at {:.0f},{:.0f} of {}x{} -> {}", mx, my, client.right, client.bottom,
                    best_address ? TypeName(PPC_LOAD_U32(best_address + 72)) : "nothing near");
      if (best_address)
        REXLOG_INFO("World Studio: selected {:08X} type {} physics {} model {}", best_handle,
                    PPC_LOAD_U32(best_address + 72), int32_t(PhysicsIndex(base, best_address)),
                    PPC_LOAD_U32(best_address + 72) == 1 ? HumanModel(base, best_address)
                    : PPC_LOAD_U32(best_address + 72) == 5 ? std::to_string(VehicleType(base, best_address)) : std::string("-"));
    } else {
      static int lines = 0;
      if (lines++ < 100) REXLOG_INFO("World Studio: dragging axis {}", g_axis_drag);
    }
  }
  if (g_axis_drag >= 0 && (g_sel_address || g_prop.kind) && shown && axis_ok[g_axis_drag]) {
    // Mouse movement along the arrow's direction on screen.
    const float vx = ex[g_axis_drag] - ox, vy = ey[g_axis_drag] - oy;
    const float len2 = vx * vx + vy * vy;
    const float mdx = float(cursor.x - g_last_cursor.x), mdy = float(cursor.y - g_last_cursor.y);
    if (len2 > 4.0f && (mdx != 0 || mdy != 0)) {
      const float t = (mdx * vx + mdy * vy) / len2 * length;
      Vec3 p = g_prop.kind ? PropPosition(base, g_prop) : ReadVec(base, g_sel_address + 20);
      if (g_axis_drag == 0) p.x += t; else if (g_axis_drag == 1) p.y += t; else p.z += t;
      if (g_prop.kind) MoveProp(base, g_prop, p);
      else PlaceObject(base, g_sel_address, p);
    }
  }
  g_last_cursor = cursor;

  draw.visible = shown;
  draw.ox = ox; draw.oy = oy;
  for (int a = 0; a < 3; ++a) { draw.ex[a] = ex[a]; draw.ey[a] = ey[a]; draw.axis_visible[a] = axis_ok[a]; }
  draw.hot = g_axis_hot;
  draw.active = g_axis_drag;
  draw.view_width = float(client.right);
  draw.view_height = float(client.bottom);
  std::lock_guard lock(g_gizmo_mutex);
  g_gizmo_draw = draw;
  g_gizmo_time = std::chrono::steady_clock::now();
}

void UpdatePlayModeKey() {
  // Hold V (0.45 s) with the game view under the mouse or focused: switch
  // between the Studio camera and playing as the player.
  static auto down_since = std::chrono::steady_clock::time_point{};
  static bool fired = false;
  const bool v = (GetAsyncKeyState('V') & 0x8000) != 0;
  bool ours = false;
  POINT cursor{};
  GetCursorPos(&cursor);
  DWORD pid = 0;
  if (HWND under = WindowFromPoint(cursor)) GetWindowThreadProcessId(under, &pid);
  ours = pid == GetCurrentProcessId();
  if (HWND fg = GetForegroundWindow()) { DWORD fpid = 0; GetWindowThreadProcessId(fg, &fpid); ours |= fpid == GetCurrentProcessId(); }
  const auto now = std::chrono::steady_clock::now();
  if (!v) { down_since = {}; fired = false; return; }
  if (down_since.time_since_epoch().count() == 0) down_since = now;
  if (!fired && ours && now - down_since > std::chrono::milliseconds(450)) {
    fired = true;
    const bool play = !g_play_mode.load();
    g_play_mode.store(play);
    REXLOG_INFO("World Studio: {}", play ? "playing as the player (hold V for the Studio camera)" : "Studio camera");
  }
}

void UpdateEditorHost(uint8_t* base) {
  g_camera_owned = false;
  if (!HostEnvironmentEnabled()) return;
  UpdatePlayModeKey();

  // Leave the title screen and save-selection flow completely untouched. The
  // editor host only takes ownership once the game has produced a live player
  // object (and therefore a loaded world).
  const uint32_t player = PPC_LOAD_U32(kPlayerPointer);
  if (!Readable(base, player, 80) || PPC_LOAD_U32(player + 72) != 1 ||
      sr::g_studio_menu_active.load()) {
    // Front end, loading, character creator or pause menu: the game keeps its
    // camera, HUD and input.
    g_editor_camera_initialized = false;
    return;
  }

  // hud_hide() is a single write to this verified game-owned flag. Reapply it
  // because UI flows may call hud_show() while the editor host is running.
  if (!g_play_mode.load()) PPC_STORE_U8(kHudVisible, 0);

  const bool f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
  if (f1 && !g_f1_down) {
    g_editor_free_camera = !g_editor_free_camera;
    g_editor_camera_initialized = false;
  }
  g_f1_down = f1;
  // Ctrl+Shift+P (as in Unity) or the Pause key freezes/resumes the world.
  const bool pause_key = (GetAsyncKeyState(VK_PAUSE) & 0x8000) ||
      ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
       (GetAsyncKeyState('P') & 0x8000));
  if (pause_key && !g_pause_key_was) g_paused.store(!g_paused.load());
  g_pause_key_was = pause_key;

  // Let the original camera own real-time cutscenes. This makes the host useful
  // for watching them as well as roaming the world.
  if (g_play_mode.load()) {  // playing as the player: the game has camera, HUD and input
    g_editor_camera_initialized = false;
    return;
  }
  if (g_editor_free_camera && (PPC_LOAD_U8(kCutscenePlaying) || PPC_LOAD_U8(kScriptedCutscenePlaying)) &&
      Readable(base, kCamera, 208)) {
    // Cutscene: its own camera shows it; objects can still be picked and
    // moved with the gizmo, projected from the cutscene camera.
    g_editor_camera_initialized = false;
    POINT cursor{};
    GetCursorPos(&cursor);
    bool hovered = false;
    if (HWND under = WindowFromPoint(cursor)) {
      DWORD pid = 0;
      GetWindowThreadProcessId(under, &pid);
      hovered = pid == GetCurrentProcessId();
    }
    g_gizmo_eye = ReadVec(base, kCamera + 44);
    UpdateGizmo(base, cursor, hovered, (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
    return;
  }
  if (!g_editor_free_camera || PPC_LOAD_U8(kCutscenePlaying) ||
      PPC_LOAD_U8(kScriptedCutscenePlaying) || !Readable(base, kCamera, 208)) {
    g_editor_camera_initialized = false;
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  float dt = g_editor_camera_tick.time_since_epoch().count()
      ? std::chrono::duration<float>(now - g_editor_camera_tick).count() : 0.0f;
  g_editor_camera_tick = now;
  dt = std::clamp(dt, 0.0f, 0.05f);

  if (!g_editor_camera_initialized) {
    g_editor_camera_position = ReadVec(base, kCamera + 44);
    Vec3 forward = ReadVec(base, kCamera + 104);
    if (!Finite(forward)) forward = {0, 0, 1};
    g_editor_camera_pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    g_editor_camera_yaw = std::atan2(forward.x, forward.z);
    g_editor_camera_initialized = true;
  }

  // Unity-style scene view controls, only for the game view under the mouse
  // (so typing in Studio fields does not move the camera):
  //   right mouse held: look around; WASD fly, Q/E down/up, Shift faster,
  //                     wheel changes the fly speed
  //   middle mouse drag: pan          wheel: move forward/back
  //   Alt + left drag:   orbit        Alt + right drag: zoom
  //   arrow keys (mouse over the view): move
  const auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
  POINT cursor{};
  GetCursorPos(&cursor);
  bool hovered = false;
  if (HWND under = WindowFromPoint(cursor)) {
    DWORD pid = 0;
    GetWindowThreadProcessId(under, &pid);
    hovered = pid == GetCurrentProcessId();
  }
  const bool alt = down(VK_MENU);
  const bool lmb = down(VK_LBUTTON), rmb = down(VK_RBUTTON), mmb = down(VK_MBUTTON);
  if (g_drag == Drag::kNone && hovered) {
    if (alt && lmb) g_drag = Drag::kOrbit;
    else if (alt && rmb) g_drag = Drag::kZoom;
    else if (rmb) g_drag = Drag::kLook;
    else if (mmb) g_drag = Drag::kPan;
    if (g_drag != Drag::kNone) g_drag_anchor = cursor;
  }
  if ((g_drag == Drag::kOrbit && !lmb) || ((g_drag == Drag::kLook || g_drag == Drag::kZoom) && !rmb) ||
      (g_drag == Drag::kPan && !mmb)) {
    g_drag = Drag::kNone;
  }
  float dx = 0, dy = 0;
  if (g_drag != Drag::kNone) {
    // Movement since the last frame; the cursor is put back where the drag
    // started so it never runs into the screen edge.
    dx = float(cursor.x - g_drag_anchor.x);
    dy = float(cursor.y - g_drag_anchor.y);
    if (dx != 0 || dy != 0) SetCursorPos(g_drag_anchor.x, g_drag_anchor.y);
  }
  const float wheel = g_wheel.exchange(0.0f);

  const auto basis = [&](Vec3& forward, Vec3& right, Vec3& up) {
    g_editor_camera_pitch = std::clamp(g_editor_camera_pitch, -1.50f, 1.50f);
    const float cp = std::cos(g_editor_camera_pitch);
    forward = {std::sin(g_editor_camera_yaw) * cp, std::sin(g_editor_camera_pitch),
               std::cos(g_editor_camera_yaw) * cp};
    right = {std::cos(g_editor_camera_yaw), 0, -std::sin(g_editor_camera_yaw)};
    up = {-std::sin(g_editor_camera_yaw) * std::sin(g_editor_camera_pitch), cp,
          -std::cos(g_editor_camera_yaw) * std::sin(g_editor_camera_pitch)};
  };
  const auto move = [&](const Vec3& axis, float scale) {
    g_editor_camera_position.x += axis.x * scale;
    g_editor_camera_position.y += axis.y * scale;
    g_editor_camera_position.z += axis.z * scale;
  };
  Vec3 forward, right, up;
  basis(forward, right, up);
  constexpr float kLook = 0.0035f;  // radians per pixel
  if (g_drag == Drag::kLook) {
    g_editor_camera_yaw += dx * kLook;
    g_editor_camera_pitch -= dy * kLook;
    if (wheel != 0) g_fly_speed = std::clamp(g_fly_speed * std::pow(1.2f, wheel), 1.0f, 400.0f);
  } else if (g_drag == Drag::kOrbit) {
    // Around the point in front of the camera.
    const Vec3 pivot{g_editor_camera_position.x + forward.x * g_orbit_distance,
                     g_editor_camera_position.y + forward.y * g_orbit_distance,
                     g_editor_camera_position.z + forward.z * g_orbit_distance};
    g_editor_camera_yaw += dx * kLook;
    g_editor_camera_pitch -= dy * kLook;
    basis(forward, right, up);
    g_editor_camera_position = {pivot.x - forward.x * g_orbit_distance, pivot.y - forward.y * g_orbit_distance,
                                pivot.z - forward.z * g_orbit_distance};
  } else if (g_drag == Drag::kZoom) {
    const float step = (dx - dy) * 0.02f * std::max(1.0f, g_orbit_distance * 0.2f);
    const float d = std::clamp(g_orbit_distance - step, 1.0f, 500.0f);
    move(forward, g_orbit_distance - d);
    g_orbit_distance = d;
  } else if (g_drag == Drag::kPan) {
    const float k = 0.002f * std::max(4.0f, g_orbit_distance);
    move(right, -dx * k);
    move(up, dy * k);
  }
  basis(forward, right, up);
  if (g_drag != Drag::kLook && hovered && wheel != 0) {
    const float step = wheel * std::max(1.0f, g_orbit_distance * 0.15f);
    move(forward, step);
    g_orbit_distance = std::clamp(g_orbit_distance - step, 1.0f, 500.0f);
  }

  float speed = g_fly_speed * (down(VK_SHIFT) ? 3.5f : 1.0f);
  if (down(VK_CONTROL)) speed *= 0.2f;
  const float amount = speed * dt;
  if (g_drag == Drag::kLook) {
    if (down('W')) move(forward, amount);
    if (down('S')) move(forward, -amount);
    if (down('D')) move(right, amount);
    if (down('A')) move(right, -amount);
    if (down('E')) g_editor_camera_position.y += amount;
    if (down('Q')) g_editor_camera_position.y -= amount;
  }
  if (hovered || g_drag != Drag::kNone) {
    if (down(VK_UP)) move(forward, amount);
    if (down(VK_DOWN)) move(forward, -amount);
    if (down(VK_RIGHT)) move(right, amount);
    if (down(VK_LEFT)) move(right, -amount);
  }

  WriteVec(base, kCamera + 44, g_editor_camera_position);
  WriteVec(base, kCamera + 56, g_editor_camera_position);
  WriteVec(base, kCamera + 80, right);
  WriteVec(base, kCamera + 92, up);
  WriteVec(base, kCamera + 104, forward);
  g_cam_forward = forward; g_cam_right = right; g_cam_up = up;
  g_camera_owned = true;
  g_gizmo_eye = g_editor_camera_position;
  UpdateGizmo(base, cursor, hovered, alt);
}

void BuildSnapshot(uint8_t* base) {
  const bool cutscene = PPC_LOAD_U8(kCutscenePlaying) != 0;
  const bool scripted = PPC_LOAD_U8(kScriptedCutscenePlaying) != 0;
  const bool active = cutscene || scripted;
  const auto now = std::chrono::steady_clock::now();
  if (active && !g_last_cutscene) g_cutscene_started = now;
  g_last_cutscene = active;
  const double observed_time = active
      ? std::chrono::duration<double>(now - g_cutscene_started).count() : 0.0;

  const uint32_t player = PPC_LOAD_U32(kPlayerPointer);
  const bool player_ok = Readable(base, player, 80) && PPC_LOAD_U32(player + 72) == 1;
  const Vec3 camera_position = ReadVec(base, kCamera + 44);
  const Vec3 camera_forward = ReadVec(base, kCamera + 104);
  const float camera_fov = ReadF32(base, kCamera + 188);

  std::ostringstream out;
  out.precision(7);
  out << R"({"type":"snapshot","protocol":1,"ready":true,"frame":)" << g_frame.load()
      << R"(,"editor_host":)" << (HostEnvironmentEnabled() ? "true" : "false")
      << R"(,"free_camera":)" << (g_editor_free_camera ? "true" : "false")
      << R"(,"paused":)" << (g_paused.load() ? "true" : "false")
      << R"(,"play_mode":)" << (g_play_mode.load() ? "true" : "false")
      << R"(,"fullscreen_seq":)" << g_fullscreen_seq.load()
      << R"(,"picked_address":)" << g_sel_address << R"(,"picked_handle":)" << g_sel_handle
      << R"(,"cutscene":{"active":)" << (active ? "true" : "false")
      << R"(,"game_flag":)" << (cutscene ? "true" : "false")
      << R"(,"scripted_flag":)" << (scripted ? "true" : "false")
      << R"(,"observed_time":)" << observed_time << R"(,"time":)" << g_cutscene_time << R"(,"name":")" << [] {
           std::lock_guard lock(g_cutscene_name_mutex);
           return g_cutscene_now;
         }() << "\"},\"camera\":{\"position\":";
  AppendVec(out, camera_position);
  out << R"(,"forward":)"; AppendVec(out, camera_forward);
  out << R"(,"fov":)" << camera_fov << "},\"player\":";
  if (player_ok) {
    out << R"({"address":)" << player << R"(,"handle":)" << PPC_LOAD_U32(player + 68)
        << R"(,"type_id":1,"position":)";
    AppendVec(out, ReadVec(base, player + 20)); out << '}';
  } else {
    out << "null";
  }
  out << R"(,"entities":[)";
  unsigned emitted = 0;
  for (uint32_t i = 0; i < kObjectSlots && emitted < 512; ++i) {
    const uint32_t object = PPC_LOAD_U32(kObjectTable + 12 + i * 16);
    if (!Readable(base, object, 80)) continue;
    const uint32_t handle = PPC_LOAD_U32(object + 68);
    const uint32_t type = PPC_LOAD_U32(object + 72);
    if (!handle || (handle & 0xffffu) != i || type == 0 || type > 64) continue;
    const Vec3 position = ReadVec(base, object + 20);
    if (!Finite(position)) continue;
    if (emitted++) out << ',';
    out << R"({"slot":)" << i << R"(,"address":)" << object
        << R"(,"handle":)" << handle << R"(,"type_id":)" << type
        << R"(,"position":)";
    AppendVec(out, position); out << '}';
  }
  out << "]}";
  std::lock_guard lock(g_snapshot_mutex);
  g_snapshot = out.str();
}

}  // namespace

bool EditorHostEnabled() { return HostEnvironmentEnabled(); }

bool PlayMode() { return HostEnvironmentEnabled() && g_play_mode.load(); }


bool OwnsCamera() {
  return g_camera_owned && HostEnvironmentEnabled() && g_editor_free_camera &&
         !sr::g_studio_menu_active.load();
}

void WriteEditorCamera(uint8_t* base) {
  if (!OwnsCamera()) return;
  WriteVec(base, kCamera + 44, g_editor_camera_position);
  WriteVec(base, kCamera + 56, g_editor_camera_position);
  WriteVec(base, kCamera + 80, g_cam_right);
  WriteVec(base, kCamera + 92, g_cam_up);
  WriteVec(base, kCamera + 104, g_cam_forward);
}

bool GetGizmoDraw(GizmoDraw& out) {
  if (!HostEnvironmentEnabled()) return false;
  std::lock_guard lock(g_gizmo_mutex);
  out = g_gizmo_draw;
  // Not updated lately (game camera, cutscene, loading): nothing to draw.
  if (std::chrono::steady_clock::now() - g_gizmo_time > std::chrono::milliseconds(250)) out.visible = false;
  out.paused = g_paused.load();
  return true;
}

void AddMouseWheel(float notches) {
  if (!HostEnvironmentEnabled()) return;
  float current = g_wheel.load();
  while (!g_wheel.compare_exchange_weak(current, current + notches)) {}
}

// World pause for the editor host: the game keeps running and drawing (free
// camera, gizmo) but game time stands still. The frame timer (8262FFE0)
// divides each frame's time by the float at 0x827AA6D4 (normally 1) before
// storing the game's frame time (0x827AA61C); a huge divisor stops time.
constexpr uint32_t kTimeDivisor = 0x827AA6D4u;
float g_saved_divisor = 1.0f;
bool g_world_frozen = false;
// Cutscenes don't use that scaled time: their camera, actors and script run
// on the raw frame time (0x827AA6C8). The game's debug command
// cutscene_step_shot (821F6068) switches the frame timer (8262FFE0) to a
// fixed step: flag 0x836FD673 = 1, rate 0x827D6D30 (steps per second), raw
// time = 1 / rate. While paused the rate is huge (a 1 us step), and the
// emulator's audio (voice streams too) is paused. Step One Frame = 1/30 s.
constexpr uint32_t kFixedStepFlag = 0x836FD673u, kFixedStepRate = 0x827D6D30u;
uint8_t g_saved_fixed_flag = 0;
// The cutscene clock: two int millisecond counters (found by
// FindCutsceneClock: 0x8370DC48 / 0x8370DC4C grow ~1000 per second during a
// cutscene; the camera, the actors' animations and the shot script follow
// them). Held at their pause-time values while paused.
constexpr uint32_t kCutsceneClockA = 0x8370DC48u, kCutsceneClockB = 0x8370DC4Cu;
uint32_t g_clock_a = 0, g_clock_b = 0;
double g_saved_time_scalar = 1.0;
bool g_clock_held = false;
float g_saved_fixed_rate = 30.0f;

void HoldCutsceneClock(uint8_t* base) {
  if (!g_clock_held) return;
  PPC_STORE_U32(kCutsceneClockA, g_clock_a);
  PPC_STORE_U32(kCutsceneClockB, g_clock_b);
}

void SetAudioPaused(bool pause) {
  static bool audio_paused = false;
  if (audio_paused == pause) return;
  audio_paused = pause;
  auto* runtime = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->emulator() : nullptr;
  auto* audio = runtime ? static_cast<rex::audio::AudioSystem*>(runtime->audio_system()) : nullptr;
  if (!audio) return;
  // Pause waits for the audio thread; never block the game thread on it.
  std::thread([audio, pause] {
    if (pause) audio->Pause(); else audio->Resume();
    REXLOG_INFO("World Studio: audio {}", pause ? "paused" : "resumed");
  }).detach();
}

void UpdateWorldPause(uint8_t* base) {
  const bool want = g_paused.load();
  if (want) {
    if (!g_world_frozen) {
      const float current = ReadF32(base, kTimeDivisor);
      g_saved_divisor = (std::isfinite(current) && current > 0.0f && current < 100.0f) ? current : 1.0f;
      g_saved_fixed_flag = PPC_LOAD_U8(kFixedStepFlag);
      const float rate = ReadF32(base, kFixedStepRate);
      g_saved_fixed_rate = (std::isfinite(rate) && rate > 0.0f && rate < 100000.0f) ? rate : 30.0f;
      g_world_frozen = true;
      g_clock_a = PPC_LOAD_U32(kCutsceneClockA);
      g_clock_b = PPC_LOAD_U32(kCutsceneClockB);
      g_clock_held = true;
      // Game-side real time (mftb / KeQueryPerformanceCounter through the
      // SDK clock) nearly stands still: the cutscene clock runs on it.
      // The clock accumulates, so there is no jump on resume.
      g_saved_time_scalar = rex::chrono::Clock::guest_time_scalar();
      if (!(g_saved_time_scalar > 0.0) || g_saved_time_scalar > 100.0) g_saved_time_scalar = 1.0;
      rex::chrono::Clock::set_guest_time_scalar(0.0001);
      SetAudioPaused(true);
      REXLOG_INFO("World Studio: world paused (time divisor was {}, fixed step {} at {})", g_saved_divisor,
                  g_saved_fixed_flag, g_saved_fixed_rate);
    }
    // One frame at normal speed when Step One Frame was pressed.
    unsigned steps = g_steps.load();
    const bool step = steps && g_steps.compare_exchange_strong(steps, steps - 1);
    WriteF32(base, kTimeDivisor, step ? g_saved_divisor : 100000.0f);
    PPC_STORE_U8(kFixedStepFlag, 1);
    WriteF32(base, kFixedStepRate, step ? 30.0f : 1000000.0f);
    if (step) {  // one frame's worth of cutscene clock
      g_clock_a += 33; g_clock_b += 33;
    }
    // Step One Frame: normal time for this frame only.
    rex::chrono::Clock::set_guest_time_scalar(step ? g_saved_time_scalar : 0.0001);
    HoldCutsceneClock(base);
  } else if (g_world_frozen) {
    WriteF32(base, kTimeDivisor, g_saved_divisor);
    PPC_STORE_U8(kFixedStepFlag, g_saved_fixed_flag);
    WriteF32(base, kFixedStepRate, g_saved_fixed_rate);
    rex::chrono::Clock::set_guest_time_scalar(g_saved_time_scalar);
    SetAudioPaused(false);
    g_clock_held = false;
    g_world_frozen = false;
    REXLOG_INFO("World Studio: world resumed");
  }
}

bool BeforeGameFrame(uint8_t* base) {
  if (!Enabled()) return true;
  Start();
  if (HostEnvironmentEnabled()) {
    unsigned steps = g_steps.load(std::memory_order_relaxed);
    if (steps && g_paused.load()) {
      // One frame at normal speed.
      g_steps.fetch_sub(1);
      if (g_world_frozen) WriteF32(base, kTimeDivisor, g_saved_divisor);
      return true;
    }
    UpdateWorldPause(base);
    WriteEditorCamera(base);
  }
  if (!g_paused.load(std::memory_order_relaxed)) return true;
  unsigned steps = g_steps.load(std::memory_order_relaxed);
  while (steps && !g_steps.compare_exchange_weak(steps, steps - 1)) {}
  if (steps) return true;
  if (HostEnvironmentEnabled()) return true;  // world pause: see UpdateWorldPause
  // Present no longer runs while the outer loop is gated, so publish a
  // throttled snapshot here. This lets the editor observe the paused state
  // and send Resume without touching guest memory from the pipe thread.
  static auto last_snapshot = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now - last_snapshot >= std::chrono::milliseconds(100)) {
    BuildSnapshot(base);
    last_snapshot = now;
  }
  Sleep(1);
  return false;
}

void AfterFrameTimer(uint8_t* base) {
  if (HostEnvironmentEnabled()) HoldCutsceneClock(base);
}

void OnPresent(PPCContext& ctx, uint8_t* base) {
  if (!Enabled()) return;
  Start();
  g_ctx = &ctx;
  struct Clear { ~Clear() { g_ctx = nullptr; } } clear;
  if (HostEnvironmentEnabled()) UpdateWorldPause(base);  // BeforeGameFrame runs only once
  UpdateEditorHost(base);
  if (HostEnvironmentEnabled()) UpdatePlayerFollow(base);
  if (HostEnvironmentEnabled()) ProcessCutscenes(base);
  if (HostEnvironmentEnabled()) FindCutsceneClock(base);
  if (HostEnvironmentEnabled()) {
    // Cutscene play position from game time (stands still while paused).
    const bool active = CutsceneActive(base);
    if (active && !g_cutscene_was_active) g_cutscene_time = 0;
    if (active) {
      const float dt = ReadF32(base, 0x827AA6C8u);  // raw frame time (cutscenes run on it)
      if (std::isfinite(dt) && dt > 0 && dt < 0.5f) g_cutscene_time += dt;
    }
    g_cutscene_was_active = active;
  }
  if (!g_client_connected.load(std::memory_order_acquire)) return;
  g_frame.fetch_add(1, std::memory_order_relaxed);
  ApplyCommands(base);
  BuildSnapshot(base);
}

}  // namespace sr::world_studio

// Records the name of every cutscene the game starts (for the Studio).
extern "C" void __imp__sub_821F9098(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_821F9098) {
  if (sr::world_studio::EditorHostEnabled()) {
    const uint32_t name = ctx.r3.u32;
    std::string text;
    for (uint32_t i = 0; name && i < 63; ++i) {
      const uint8_t ch = PPC_LOAD_U8(name + i);
      if (!ch) break;
      text.push_back(char(ch));
    }
    REXLOG_INFO("World Studio: game starts cutscene {}", text);
    std::lock_guard lock(sr::world_studio::g_cutscene_name_mutex);
    sr::world_studio::g_cutscene_now = text;
  }
  __imp__sub_821F9098(ctx, base);
}
