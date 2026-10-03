// Zombies (built in): a multiplayer game mode in the spirit of Black Ops
// zombies. In a multiplayer lobby (System Link, Player Match, Ranked, custom)
// the GAME list has "Zombies: Off / On" (the exe's coop_menu.cpp hands the
// choice over with ZombiesSetMatch). When the host's match loads with it on,
// this game (the host) runs rounds of zombies on the map: a countdown before
// each round (the game's HUD timer), more and tougher zombies every round,
// points for the damage done to them plus a kill bonus.
//
// Version 0.9: synced. The host runs the game and sends its state ~10 times a
// second over the runtime's mod channel (rexruntime SrModSend / SrModPoll,
// same virtual LAN as the game's packets): phase, round, countdown, every
// player's points, bought doors, and every zombie (id, model, position,
// health, dead). The other players make their own copy of each zombie (an
// attacking character, put back on the host's position when > 3 m off, its
// health kept at the host's, killed when the host's dies), see the same
// banner / countdown / prompts, and send the host their position, whether
// they're down, the damage they did to each zombie (running totals: a lost
// packet loses nothing) and buy requests (door / wall weapon; the host checks
// their points and answers in its state). Points are the host's: it puts
// everyone's on the scoreboard. Without the new runtime: host only, as before.
//
// Game functions used (see the co-op mod for the same calls):
//   82114928 character definition by name, 824C2CF8 + 82479AB0 make a
//   character, 823ACF50 remove an object, 820B04E8 AI order (62 = attack,
//   what attack_do 824C7228 sends), 824470D0 damage entry (f1 = amount),
//   82454A30 melee hit, 822EF440 objective line (UTF-16BE text),
//   822DA538 / 822DA9F8 HUD timer (hud_timer_set_do 824CE5D8, timer record
//   0x82820CB0), 82209E30 game update.
//   Character: +20 position, +56/+64 facing, +68 handle, +72 kind (1 =
//   human), +216 bit 0x4000 set up, +232 team (Playas 0, Los Carnales 1,
//   Vice Kings 2, Rollerz 3, Neutral 4, Police 5, Civilian 6), +1908 max /
//   +1912 current hit points, +2496 vehicle, +3692 bit 0x08 combat off,
//   +3695 bit 0x08 ordered attack, +3697 bit 0x10 attacks the player.
//   Players: 12 player objects of 22128 bytes at 0x83B57148. Multiplayer:
//   byte 0x8370E9F6; this game hosts the session: byte 0x8370E9DF == 1;
//   in a match: top-level mode [0x827D578C] == 13 (6 = menus / lobby).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>
#include <rex/ppc/context.h>
#include "wml.h"
#include "wml_pagequery.h"

namespace {

const WmlApi* api = nullptr;
const WmlMod* self = nullptr;

constexpr uint32_t kPlayerPtr = 0x8309ABECu;
constexpr uint32_t kAmbientPeds = 0x8309ABB4u;  // first ambient person; next at +3600
constexpr uint32_t kObjectTable = 0x830866C8u;
constexpr uint32_t kStreamTable = 0x83E876B0u;
constexpr uint32_t kMultiplayerFlag = 0x8370E9F6u;
constexpr uint32_t kPedSpawnFlag = 0x8309A365u, kPedSpawnTimer = 0x82832740u;
constexpr uint32_t kCarSpawnFlag = 0x8309A35Du, kCarSpawnTimer = 0x82832744u;
constexpr uint32_t kAmbientCops = 0x827AD06Cu, kAmbientGangs = 0x827AD06Du;
constexpr uint32_t kPoliceNotoriety = 0x828316F0u;  // + 645: no reinforcements
constexpr int kZombieTeam = 3;

void Log(const std::string& s) { api->log(self, s.c_str()); }

bool Readable(uint32_t address, size_t length) {
  if (!address || uint64_t(address) + length > UINT32_MAX) return false;
  return WmlHostRangeReadable(uintptr_t(api->guest_pointer(address)), length);
}

uint64_t NowMs() { return GetTickCount64(); }

struct GuestScratch {
  PPCContext ctx;
  uint32_t start;
  std::vector<uint8_t> saved;
  explicit GuestScratch(WmlContext* raw) {
    std::memcpy(&ctx, raw, sizeof(ctx));
    start = ctx.r1.u32 - 0x5000;
    saved.resize(0x5000);
    std::memcpy(saved.data(), api->guest_pointer(start), saved.size());
    ctx.r1.u32 -= 0x2000;
  }
  ~GuestScratch() { std::memcpy(api->guest_pointer(start), saved.data(), saved.size()); }
  uint32_t data() const { return start + 0x3800; }
  bool Call(uint32_t fn) { return api->call(reinterpret_cast<WmlContext*>(&ctx), fn) == 0; }
};

uint32_t Player() {
  const uint32_t p = api->read_u32(kPlayerPtr);
  return Readable(p, 4252) ? p : 0;
}
uint32_t ResolveHuman(uint32_t handle) {
  if (!handle || (handle & 0xffff) >= 4096) return 0;
  const uint32_t obj = api->read_u32(kObjectTable + 12 + (handle & 0xffff) * 16);
  return Readable(obj, 4252) && api->read_u32(obj + 68) == handle && api->read_u32(obj + 72) == 1 &&
                 obj != api->read_u32(kPlayerPtr)
             ? obj
             : 0;
}
bool Dead(uint32_t obj) { return api->read_f32(obj + 1912) <= 0.0f; }
bool SetUp(uint32_t obj) { return (api->read_u32(obj + 216) & 0x4000) != 0; }
float Dist2(uint32_t obj, const float* p) {
  const float dx = api->read_f32(obj + 20) - p[0], dz = api->read_f32(obj + 28) - p[2];
  return dx * dx + dz * dz;
}

uint32_t LookupModel(GuestScratch& s, const char* name) {
  const uint32_t buffer = s.data() + 1024;
  const size_t n = strnlen(name, 63);
  std::memcpy(api->guest_pointer(buffer), name, n);
  api->write_u8(buffer + uint32_t(n), 0);
  s.ctx.r3.u64 = buffer;
  s.ctx.r4.u64 = 0;
  return s.Call(0x82114928) ? s.ctx.r3.u32 : 0;
}

// A scripted character (kind 0) at pos, like the co-op mod's CreateActor.
uint32_t CreateCharacter(GuestScratch& s, uint32_t definition, const float* p, float yaw) {
  if (!Readable(definition, 612) || !Readable(api->read_u32(definition + 24), 9)) return 0;
  const uint32_t pos = s.data() + 1200, matrix = s.data() + 1216;
  for (int i = 0; i < 3; ++i) api->write_f32(pos + i * 4, p[i]);
  api->write_f32(pos + 12, 0);
  const float c = std::cos(yaw), sn = std::sin(yaw);
  const float rows[9] = {c, 0, -sn, 0, 1, 0, sn, 0, c};
  for (int i = 0; i < 9; ++i) api->write_f32(matrix + i * 4, rows[i]);
  s.ctx.r3.u64 = pos;
  if (!s.Call(0x824C2CF8)) return 0;
  api->write_u32(s.ctx.r1.u32 + 84, 0);
  api->write_u16(s.ctx.r1.u32 + 94, 8);
  api->write_u8(s.ctx.r1.u32 + 103, 0);
  s.ctx.r3.u64 = 0; s.ctx.r4.u64 = 2; s.ctx.r5.u64 = definition;
  s.ctx.r6.u64 = pos; s.ctx.r7.u64 = matrix; s.ctx.r8.u64 = 255;
  s.ctx.r9.u64 = 0; s.ctx.r10.u64 = 0;
  if (!s.Call(0x82479AB0)) return 0;
  const uint32_t obj = s.ctx.r3.u32;
  if (!Readable(obj, 4252) || obj == api->read_u32(kPlayerPtr)) return 0;
  const uint32_t handle = api->read_u32(obj + 68);
  if (ResolveHuman(handle) != obj) return 0;
  // Streaming reference on its model (the game would wait on it otherwise).
  const uint32_t desc = api->read_u32(obj + 228);
  if (Readable(desc, 612)) {
    const uint32_t id = api->read_u32(desc + 608);
    s.ctx.r3.u64 = kStreamTable + (id >> 24) * 204; s.ctx.r4.u64 = id;
    s.ctx.r5.u64 = 5; s.ctx.r6.u64 = 0; s.ctx.r7.u64 = 1;
    s.Call(0x8250C750);
  }
  return handle;
}

void RemoveObject(GuestScratch& s, uint32_t handle) {
  s.ctx.r3.u64 = handle;
  s.Call(0x823ACF50);
}

// Objective line at the top of the screen (UTF-16BE; empty = cleared).
std::string g_objective_shown;
void ShowObjective(GuestScratch& s, const std::string& text) {
  if (text == g_objective_shown) return;
  g_objective_shown = text;
  if (text.empty()) {
    s.ctx.r3.u64 = 0;
  } else {
    const uint32_t buf = s.data() + 2048;
    uint32_t n = 0;
    for (char c : text) {
      if (n >= 250) break;
      api->write_u16(buf + n * 2, uint16_t(uint8_t(c)));
      ++n;
    }
    api->write_u16(buf + n * 2, 0);
    s.ctx.r3.u64 = buf;
  }
  s.Call(0x822EF440);
}

// --- HUD timer (countdown to the next round) -------------------------------

constexpr uint32_t kHudTimer = 0x82820CB0u;
bool g_timer_on = false;
void StartCountdown(GuestScratch& s, uint32_t ms) {
  s.ctx.r3.u64 = 1; s.ctx.r4.u64 = 0; s.ctx.r5.u64 = 0; s.ctx.r6.u64 = kHudTimer;
  s.ctx.r7.u64 = ms; s.ctx.r8.u64 = 1; s.ctx.r9.u64 = 4; s.ctx.r10.u64 = ~0ull;
  s.Call(0x822DA538);
  s.Call(0x822DA9F8);
  g_timer_on = true;
}
void StopCountdown(GuestScratch& s) {
  if (!g_timer_on) return;
  api->write_u32(kHudTimer + 552, 0xFFFFFFFFu);
  api->write_u32(kHudTimer + 556, 0xFFFFFFFFu);
  api->write_u32(kHudTimer + 564, 0xFFFFFFFFu);
  api->write_u32(0x82820958u + 300, 0);
  s.Call(0x822DA9F8);
  g_timer_on = false;
}

// --- Game state ------------------------------------------------------------

constexpr uint32_t kPlayerArray = 0x83B57148u, kPlayerSize = 22128u;
constexpr uint32_t kScoreLimit = 0x8307F254u;  // the match's score limit (0 = none)
int g_score_given = 0;                          // points already on the scoreboard
constexpr uint32_t kHostFlag = 0x8370E9DFu;      // lis 0x8371, -5665: this game hosts the session
constexpr uint32_t kTopMode = 0x827D578Cu;       // top-level mode: 6 multiplayer menus / lobby, 13 in a match

struct Zombie {
  uint32_t handle = 0;
  uint16_t id = 0;        // sent to the other players
  uint8_t def = 0;        // 0 generic, 1 Lin
  bool hostile = false;   // team / flags set (after its model is set up)
  bool dead = false;
  bool counted = false;
  uint64_t made = 0, died = 0, next_order = 0;
  uint64_t progress = 0;      // last time it moved 1.5 m, was hit, or got close
  float last_pos[3] = {};
};

enum class Phase { kOff, kBreak, kRound, kGameOver };
Phase g_phase = Phase::kOff;
int g_round = 0, g_total = 0, g_spawned = 0, g_killed = 0;
double g_points = 0;
uint64_t g_phase_until = 0, g_next_spawn = 0, g_next_trail = 0, g_next_status = 0;
std::vector<Zombie> g_zombies;
std::vector<std::array<float, 3>> g_spots;
std::mt19937 g_rng{std::random_device{}()};
uint32_t g_definitions[2] = {};
bool g_match_over = false;   // this match ended (everyone down): no new game until the next match
bool g_match_wanted = false;  // the lobby's "Zombies: On" (exe)
double g_points_per_damage = 1.0;
int g_kill_points = 50, g_max_alive = 24;
uint32_t g_first_break = 10000, g_break = 8000;
thread_local bool g_in_melee = false;
DWORD g_game_thread = 0;  // the thread running the game update / damage
unsigned g_damage_logged = 0;
unsigned g_prompt_polls = 0;  // centre prompt line requests since the last status line

// --- Sync (runtime mod channel) ---------------------------------------------
constexpr uint32_t kChannel = 0x314D425Au;  // "ZBM1"
constexpr uint8_t kNetVersion = 1;
constexpr uint8_t kMsgState = 1, kMsgClient = 2;
using ModSendFn = int (*)(uint32_t, const void*, int, uint32_t);
using ModPollFn = int (*)(uint32_t, void*, int, uint32_t*);
using ModIpFn = uint32_t (*)();
ModSendFn g_net_send = nullptr;
ModPollFn g_net_poll = nullptr;
ModIpFn g_net_my_ip = nullptr;
bool NetReady() {
  static bool tried = false;
  if (!tried) {
    tried = true;
    if (HMODULE m = GetModuleHandleA("rexruntime.dll")) {
      g_net_send = reinterpret_cast<ModSendFn>(GetProcAddress(m, "SrModSend"));
      g_net_poll = reinterpret_cast<ModPollFn>(GetProcAddress(m, "SrModPoll"));
      g_net_my_ip = reinterpret_cast<ModIpFn>(GetProcAddress(m, "SrModMyIp"));
    }
    if (!g_net_send || !g_net_poll || !g_net_my_ip) {
      g_net_send = nullptr; g_net_poll = nullptr; g_net_my_ip = nullptr;
      api->log(self, "[Zombies] this runtime has no mod channel (SrModSend): zombies are not synced to other players");
    }
  }
  return g_net_send != nullptr;
}

struct Writer {
  std::vector<uint8_t> b;
  template <class T> void put(T v) { const auto* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + sizeof(T)); }
};
struct Reader {
  const uint8_t* p; size_t n, i = 0; bool ok = true;
  Reader(const uint8_t* d, size_t len) : p(d), n(len) {}
  template <class T> T get() {
    T v{};
    if (i + sizeof(T) > n) { ok = false; return v; }
    std::memcpy(&v, p + i, sizeof(T)); i += sizeof(T);
    return v;
  }
};

