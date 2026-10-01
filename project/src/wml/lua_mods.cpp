// Whompay's Mod Loader - Lua script mods.
//
// Each script mod gets its own Lua state running <mod>/main.lua, with a global
// `wml` table (see modding/README.md for the full list):
//
//   wml.log(...)                       write to mods/wml.log
//   wml.read_u8/u16/u32/f32(address)   read guest memory
//   wml.write_u8/u16/u32/f32(a, v)     write guest memory
//   wml.key_down(key) / key_pressed(key)
//   wml.on_frame(function() ... end)
//   wml.hook(address, function(ctx) ... end)
//   wml.setting(name, default)         value from [settings] in mod.ini
//   wml.mod_name, wml.mod_folder, wml.settings

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/ppc/context.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "wml_internal.h"
#include "mod_loader.h"

namespace wml {
namespace {

constexpr int kMaxLuaErrorsLogged = 20;

struct LuaMod {
  std::string name;
  lua_State* L = nullptr;
  std::recursive_mutex mutex;
  int lock_depth = 0;  // times the owning thread holds `mutex`
  std::vector<int> frame_callbacks;  // registry refs
  std::vector<std::pair<std::string, std::string>> settings;
  int errors = 0;
};

std::vector<std::unique_ptr<LuaMod>> g_lua_mods;

LuaMod* ModOf(lua_State* L) {
  return static_cast<LuaMod*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// Holds a mod's Lua lock and counts how deep, so the lock can be let go
// completely while the game's own code runs (see ReleaseForGameCall).
struct ModLock {
  LuaMod* mod;
  explicit ModLock(LuaMod* m) : mod(m) { mod->mutex.lock(); ++mod->lock_depth; }
  ~ModLock() { --mod->lock_depth; mod->mutex.unlock(); }
  ModLock(const ModLock&) = delete;
  ModLock& operator=(const ModLock&) = delete;
};

// Lets other threads run this mod's hooks while this thread is inside game
// code. Every thread runs hooks on its own Lua thread, so their stacks never
// mix; the lock only keeps two threads from running Lua at the same time.
struct ReleaseForGameCall {
  LuaMod* mod;
  int depth;
  explicit ReleaseForGameCall(LuaMod* m) : mod(m), depth(m->lock_depth) {
    mod->lock_depth = 0;
    for (int i = 0; i < depth; ++i) mod->mutex.unlock();
  }
  ~ReleaseForGameCall() {
    for (int i = 0; i < depth; ++i) mod->mutex.lock();
    mod->lock_depth = depth;
  }
};

void ReportError(LuaMod* mod, const char* where, lua_State* L = nullptr) {
  if (!L) L = mod->L;
  const char* message = lua_tostring(L, -1);
  if (mod->errors < kMaxLuaErrorsLogged) {
    Log(mod->name, std::string("Lua error in ") + where + ": " + (message ? message : "?"));
    if (++mod->errors == kMaxLuaErrorsLogged) {
      Log(mod->name, "Too many errors; further errors are not logged");
    }
  }
  lua_pop(L, 1);
}

uint32_t CheckAddress(lua_State* L, int index) {
  return static_cast<uint32_t>(luaL_checkinteger(L, index));
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

int KeyFromName(const std::string& name) {
#ifdef _WIN32
  static const std::unordered_map<std::string, int> kNames = {
      {"SPACE", VK_SPACE},     {"ENTER", VK_RETURN},     {"RETURN", VK_RETURN},
      {"ESC", VK_ESCAPE},      {"ESCAPE", VK_ESCAPE},    {"TAB", VK_TAB},
      {"BACKSPACE", VK_BACK},  {"SHIFT", VK_SHIFT},      {"CTRL", VK_CONTROL},
      {"CONTROL", VK_CONTROL}, {"ALT", VK_MENU},         {"UP", VK_UP},
      {"DOWN", VK_DOWN},       {"LEFT", VK_LEFT},        {"RIGHT", VK_RIGHT},
      {"INSERT", VK_INSERT},   {"DELETE", VK_DELETE},    {"HOME", VK_HOME},
      {"END", VK_END},         {"PAGEUP", VK_PRIOR},     {"PAGEDOWN", VK_NEXT},
      {"PLUS", VK_OEM_PLUS},   {"MINUS", VK_OEM_MINUS},
  };
  std::string upper;
  for (char c : name) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (upper.size() == 1 && ((upper[0] >= 'A' && upper[0] <= 'Z') ||
                            (upper[0] >= '0' && upper[0] <= '9'))) {
    return upper[0];
  }
  if (upper.size() >= 2 && upper[0] == 'F') {
    int n = std::atoi(upper.c_str() + 1);
    if (n >= 1 && n <= 24) return VK_F1 + n - 1;
  }
  if (upper.rfind("NUMPAD", 0) == 0 && upper.size() == 7 && upper[6] >= '0' && upper[6] <= '9') {
    return VK_NUMPAD0 + (upper[6] - '0');
  }
  auto it = kNames.find(upper);
  return it == kNames.end() ? 0 : it->second;
#else
  (void)name;
  return 0;
#endif
}

int CheckKey(lua_State* L, int index) {
  if (lua_type(L, index) == LUA_TNUMBER) return static_cast<int>(lua_tointeger(L, index));
  const char* name = luaL_checkstring(L, index);
  int vk = KeyFromName(name);
  if (!vk) luaL_error(L, "unknown key name '%s'", name);
  return vk;
}

// ---------------------------------------------------------------------------
// Hook context object passed to Lua hook functions
// ---------------------------------------------------------------------------

struct LuaHookSlot {
  LuaMod* mod = nullptr;
  int function_ref = LUA_NOREF;
  HostFunction original = nullptr;
  uint32_t address = 0;
};

struct LuaContext {
  PPCContext* ctx;
  uint8_t* base;
  LuaHookSlot* slot;
  bool called_original;
};

constexpr const char* kContextMeta = "wml.context";

LuaContext* CheckContext(lua_State* L) {
  auto* c = static_cast<LuaContext*>(luaL_checkudata(L, 1, kContextMeta));
  if (!c->ctx) luaL_error(L, "the context can only be used inside its hook call");
  return c;
}

int CtxR(lua_State* L) {
  auto* c = CheckContext(L);
  lua_pushinteger(L, static_cast<lua_Integer>(GetR(*c->ctx, (int)luaL_checkinteger(L, 2))));
  return 1;
}
int CtxSetR(lua_State* L) {
  auto* c = CheckContext(L);
  SetR(*c->ctx, (int)luaL_checkinteger(L, 2), static_cast<uint64_t>(luaL_checkinteger(L, 3)));
  return 0;
}
int CtxF(lua_State* L) {
  auto* c = CheckContext(L);
  lua_pushnumber(L, GetF(*c->ctx, (int)luaL_checkinteger(L, 2)));
  return 1;
}
int CtxSetF(lua_State* L) {
  auto* c = CheckContext(L);
  SetF(*c->ctx, (int)luaL_checkinteger(L, 2), luaL_checknumber(L, 3));
  return 0;
}
int CtxLr(lua_State* L) {
  auto* c = CheckContext(L);
  lua_pushinteger(L, static_cast<lua_Integer>(static_cast<uint32_t>(c->ctx->lr)));
  return 1;
}
int CtxCallOriginal(lua_State* L) {
  auto* c = CheckContext(L);
  c->called_original = true;
  PPCContext& ctx = *c->ctx;
  uint8_t* base = c->base;
  HostFunction original = c->slot->original;
  {
    ReleaseForGameCall unlocked(c->slot->mod);
    original(ctx, base);
  }
  return 0;
}
int CtxCall(lua_State* L) {
  auto* c = CheckContext(L);
  uint32_t address = CheckAddress(L, 2);
  HostFunction fn = FindFunction(address);
  if (!fn) return luaL_error(L, "no game function at %08X", (unsigned)address);
  PPCContext& ctx = *c->ctx;
  uint8_t* base = c->base;
  {
    ReleaseForGameCall unlocked(c->slot->mod);
    fn(ctx, base);
  }
  return 0;
}

void RegisterContextMeta(lua_State* L) {
  luaL_newmetatable(L, kContextMeta);
  static const luaL_Reg kMethods[] = {
      {"r", CtxR},     {"set_r", CtxSetR}, {"f", CtxF},       {"set_f", CtxSetF},
      {"lr", CtxLr},   {"call_original", CtxCallOriginal},    {"call", CtxCall},
      {nullptr, nullptr}};
  lua_newtable(L);
  luaL_setfuncs(L, kMethods, 0);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
}

// A fixed pool of distinct native hook functions, one per Lua hook.
constexpr size_t kMaxLuaHooks = 256;
std::array<LuaHookSlot, kMaxLuaHooks> g_hook_slots;
size_t g_hook_slots_used = 0;
std::mutex g_hook_slots_mutex;

// Runs the Lua function of a hook. The game's own code only runs if the
// function calls ctx:call_original().
void RunLuaHook(LuaHookSlot& slot, PPCContext& ctx, uint8_t* base) {
  LuaMod* mod = slot.mod;
  ModLock lock(mod);
  // This OS thread's own Lua thread for the mod, created on first use and
  // kept for the life of the mod (anchored in the registry).
  thread_local std::vector<std::pair<LuaMod*, lua_State*>> t_threads;
  lua_State* L = nullptr;
  for (auto& t : t_threads) if (t.first == mod) { L = t.second; break; }
  if (!L) {
    L = lua_newthread(mod->L);
    luaL_ref(mod->L, LUA_REGISTRYINDEX);
    t_threads.emplace_back(mod, L);
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, slot.function_ref);
  auto* c = static_cast<LuaContext*>(lua_newuserdatauv(L, sizeof(LuaContext), 0));
  *c = {&ctx, base, &slot, false};
  luaL_setmetatable(L, kContextMeta);
  lua_pushvalue(L, -1);
  int context_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    char where[48];
    std::snprintf(where, sizeof(where), "hook %08X", (unsigned)slot.address);
    ReportError(mod, where, L);
  }
  // The context is only valid during this call.
  lua_rawgeti(L, LUA_REGISTRYINDEX, context_ref);
  static_cast<LuaContext*>(lua_touserdata(L, -1))->ctx = nullptr;
  lua_pop(L, 1);
  luaL_unref(L, LUA_REGISTRYINDEX, context_ref);
}

template <size_t I>
void HookThunk(PPCContext& ctx, uint8_t* base) {
  RunLuaHook(g_hook_slots[I], ctx, base);
}

template <size_t... I>
constexpr std::array<HostFunction, sizeof...(I)> MakeThunks(std::index_sequence<I...>) {
  return {&HookThunk<I>...};
}
constexpr auto kHookThunks = MakeThunks(std::make_index_sequence<kMaxLuaHooks>{});

// ---------------------------------------------------------------------------
// The wml table
// ---------------------------------------------------------------------------

int LLog(lua_State* L) {
  LuaMod* mod = ModOf(L);
  std::string text;
  int n = lua_gettop(L);
  for (int i = 1; i <= n; ++i) {
    if (i > 1) text += " ";
    text += luaL_tolstring(L, i, nullptr);
    lua_pop(L, 1);
  }
  Log(mod->name, text);
  return 0;
}

int LReadU8(lua_State* L) { lua_pushinteger(L, ReadU8(CheckAddress(L, 1))); return 1; }
int LReadU16(lua_State* L) { lua_pushinteger(L, ReadU16(CheckAddress(L, 1))); return 1; }
int LReadU32(lua_State* L) { lua_pushinteger(L, ReadU32(CheckAddress(L, 1))); return 1; }
int LReadF32(lua_State* L) { lua_pushnumber(L, ReadF32(CheckAddress(L, 1))); return 1; }
int LWriteU8(lua_State* L) {
  WriteU8(CheckAddress(L, 1), static_cast<uint8_t>(luaL_checkinteger(L, 2)));
  return 0;
}
int LWriteU16(lua_State* L) {
  WriteU16(CheckAddress(L, 1), static_cast<uint16_t>(luaL_checkinteger(L, 2)));
  return 0;
}
int LWriteU32(lua_State* L) {
  WriteU32(CheckAddress(L, 1), static_cast<uint32_t>(luaL_checkinteger(L, 2)));
  return 0;
}
int LWriteF32(lua_State* L) {
  WriteF32(CheckAddress(L, 1), static_cast<float>(luaL_checknumber(L, 2)));
  return 0;
}
int LKeyDown(lua_State* L) { lua_pushboolean(L, KeyDown(CheckKey(L, 1))); return 1; }
int LKeyPressed(lua_State* L) { lua_pushboolean(L, KeyPressed(CheckKey(L, 1))); return 1; }
int LTakeKey(lua_State* L) {
  TakeKey(CheckKey(L, 1), lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2) != 0);
  return 0;
}
int LForceKey(lua_State* L) { ForceKey(CheckKey(L, 1), lua_toboolean(L, 2) != 0); return 0; }
int LTurnCamera(lua_State* L) { TurnCamera(luaL_checknumber(L, 1)); return 0; }
// wml.set_look({sharpen, bloom, ...}) - see wml.h set_look; wml.set_look() = original picture.
int LSetLook(lua_State* L) {
  if (!lua_istable(L, 1)) {
    SetLook(nullptr, 0);
    return 0;
  }
  float v[32];
  int n = 0;
  for (; n < 32; ++n) {
    lua_rawgeti(L, 1, n + 1);
    if (!lua_isnumber(L, -1)) { lua_pop(L, 1); break; }
    v[n] = float(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  SetLook(v, n);
  return 0;
}
// wml.mouse_look() -> yaw, pitch: radians the mouse / right stick would have
// turned the camera since the last call (positive = right / up).
int LMouseLook(lua_State* L) {
  double yaw = 0, pitch = 0;
  TakeMouseLook(yaw, pitch);
  lua_pushnumber(L, yaw);
  lua_pushnumber(L, pitch);
  return 2;
}
// wml.limit_camera(yaw, pitch, yaw_limit, pitch_up, pitch_down) - radians;
// call every frame while it should apply. wml.limit_camera() turns it off.
int LLimitCamera(lua_State* L) {
  CameraLimit l;
  if (lua_gettop(L) >= 5) {
    l.active = true;
    l.yaw = luaL_checknumber(L, 1); l.pitch = luaL_checknumber(L, 2);
    l.yaw_limit = luaL_checknumber(L, 3); l.pitch_up = luaL_checknumber(L, 4);
    l.pitch_down = luaL_checknumber(L, 5);
  }
  SetCameraLimit(l);
  return 0;
}

int LSetting(lua_State* L) { return PushSetting(L, ModOf(L)->settings); }

int LOnFrame(lua_State* L) {
  LuaMod* mod = ModOf(L);
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_pushvalue(L, 1);
  mod->frame_callbacks.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
  return 0;
}

int LHook(lua_State* L) {
  LuaMod* mod = ModOf(L);
  uint32_t address = CheckAddress(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  size_t index;
  {
    std::lock_guard<std::mutex> lock(g_hook_slots_mutex);
    if (g_hook_slots_used >= kMaxLuaHooks) {
      return luaL_error(L, "too many Lua hooks (max %d)", (int)kMaxLuaHooks);
    }
    index = g_hook_slots_used++;
  }
  LuaHookSlot& slot = g_hook_slots[index];
  slot.mod = mod;
  slot.address = address;
  lua_pushvalue(L, 2);
  slot.function_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  if (InstallHook(address, kHookThunks[index], &slot.original) != 0) {
    return luaL_error(L, "could not hook %08X", (unsigned)address);
  }
  return 0;
}

void OpenWmlLibrary(LuaMod* mod, const ModInfo& info) {
  lua_State* L = mod->L;
  static const luaL_Reg kFunctions[] = {
      {"log", LLog},
      {"read_u8", LReadU8},       {"read_u16", LReadU16},
      {"read_u32", LReadU32},     {"read_f32", LReadF32},
      {"write_u8", LWriteU8},     {"write_u16", LWriteU16},
      {"write_u32", LWriteU32},   {"write_f32", LWriteF32},
      {"key_down", LKeyDown},     {"key_pressed", LKeyPressed},
      {"on_frame", LOnFrame},     {"hook", LHook},
      {"setting", LSetting},      {"take_key", LTakeKey},
      {"force_key", LForceKey},   {"turn_camera", LTurnCamera}, {"limit_camera", LLimitCamera},
      {"mouse_look", LMouseLook}, {"set_look", LSetLook},
      {nullptr, nullptr}};
  lua_newtable(L);
  lua_pushlightuserdata(L, mod);
  luaL_setfuncs(L, kFunctions, 1);
  lua_pushstring(L, info.name.c_str());
  lua_setfield(L, -2, "mod_name");
  lua_pushstring(L, PathToUtf8(info.folder).c_str());
  lua_setfield(L, -2, "mod_folder");
  lua_newtable(L);
  for (const auto& kv : info.settings) {
    lua_pushstring(L, kv.second.c_str());
    lua_setfield(L, -2, kv.first.c_str());
  }
  lua_setfield(L, -2, "settings");
  lua_setglobal(L, "wml");
  RegisterContextMeta(L);

  // Let require() find modules in the mod folder.
  lua_getglobal(L, "package");
  std::string path = PathToUtf8(info.folder) + "/?.lua;" + PathToUtf8(info.folder) + "/?/init.lua";
  lua_pushstring(L, path.c_str());
  lua_setfield(L, -2, "path");
  lua_pop(L, 1);
}

}  // namespace

void StartLuaMod(const ModInfo& info) {
  auto mod = std::make_unique<LuaMod>();
  mod->name = info.name;
  mod->settings = info.settings;
  mod->L = luaL_newstate();
  if (!mod->L) {
    Log(info.name, "Could not create a Lua state");
    return;
  }
  luaL_openlibs(mod->L);
  OpenWmlLibrary(mod.get(), info);

  ModLock lock(mod.get());
  std::string script = PathToUtf8(info.script);
  if (luaL_loadfile(mod->L, script.c_str()) != LUA_OK || lua_pcall(mod->L, 0, 0, 0) != LUA_OK) {
    ReportError(mod.get(), PathToUtf8(info.script.filename()).c_str());
  } else {
    Log(info.name, "Loaded " + PathToUtf8(info.script.filename()));
  }
  g_lua_mods.push_back(std::move(mod));
}

void LuaOnFrame() {
  for (auto& mod : g_lua_mods) {
    if (mod->frame_callbacks.empty()) continue;
    ModLock lock(mod.get());
    // Index loop: a callback may register more callbacks.
    for (size_t i = 0; i < mod->frame_callbacks.size(); ++i) {
      lua_rawgeti(mod->L, LUA_REGISTRYINDEX, mod->frame_callbacks[i]);
      if (lua_pcall(mod->L, 0, 0, 0) != LUA_OK) {
        ReportError(mod.get(), "on_frame");
      }
    }
  }
}

}  // namespace wml