// Host: one per other player that sent us something.
struct Client {
  uint32_t ip = 0;
  uint32_t obj = 0;          // their player object in this game (found by position)
  float pos[3] = {};
  bool dead = false;
  double points = 0;
  int score_given = 0;
  std::vector<std::pair<uint16_t, float>> dmg;  // damage already counted, per zombie id
  uint16_t last_req = 0;
  uint8_t last_ok = 0;
  uint64_t seen = 0;
};
std::vector<Client> g_clients;
uint16_t g_next_zombie_id = 1;
uint64_t g_next_broadcast = 0;

// Other players: the host's game as received.
struct Copy {
  uint32_t handle = 0;
  uint8_t def = 0;
  float host_pos[3] = {};
  float host_hp = 0;
  bool host_dead = false;
  bool hostile = false;
  uint64_t seen = 0, made = 0, next_make = 0, next_fix = 0, next_order = 0, dead_here_since = 0;
};
bool g_client = false;          // in a match whose host runs zombies
uint32_t g_host_ip = 0;
uint64_t g_last_state = 0, g_next_client_send = 0;
std::map<uint16_t, Copy> g_copies;
struct MyHit { uint16_t id; float total; uint8_t flags; uint64_t at; };  // flags 1 killed, 2 melee
std::vector<MyHit> g_my_hits;
uint16_t g_req_seq = 0;          // our last buy request (0 = none)
uint8_t g_req_kind = 0;          // 1 door, 2 wall weapon
uint16_t g_req_index = 0;
uint8_t g_req_have = 0;
bool g_req_pending = false;
uint64_t g_doors_mask = 0;       // host's open doors, as last received
int CopyIdOf(uint32_t obj) {
  if (!Readable(obj, 4252)) return -1;
  const uint32_t h = api->read_u32(obj + 68);
  for (auto& [k, c] : g_copies) if (c.handle == h) return k;
  return -1;
}

// Zombies in a round: 6, 12, 18, ... (+6 a round) up to 150; at once at most
// max_alive, and 6 + 3 per round below that.
int RoundTotal(int r) { return std::min(6 + (r - 1) * 6, 150); }
int AliveCap(int r) { return std::min(g_max_alive, 6 + r * 3); }
// Zombie health by round (Black Ops style): health_round1, + health_per_round
// each round up to round 9, then x1.1 per round. A pistol shot does about 70.
float g_health_round1 = 150.0f, g_health_per_round = 100.0f;
float RoundHealth(int r) {
  if (r <= 9) return g_health_round1 + g_health_per_round * float(r - 1);
  return (g_health_round1 + g_health_per_round * 8.0f) * std::pow(1.1f, float(r - 9));
}
uint64_t SpawnGap(int r) { return uint64_t(std::max(450, 2200 - r * 150)); }

Zombie* FindZombie(uint32_t obj) {
  if (!Readable(obj, 4252)) return nullptr;
  const uint32_t h = api->read_u32(obj + 68);
  for (auto& z : g_zombies)
    if (z.handle == h) return &z;
  return nullptr;
}

// Players in the match (local and remote): player objects in use.
std::vector<uint32_t> Players() {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < 12; ++i) {
    const uint32_t obj = kPlayerArray + i * kPlayerSize;
    if (!Readable(obj, 4252) || api->read_u32(obj + 72) != 1) continue;
    const uint32_t h = api->read_u32(obj + 68);
    if (!h || (h & 0xffff) >= 4096 || api->read_u32(kObjectTable + 12 + (h & 0xffff) * 16) != obj) continue;
    if (!SetUp(obj)) continue;
    out.push_back(obj);
  }
  return out;
}

void RecordTrails(const std::vector<uint32_t>& players, uint64_t now) {
  if (now < g_next_trail) return;
  g_next_trail = now + 500;
  for (uint32_t pl : players) {
    if (api->read_u32(pl + 2496) || Dead(pl)) continue;
    const float p[3] = {api->read_f32(pl + 20), api->read_f32(pl + 24), api->read_f32(pl + 28)};
    bool close_by = false;
    for (const auto& s : g_spots) {
      const float dx = s[0] - p[0], dy = s[1] - p[1], dz = s[2] - p[2];
      if (dx * dx + dy * dy + dz * dz < 9.0f) { close_by = true; break; }
    }
    if (close_by) continue;
    g_spots.push_back({p[0], p[1], p[2]});
    if (g_spots.size() > 800) g_spots.erase(g_spots.begin());
  }
}

// A spot 12-45 m from every living player, out of their view where possible.
bool PickSpot(const std::vector<uint32_t>& players, float* out) {
  std::vector<size_t> hidden, ok, fallback;
  for (size_t i = 0; i < g_spots.size(); ++i) {
    const auto& s = g_spots[i];
    float nearest = 1e9f;
    bool seen = false;
    for (uint32_t pl : players) {
      if (Dead(pl)) continue;
      const float dx = s[0] - api->read_f32(pl + 20), dz = s[2] - api->read_f32(pl + 28);
      const float d2 = dx * dx + dz * dz;
      nearest = std::min(nearest, d2);
      const float d = std::sqrt(std::max(d2, 0.01f));
      if ((dx * api->read_f32(pl + 56) + dz * api->read_f32(pl + 64)) / d > 0.5f) seen = true;
    }
    if (nearest < 8.0f * 8.0f) continue;
    fallback.push_back(i);
    if (nearest < 12.0f * 12.0f || nearest > 45.0f * 45.0f) continue;
    ok.push_back(i);
    if (!seen) hidden.push_back(i);
  }
  const auto& pool = !hidden.empty() ? hidden : !ok.empty() ? ok : fallback;
  if (pool.empty()) return false;
  const auto& s = g_spots[pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(g_rng)]];
  out[0] = s[0] + std::uniform_real_distribution<float>(-1.0f, 1.0f)(g_rng);
  out[1] = s[1];
  out[2] = s[2] + std::uniform_real_distribution<float>(-1.0f, 1.0f)(g_rng);
  return true;
}


// --- Map data from the map editor (doors / paths, wall weapons, zombie spawns) ---
// The editor exports dist\maps\<pack>\levels\<map>.zombies.txt. The loaded map is
// found by the box of its first mesh among the loaded chunks' static records
// (list 0x829A97E8: +0 count, +16 chunk pointers; chunk +212 record count, +216
// records of 80 bytes: box min +0, max +16, instance +60). A door's pieces are
// their own records: bought, they are hidden like World Studio deletes a piece
// (instance rotation +16..+48 zeroed, box moved away). They have no baked
// collision: while closed, players and zombies are pushed out of their boxes.
constexpr uint32_t kChunkList = 0x829A97E8u;
struct Box { float mn[3], mx[3]; };
struct ZDoor {
  std::string name;
  int cost = 750;
  std::vector<Box> pieces;
  std::vector<uint32_t> records;  // matching static records in the loaded chunk
  bool open = false;
};
struct ZWallWeapon { std::string type; int cost = 1000; float pos[3] = {}; float yaw = 0; };
struct ZSpawn { float pos[3] = {}; std::string link; };
struct ZMap {
  bool loaded = false;
  std::string file;
  Box signature{};
  std::string chunk_name;  // "chunk <name>": the map is found by its chunk's name instead of a box
  std::vector<ZDoor> doors;
  std::vector<ZWallWeapon> weapons;
  std::vector<ZSpawn> spawns;
};
ZMap g_map;

bool SameBox(const Box& a, uint32_t rec) {
  for (int i = 0; i < 3; ++i)
    if (std::fabs(api->read_f32(rec + i * 4) - a.mn[i]) > 0.02f || std::fabs(api->read_f32(rec + 16 + i * 4) - a.mx[i]) > 0.02f)
      return false;
  return true;
}

bool ParseMapFile(const std::string& path, ZMap& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char line[512];
  bool header = false, have_sig = false;
  while (std::fgets(line, sizeof(line), f)) {
    std::string l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    if (l.rfind("saints-reborn-zombies", 0) == 0) { header = true; continue; }
    if (l.empty() || l.rfind("//", 0) == 0) continue;
    char rest[256] = "";
    Box b{};
    int idx = 0, cost = 0, n = 0;
    if (std::sscanf(l.c_str(), "map %f %f %f %f %f %f", &b.mn[0], &b.mn[1], &b.mn[2], &b.mx[0], &b.mx[1], &b.mx[2]) == 6) {
      out.signature = b; have_sig = true;
    } else if (l.rfind("chunk ", 0) == 0) {
      out.chunk_name = l.substr(6);
      while (!out.chunk_name.empty() && out.chunk_name.back() == ' ') out.chunk_name.pop_back();
      have_sig = !out.chunk_name.empty();
    } else if (std::sscanf(l.c_str(), "door %d %d %n", &idx, &cost, &n) == 2 && n > 0) {
      if (idx < 0 || idx > 1000) continue;
      if (out.doors.size() <= size_t(idx)) out.doors.resize(size_t(idx) + 1);
      out.doors[size_t(idx)].cost = cost;
      out.doors[size_t(idx)].name = l.substr(size_t(n));
    } else if (std::sscanf(l.c_str(), "piece %d %f %f %f %f %f %f", &idx, &b.mn[0], &b.mn[1], &b.mn[2], &b.mx[0], &b.mx[1], &b.mx[2]) == 7) {
      if (idx < 0 || idx > 1000) continue;
      if (out.doors.size() <= size_t(idx)) out.doors.resize(size_t(idx) + 1);
      out.doors[size_t(idx)].pieces.push_back(b);
    } else if (l.rfind("weapon ", 0) == 0) {
      ZWallWeapon w;
      if (std::sscanf(l.c_str(), "weapon %d %f %f %f %f %n", &w.cost, &w.pos[0], &w.pos[1], &w.pos[2], &w.yaw, &n) == 5 && n > 0) {
        w.type = l.substr(size_t(n));
        out.weapons.push_back(w);
      }
    } else if (l.rfind("zspawn ", 0) == 0) {
      ZSpawn z;
      if (std::sscanf(l.c_str(), "zspawn %f %f %f %n", &z.pos[0], &z.pos[1], &z.pos[2], &n) == 3 && n > 0) {
        z.link = l.substr(size_t(n));
        if (z.link == "-") z.link.clear();
        out.spawns.push_back(z);
      }
    }
    (void)rest;
  }
  std::fclose(f);
  out.file = path;
  return header && have_sig;
}

// The chunk holding the map's first mesh, or 0.
uint32_t FindMapChunk(const Box& sig) {
  const uint32_t count = api->read_u32(kChunkList);
  if (count > 512) return 0;
  for (uint32_t c = 0; c < count; ++c) {
    const uint32_t chunk = api->read_u32(kChunkList + 16 + c * 4);
    if (!Readable(chunk, 416)) continue;
    const uint32_t n = api->read_u32(chunk + 212), recs = api->read_u32(chunk + 216);
    if (!n || n > 65536 || !Readable(recs, size_t(n) * 80)) continue;
    for (uint32_t i = 0; i < n; ++i)
      if (SameBox(sig, recs + i * 80)) return chunk;
  }
  return 0;
}

// The loaded chunk with this name (chunk +16 = its name), or 0.
uint32_t FindChunkByName(const std::string& name) {
  const uint32_t count = api->read_u32(kChunkList);
  if (count > 512) return 0;
  for (uint32_t c = 0; c < count; ++c) {
    const uint32_t chunk = api->read_u32(kChunkList + 16 + c * 4);
    if (!Readable(chunk, 416)) continue;
    const uint32_t np = api->read_u32(chunk + 16);
    if (!Readable(np, 64)) continue;
    const char* n = static_cast<const char*>(api->guest_pointer(np));
    if (strnlen(n, 64) == name.size() && _strnicmp(n, name.c_str(), name.size()) == 0) return chunk;
  }
  return 0;
}

// A Zombies map file whose map is loaded now (by chunk name or by the box of
// its first mesh), with its door pieces matched to the chunk's records.
bool TryMapFile(const std::string& path) {
  ZMap m;
  if (!ParseMapFile(path, m)) return false;
  const uint32_t chunk = m.chunk_name.empty() ? FindMapChunk(m.signature) : FindChunkByName(m.chunk_name);
  if (!chunk) return false;
  const uint32_t n = api->read_u32(chunk + 212), recs = api->read_u32(chunk + 216);
  for (auto& d : m.doors)
    for (const Box& b : d.pieces)
      for (uint32_t i = 0; i < n && n <= 65536; ++i)
        if (SameBox(b, recs + i * 80)) { d.records.push_back(recs + i * 80); break; }
  m.loaded = true;
  g_map = std::move(m);
  return true;
}

// Reads the Zombies files (this mod's maps folder for the game's own maps,
// then every map pack's) and keeps the one whose map is loaded.
void LoadMapData() {
  g_map = ZMap{};
  {
    const std::string own = std::string(self->folder) + "\\maps\\";
    WIN32_FIND_DATAA zf;
    HANDLE hz = FindFirstFileA((own + "*.zombies.txt").c_str(), &zf);
    if (hz != INVALID_HANDLE_VALUE) {
      do {
        if (TryMapFile(own + zf.cFileName)) break;
      } while (FindNextFileA(hz, &zf));
      FindClose(hz);
    }
  }
  if (g_map.loaded) {
    Log("[Zombies] map data " + g_map.file + ": " + std::to_string(g_map.doors.size()) + " doors, " +
        std::to_string(g_map.weapons.size()) + " wall weapons, " + std::to_string(g_map.spawns.size()) + " zombie spawns");
    return;
  }
  const std::string maps = std::string(self->folder) + "\\..\\..\\maps\\";
  WIN32_FIND_DATAA pack;
  HANDLE hp = FindFirstFileA((maps + "*").c_str(), &pack);
  if (hp == INVALID_HANDLE_VALUE) return;
  int files = 0;
  do {
    if (!(pack.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || pack.cFileName[0] == '.') continue;
    const std::string levels = maps + pack.cFileName + "\\levels\\";
    WIN32_FIND_DATAA zf;
    HANDLE hz = FindFirstFileA((levels + "*.zombies.txt").c_str(), &zf);
    if (hz == INVALID_HANDLE_VALUE) continue;
    do {
      ++files;
      TryMapFile(levels + zf.cFileName);
    } while (!g_map.loaded && FindNextFileA(hz, &zf));
    FindClose(hz);
  } while (!g_map.loaded && FindNextFileA(hp, &pack));
  FindClose(hp);
  if (g_map.loaded) {
    size_t found = 0, pieces = 0;
    for (auto& d : g_map.doors) { found += d.records.size(); pieces += d.pieces.size(); }
    Log("[Zombies] map data " + g_map.file + ": " + std::to_string(g_map.doors.size()) + " doors (" + std::to_string(found) + "/" +
        std::to_string(pieces) + " pieces found), " + std::to_string(g_map.weapons.size()) + " wall weapons, " +
        std::to_string(g_map.spawns.size()) + " zombie spawns");
  } else if (files) {
    static int told = 0;
    if (told++ < 2) Log("[Zombies] " + std::to_string(files) + " Zombies map file(s) in dist\\maps, none for this map");
  }
}

void HideDoor(ZDoor& d) {
  for (uint32_t rec : d.records) {
    const uint32_t inst = api->read_u32(rec + 60);
    if (Readable(inst, 96))
      for (int i = 0; i < 9; ++i) api->write_f32(inst + 16 + i * 4, 0.0f);
    for (int i = 0; i < 3; ++i) {
      api->write_f32(rec + i * 4, i == 1 ? -5000.0f : 0.0f);
      api->write_f32(rec + 16 + i * 4, i == 1 ? -5000.0f : 0.0f);
    }
  }
  d.open = true;
}

bool DoorOpen(const std::string& name) {
  for (auto& d : g_map.doors)
    if (d.name == name) return d.open;
  return true;  // unknown door: don't hold the spawn back
}

// Moves a human like the game teleports characters (8243E6F8: object, position, 0, nav node -1).
void Teleport(GuestScratch& s, uint32_t obj, const float* p) {
  const uint32_t pos = s.data() + 1400;
  for (int i = 0; i < 3; ++i) api->write_f32(pos + i * 4, p[i]);
  s.ctx.r3.u64 = obj; s.ctx.r4.u64 = pos; s.ctx.r5.u64 = 0; s.ctx.r6.u64 = ~0ull;
  s.Call(0x8243E6F8);
}

// Closed doors: anyone inside a piece's box (grown by their radius) is put back
// out on the nearer side.
void BlockClosedDoors(GuestScratch& s, const std::vector<uint32_t>& people) {
  constexpr float r = 0.4f;
  for (auto& d : g_map.doors) {
    if (d.open) continue;
    for (const Box& b : d.pieces) {
      for (uint32_t obj : people) {
        if (!Readable(obj, 4252) || Dead(obj)) continue;
        float p[3] = {api->read_f32(obj + 20), api->read_f32(obj + 24), api->read_f32(obj + 28)};
        if (p[1] + 1.6f < b.mn[1] || p[1] > b.mx[1] + 0.2f) continue;
        const float x0 = b.mn[0] - r, x1 = b.mx[0] + r, z0 = b.mn[2] - r, z1 = b.mx[2] + r;
        if (p[0] <= x0 || p[0] >= x1 || p[2] <= z0 || p[2] >= z1) continue;
        const float push[4] = {p[0] - x0, x1 - p[0], p[2] - z0, z1 - p[2]};
        int k = 0;
        for (int i = 1; i < 4; ++i) if (push[i] < push[k]) k = i;
        if (k == 0) p[0] = x0 - 0.05f;
        else if (k == 1) p[0] = x1 + 0.05f;
        else if (k == 2) p[2] = z0 - 0.05f;
        else p[2] = z1 + 0.05f;
        Teleport(s, obj, p);
      }
    }
  }
}

// Wall weapons: names as the lobby / HUD would say them.
std::string WeaponLabel(const std::string& type) {
  static const std::pair<const char*, const char*> names[] = {
      {"beretta", "Beretta"}, {"glock", "Glock"}, {"desert eagle", "Desert Eagle"}, {"magnum", "Magnum"},
      {"tec9", "Tec-9"}, {"mac10", "Mac-10"}, {"pump_action_shotgun", "Pump Shotgun"}, {"twelve_gauge", "Twelve Gauge"},
      {"spas12", "SPAS-12"}, {"m16", "M16"}, {"ak47", "AK-47"}, {"sniper_rifle", "Sniper Rifle"},
      {"rpg_launcher", "RPG"}, {"grenade", "Grenades"}, {"molotov", "Molotovs"}, {"pipe_bomb", "Pipe Bombs"},
      {"baseball_bat", "Baseball Bat"}, {"knife", "Knife"}, {"nightstick", "Nightstick"}};
  for (auto& n : names)
    if (type == n.first) return n.second;
  return type;
}

uint32_t GuestName(GuestScratch& s, const std::string& name) {
  const uint32_t buf = s.data() + 2048;
  const size_t n = std::min<size_t>(name.size(), 63);
  std::memcpy(api->guest_pointer(buf), name.data(), n);
  api->write_u8(buf + uint32_t(n), 0);
  return buf;
}

bool HasWeapon(GuestScratch& s, uint32_t p, const std::string& type) {
  const uint32_t name = GuestName(s, type);
  s.ctx.r3.u64 = p; s.ctx.r4.u64 = name;
  if (s.Call(0x82446200) && s.ctx.r3.u32) return true;
  s.ctx.r3.u64 = p; s.ctx.r4.u64 = name;
  return s.Call(0x8245C8B8) && s.ctx.r3.u32;
}

// As inv_item_add / inv_item_add_ammo do (same calls as the Superpowers mod):
// 823F7840(name) item info -> 82445C80(human, info, 1); ammo: 825BEC40(name)
// weapon info -> 823F7958 -> 823F79F0(human, ammo type) -> 82445B88(human, record, rounds, 0).
bool GiveWeapon(GuestScratch& s, uint32_t p, const std::string& type, bool weapon, int rounds) {
  if (weapon) {
    s.ctx.r3.u64 = GuestName(s, type);
    if (!s.Call(0x823F7840) || !s.ctx.r3.u32) return false;
    const uint32_t info = s.ctx.r3.u32;
    s.ctx.r3.u64 = p; s.ctx.r4.u64 = info; s.ctx.r5.u64 = 1;
    if (!s.Call(0x82445C80) || !s.ctx.r3.u32) return false;
  }
  s.ctx.r3.u64 = GuestName(s, type);
  if (!s.Call(0x825BEC40) || !s.ctx.r3.u32) return weapon;
  s.Call(0x823F7958);
  s.ctx.r4.u64 = s.ctx.r3.u32; s.ctx.r3.u64 = p;
  if (!s.Call(0x823F79F0) || !s.ctx.r3.u32) return weapon;
  s.ctx.r4.u64 = s.ctx.r3.u32; s.ctx.r3.u64 = p; s.ctx.r5.u64 = uint64_t(rounds); s.ctx.r6.u64 = 0;
  s.Call(0x82445B88);
  return true;
}

// The use button: E on the keyboard, Y on a controller (pressed this update).
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);
bool UsePressed() {
  static bool was = false;
  static XInputGetStateFn xget = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
      if (HMODULE h = LoadLibraryA(dll)) { xget = reinterpret_cast<XInputGetStateFn>(GetProcAddress(h, "XInputGetState")); if (xget) break; }
  }
  bool down = api->key_down('E') != 0;
  if (xget) {
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st{};
    if (xget(0, &st) == 0 && (st.buttons & 0x8000)) down = true;  // XINPUT_GAMEPAD_Y
  }
  const bool pressed = down && !was;
  was = down;
  return pressed;
}

// What the player can buy where they stand (shown on the prompt line).
std::string g_buy_text, g_buy_note;
uint64_t g_note_until = 0;
void Note(const std::string& text) { g_buy_note = text; g_note_until = NowMs() + 1500; }
uint64_t g_req_since = 0;
// Other players: a buy goes to the host (sent with every update until it answers).
void Request(uint8_t kind, uint16_t index, uint8_t have) {
  if (g_req_pending) { Note("Please wait"); return; }
  if (++g_req_seq == 0) g_req_seq = 1;
  g_req_kind = kind; g_req_index = index; g_req_have = have;
  g_req_pending = true;
  g_req_since = NowMs();
  g_next_client_send = 0;
}
void Prompt(GuestScratch& s, uint32_t me);
void UpdateBuying(GuestScratch& s, uint32_t me) {
  Prompt(s, me);
  if (NowMs() < g_note_until) g_buy_text = g_buy_note;
}
void Prompt(GuestScratch& s, uint32_t me) {
  g_buy_text.clear();
  const bool press = UsePressed();
  if (!g_map.loaded || !me || Dead(me) || api->read_u32(me + 2496)) return;
  const float p[3] = {api->read_f32(me + 20), api->read_f32(me + 24), api->read_f32(me + 28)};
  char line[128];
  // doors first: the nearest closed one within 1.5 m of its box
  ZDoor* door = nullptr;
  float best = 1.5f * 1.5f;
  for (auto& d : g_map.doors) {
    if (d.open) continue;
    for (const Box& b : d.pieces) {
      if (p[1] + 1.6f < b.mn[1] - 0.5f || p[1] > b.mx[1] + 0.5f) continue;
      const float dx = std::max({b.mn[0] - p[0], 0.0f, p[0] - b.mx[0]}), dz = std::max({b.mn[2] - p[2], 0.0f, p[2] - b.mx[2]});
      if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; door = &d; }
    }
  }
  if (door) {
    std::snprintf(line, sizeof(line), "Press E to open %s [Cost: %d]", door->name.c_str(), door->cost);
    g_buy_text = line;
    if (press && g_client) {
      if (g_points >= door->cost) Request(1, uint16_t(door - g_map.doors.data()), 0);
      else Note("Not enough points");
    } else if (press) {
      if (g_points >= door->cost) {
        g_points -= door->cost;
        HideDoor(*door);
        Note(door->name + " opened");
        Log("[Zombies] bought " + door->name + " for " + std::to_string(door->cost));
      } else {
        Note("Not enough points");
      }
    }
    return;
  }
  const ZWallWeapon* wall = nullptr;
  best = 1.6f * 1.6f;
  for (auto& w : g_map.weapons) {
    const float dx = w.pos[0] - p[0], dz = w.pos[2] - p[2];
    if (std::fabs(w.pos[1] - (p[1] + 1.0f)) > 2.0f) continue;
    if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; wall = &w; }
  }
  if (!wall) return;
  const bool have = HasWeapon(s, me, wall->type);
  const int cost = have ? std::max(1, wall->cost / 2) : wall->cost;
  std::snprintf(line, sizeof(line), have ? "Press E to buy %s ammo [Cost: %d]" : "Press E to buy %s [Cost: %d]",
                WeaponLabel(wall->type).c_str(), cost);
  g_buy_text = line;
  if (!press) return;
  if (g_points < cost) { Note("Not enough points"); return; }
  if (g_client) { Request(2, uint16_t(wall - g_map.weapons.data()), have ? 1 : 0); return; }
  if (GiveWeapon(s, me, wall->type, !have, 300)) {
    g_points -= cost;
    Note(have ? "Ammo bought" : WeaponLabel(wall->type) + " bought");
    Log("[Zombies] bought " + wall->type + (have ? " ammo" : "") + " for " + std::to_string(cost));
  } else {
    Log("[Zombies] couldn't give " + wall->type);
  }
}

// Zombie spawns from the map (those whose door is bought), spread over the
// whole map: any spot 12-75 m from the nearest living player (else any spot
// at least 8 m away, else the farthest), never one of the last 4 used.
std::vector<size_t> g_recent_spawns;
bool PickMapSpawn(const std::vector<uint32_t>& players, float* out) {
  std::vector<size_t> good, ok;
  size_t farthest = SIZE_MAX;
  float far_d = -1.0f;
  for (size_t i = 0; i < g_map.spawns.size(); ++i) {
    const ZSpawn& z = g_map.spawns[i];
    if (!z.link.empty() && !DoorOpen(z.link)) continue;
    float nearest = 1e12f;
    for (uint32_t pl : players)
      if (!Dead(pl)) nearest = std::min(nearest, Dist2(pl, z.pos));
    if (nearest > far_d) { far_d = nearest; farthest = i; }
    if (std::find(g_recent_spawns.begin(), g_recent_spawns.end(), i) != g_recent_spawns.end()) continue;
    if (nearest >= 12.0f * 12.0f && nearest <= 75.0f * 75.0f) good.push_back(i);
    else if (nearest >= 8.0f * 8.0f) ok.push_back(i);
  }
  const auto& pool = !good.empty() ? good : ok;
  size_t pick;
  if (!pool.empty()) pick = pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(g_rng)];
  else if (farthest != SIZE_MAX) pick = farthest;
  else return false;
  g_recent_spawns.push_back(pick);
  if (g_recent_spawns.size() > 4) g_recent_spawns.erase(g_recent_spawns.begin());
  const ZSpawn* z = &g_map.spawns[pick];
  out[0] = z->pos[0] + std::uniform_real_distribution<float>(-0.6f, 0.6f)(g_rng);
  out[1] = z->pos[1];
  out[2] = z->pos[2] + std::uniform_real_distribution<float>(-0.6f, 0.6f)(g_rng);
  return true;
}

uint32_t NearestPlayer(uint32_t z, const std::vector<uint32_t>& players) {
  uint32_t best = 0;
  float best_d = 1e12f;
  const float p[3] = {api->read_f32(z + 20), api->read_f32(z + 24), api->read_f32(z + 28)};
  for (uint32_t pl : players) {
    if (Dead(pl)) continue;
    const float d = Dist2(pl, p);
    if (d < best_d) { best_d = d; best = pl; }
  }
  return best;
}

void OrderAttack(GuestScratch& s, uint32_t z, uint32_t target) {
  api->write_u8(z + 3695, uint8_t(api->read_u8(z + 3695) | 0x08));
  const uint32_t msg = s.data() + 256;
  api->write_u32(msg, 0);
  api->write_u8(msg, 0x80);
  api->write_u32(msg + 4, api->read_u32(target + 68));
  const uint32_t h = api->read_u32(z + 68);
  s.ctx.r3.u64 = 62; s.ctx.r4.u64 = h; s.ctx.r5.u64 = h; s.ctx.r6.u64 = 0;
  s.ctx.r7.u64 = ~0ull; s.ctx.r8.u64 = 0; s.ctx.r9.u64 = msg;
  s.Call(0x820B04E8);
}

void MakeHostile(GuestScratch& s, uint32_t z, uint32_t target, float health) {
  api->write_u32(z + 232, kZombieTeam);
  api->write_u8(z + 3692, uint8_t(api->read_u8(z + 3692) & ~0x08));
  api->write_u8(z + 3697, uint8_t((api->read_u8(z + 3697) | 0x10) & ~0x20));
  if (health > 0) {
    api->write_u32(z + 1908, uint32_t(int32_t(health)));
    api->write_f32(z + 1912, health);
  }
  if (target) OrderAttack(s, z, target);
}

void HideHud();
void NoLimits();
void ClearZombies(GuestScratch& s) {
  for (auto& z : g_zombies)
    if (ResolveHuman(z.handle)) RemoveObject(s, z.handle);
  g_zombies.clear();
}

int LookupDefinitions(GuestScratch& s) {
  // The generic zombie's character name isn't known for sure (its table is
  // GFL_Zombie.xtbl); a few spellings are tried.
  const char* generic[] = {"GFL_Zombie", "GFL_Zombie1", "GFL_Zombie-01", "PD_X_F_Zombie", "Zombie"};
  g_definitions[0] = 0;
  for (const char* n : generic)
    if ((g_definitions[0] = LookupModel(s, n))) { Log(std::string("[Zombies] character ") + n + " found"); break; }
  if (!g_definitions[0]) Log("[Zombies] no generic zombie character found");
  g_definitions[1] = LookupModel(s, "TS_A_F_zombieLin");
  Log(std::string("[Zombies] character TS_A_F_zombieLin ") + (g_definitions[1] ? "found" : "not found"));
  return (g_definitions[0] ? 1 : 0) + (g_definitions[1] ? 1 : 0);
}

void StartGame(GuestScratch& s) {
  const int found = LookupDefinitions(s);
  g_clients.clear();
  g_next_zombie_id = 1;
  NetReady();
  if (!found) {
    ShowObjective(s, "Zombies: no zombie characters in this game");
    return;
  }
  g_spots.clear();
  g_zombies.clear();
  LoadMapData();
  g_round = 0; g_points = 0; g_score_given = 0;
  g_phase = Phase::kBreak;
  g_phase_until = NowMs() + g_first_break;
  NoLimits();
  Log("[Zombies] match started (this game hosts): first round in " + std::to_string(g_first_break / 1000) + " s");
}

void HostBroadcast(uint64_t now, bool force);
void DrawWallWeaponBeams(uint32_t me);
void StopGame(GuestScratch& s, const char* why) {
  ClearZombies(s);
  StopCountdown(s);
  HideHud();
  g_phase = Phase::kOff;
  DrawWallWeaponBeams(0);
  HostBroadcast(NowMs(), true);  // the other players stop too
  g_clients.clear();
  Log(std::string("[Zombies] stopped (") + why + "), round " + std::to_string(g_round) + ", " +
      std::to_string(int(g_points)) + " points");
}

// The countdown and round banner use the game's own respawn countdown line
// ("Respawning in 3": sub_822E2928 fills its text and says whether to show
// it; the HUD draws it yellow, centred, above the middle). Points go to the
// scoreboard's SCORE (below).
uint64_t g_banner_until = 0;  // "Round N" shown until then
bool g_respawn_held = false;  // this player died during a round: no respawn until the next round starts

// Respawn hold. A player's match record (list from [0x8307F25C], next +52,
// player at +32: what 823D6BB0 walks) has the respawn timer at +16 (the
// game's timer: clock value [0x827AA6E0] to fire at, < 0 = off; the
// "Respawning in" line reads it). Dead in a round: the timer is switched off;
// when the next round starts it's set to now, and the game respawns them.
constexpr uint32_t kClock = 0x827AA6E0u;
uint32_t MatchRecord(uint32_t player) {
  const uint32_t head = api->read_u32(0x8307F25Cu);
  uint32_t r = head;
  for (int guard = 0; r && guard < 64 && Readable(r, 56); ++guard) {
    if (api->read_u32(r + 32) == player) return r;
    r = api->read_u32(r + 52);
    if (r == head) break;
  }
  return 0;
}
void HoldRespawn(uint32_t player) {
  const uint32_t rec = MatchRecord(player);
  if (!rec) return;
  if (int32_t(api->read_u32(rec + 16)) >= 0) api->write_u32(rec + 16, 0xFFFFFFFFu);
  if (!g_respawn_held) Log("[Zombies] player down: respawn held until the next round");
  g_respawn_held = true;
}
void ReleaseRespawn(uint32_t player) {
  if (!g_respawn_held) return;
  g_respawn_held = false;
  if (const uint32_t rec = MatchRecord(player)) api->write_u32(rec + 16, api->read_u32(kClock));
  Log("[Zombies] respawn released (new round)");
}
void Hud(GuestScratch& s) { (void)s; }
void HideHud() {}
bool BannerText(std::u16string& out) {
  if (g_phase == Phase::kOff) return false;
  const uint64_t now = NowMs();
  char line[160];
  if (g_phase == Phase::kGameOver) {
    std::snprintf(line, sizeof(line), "Game Over - you reached round %d", g_round);
  } else if (g_respawn_held) {
    std::snprintf(line, sizeof(line), "You respawn when the next round starts");
  } else if (!g_buy_text.empty()) {
    std::snprintf(line, sizeof(line), "%s", g_buy_text.c_str());
  } else if (g_phase == Phase::kBreak) {
    const int left = g_phase_until > now ? int((g_phase_until - now + 999) / 1000) : 0;
    std::snprintf(line, sizeof(line), "Round %d starts in %d", g_round + 1, left);
  } else if (now < g_banner_until) {
    std::snprintf(line, sizeof(line), "Round %d - %d zombies", g_round, RoundTotal(g_round));
  } else {
    return false;
  }
  out.clear();
  for (const char* c = line; *c; ++c) out.push_back(char16_t(uint8_t(*c)));
  return true;
}

// Points -> the player's scoreboard SCORE: what mp_change_score does on the
// host (823D6C28(player, change, -1) and 823DAC08(player, change) to tell the
// other players). The score limit is switched off for the match (0 at
// 0x8307F254: no limit, the HUD drops "SCORE LIMIT").
// No score or time limit in a zombie match: score limit [0x8307F254] and time
// limit [0x8307F250] (seconds) 0, the match's end time [0x8307F24C] (set by
// the mode's start, sub_823CCFE0: limit x 1000 + clock [0x83061944]) 0 - the
// mode's update (sub_823CD720) and end checks skip a 0 end time - and the
// match clock on the HUD (timer 0x8282FB80, +552 running) stopped.
void NoLimits() {
  static bool told = false;
  if (!told && (api->read_u32(kScoreLimit) || api->read_u32(0x8307F250u))) {
    told = true;
    Log("[Zombies] score limit " + std::to_string(api->read_u32(kScoreLimit)) + " and time limit " +
        std::to_string(api->read_u32(0x8307F250u)) + " s switched off for the zombie match");
  }
  if (api->read_u32(kScoreLimit)) api->write_u32(kScoreLimit, 0);
  if (api->read_u32(0x8307F250u)) api->write_u32(0x8307F250u, 0);
  if (api->read_u32(0x8307F24Cu)) api->write_u32(0x8307F24Cu, 0);
  if (api->read_u32(0x8282FB80u + 552)) api->write_u32(0x8282FB80u + 552, 0);
}
// The match records list (head [0x8307F25C], circular: next +52, prev +56)
// is what the scoreboard and the HUD leader line read, top to bottom. The
// game only re-sorts it in score modes; keep it highest score first.
void SortRecords() {
  const uint32_t head = api->read_u32(0x8307F25Cu);
  if (!head || !Readable(head, 60)) return;
  std::vector<uint32_t> recs;
  uint32_t r = head;
  for (int guard = 0; r && guard < 64 && Readable(r, 60); ++guard) {
    recs.push_back(r);
    r = api->read_u32(r + 52);
    if (r == head) break;
  }
  if (r != head || recs.size() < 2) return;
  auto score = [](uint32_t rec) { return int16_t(api->read_u16(rec + 2)); };
  bool sorted = true;
  for (size_t i = 1; i < recs.size(); ++i)
    if (score(recs[i]) > score(recs[i - 1])) sorted = false;
  if (sorted) return;
  std::stable_sort(recs.begin(), recs.end(), [&](uint32_t a, uint32_t b) { return score(a) > score(b); });
  const size_t n = recs.size();
  for (size_t i = 0; i < n; ++i) {
    api->write_u32(recs[i] + 52, recs[(i + 1) % n]);
    api->write_u32(recs[i] + 56, recs[(i + n - 1) % n]);
  }
  api->write_u32(0x8307F25Cu, recs[0]);
}

// Points -> the scoreboard SCORE (int16 in the player's match record, +2).
// Up to 9999 through the game's own score change (823D6C28 + 823DAC08, which
// also tells the other players; spending sends a negative change); the game
// stops there, so above it the record is written directly (to 32767). The
// host does this for every player (the other players' points are its own).
void ScoreTo(GuestScratch& s, uint32_t player, double points, int& given) {
  if (!player) return;
  const int want = std::clamp(int(points), 0, 32767);
  const int game_want = std::min(want, 9999);
  const int change = game_want - given;
  if (change != 0) {
    s.ctx.r3.u64 = player; s.ctx.r4.u64 = uint64_t(int64_t(change)); s.ctx.r5.u64 = ~0ull;
    s.Call(0x823D6C28);
    s.ctx.r3.u64 = player; s.ctx.r4.u64 = uint64_t(int64_t(change));
    s.Call(0x823DAC08);
    given += change;
  }
  if (const uint32_t rec = MatchRecord(player)) {
    const int have = int16_t(api->read_u16(rec + 2));
    if (have != want) api->write_u16(rec + 2, uint16_t(want));
  }
}
void SyncScore(GuestScratch& s) {
  ScoreTo(s, Player(), g_points, g_score_given);
  for (auto& c : g_clients)
    if (c.obj && Readable(c.obj, 4252)) ScoreTo(s, c.obj, c.points, c.score_given);
  SortRecords();
}

// Kills a person the way npc_kill does (824D4DA8 -> damage entry 824470D0
// with the game's "kill" amount), as the co-op mod's KillCopy.
void KillHuman(GuestScratch& s, uint32_t obj) {
  s.ctx.r3.u64 = obj; s.ctx.r4.u64 = 0; s.ctx.r5.u64 = 0; s.ctx.r6.u64 = 0;
  s.ctx.r7.u64 = 0; s.ctx.r8.u64 = 0; s.ctx.r9.u64 = 0; s.ctx.r10.u64 = 0;
  s.ctx.f1.f64 = api->read_f32(0x827AC214u);
  api->write_u8(s.ctx.r1.u32 + 87, 0);
  api->write_u8(s.ctx.r1.u32 + 95, 0);
  s.Call(0x824470D0);
}

std::string IpText(uint32_t ip) {
  char b[24];
  std::snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
  return b;
}

// --- Host side of the sync -------------------------------------------------

// The other player's object here: the remote player nearest to where they say
// they are (the one we had while it stays within 6 m).
uint32_t MatchClientObject(const Client& c, const std::vector<uint32_t>& players) {
  const uint32_t me = Player();
  auto d2 = [&](uint32_t o) {
    const float dx = api->read_f32(o + 20) - c.pos[0], dy = api->read_f32(o + 24) - c.pos[1], dz = api->read_f32(o + 28) - c.pos[2];
    return dx * dx + dy * dy + dz * dz;
  };
  if (c.obj && c.obj != me && std::find(players.begin(), players.end(), c.obj) != players.end() && d2(c.obj) < 6.0f * 6.0f)
    return c.obj;
  uint32_t best = 0;
  float best_d = 4.0f * 4.0f;
  for (uint32_t pl : players) {
    if (pl == me) continue;
    bool taken = false;
    for (const auto& o : g_clients) taken |= &o != &c && o.obj == pl;
    if (taken) continue;
    const float d = d2(pl);
    if (d < best_d) { best_d = d; best = pl; }
  }
  return best;
}

void ApplyClientHit(GuestScratch& s, Client& c, uint16_t id, float total, uint8_t flags, uint64_t now) {
  float* counted = nullptr;
  for (auto& [zid, v] : c.dmg) if (zid == id) counted = &v;
  if (!counted) { c.dmg.push_back({id, 0.0f}); counted = &c.dmg.back().second; }
  const float delta = std::max(0.0f, total - *counted);
  *counted = std::max(*counted, total);
  if (delta <= 0.0f && !(flags & 1)) return;
  Zombie* z = nullptr;
  for (auto& zz : g_zombies) if (zz.id == id) z = &zz;
  if (!z || z->dead) return;
  const uint32_t obj = ResolveHuman(z->handle);
  if (!obj || !SetUp(obj) || Dead(obj)) return;
  z->progress = now;
  const float hp = api->read_f32(obj + 1912);
  c.points += double(std::min(delta, hp)) * g_points_per_damage;
  const bool kill = hp - delta <= 0.0f || (flags & 1);
  if (kill) {
    KillHuman(s, obj);
    z->dead = true; z->died = now;
    c.points += (flags & 2) ? g_kill_points * 2 : g_kill_points;
  } else {
    api->write_f32(obj + 1912, hp - delta);
  }
  static unsigned logged = 0;
  if (logged++ < 20) {
    char line[160];
    std::snprintf(line, sizeof(line), "[Zombies] %s hit zombie %u: %.0f damage, health %.0f -> %.0f%s", IpText(c.ip).c_str(), id,
                  delta, hp, kill ? 0.0f : hp - delta, kill ? ((flags & 2) ? ", melee kill" : ", killed") : "");
    Log(line);
  }
}

void HandleRequest(Client& c, uint16_t seq, uint8_t kind, uint16_t index, uint8_t have) {
  c.last_req = seq;
  c.last_ok = 0;
  if (kind == 1 && index < g_map.doors.size()) {
    ZDoor& d = g_map.doors[index];
    if (d.open) { c.last_ok = 1; return; }
    if (c.points >= d.cost) {
      c.points -= d.cost;
      HideDoor(d);
      c.last_ok = 1;
      Log("[Zombies] " + IpText(c.ip) + " bought " + d.name + " for " + std::to_string(d.cost));
    }
  } else if (kind == 2 && index < g_map.weapons.size()) {
    const ZWallWeapon& w = g_map.weapons[index];
    const int cost = have ? std::max(1, w.cost / 2) : w.cost;
    if (c.points >= cost) {
      c.points -= cost;
      c.last_ok = 1;
      Log("[Zombies] " + IpText(c.ip) + " bought " + w.type + (have ? " ammo" : "") + " for " + std::to_string(cost));
    }
  }
}

void HostReceive(GuestScratch& s, const std::vector<uint32_t>& players, uint64_t now) {
  if (!NetReady()) return;
  uint8_t buf[1400];
  for (int guard = 0; guard < 96; ++guard) {
    uint32_t from = 0;
    const int n = g_net_poll(kChannel, buf, sizeof(buf), &from);
    if (n == 0) break;
    if (n < 2) continue;
    Reader r(buf, size_t(n));
    if (r.get<uint8_t>() != kMsgClient || r.get<uint8_t>() != kNetVersion) continue;
    Client* c = nullptr;
    for (auto& o : g_clients) if (o.ip == from) c = &o;
    if (!c) {
      g_clients.push_back(Client{});
      c = &g_clients.back();
      c->ip = from;
      Log("[Zombies] " + IpText(from) + " is in the zombie game (synced)");
    }
    c->seen = now;
    for (int i = 0; i < 3; ++i) c->pos[i] = r.get<float>();
    c->dead = r.get<uint8_t>() != 0;
    const uint8_t nh = r.get<uint8_t>();
    for (uint8_t k = 0; k < nh && r.ok; ++k) {
      const uint16_t id = r.get<uint16_t>();
      const float total = r.get<float>();
      const uint8_t flags = r.get<uint8_t>();
      if (r.ok && std::isfinite(total)) ApplyClientHit(s, *c, id, std::min(total, 1e7f), flags, now);
    }
    const uint16_t seq = r.get<uint16_t>();
    const uint8_t kind = r.get<uint8_t>();
    const uint16_t index = r.get<uint16_t>();
    const uint8_t have = r.get<uint8_t>();
    if (r.ok && seq && seq != c->last_req) HandleRequest(*c, seq, kind, index, have);
    const uint32_t obj = MatchClientObject(*c, players);
    if (obj != c->obj) {
      c->obj = obj;
      if (obj) {
        const uint32_t rec = MatchRecord(obj);
        c->score_given = rec ? std::clamp(int(int16_t(api->read_u16(rec + 2))), 0, 9999) : 0;
        char line[120];
        std::snprintf(line, sizeof(line), "[Zombies] %s is player object %08X", IpText(c->ip).c_str(), obj);
        Log(line);
      }
    }
    if (c->dmg.size() > 96) {
      std::erase_if(c->dmg, [](const auto& d) {
        return std::none_of(g_zombies.begin(), g_zombies.end(), [&](const Zombie& z) { return z.id == d.first; });
      });
    }
  }
  std::erase_if(g_clients, [&](const Client& c) {
    if (now - c.seen < 15000) return false;
    Log("[Zombies] " + IpText(c.ip) + " stopped sending (left?)");
    return true;
  });
}

void HostBroadcast(uint64_t now, bool force) {
  if (!NetReady() || (!force && now < g_next_broadcast)) return;
  g_next_broadcast = now + 100;
  Writer w;
  w.put(kMsgState); w.put(kNetVersion);
  w.put(uint8_t(g_phase)); w.put(uint16_t(g_round));
  w.put(uint32_t(g_phase == Phase::kBreak && g_phase_until > now ? g_phase_until - now : 0));
  w.put(uint32_t(g_banner_until > now ? g_banner_until - now : 0));
  uint64_t mask = 0;
  for (size_t i = 0; i < g_map.doors.size() && i < 64; ++i)
    if (g_map.doors[i].open) mask |= 1ull << i;
  w.put(mask);
  w.put(uint8_t(1 + std::min<size_t>(g_clients.size(), 11)));
  w.put(g_net_my_ip()); w.put(int32_t(g_points)); w.put(uint16_t(0)); w.put(uint8_t(0));
  for (size_t i = 0; i < g_clients.size() && i < 11; ++i) {
    const Client& c = g_clients[i];
    w.put(c.ip); w.put(int32_t(c.points)); w.put(c.last_req); w.put(c.last_ok);
  }
  const size_t count_at = w.b.size();
  w.put(uint8_t(0));
  uint8_t count = 0;
  for (const auto& z : g_zombies) {
    if (count >= 44) break;
    const uint32_t obj = ResolveHuman(z.handle);
    if (!obj) continue;
    const bool up = SetUp(obj);
    w.put(z.id); w.put(z.def);
    w.put(uint8_t((z.dead || (up && Dead(obj)) ? 1 : 0) | (up ? 2 : 0)));
    for (int i = 0; i < 3; ++i) w.put(api->read_f32(obj + 20 + i * 4));
    w.put(up ? std::max(0.0f, api->read_f32(obj + 1912)) : RoundHealth(std::max(g_round, 1)));
    ++count;
  }
  w.b[count_at] = count;
  g_net_send(kChannel, w.b.data(), int(w.b.size()), 0);
}

// --- The other players' side of the sync -------------------------------------

void ClientStop(GuestScratch& s, const std::string& why) {
  for (auto& [id, c] : g_copies)
    if (ResolveHuman(c.handle)) RemoveObject(s, c.handle);
  g_copies.clear();
  g_my_hits.clear();
  if (g_client) Log("[Zombies] zombie game with the host ended (" + why + ")");
  if (const uint32_t me = Player()) ReleaseRespawn(me);
  g_client = false;
  g_phase = Phase::kOff;
  DrawWallWeaponBeams(0);
  g_buy_text.clear();
  g_req_pending = false;
  g_doors_mask = 0;
  g_map = ZMap{};
}

void ClientReceive(GuestScratch& s, uint32_t me, uint64_t now) {
  uint8_t buf[1400];
  const uint32_t my_ip = g_net_my_ip();
  for (int guard = 0; guard < 64; ++guard) {
    uint32_t from = 0;
    const int n = g_net_poll(kChannel, buf, sizeof(buf), &from);
    if (n == 0) break;
    if (n < 2) continue;
    Reader r(buf, size_t(n));
    if (r.get<uint8_t>() != kMsgState || r.get<uint8_t>() != kNetVersion) continue;
    const auto phase = Phase(r.get<uint8_t>());
    const int round = r.get<uint16_t>();
    const uint32_t break_left = r.get<uint32_t>(), banner_left = r.get<uint32_t>();
    const uint64_t mask = r.get<uint64_t>();
    if (!r.ok) continue;
    if (phase == Phase::kOff) {
      if (g_client) ClientStop(s, "the host stopped");
      continue;
    }
    if (!g_client) {
      g_client = true;
      g_copies.clear();
      g_my_hits.clear();
      g_points = 0;
      g_req_pending = false;
      g_doors_mask = 0;
      LookupDefinitions(s);
      LoadMapData();
      Log("[Zombies] joined the host's zombie game (" + IpText(from) + "), round " + std::to_string(round));
    }
    g_host_ip = from;
    g_last_state = now;
    if (round > g_round && phase == Phase::kRound && me) ReleaseRespawn(me);
    g_phase = phase;
    g_round = round;
    g_phase_until = now + break_left;
    g_banner_until = now + banner_left;
    g_doors_mask = mask;
    const uint8_t np = r.get<uint8_t>();
    for (uint8_t k = 0; k < np && r.ok; ++k) {
      const uint32_t ip = r.get<uint32_t>();
      const int32_t points = r.get<int32_t>();
      const uint16_t last_req = r.get<uint16_t>();
      const uint8_t ok = r.get<uint8_t>();
      if (!r.ok || ip != my_ip) continue;
      g_points = points;
      if (g_req_pending && last_req == g_req_seq) {
        g_req_pending = false;
        if (!ok) {
          Note("Not enough points");
        } else if (g_req_kind == 1) {
          Note((g_req_index < g_map.doors.size() ? g_map.doors[g_req_index].name : std::string("Door")) + " opened");
        } else if (g_req_kind == 2 && g_req_index < g_map.weapons.size()) {
          const ZWallWeapon& w = g_map.weapons[g_req_index];
          if (me && GiveWeapon(s, me, w.type, !g_req_have, 300)) Note(g_req_have ? "Ammo bought" : WeaponLabel(w.type) + " bought");
          else Log("[Zombies] couldn't give " + w.type);
        }
      }
    }
    const uint8_t nz = r.get<uint8_t>();
    for (uint8_t k = 0; k < nz && r.ok; ++k) {
      const uint16_t id = r.get<uint16_t>();
      const uint8_t def = r.get<uint8_t>(), flags = r.get<uint8_t>();
      float p[3];
      for (auto& v : p) v = r.get<float>();
      const float hp = r.get<float>();
      if (!r.ok) break;
      Copy& c = g_copies[id];
      c.def = def;
      std::memcpy(c.host_pos, p, sizeof(p));
      c.host_hp = hp;
      c.host_dead = (flags & 1) != 0;
      c.seen = now;
    }
  }
  if (!g_client) return;
  for (size_t i = 0; i < g_map.doors.size() && i < 64; ++i)
    if ((g_doors_mask >> i) & 1 && !g_map.doors[i].open) HideDoor(g_map.doors[i]);
}

void ClientCopies(GuestScratch& s, uint32_t me, const std::vector<uint32_t>& players, uint64_t now) {
  int made = 0;
  for (auto it = g_copies.begin(); it != g_copies.end();) {
    Copy& c = it->second;
    uint32_t obj = c.handle ? ResolveHuman(c.handle) : 0;
    if (now - c.seen > 2000) {  // the host removed it
      if (obj) RemoveObject(s, c.handle);
      it = g_copies.erase(it);
      continue;
    }
    ++it;
    if (!obj) {
      c.handle = 0;
      if (c.host_dead || now < c.next_make || made >= 2) continue;
      uint32_t def = g_definitions[c.def ? 1 : 0];
      if (!def) def = g_definitions[c.def ? 0 : 1];
      if (!def) continue;
      const uint32_t face = me ? me : (players.empty() ? 0 : players[0]);
      const float yaw = face ? std::atan2(api->read_f32(face + 20) - c.host_pos[0], api->read_f32(face + 28) - c.host_pos[2]) : 0.0f;
      c.handle = CreateCharacter(s, def, c.host_pos, yaw);
      c.made = now; c.hostile = false; c.dead_here_since = 0;
      c.next_make = now + 3000;
      ++made;
      continue;
    }
    if (!SetUp(obj)) {
      if (now - c.made > 20000) { RemoveObject(s, c.handle); c.handle = 0; }
      continue;
    }
    if (c.host_dead) {
      if (!Dead(obj)) KillHuman(s, obj);
      continue;
    }
    if (Dead(obj)) {  // died here but not on the host: made again after 3 s
      if (!c.dead_here_since) c.dead_here_since = now;
      else if (now - c.dead_here_since > 3000) { RemoveObject(s, c.handle); c.handle = 0; c.next_make = now; }
      continue;
    }
    const uint32_t target = NearestPlayer(obj, players);
    if (!c.hostile) {
      MakeHostile(s, obj, target, c.host_hp);
      c.hostile = true;
      c.next_order = now + 3000;
    } else if (now >= c.next_order && target) {
      OrderAttack(s, obj, target);
      c.next_order = now + 3000;
    }
    if (c.host_hp > 0.0f && c.host_hp < api->read_f32(obj + 1912)) api->write_f32(obj + 1912, c.host_hp);
    if (now >= c.next_fix) {
      c.next_fix = now + 250;
      const float dx = api->read_f32(obj + 20) - c.host_pos[0], dy = api->read_f32(obj + 24) - c.host_pos[1],
                  dz = api->read_f32(obj + 28) - c.host_pos[2];
      if (dx * dx + dy * dy + dz * dz > 3.0f * 3.0f) Teleport(s, obj, c.host_pos);
    }
  }
}

void ClientSend(uint32_t me, uint64_t now) {
  if (now < g_next_client_send || !g_host_ip) return;
  g_next_client_send = now + 100;
  if (g_req_pending && now - g_req_since > 5000) { g_req_pending = false; Note("No answer from the host"); }
  std::erase_if(g_my_hits, [&](const MyHit& h) { return now - h.at > 4000; });
  Writer w;
  w.put(kMsgClient); w.put(kNetVersion);
  for (int i = 0; i < 3; ++i) w.put(me ? api->read_f32(me + 20 + i * 4) : 0.0f);
  w.put(uint8_t(me && Dead(me) ? 1 : 0));
  const size_t nh = std::min<size_t>(g_my_hits.size(), 60);
  w.put(uint8_t(nh));
  for (size_t i = g_my_hits.size() - nh; i < g_my_hits.size(); ++i) {
    w.put(g_my_hits[i].id); w.put(g_my_hits[i].total); w.put(g_my_hits[i].flags);
  }
  w.put(uint16_t(g_req_pending ? g_req_seq : 0)); w.put(g_req_kind); w.put(g_req_index); w.put(g_req_have);
  g_net_send(kChannel, w.b.data(), int(w.b.size()), g_host_ip);
}

void ClientUpdate(GuestScratch& s) {
  const uint64_t now = NowMs();
  const bool in_match = api->read_u8(kMultiplayerFlag) != 0 && api->read_u32(kTopMode) == 13;
  if (!in_match) {
    if (g_client) ClientStop(s, "left the match");
    return;
  }
  if (!NetReady()) return;
  const uint32_t me = Player();
  ClientReceive(s, me, now);
  if (!g_client) return;
  if (now - g_last_state > 5000) { ClientStop(s, "no word from the host for 5 s"); return; }
  static uint64_t next_map_try = 0;
  if (!g_map.loaded && now >= next_map_try) { next_map_try = now + 5000; LoadMapData(); }
  const std::vector<uint32_t> players = Players();
  if (me && Dead(me) && g_phase != Phase::kOff) HoldRespawn(me);
  if (g_map.loaded && g_phase != Phase::kGameOver && me) {
    std::vector<uint32_t> people{me};
    for (const auto& [id, c] : g_copies)
      if (const uint32_t obj = c.handle ? ResolveHuman(c.handle) : 0; obj && SetUp(obj)) people.push_back(obj);
    BlockClosedDoors(s, people);
    UpdateBuying(s, me);
  } else {
    g_buy_text.clear();
  }
  DrawWallWeaponBeams(me);
  ClientCopies(s, me, players, now);
  NoLimits();
  ClientSend(me, now);
  if (now >= g_next_status) {
    g_next_status = now + 10000;
    int up = 0, loading = 0, dead = 0;
    for (const auto& [id, c] : g_copies) {
      const uint32_t obj = c.handle ? ResolveHuman(c.handle) : 0;
      if (!obj) continue;
      if (!SetUp(obj)) ++loading; else if (Dead(obj)) ++dead; else ++up;
    }
    Log("[Zombies] (synced) round " + std::to_string(g_round) + ": host sends " + std::to_string(g_copies.size()) + " zombies; here " +
        std::to_string(up) + " up, " + std::to_string(loading) + " loading, " + std::to_string(dead) + " dead; " +
        std::to_string(int(g_points)) + " points");
  }
}

// --- Wall weapon markers ------------------------------------------------------
// A gold light pillar at each wall weapon within 120 m (the loader's
// overlay_beams: 10 floats per beam, drawn over the game, gone after 150 ms
// unless sent again).
bool g_beams_shown = false;
void DrawWallWeaponBeams(uint32_t me) {
  if (api->size < sizeof(WmlApi) || !api->overlay_beams) return;
  std::vector<float> b;
  if (me && g_map.loaded && g_phase != Phase::kOff) {
    const float p[3] = {api->read_f32(me + 20), api->read_f32(me + 24), api->read_f32(me + 28)};
    for (const auto& w : g_map.weapons) {
      const float dx = w.pos[0] - p[0], dz = w.pos[2] - p[2];
      if (dx * dx + dz * dz > 120.0f * 120.0f || b.size() >= 60 * 10) continue;
      const float beam[10] = {w.pos[0], w.pos[1] - 1.0f, w.pos[2], w.pos[0], w.pos[1] + 1.4f, w.pos[2], 5.0f, 1.0f, 0.75f, 0.15f};
      b.insert(b.end(), beam, beam + 10);
    }
  }
  if (!b.empty()) { api->overlay_beams(b.data(), int(b.size() / 10)); g_beams_shown = true; }
  else if (g_beams_shown) { api->overlay_beams(nullptr, 0); g_beams_shown = false; }
}

// --- Lobby: the zombie map only ---------------------------------------------------
// Multiplayer levels: per-mode tables of 60-byte records (sub_82373118 from
// multiplayer_levels.xtbl): counts [0x83074B1C][slot], records
// [0x83074B20][slot], slot = mode - 13 (Gangsta Brawl = 0); +0 name,
// +4 display text, +8 mode, +12 map name,
// +29 "Disabled" (the lobby's list and the random pick skip those:
// 823734D8 count, 82373598 n-th, 823739E8 weighted pick). The lobby's chosen
// level is an index among the enabled ones at 0x8370F294 (-1 = random). While
// the lobby has Zombies (the game's mode underneath is Gangsta Brawl), every
// Gangsta Brawl level but the zombie map is disabled and the zombie map
// (The Ultor Dome, mp_arena: GB 4, a Disabled DLC level otherwise) enabled
// and chosen; switched back when Zombies is left.
constexpr uint32_t kLevelCounts = 0x83074B1Cu, kLevelRecords = 0x83074B20u, kLevelsBuilt = 0x8370F22Fu;
constexpr uint32_t kLobbyLevel = 0x8370F294u;
std::string g_zombie_map = "mp_arena";
std::vector<std::pair<uint32_t, uint8_t>> g_level_saved;  // record, its own disabled byte
bool g_levels_patched = false;
// Custom / System Link lobbies list levels from a per-mode playlist instead
// (sub_8239AD20 fills the Level selector 0x8282CDF4): counts 0x8307B0C8[slot],
// u16 level id arrays [0x8307B0E4][slot], flag byte arrays [0x8307B100][slot];
// the chosen level's id is u16 0x827AD56C (the match loads it by id with
// 82373788, which ignores the Disabled byte). While Zombies is chosen, Gangsta
// Brawl's playlist is cut to the zombie map alone and its id kept chosen.
constexpr uint32_t kPlaylistCounts = 0x8307B0C8u, kPlaylistIds = 0x8307B0E4u, kPlaylistFlags = 0x8307B100u;
constexpr uint32_t kChosenLevelId = 0x827AD56Cu;
bool g_playlist_patched = false;
// The lobby's Level selector (static item 0x8282CDF4, vtable 0x82068024) is
// rebuilt with the game's own fill (sub_8239AD20) only while it really is the
// Level row: every choice's text is one of the levels' display texts. The
// same static item is used by other menus (Custom Match: Type / Mode); filling
// it there crashed the game (2026-10-03 01:27).
bool LevelRowShown() {
  constexpr uint32_t kItem = 0x8282CDF4u, kSelectorVtable = 0x82068024u;
  if (api->read_u32(kTopMode) != 6 || !Readable(kItem, 800) || api->read_u32(kItem) != kSelectorVtable) return false;
  const uint32_t n = api->read_u32(kItem + 784);
  if (n == 0 || n > 40) return false;
  const uint32_t counts = api->read_u32(kLevelCounts), recs_tab = api->read_u32(kLevelRecords);
  if (!Readable(counts, 4) || !Readable(recs_tab, 4)) return false;
  const uint32_t m = api->read_u32(counts), recs = api->read_u32(recs_tab);
  if (!m || m > 64 || !Readable(recs, m * 60)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t t = api->read_u32(kItem + 16 + i * 4);
    bool level = false;
    for (uint32_t k = 0; k < m && !level; ++k) level = api->read_u32(recs + k * 60 + 4) == t;
    if (!level) return false;
  }
  return true;
}
uint32_t g_pl_count = 0;
uint16_t g_pl_id0 = 0;
uint8_t g_pl_flag0 = 0;
uint16_t g_zombie_level_id = 0xFFFF;
void ZombiePlaylist(GuestScratch& s, bool on) {
  const uint32_t ids = api->read_u32(kPlaylistIds), flags = api->read_u32(kPlaylistFlags);
  if (!on) {
    if (!g_playlist_patched) return;
    if (Readable(ids, 2) && Readable(flags, 1)) {
      api->write_u32(kPlaylistCounts, g_pl_count);
      api->write_u16(ids, g_pl_id0);
      api->write_u8(flags, g_pl_flag0);
      api->write_u16(kChosenLevelId, g_pl_id0);
      // (No refill here: this runs while the lobby closes too. The game
      // refills the Level row itself when the mode changes.)
    }
    g_playlist_patched = false;
    Log("[Zombies] lobby: Gangsta Brawl playlist back to normal (" + std::to_string(g_pl_count) + " levels)");
    return;
  }
  if (g_zombie_level_id == 0xFFFF || !Readable(ids, 2) || !Readable(flags, 1)) return;
  const uint32_t count = api->read_u32(kPlaylistCounts);
  if (count == 0 || count > 40) return;
  if (count != 1 || api->read_u16(ids) != g_zombie_level_id || api->read_u8(flags) != 0) {
    // (Re)built by the game: keep its values to put back later.
    g_pl_count = count;
    g_pl_id0 = api->read_u16(ids);
    g_pl_flag0 = api->read_u8(flags);
    api->write_u32(kPlaylistCounts, 1);
    api->write_u16(ids, g_zombie_level_id);
    api->write_u8(flags, 0);
    if (!g_playlist_patched)
      Log("[Zombies] lobby: Gangsta Brawl playlist cut to the zombie map (level id " + std::to_string(g_zombie_level_id) +
          ", was " + std::to_string(count) + " levels)");
    g_playlist_patched = true;
    api->write_u16(kChosenLevelId, g_zombie_level_id);
    if (LevelRowShown()) {
      s.Call(0x8239AD20);  // the lobby's Level row: the zombie map only
      Log("[Zombies] lobby: Level row rebuilt");
    }
  }
  if (api->read_u16(kChosenLevelId) != g_zombie_level_id) api->write_u16(kChosenLevelId, g_zombie_level_id);
}
void ZombieLevels(GuestScratch& s, bool on) {
  ZombiePlaylist(s, on && g_levels_patched);
  if (!on) {
    if (!g_levels_patched) return;
    for (auto& [rec, v] : g_level_saved)
      if (Readable(rec, 60)) api->write_u8(rec + 29, v);
    g_level_saved.clear();
    g_levels_patched = false;
    api->write_u32(kLobbyLevel, 0);
    Log("[Zombies] lobby: Gangsta Brawl levels back to normal");
    return;
  }
  if (!api->read_u8(kLevelsBuilt)) return;
  const uint32_t counts = api->read_u32(kLevelCounts), recs_tab = api->read_u32(kLevelRecords);
  if (!Readable(counts, 4) || !Readable(recs_tab, 4)) return;
  const uint32_t n = api->read_u32(counts), recs = api->read_u32(recs_tab);
  if (!n || n > 64 || !Readable(recs, n * 60)) return;
  if (!g_levels_patched) {
    uint32_t dome = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t rec = recs + i * 60, name = api->read_u32(rec + 12);  // +12 map (+8 is the mode)
      if (Readable(name, 32) && _stricmp(static_cast<const char*>(api->guest_pointer(name)), g_zombie_map.c_str()) == 0) dome = rec;
    }
    if (!dome) {
      static bool told = false;
      if (!told) { told = true; Log("[Zombies] lobby: no Gangsta Brawl level with map " + g_zombie_map + " - levels left alone"); }
      return;
    }
    g_zombie_level_id = api->read_u16(dome + 36);
    g_level_saved.clear();
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t rec = recs + i * 60;
      g_level_saved.push_back({rec, api->read_u8(rec + 29)});
      api->write_u8(rec + 29, rec == dome ? 0 : 1);
    }
    g_levels_patched = true;
    Log("[Zombies] lobby: only " + g_zombie_map + " (of " + std::to_string(n) + " Gangsta Brawl levels) can be chosen");
  }
  if (api->read_u32(kLobbyLevel) != 0) api->write_u32(kLobbyLevel, 0);
}

bool InHostedMatch() {
  return api->read_u8(kMultiplayerFlag) != 0 && api->read_u8(kHostFlag) == 1 && api->read_u32(kTopMode) == 13 &&
         Player() && SetUp(Player());
}

void ClientUpdate(GuestScratch& s);
void Update(GuestScratch& s) {
  const uint64_t now = NowMs();
  // Not hosting a multiplayer match: maybe the host runs zombies (sync).
  if (api->read_u8(kMultiplayerFlag) != 0 && api->read_u8(kHostFlag) != 1 && g_phase == Phase::kOff) {
    ClientUpdate(s);
    return;
  }
  if (g_client) { ClientUpdate(s); return; }
  const bool want = g_match_wanted && InHostedMatch();
  static uint64_t next_wait_log = 0;
  if (g_match_wanted && !want && g_phase == Phase::kOff && now >= next_wait_log) {
    next_wait_log = now + 10000;
    const uint32_t p = api->read_u32(kPlayerPtr);
    char line[200];
    std::snprintf(line, sizeof(line), "[Zombies] waiting for a hosted match: multiplayer %u, host %u, top mode %d, player %08X%s",
                  api->read_u8(kMultiplayerFlag), api->read_u8(kHostFlag), int32_t(api->read_u32(kTopMode)), p,
                  Readable(p, 4252) ? (SetUp(p) ? " set up" : " not set up") : "");
    Log(line);
  }
  if (api->read_u32(kTopMode) != 13) g_match_over = false;  // left the match
  if (g_phase == Phase::kOff) {
    if (want && !g_match_over) StartGame(s);
    return;
  }
  if (!want) { StopGame(s, g_match_wanted ? "match over" : "zombies turned off"); return; }

  const std::vector<uint32_t> players = Players();
  RecordTrails(players, now);
  HostReceive(s, players, now);
  HostBroadcast(now, false);
  const uint32_t me = Player();
  if (me && Dead(me) && (g_phase == Phase::kRound || g_phase == Phase::kBreak || g_phase == Phase::kGameOver))
    HoldRespawn(me);
  {
    static uint64_t next_map_try = 0;
    if (!g_map.loaded && now >= next_map_try) { next_map_try = now + 5000; LoadMapData(); }
  }
  if (g_map.loaded && g_phase != Phase::kGameOver) {
    // Each game keeps its own player out of closed doors (and its zombies).
    std::vector<uint32_t> people;
    if (me) people.push_back(me);
    for (const auto& z : g_zombies)
      if (const uint32_t obj = ResolveHuman(z.handle); obj && SetUp(obj)) people.push_back(obj);
    BlockClosedDoors(s, people);
    UpdateBuying(s, me);
  } else {
    g_buy_text.clear();
  }
  DrawWallWeaponBeams(me);
  if (g_phase == Phase::kGameOver) {
    if (now >= g_phase_until) {
      // Back to the lobby: the match ends the way a reached score limit ends
      // it (sub_823CD588, as sub_823CC878 calls it): the game's end of match
      // (results, then the lobby) for everyone.
      ClearZombies(s);
      g_phase = Phase::kOff;
      g_match_over = true;
      Log("[Zombies] game over: ending the match (back to the lobby), round " + std::to_string(g_round) + ", " +
          std::to_string(int(g_points)) + " points");
      s.ctx.r3.u64 = 0;
      s.Call(0x823CD588);
    }
    return;
  }
  if (g_phase == Phase::kRound && !players.empty()) {
    // The other players say whether they're down (their object here may lag).
    bool all_down = true;
    for (uint32_t pl : players) {
      bool down = Dead(pl);
      for (const auto& c : g_clients)
        if (c.obj == pl && now - c.seen < 3000) down = c.dead;
      all_down = all_down && down;
    }
    if (all_down) {
      g_phase = Phase::kGameOver;
      g_phase_until = now + 6000;
      Log("[Zombies] everyone is down: game over in round " + std::to_string(g_round));
      return;
    }
  }

  // Zombies: set up, keep attacking the nearest player, count deaths, clear bodies.
  int alive = 0;
  for (auto it = g_zombies.begin(); it != g_zombies.end();) {
    Zombie& z = *it;
    const uint32_t obj = ResolveHuman(z.handle);
    if (!obj) {
      if (!z.counted) { ++g_killed; z.counted = true; }
      it = g_zombies.erase(it);
      continue;
    }
    if (!z.dead && SetUp(obj) && Dead(obj)) { z.dead = true; z.died = now; }
    if (z.dead) {
      if (!z.counted) { ++g_killed; z.counted = true; }
      if (now - z.died > 8000) { RemoveObject(s, z.handle); it = g_zombies.erase(it); continue; }
      ++it;
      continue;
    }
    if (!SetUp(obj)) {
      if (now - z.made > 20000) {  // its model never loaded: try again elsewhere
        RemoveObject(s, z.handle); --g_spawned;
        it = g_zombies.erase(it);
        continue;
      }
      ++alive; ++it;
      continue;
    }
    // Stuck (standing still, never hit, not next to a player) for 15 s: made
    // again elsewhere, or the round could never end (round 2 never started:
    // the last zombie stood 4-8 m away, unseen, for minutes).
    {
      const float p[3] = {api->read_f32(obj + 20), api->read_f32(obj + 24), api->read_f32(obj + 28)};
      const float dx = p[0] - z.last_pos[0], dz = p[2] - z.last_pos[2];
      if (!z.progress || dx * dx + dz * dz > 1.5f * 1.5f) {
        z.progress = now;
        std::memcpy(z.last_pos, p, sizeof(p));
      }
      float nearest = 1e12f;
      for (uint32_t pl : players)
        if (!Dead(pl)) nearest = std::min(nearest, Dist2(pl, p));
      if (nearest < 2.5f * 2.5f) z.progress = now;
      if (now - z.progress > 15000) {
        Log("[Zombies] a zombie stood still for 15 s: made again elsewhere");
        RemoveObject(s, z.handle); --g_spawned;
        it = g_zombies.erase(it);
        continue;
      }
    }
    const uint32_t target = NearestPlayer(obj, players);
    if (!z.hostile) { MakeHostile(s, obj, target, RoundHealth(g_round)); z.hostile = true; z.next_order = now + 3000; }
    else if (now >= z.next_order && target) { OrderAttack(s, obj, target); z.next_order = now + 3000; }
    const float lost = g_map.loaded && !g_map.spawns.empty() ? 200.0f : 80.0f;
    if (target && Dist2(target, std::array<float, 3>{api->read_f32(obj + 20), 0, api->read_f32(obj + 28)}.data()) > lost * lost) {
      RemoveObject(s, z.handle); --g_spawned;  // lost far away: comes again
      it = g_zombies.erase(it);
      continue;
    }
    ++alive; ++it;
  }

  SyncScore(s);
  NoLimits();
  if (now >= g_next_status) {
    g_next_status = now + 10000;
    // Each zombie: set up (model loaded) or not, distance to the nearest player, health.
    std::string detail;
    for (const auto& z : g_zombies) {
      const uint32_t obj = ResolveHuman(z.handle);
      if (!obj) continue;
      float best = 1e9f;
      for (uint32_t pl : players) {
        const float p[3] = {api->read_f32(obj + 20), api->read_f32(obj + 24), api->read_f32(obj + 28)};
        best = std::min(best, std::sqrt(Dist2(pl, p)));
      }
      char one[96];
      std::snprintf(one, sizeof(one), " [%s %.0fm %.0fhp y%.1f%s]", SetUp(obj) ? "up" : "loading", best,
                    api->read_f32(obj + 1912), api->read_f32(obj + 24), z.dead ? " dead" : "");
      detail += one;
      if (detail.size() > 900) break;
    }
    if (!detail.empty()) Log("[Zombies] zombies:" + detail);
    Log("[Zombies] round " + std::to_string(g_round) + ": " + std::to_string(alive) + " alive, " +
        std::to_string(g_killed) + "/" + std::to_string(g_total) + " killed, " + std::to_string(int(g_points)) +
        " points, " + std::to_string(g_spots.size()) + " spots, " + std::to_string(players.size()) + " player(s), prompt line polled " +
        std::to_string(g_prompt_polls) + "x");
    g_prompt_polls = 0;
  }

  if (g_phase == Phase::kBreak) {
    Hud(s);
    if (now >= g_phase_until) {
      ++g_round;
      g_banner_until = now + 3000;
      if (me) ReleaseRespawn(me);
      g_total = RoundTotal(g_round);
      g_spawned = 0; g_killed = 0;
      g_phase = Phase::kRound;
      g_next_spawn = now;
      Log("[Zombies] round " + std::to_string(g_round) + ": " + std::to_string(g_total) + " zombies");
    }
    return;
  }

  if (g_spawned < g_total && alive < AliveCap(g_round) && now >= g_next_spawn) {
    float spot[3];
    if ((g_map.loaded && PickMapSpawn(players, spot)) || PickSpot(players, spot)) {
      const bool lin = g_definitions[1] && (!g_definitions[0] || std::uniform_int_distribution<int>(0, 9)(g_rng) == 0);
      const uint32_t def = g_definitions[lin ? 1 : 0];
      const uint32_t pl = players.empty() ? Player() : players[0];
      const float yaw = std::atan2(api->read_f32(pl + 20) - spot[0], api->read_f32(pl + 28) - spot[2]);
      if (const uint32_t h = CreateCharacter(s, def, spot, yaw)) {
        Zombie z;
        z.handle = h; z.made = now;
        z.def = lin ? 1 : 0;
        z.id = g_next_zombie_id++;
        if (!g_next_zombie_id) g_next_zombie_id = 1;
        g_zombies.push_back(z);
        ++g_spawned;
      } else {
        static unsigned told = 0;
        if (told++ < 10) Log("[Zombies] a zombie couldn't be made (the game's pool of people may be full)");
      }
    }
    g_next_spawn = now + SpawnGap(g_round);
  }
  if (g_killed >= g_total) {
    g_phase = Phase::kBreak;
    g_phase_until = now + g_break;
    Log("[Zombies] round " + std::to_string(g_round) + " cleared, " + std::to_string(int(g_points)) + " points");
  }
  Hud(s);
}

// --- Hooks -----------------------------------------------------------------

WmlGuestFunction original_update = nullptr, original_damage = nullptr, original_melee = nullptr,
                 original_input = nullptr;

// Runs from the story's game update (82209E30) and from the pad read
// (827166D0, every frame in every mode - multiplayer matches don't go
// through the first); at most once per 15 ms, never nested.
void RunUpdate(WmlContext* raw) {
  static uint64_t last = 0;
  static bool busy = false;
  // Off and not wanted: still look for a host's zombie game while in a
  // multiplayer match (the other players).
  if (g_phase == Phase::kOff && !g_match_wanted && !g_client && !g_levels_patched &&
      !(api->read_u8(kMultiplayerFlag) != 0 && api->read_u32(kTopMode) == 13))
    return;
  const uint64_t now = NowMs();
  if (busy || now - last < 15) return;
  busy = true;
  last = now;
  {
    GuestScratch s(raw);
    // The lobby's level list follows the Zombies choice (this game's lobby).
    ZombieLevels(s, g_match_wanted && api->read_u8(kMultiplayerFlag) != 0);
    Update(s);
  }
  busy = false;
}
void UpdateHook(WmlContext* raw, uint8_t* base) {
  original_update(raw, base);
  g_game_thread = GetCurrentThreadId();
  RunUpdate(raw);
}
void InputHook(WmlContext* raw, uint8_t* base) {
  original_input(raw, base);
  static bool told = false;
  if (!told && g_match_wanted) {
    told = true;
    char line[120];
    std::snprintf(line, sizeof(line), "[Zombies] pad read on thread %lu, game update thread %lu", GetCurrentThreadId(), g_game_thread);
    Log(line);
  }
  if (!g_game_thread || GetCurrentThreadId() == g_game_thread) RunUpdate(raw);
}

// Points: the damage this game's player does to zombies (scaled by
// points_per_damage), plus a kill bonus (doubled for melee kills).
void DamageHook(WmlContext* c, uint8_t* b) {
  const uint32_t victim = uint32_t(api->get_r(c, 3)), attacker = uint32_t(api->get_r(c, 4));
  g_game_thread = GetCurrentThreadId();
  const bool was_alive = g_phase != Phase::kOff && Readable(victim, 4252) && !Dead(victim);
  const float before = was_alive ? api->read_f32(victim + 1912) : 0.0f;
  original_damage(c, b);
  if (!was_alive || !attacker || attacker != api->read_u32(kPlayerPtr)) return;
  if (g_client) {
    // Another player's game: the damage goes to the host (running total per zombie).
    const int id = CopyIdOf(victim);
    if (id < 0) return;
    const float after = std::max(0.0f, api->read_f32(victim + 1912));
    MyHit* h = nullptr;
    for (auto& m : g_my_hits) if (m.id == uint16_t(id)) h = &m;
    if (!h) { g_my_hits.push_back(MyHit{uint16_t(id), 0.0f, 0, 0}); h = &g_my_hits.back(); }
    if (before > after) h->total += before - after;
    if (Dead(victim)) h->flags |= uint8_t(1 | (g_in_melee ? 2 : 0));
    h->at = NowMs();
    g_next_client_send = 0;  // tell the host now
    return;
  }
  Zombie* z = FindZombie(victim);
  if (!z || z->dead) return;
  z->progress = NowMs();
  const float after = std::max(0.0f, api->read_f32(victim + 1912));
  const double done = double(before - after);
  if (done > 0) g_points += done * g_points_per_damage;
  if (g_damage_logged < 20) {
    ++g_damage_logged;
    char line[160];
    std::snprintf(line, sizeof(line), "[Zombies] hit: %.1f damage (amount %.1f), zombie health %.0f -> %.0f of %d%s",
                  done, api->get_f(c, 1), before, after, int32_t(api->read_u32(victim + 1908)), Dead(victim) ? ", killed" : "");
    Log(line);
  }
  if (Dead(victim)) g_points += g_in_melee ? g_kill_points * 2 : g_kill_points;
}

// sub_82169CE0(text): the HUD's centre prompt line each frame (the
// "Respawning in 3" line and other prompts; shown with the help box 822E52C8
// in its centred style 4 when it returns true).
WmlGuestFunction original_respawn_text = nullptr;

void RespawnTextHook(WmlContext* c, uint8_t* b) {
  const uint32_t out = uint32_t(api->get_r(c, 3));
  ++g_prompt_polls;
  original_respawn_text(c, b);
  if (!out) return;
  if ((api->get_r(c, 3) & 0xFF) && !g_respawn_held && g_phase != Phase::kGameOver) return;  // the game's own prompt wins
  std::u16string text;
  if (!BannerText(text)) return;
  for (size_t i = 0; i < text.size(); ++i) api->write_u16(out + uint32_t(i) * 2, uint16_t(text[i]));
  api->write_u16(out + uint32_t(text.size()) * 2, 0);
  api->set_r(c, 3, 1);
}

// sub_82367C28(text, seconds, ...): a multiplayer HUD message. The mode's
// start (sub_823CCFE0, returning to 0x823CD078) shows the welcome with it; it
// shares the HUD's message box with the centre prompt line, so in a zombie
// match the welcome is kept short (3 s) and "Round 1 starts in" shows after.
WmlGuestFunction original_message = nullptr;
void MessageHook(WmlContext* c, uint8_t* b) {
  if (g_match_wanted && api->get_lr(c) == 0x823CD078u) {
    api->set_f(c, 1, 3.0);
    Log("[Zombies] welcome message shortened to 3 s");
  }
  original_message(c, b);
}

void MeleeHook(WmlContext* c, uint8_t* b) {
  g_in_melee = true;
  original_melee(c, b);
  g_in_melee = false;
}

}  // namespace

// For the exe's lobby: the GAME list's "Zombies: Off / On".
extern "C" __declspec(dllexport) int ZombiesState() { return g_phase != Phase::kOff || g_client ? 1 : 0; }
// For the exe's scoreboard title: the round now (0 before round 1), -1 when no zombie game runs here.
extern "C" __declspec(dllexport) int ZombiesRound() { return g_phase != Phase::kOff ? g_round : -1; }
extern "C" __declspec(dllexport) void ZombiesSetMatch(int on) {
  if (g_match_wanted != (on != 0)) {
    g_match_wanted = on != 0;
    Log(std::string("[Zombies] lobby: zombies ") + (g_match_wanted ? "on" : "off"));
  }
}

extern "C" WML_EXPORT int wml_mod_init(const WmlApi* loader, const WmlMod* mod) {
  if (!loader || loader->version != WML_API_VERSION) return 1;
  api = loader;
  self = mod;
  const std::string ini = std::string(self->folder) + "\\mod.ini";
  char buf[32]{};
  GetPrivateProfileStringA("settings", "points_per_damage", "1", buf, sizeof(buf), ini.c_str());
  g_points_per_damage = std::clamp(std::atof(buf), 0.0, 1000.0);
  g_max_alive = std::clamp(int(GetPrivateProfileIntA("settings", "max_alive", 24, ini.c_str())), 4, 32);
  g_kill_points = int(GetPrivateProfileIntA("settings", "points_kill", 50, ini.c_str()));
  g_first_break = 1000u * std::clamp(unsigned(GetPrivateProfileIntA("settings", "first_round_delay", 10, ini.c_str())), 3u, 120u);
  g_break = 1000u * std::clamp(unsigned(GetPrivateProfileIntA("settings", "round_break", 8, ini.c_str())), 3u, 120u);
  g_health_round1 = float(std::clamp(int(GetPrivateProfileIntA("settings", "health_round1", 150, ini.c_str())), 10, 100000));
  g_health_per_round = float(std::clamp(int(GetPrivateProfileIntA("settings", "health_per_round", 100, ini.c_str())), 0, 100000));
  if (api->hook(0x82209E30, UpdateHook, &original_update) != 0) return 2;
  api->hook(0x827166D0, InputHook, &original_input);
  api->hook(0x824470D0, DamageHook, &original_damage);
  api->hook(0x82454A30, MeleeHook, &original_melee);
  api->hook(0x82169CE0, RespawnTextHook, &original_respawn_text);
  api->hook(0x82367C28, MessageHook, &original_message);
  Log("[Zombies] ready: multiplayer lobby > GAME > Zombies: On");
  return 0;
}
