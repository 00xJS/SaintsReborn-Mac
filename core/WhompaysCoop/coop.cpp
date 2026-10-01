#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "user32.lib")
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <rex/ppc/context.h>
#include <sr_reserved_names.h>
#include "wml.h"
#include "wml_pagequery.h"
#include "native_packet.h"
#include "eos_link.h"

#pragma comment(lib, "ws2_32.lib")

namespace {
const WmlApi* api = nullptr;
const WmlMod* self = nullptr;
WmlGuestFunction original_update = nullptr;
WmlGuestFunction original_mission_start = nullptr;
std::mutex probe_mutex;
std::mutex game_mutex;
coop::NativePacket native_out{}, native_in{};
bool native_pending = false;
std::atomic<uint32_t> native_encoded{0}, native_received{0}, native_available{0};
std::atomic<uint64_t> update_ticks{0};
std::atomic<uint32_t> replica_count{0}, replica_updates{0};
std::atomic<bool> cleanup_requested{false};
uint32_t tracked_host=0, replica_host=0, replica_local=0;
uint64_t replica_seen=0;
// Remote player avatar and the local test actor: mod-owned actors only.
uint32_t avatar_local=0, test_local=0;
uint64_t avatar_seen=0;
std::atomic<uint32_t> avatar_count{0}, avatar_updates{0};
std::atomic<bool> test_toggle{false}, copy_toggle{false};
bool diagnostics_done=false;
bool avatar_player=false;          // avatar is a pool player object (dressed)
bool use_player_avatar=true;       // mod.ini [settings] avatar = player | npc
std::vector<uint8_t> local_worn, remote_worn; // guarded by pose_mutex
bool remote_worn_new=false;
std::vector<uint8_t> applied_worn; // game thread
int dress_step=0;                  // game thread: 0 idle, 1 taking off, 2 waiting to wear
uint32_t dressed_record=0;         // game thread: customization record last dressed
std::vector<float> local_morphs, remote_morphs; // guarded by pose_mutex
bool remote_morphs_new=false;
bool sync_body=false;              // mod.ini [settings] body = 1 (experimental)
bool share_peds=true;             // mod.ini [settings] peds = shared | own
bool share_traffic=true;           // mod.ini [settings] traffic = shared | own
std::vector<float> applied_morphs; // game thread
// Appearance extras: skin tone, hair colour and the other colour choices of
// the character (the list multiplayer customization sync packed with
// 82321F38 and applied with 82322050; up to 5 entries of 8 bytes).
std::vector<uint8_t> local_extras, remote_extras; // guarded by pose_mutex
bool remote_extras_new=false;
std::vector<uint8_t> applied_extras; // game thread
// Hair colour is taken when the hair is put on (multiplayer sets the colours
// right after the clothes, before they load): colours that arrive or change
// after dressing need the clothes put on again (1.29 and earlier: skin tone
// right, hair colour not).
bool redress_for_colours=false; std::vector<uint8_t> extras_at_dress; // game thread
bool body_test=false;              // mod.ini [settings] body_test = 1: player copy body-shape diagnostic
// Milliseconds from a high-resolution clock. GetTickCount64 only moves in
// ~16 ms steps, which made the other player's movement step unevenly.
inline uint64_t MonoMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct PlayerPose {
  uint32_t sequence=0; float pos[3]{}; float rows[9]{}; char model[64]{};
  uint8_t look_length=0; uint8_t look[127]{};
  uint32_t time=0; // sender clock, ms
  // Vehicle the player sits in: info index (-1 on foot), seat, its pose.
  int32_t vehicle=-1; uint32_t seat=0;
  float vpos[3]{}, vrows[9]{}, vvel[3]{}, vang[3]{};
  // Which game's car it is: 1 = the sender's own, 0 = the receiver's (the
  // sender sits in its copy); vid is the handle in the owning game.
  uint32_t vowner=1, vid=0;
  uint32_t flags=0; // bit 0: crouching
};
std::mutex pose_mutex; // guards local_pose, remote_pose, remote_pose_new, avatar_model
PlayerPose local_pose, remote_pose;
bool remote_pose_new=false;
std::string avatar_model;
constexpr char kFallbackModel[]="PD_X_M_GenericYoung-01";
constexpr uint32_t kStreamTable=0x83E876B0; // streaming groups, 204 bytes each
struct Watch { uint32_t handle; uint64_t start; int stage; const char* tag; };
std::vector<Watch> watches; // game thread only
std::mutex probe_log_mutex;
std::ofstream probe_log;

void ProbeLog(const char* message) {
  std::lock_guard lock(probe_log_mutex);
  probe_log << MonoMs() << " " << message << std::endl;
}
std::atomic<bool> visible{false};
std::atomic<bool> running{false};
std::atomic<bool> connected{false};
std::atomic<bool> hosting{false};
std::atomic<bool> stop_thread{false};
std::thread net_thread;
std::mutex status_mutex;
std::string status = "F6 open  |  1 host  2 join  3 stop  4 host online  5 copy of you";
std::string host_ip = "127.0.0.1";
constexpr uint16_t kPort = 27015;
SOCKET sock = INVALID_SOCKET;
constexpr uint32_t kObjectTable = 0x830866C8;
std::atomic<float> local_x{0}, local_y{0}, local_z{0};
std::atomic<float> remote_x{0}, remote_y{0}, remote_z{0};
// Story missions follow the host: the joiner can't start missions while in a
// session, and is brought next to the host when the host starts one.
bool share_missions=true;                 // mod.ini [settings] missions = host | own
bool share_cutscenes=true;                // mod.ini [settings] cutscenes = shared | own
// Mission mirroring: the joiner's game runs the host's mission too (so its
// world data, characters and cutscenes are there), but the host's game is in
// charge: the joiner's own mission HUD, mission end and mission characters
// are held back and the host's are shown.
enum : uint32_t { kHudObjective=1, kHudHelp=2, kHudMissionEnd=3,
                  kGroupCreate=4,   // the host's mission turned a group of characters on: 824C43B0(hash, hidden)
                  kGroupDestroy=5   // ... and off again: 824C4B58(hash)
};
struct HudEvent { uint32_t kind=0, a[8]{}, length=0; uint16_t text[300]{}; };
std::mutex hud_mutex;
std::vector<HudEvent> hud_out, hud_in;
uint32_t hud_out_sequence=0;
std::atomic<uint32_t> host_mission_hash{0};
bool mirror_mission=false;           // game thread (joiner): this game runs the host's mission
uint64_t mirror_since=0;             // game thread (joiner): when it started (its data takes a while to load)
thread_local int applying_remote=0;  // replaying the host's HUD / starting its mission
std::atomic<uint32_t> hud_sent{0}, hud_applied{0}, hud_blocked{0};
bool Readable(uint32_t address, size_t length);
unsigned ReadWide(uint32_t address,uint16_t* out,unsigned capacity) {
  unsigned n=0;
  if(!address) { out[0]=0; return 0; }
  while(n+1<capacity && Readable(address+n*2,2)) { const uint16_t c=api->read_u16(address+n*2); if(!c) break; out[n++]=c; }
  out[n]=0;
  return n;
}
// Host's cutscenes (821F9098: name, r4-r7), played in the joiner's game too.
struct CutsceneStart { uint32_t sequence=0, r4=0, r5=0, r6=0, r7=0; char name[64]{}; };
std::mutex cutscene_mutex;
CutsceneStart cutscene_out, cutscene_in;  // host: latest start; joiner: latest received
bool cutscene_in_new=false;
uint64_t cutscene_out_time=0;
std::atomic<bool> local_mission{false}, local_cutscene{false};   // game thread -> network
std::atomic<bool> host_mission{false}, host_cutscene{false};     // network -> game thread
std::atomic<uint64_t> host_state_seen{0};
std::atomic<bool> host_paused{false};    // network -> game thread (joiner): the host has the pause menu open
bool HostPauseHold(uint64_t now);        // joiner: keep the host's world as it is while the host is paused
// Game clock (read by get_game_time 824CDA28): 16 bytes at 0x82B2EF30.
constexpr uint32_t kGameTime=0x82B2EF30;
std::mutex time_mutex;
uint8_t local_time[16]{}, host_time[16]{};
bool host_time_new=false;
// Current mission slot: 0 in free roam, set when a mission starts (0.49 log:
// 0x827D578C is 4 in free roam and 0x82B31080 stays 4 in a mission, so
// neither of those marks one).
bool MissionActive() {
  return api->read_u32(0x82B3106C)!=0;
}
std::atomic<bool> dump_request{false};
std::atomic<bool> clothing_dump_request{false}, redress_request{false};  // F8 / F9
bool CutscenePlaying() { return api->read_u8(0x8370D991) || api->read_u8(0x8370D990); }

#pragma pack(push, 1)
// Player pose, sent both ways at 20 Hz: position (+20), orientation rows
// (+32/+44/+56, right/up/forward) and the player's model name.
struct MorphPacket {
  char magic[4];     // WMO1
  float values[128];
};
struct WornPacket {
  char magic[4];     // WWI1
  uint16_t length;
  uint8_t bytes[256];
};
struct ExtrasPacket {
  char magic[4];     // WAX1: skin tone, hair colour etc. (see ReadExtras)
  uint8_t length;
  uint8_t bytes[40];
};
struct PosePacket {
  char magic[4];
  uint32_t sequence;
  float pos[3];
  float rows[9];
  char model[64];
  uint8_t look_length;   // appearance block, see ReadLook
  uint8_t look[127];
  uint32_t time;         // sender clock, ms
  int32_t vehicle;       // vehicle info index, -1 on foot
  uint32_t seat;
  float vpos[3], vrows[9], vvel[3], vang[3];
  uint32_t vowner, vid;
  uint32_t flags;        // bit 0: crouching
};

#pragma pack(pop)

bool Readable(uint32_t address, size_t length) {
  if (!address || uint64_t(address) + length > UINT32_MAX) return false;
  auto begin = reinterpret_cast<uintptr_t>(api->guest_pointer(address));
  auto end = begin + length;
  // VirtualQuery is a system call and the host's NPC search checks thousands
  // of addresses a second. Remember readable regions for a second.
  struct Range { uintptr_t begin, end; uint64_t expires; };
  thread_local Range cache[32]{};
  thread_local unsigned next_slot=0;
  const uint64_t now=MonoMs();
  while (begin < end) {
    bool cached=false;
    for (const Range& r : cache)
      if (begin>=r.begin && begin<r.end && now<r.expires) { begin=r.end; cached=true; break; }
    if (cached) continue;
    // One page at a time (see wml_pagequery.h: VirtualQuery walks huge regions).
    const uintptr_t page = begin & ~uintptr_t(0xFFF);
    if (!WmlHostRangeReadable(page, 1)) return false;
    const uintptr_t next = page + 0x1000;
    cache[next_slot++%32]={page,next,now+1000};
    begin = next;
  }
  return true;
}

// Preserve the complete CPU context and the scratch stack, not just argument
// registers. No engine function is ever called from the render/network thread.
std::atomic<uint32_t> mod_in_call{0}, mod_last_call{0};  // freeze reports
std::atomic<const char*> mod_stage{"start"};
struct GuestScratch {
  PPCContext ctx;
  uint32_t start;
  std::vector<uint8_t> saved;
  explicit GuestScratch(WmlContext* raw) {
    std::memcpy(&ctx,raw,sizeof(ctx));
    start=ctx.r1.u32-0x5000;
    saved.resize(0x5000);
    std::memcpy(saved.data(),api->guest_pointer(start),saved.size());
    ctx.r1.u32-=0x2000;
  }
  ~GuestScratch() { std::memcpy(api->guest_pointer(start),saved.data(),saved.size()); }
  uint32_t data() const { return start+0x3800; }
  bool Call(uint32_t fn) {
    // For freeze reports: the game function the mod is inside of, if any.
    const uint32_t outer=mod_in_call.exchange(fn);
    const bool ok=api->call(reinterpret_cast<WmlContext*>(&ctx),fn)==0;
    mod_in_call.store(outer); mod_last_call.store(fn);
    return ok;
  }
};

bool NpcReady(uint32_t object, uint32_t index, uint32_t player, unsigned* stages) {
  if (object==player || !Readable(object,4244) || api->read_u32(object+72)!=1) return false;
  ++stages[0];
  const uint32_t handle=api->read_u32(object+68);
  if (!handle || (handle&0xffff)!=index) return false;
  if (handle==avatar_local || handle==test_local || handle==replica_local) return false;
  const uint32_t desc=api->read_u32(object+228);
  if (!Readable(desc,12)) return false;
  const uint32_t name=api->read_u32(desc+8);
  if (!Readable(name,256) || !std::memchr(api->guest_pointer(name),0,256)) return false;
  ++stages[1];
  const uint32_t skeleton=api->read_u32(object+568);
  if (!Readable(skeleton,3744)) return false;
  ++stages[2];
  if (api->read_u8(skeleton+3740)!=1) return false;
  ++stages[3];
  const uint32_t state=api->read_u32(skeleton+3736);
  if (state && (!Readable(state,900) || api->read_u32(state+640)>255)) return false;
  return true;
}

uint32_t ResolveHuman(uint32_t handle) {
  if(!handle || (handle&0xffff)>=4096) return 0;
  const auto obj=api->read_u32(kObjectTable+12+(handle&0xffff)*16);
  return Readable(obj,4252) && api->read_u32(obj+68)==handle &&
    api->read_u32(obj+72)==1 && obj!=api->read_u32(0x8309ABEC) ? obj : 0;
}

void DestroyActor(GuestScratch& scratch,uint32_t& handle,const char* tag) {
  if(ResolveHuman(handle)) {
    scratch.ctx.r3.u64=handle;
    scratch.Call(0x823ACF50); // Native destructor validates full handle again.
    char line[120];
    std::snprintf(line,sizeof(line),"Destroyed mod-owned %s actor %08X",tag,handle);
    ProbeLog(line);
  }
  handle=0;
}

void DestroyReplica(GuestScratch& scratch) {
  DestroyActor(scratch,replica_local,"NPC replica");
  replica_host=0; replica_local=0; replica_count=0;
}

void ReleasePlayerObject(GuestScratch& scratch,uint32_t obj);
void OrderExitVehicle(GuestScratch& scratch,uint32_t human);
uint32_t ResolveVehicle(uint32_t handle);
uint64_t avatar_exit_ordered=0;
// Returns with the character kept while it still sits in a car: it gets out
// first (released while seated, the car kept a stale pointer and the game
// crashed a moment later - both crashes in the 0.99 test were at a removal).
bool avatar_force_remake=false;  // stuck in a car the other player left: remove, make again
void DestroyAvatar(GuestScratch& scratch) {
  if(const uint32_t obj=ResolveHuman(avatar_local)) {
    if(ResolveVehicle(api->read_u32(obj+2496))) {
      const uint64_t t=MonoMs();
      if(!avatar_exit_ordered) {
        OrderExitVehicle(scratch,obj); avatar_exit_ordered=t;
        ProbeLog("Remote player character leaves its car before removal");
      }
      if(t-avatar_exit_ordered<6000) return;
      ProbeLog("Remote player character still in its car after 6 s: removed anyway");
    }
  }
  avatar_exit_ordered=0;
  avatar_force_remake=false;  // removed now; the next update makes a fresh one
  {
    const uint32_t player=api->read_u32(0x8309ABEC);
    if(avatar_local && api->read_u32(player+4128)==avatar_local) api->write_u32(player+4128,0);
  }
  if(avatar_player) {
    if(const uint32_t obj=ResolveHuman(avatar_local)) {
      ReleasePlayerObject(scratch,obj);
      ProbeLog("Remote player character returned to the player pool");
    }
    avatar_local=0; avatar_player=false;
  } else DestroyActor(scratch,avatar_local,"remote player");
  applied_worn.clear(); applied_morphs.clear(); applied_extras.clear();
  avatar_count=0;
}

void LogActor(const char* tag,uint32_t obj) {
  char line[260];
  std::snprintf(line,sizeof(line),
    "%s actor %08X: flags200=%08X flags216=%08X flags120=%08X physics=%u alpha=%.2f pos=%.1f %.1f %.1f",
    tag,obj,api->read_u32(obj+200),api->read_u32(obj+216),api->read_u32(obj+120),
    api->read_u32(obj+784),api->read_f32(obj+2484),
    api->read_f32(obj+20),api->read_f32(obj+24),api->read_f32(obj+28));
  ProbeLog(line);
}

// Logs a new actor's setup state 1 s and 4 s after creation. flags216 bit
// 0x4000 means its model setup finished (physics body, rendering).
void WatchActors(uint64_t now) {
  for(size_t i=0;i<watches.size();) {
    Watch& w=watches[i];
    const uint32_t obj=ResolveHuman(w.handle);
    if(!obj) { watches.erase(watches.begin()+i); continue; }
    if(now-w.start>=(w.stage?4000u:1000u)) {
      LogActor(w.tag,obj);
      if(++w.stage==2) { watches.erase(watches.begin()+i); continue; }
    }
    ++i;
  }
}

// Same order as the game's network NPC creator (823A7DB0): move the spot off
// any human standing exactly there, construct, then take a streaming reference
// on the model (category 5). Without that reference the actor's setup waits on
// a model nobody requested: no physics body, nothing drawn. The human
// destructor releases the reference (8247BA28 -> 8250CA48).
unsigned actors_refused=0;       // game thread
uint64_t actors_blocked_until=0;  // after a refusal, no new people for a moment
// Kind of character made (82479AB0 r3): 3 = a passer-by of this game, which
// the game removes by itself when it thinks it is out of the way (copies
// vanished 10-18 times in 10 s: people popping in and out); 0 = a scripted
// character like the missions make, left alone. mod.ini copy_kind.
unsigned copy_kind=0;
unsigned actors_kind_fallback=0;
uint32_t CreateActor(GuestScratch& scratch,uint32_t definition,uint32_t pos,uint32_t matrix,const char* tag,bool quiet=false) {
  if(!Readable(definition,612)) return 0;
  const uint32_t model=api->read_u32(definition+24);
  if(!Readable(model,9)) return 0;
  scratch.ctx.r3.u64=pos;
  if(!scratch.Call(0x824C2CF8)) return 0;
  api->write_u32(scratch.ctx.r1.u32+84,0);
  api->write_u16(scratch.ctx.r1.u32+94,8);
  api->write_u8(scratch.ctx.r1.u32+103,0);
  auto make=[&](unsigned kind)->uint32_t {
    api->write_u32(scratch.ctx.r1.u32+84,0);
    api->write_u16(scratch.ctx.r1.u32+94,8);
    api->write_u8(scratch.ctx.r1.u32+103,0);
    scratch.ctx.r3.u64=kind; scratch.ctx.r4.u64=2; scratch.ctx.r5.u64=definition;
    scratch.ctx.r6.u64=pos; scratch.ctx.r7.u64=matrix; scratch.ctx.r8.u64=255;
    scratch.ctx.r9.u64=0; scratch.ctx.r10.u64=0;
    return scratch.Call(0x82479AB0) ? scratch.ctx.r3.u32 : 0;
  };
  uint32_t obj=make(copy_kind);
  if(copy_kind!=3 && (!Readable(obj,4252) || obj==api->read_u32(0x8309ABEC))) { ++actors_kind_fallback; obj=make(3); }
  if(!Readable(obj,4252) || obj==api->read_u32(0x8309ABEC)) {
    // The game's pool of people is full. Wait a little before trying again
    // (it was tried several times a second and flooded the log).
    ++actors_refused;
    actors_blocked_until=MonoMs()+2000;
    static uint64_t next_log=0;
    if(MonoMs()>=next_log) { next_log=MonoMs()+10000; ProbeLog("Native actor constructor returned no usable actor (the game's pool of people is full)"); }
    return 0;
  }
  const uint32_t handle=api->read_u32(obj+68);
  if(ResolveHuman(handle)!=obj) return 0;
  const uint32_t desc=api->read_u32(obj+228);
  uint32_t id=0;
  if(Readable(desc,612)) {
    id=api->read_u32(desc+608);
    scratch.ctx.r3.u64=kStreamTable+(id>>24)*204; scratch.ctx.r4.u64=id;
    scratch.ctx.r5.u64=5; scratch.ctx.r6.u64=0; scratch.ctx.r7.u64=1;
    scratch.Call(0x8250C750);
  } else if(!quiet) ProbeLog("Created actor has no model descriptor; no streaming reference taken");
  if(quiet) return handle;
  char line[160];
  std::snprintf(line,sizeof(line),"%s created: local=%08X object=%08X model id=%08X",tag,handle,obj,id);
  ProbeLog(line);
  LogActor(tag,obj);
  watches.push_back({handle,MonoMs(),0,tag});
  return handle;
}

// Native physics setters used by 823A8240. object+784 is a physics INDEX,
// not a guest pointer. Do not write render transforms directly.
bool PoseActor(GuestScratch& scratch,uint32_t obj,uint32_t pos,uint32_t matrix) {
  const uint32_t physics=api->read_u32(obj+784);
  const uint64_t body=uint64_t(0x839AB550)+uint64_t(physics)*5968;
  if(physics>=4096 || body>UINT32_MAX || !Readable(static_cast<uint32_t>(body),5968) ||
     !(api->read_u32(obj+216)&0x4000)) return false;
  const uint32_t vtable=api->read_u32(static_cast<uint32_t>(body)+5172);
  if(!Readable(vtable,8)) return false;
  scratch.ctx.r3.u64=physics; scratch.ctx.r4.u64=matrix;
  if(!scratch.Call(0x822679F8)) return false;
  scratch.ctx.r3.u64=physics; scratch.ctx.r4.u64=pos;
  return scratch.Call(0x822678F0);
}

// Exact model lookup by name; r4=0 disables the default-model fallback.
uint32_t LookupModel(GuestScratch& scratch,const char* name,uint32_t buffer) {
  const size_t n=strnlen(name,63);
  if(!n) return 0;
  std::memcpy(api->guest_pointer(buffer),name,n);
  api->write_u8(buffer+uint32_t(n),0);
  scratch.ctx.r3.u64=buffer; scratch.ctx.r4.u64=0;
  return scratch.Call(0x82114928) ? scratch.ctx.r3.u32 : 0;
}

bool ApplyLook(GuestScratch& scratch,uint32_t handle,const uint8_t* look,unsigned length);
bool MoveHuman(GuestScratch& scratch,uint32_t obj,uint32_t pos,uint32_t matrix,uint32_t velocity,bool find_node=true);

void SetPendingLook(const uint8_t* look,unsigned length);
void ResetReplicaFollow();
void FeedReplica(uint32_t time,const float* pos,const float* rows,uint64_t now);

void ApplyReplica(GuestScratch& scratch,const coop::NativePacket& p,uint32_t definition) {
  // One explicitly created actor, never an existing local NPC. The full
  // network creator needs a multiplayer-only pool, so use its steps directly.
  const uint32_t data=scratch.data(), pos=data+1056, quat=data+1072, matrix=data+1104;
  float values[7]; std::memcpy(values,p.payload,sizeof(values));
  const float dx=values[0]-local_x.load(),dy=values[1]-local_y.load(),dz=values[2]-local_z.load();
  if(dx*dx+dy*dy+dz*dz>60*60) return; // Don't spawn into unstreamed distant areas.
  for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,values[i]);
  for(unsigned i=0;i<4;++i) api->write_f32(quat+i*4,values[i+3]);
  scratch.ctx.r3.u64=quat; scratch.ctx.r4.u64=matrix;
  if(!scratch.Call(0x8263BBC0)) return;
  if(replica_host && replica_host!=p.host_handle) { DestroyReplica(scratch); SetPendingLook(nullptr,0); }
  uint32_t obj=ResolveHuman(replica_local);
  if(!obj) {
    static uint64_t next_attempt=0;
    const uint64_t now=MonoMs();
    if(now<next_attempt) return;
    next_attempt=now+1000;
    replica_local=CreateActor(scratch,definition,pos,matrix,"NPC replica");
    obj=ResolveHuman(replica_local);
    if(!obj) { replica_local=0; return; }
    replica_host=p.host_handle; replica_count=1;
    char line[160];
    const unsigned name_end=28+unsigned(strnlen(reinterpret_cast<const char*>(p.payload+28),p.payload_size-28));
    const unsigned look_length=name_end+1<p.payload_size ? p.payload[name_end+1] : 0;
    const bool look=look_length && ApplyLook(scratch,replica_local,p.payload+name_end+2,look_length);
    if(look_length && !look) SetPendingLook(p.payload+name_end+2,look_length);
    std::snprintf(line,sizeof(line),"NPC replica maps host=%08X to local=%08X, appearance %u bytes %s",
                  replica_host,replica_local,look_length,look?"applied":"waiting for setup");
    ProbeLog(line);
    ResetReplicaFollow();
  }
  replica_seen=MonoMs();
  float rows[9];
  for(unsigned i=0;i<9;++i) rows[i]=api->read_f32(matrix+i*4);
  FeedReplica(p.time,values,rows,replica_seen);
}

// A human's appearance: which variant is chosen for each part of its model
// (clothing, hair, textures). Stored on its skeleton (+568) as a small state
// block, +640 length and +644 bytes. This is what the game's network create
// record carries (8238B0B8 writes it, 8238B258 applies it).
unsigned ReadLook(uint32_t human,uint8_t* out,unsigned capacity) {
  const uint32_t skeleton=api->read_u32(human+568);
  if(!Readable(skeleton,3744) || api->read_u8(skeleton+3740)!=1) return 0;
  const uint32_t state=api->read_u32(skeleton+3736);
  if(!Readable(state,648)) return 0;
  const uint32_t length=api->read_u32(state+640);
  if(!length || length>capacity || !Readable(state+644,length)) return 0;
  std::memcpy(out,api->guest_pointer(state+644),length);
  return length;
}

// Native apply (8238B258) reads a signed length byte, so at most 127 bytes.
bool ApplyLook(GuestScratch& scratch,uint32_t handle,const uint8_t* look,unsigned length) {
  const uint32_t obj=ResolveHuman(handle);
  if(!obj || !length || length>127 || !(api->read_u32(obj+216)&0x4000)) return false;
  const uint32_t skeleton=api->read_u32(obj+568);
  if(!Readable(skeleton,3744) || api->read_u8(skeleton+3740)!=1) return false;
  const uint32_t cursor=scratch.data()+1480, buffer=scratch.data()+1488;
  api->write_u32(cursor,0);
  api->write_u8(buffer,uint8_t(length));
  std::memcpy(api->guest_pointer(buffer+1),look,length);
  scratch.ctx.r3.u64=handle; scratch.ctx.r4.u64=cursor; scratch.ctx.r5.u64=buffer;
  return scratch.Call(0x8238B258);
}

// ---- Vehicles ----
// Vehicles are 32 pooled objects (constructor 82567DE0, 56628 bytes, type 5).
// +50324 points at the vehicle's entry in the info table (1196 bytes each,
// same order in every copy of the game, so the index identifies the model).
// A human's vehicle handle is at +2496.
constexpr uint32_t kVehicleInfo=0x83E8ABD8, kVehicleInfoCount=0x8371023C, kVehicleInfoSize=1196;
constexpr uint32_t kVehicleSize=56628;

uint32_t ResolveVehicle(uint32_t handle) {
  if(!handle || (handle&0xffff)>=4096) return 0;
  const auto obj=api->read_u32(kObjectTable+12+(handle&0xffff)*16);
  return Readable(obj,kVehicleSize) && api->read_u32(obj+68)==handle && api->read_u32(obj+72)==5 ? obj : 0;
}

int32_t VehicleType(uint32_t vehicle) {
  const uint32_t info=api->read_u32(vehicle+50324);
  const uint32_t count=api->read_u32(kVehicleInfoCount);
  if(info<kVehicleInfo || (info-kVehicleInfo)%kVehicleInfoSize || count>512) return -1;
  const uint32_t index=(info-kVehicleInfo)/kVehicleInfoSize;
  return index<count ? int32_t(index) : -1;
}

// Linear velocity of the vehicle's rigid body (as 82534368 reads it). Only
// valid while the vehicle is simulated (byte +50300 == 1).
// Copy of the other player's car in this game: local handle, and its handle
// in the other game. Kept by the game thread.
uint32_t proxy_local=0, proxy_peer=0;
// A copy of one of our own cars, made when the other player drives it here
// and this game had removed the original: it goes by the original's handle.
uint32_t own_copy_local=0, own_copy_id=0;
uint32_t OwnId(uint32_t handle) { return own_copy_local && handle==own_copy_local ? own_copy_id : handle; }
// Joiner: copies of the host's traffic cars this player got into (local car
// handle -> the host's handle). Reported with the host's handle, so the
// host's own car is the one its copy of us gets into (it was a second car).
std::unordered_map<uint32_t,uint32_t> taken_cars;

// A vehicle's appearance: paint colours and the other variants the game's
// own network code sends (8238B0B8 / 8238B258 for type 5 objects). The type
// alone gave copies random colours.
#pragma pack(push, 1)
struct CarLookPacket {
  char magic[4];        // WVL1
  uint32_t id;          // the sender's handle for the car
  uint16_t length, pad;
  uint8_t look[320];
};
#pragma pack(pop)
#pragma pack(push, 1)
// Mission HUD from the host: objective line (822EF440), help messages
// (822E52C8) and the mission's end (82215360), replayed in the joiner's game.
struct HudPacket { char magic[4]; uint32_t sequence, kind, a[8], length; uint16_t text[300]; };  // WHU1, host -> joiner
struct MissionIdPacket { char magic[4]; uint32_t hash; };  // WMN1, host -> joiner: running mission (0 = none)
struct CutscenePacket { char magic[4]; uint32_t sequence, r4, r5, r6, r7; char name[64]; };  // WCS1, host -> joiner
struct MissionStatePacket { char magic[4]; uint8_t mission, cutscene, paused, pad; uint8_t time[16]; };  // WMS1, host -> joiner
// Chat (T in the game, the exe's chat.cpp): both ways, sent twice, dropped
// again by sequence on the other side.
struct ChatPacket { char magic[4]; uint32_t sequence; char name[16]; char text[160]; };  // WCH1
std::mutex chat_mutex;
std::deque<std::string> chat_out;  // lines to send
std::deque<std::string> chat_in;   // "name\ttext" received
#pragma pack(pop)
std::mutex car_look_mutex;
std::unordered_map<uint32_t,std::vector<uint8_t>> car_looks_out; // local handle -> look (sent)
std::unordered_map<uint32_t,std::vector<uint8_t>> car_looks_in;  // sender handle -> look
std::unordered_map<uint32_t,uint64_t> car_look_read;             // game thread: when last read

std::vector<uint8_t> ReadCarLook(struct GuestScratch& scratch,uint32_t vehicle);
bool ApplyCarLook(struct GuestScratch& scratch,uint32_t vehicle,const std::vector<uint8_t>& look);
// Game thread: keep this car's look in the outgoing set (re-read every 2 s:
// paint shops, damage variants).
void ShareCarLook(struct GuestScratch& scratch,uint32_t vehicle,uint64_t now);

uint32_t VehicleVelocityAddress(uint32_t vehicle) {
  if(api->read_u8(vehicle+50300)!=1) return 0;
  const uint32_t body=api->read_u32(vehicle+244);
  if(!Readable(body,92)) return 0;
  const uint32_t motion=api->read_u32(body+88);
  return Readable(motion,224) ? motion+208 : 0;
}

// Car impacts. A car driven from the other game is steered along its path
// here; when our player's car hits it, the local physics changes its
// velocity away from what we set. That change is sent to the game that
// drives it, which applies it to its car, so the hit happens on both screens
// (before, the other game's car drove on as if nothing had happened).
struct ImpactWatch { float cmd[3]{}; bool has=false; uint64_t quiet_until=0, next_send=0; };
std::atomic<uint32_t> impacts_sent{0}, impacts_applied{0};
void QueueEvent(const struct Event& e);
bool WatchImpact(uint32_t vehicle,ImpactWatch& w,uint32_t peer_handle,uint64_t now);
void AfterSteer(uint32_t vehicle,ImpactWatch& w) {
  const uint32_t v=VehicleVelocityAddress(vehicle);
  if(!v) { w.has=false; return; }
  for(unsigned i=0;i<3;++i) w.cmd[i]=api->read_f32(v+i*4);
  w.has=true;
}
// Car damage the other game shows: smoke and fire (vehicle +53976 bits
// 0x800 / 0x04000000, set with 8253CD70(handle, smoke, fire) as
// vehicle_set_smoke_and_fire_state does) and wrecks (bit 0x40000000,
// blown up here with the game's detonation 825397E0 as vehicle_detonate).
uint8_t CarDamage(uint32_t vehicle) {
  const uint32_t f=api->read_u32(vehicle+53976);
  return uint8_t(((f&0x800u)?1:0) | ((f&0x04000000u)?2:0) | ((f&0x40000000u)?4:0));
}
unsigned car_damage_applied=0;
void ApplyCarDamage(struct GuestScratch& scratch,uint32_t vehicle,uint8_t want);
void CaptureVehicle(uint32_t player,PlayerPose& p) {
  p.vehicle=-1;
  const uint32_t vehicle=ResolveVehicle(api->read_u32(player+2496));
  if(!vehicle) return;
  p.vehicle=VehicleType(vehicle);
  if(p.vehicle<0) return;
  p.seat=api->read_u32(player+2500); // seat index (825765B8 stores it)
  if(p.seat>=8) p.seat=0;
  for(unsigned i=0;i<3;++i) p.vpos[i]=api->read_f32(vehicle+20+i*4);
  for(unsigned i=0;i<9;++i) p.vrows[i]=api->read_f32(vehicle+32+i*4);
  if(const uint32_t v=VehicleVelocityAddress(vehicle))
    for(unsigned i=0;i<3;++i) { p.vvel[i]=api->read_f32(v+i*4); p.vang[i]=api->read_f32(v+16+i*4); }
  const uint32_t handle=api->read_u32(vehicle+68);
  if(proxy_local && handle==proxy_local) { p.vowner=0; p.vid=proxy_peer; }
  else if(auto t=taken_cars.find(handle); t!=taken_cars.end()) { p.vowner=0; p.vid=t->second; }
  else { p.vowner=1; p.vid=OwnId(handle); }
}

void CaptureLocalPose(uint32_t player) {
  PlayerPose p;
  for(unsigned i=0;i<3;++i) p.pos[i]=api->read_f32(player+20+i*4);
  for(unsigned i=0;i<9;++i) p.rows[i]=api->read_f32(player+32+i*4);
  // Thrown about (jumping out of a moving car) the position read ~700 m off
  // for a moment and the other game showed us there (invisible until
  // bumped into). A jump of over 60 m is only taken once it holds for 1 s.
  {
    static float good[3]={0,0,0}; static bool have=false; static uint64_t jump_since=0;
    const float dx=p.pos[0]-good[0], dy=p.pos[1]-good[1], dz=p.pos[2]-good[2];
    const bool origin=std::fabs(p.pos[0])<1.0f && std::fabs(p.pos[2])<1.0f;
    const uint64_t t=MonoMs();
    if(have && (origin || dx*dx+dy*dy+dz*dz>60.0f*60.0f)) {
      if(!jump_since) jump_since=t;
      if(origin || t-jump_since<1000) { for(unsigned i=0;i<3;++i) p.pos[i]=good[i]; }
      else { for(unsigned i=0;i<3;++i) good[i]=p.pos[i]; jump_since=0; }
    } else { for(unsigned i=0;i<3;++i) good[i]=p.pos[i]; have=true; jump_since=0; }
  }
  const uint32_t desc=api->read_u32(player+228);
  if(Readable(desc,12)) {
    const uint32_t name=api->read_u32(desc+8);
    if(Readable(name,64)) {
      const char* text=static_cast<const char*>(api->guest_pointer(name));
      for(unsigned i=0;i<63 && text[i]>=32 && text[i]<=126;++i) p.model[i]=text[i];
    }
  }
  p.look_length=uint8_t(ReadLook(player,p.look,sizeof(p.look)));
  static unsigned logged_length=1000;
  if(p.look_length!=logged_length) {
    logged_length=p.look_length;
    char line[120];
    std::snprintf(line,sizeof(line),"Local player appearance block: %u bytes",p.look_length);
    ProbeLog(line);
  }
  CaptureVehicle(player,p);
  std::lock_guard lock(pose_mutex);
  p.sequence=local_pose.sequence+1;
  p.time=uint32_t(MonoMs());
  if(api->read_u32(player+216)&0x20000000u) p.flags|=1; // crouching (crouch_start 82461F58 sets it)
  if(const uint32_t car=ResolveVehicle(api->read_u32(player+2496))) {
    p.flags|=uint32_t(CarDamage(car))<<1; // bits 1-3: car damage
    const uint32_t task=api->read_u32(player+2556);
    if(!(task>=3 && task<=0xE) && task!=0x13) p.flags|=0x10;  // bit 4: sitting in it (not still getting in)
  }
  local_pose=p;
}

void WritePose(const PlayerPose& p,uint32_t pos,uint32_t matrix) {
  for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,p.pos[i]);
  for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,p.rows[i]);
}

// Moves a human the way the game teleports characters (8243E6F8: position,
// previous positions, character controller, nav node, matrices), sets its
// facing rows and gives its controller a velocity (8243E948). Humans have no
// physics body index (+784 is -1), so the physics setters don't apply to them.
// find_node: look up the navigation node for the new position (the teleport
// does this when given -1). Passing the current node keeps it, which is only
// right for small moves; a stale node breaks move orders further away.
bool MoveHuman(GuestScratch& scratch,uint32_t obj,uint32_t pos,uint32_t matrix,uint32_t velocity,bool find_node) {
  if(!(api->read_u32(obj+216)&0x4000)) return false;
  for(unsigned i=0;i<9;++i) api->write_f32(obj+32+i*4,api->read_f32(matrix+i*4));
  const uint32_t node=api->read_u32(obj+392);
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=pos; scratch.ctx.r5.u64=0;
  scratch.ctx.r6.u64=find_node || node==0xFFFFFFFFu ? 0xFFFFFFFFFFFFFFFFull : node;
  if(!scratch.Call(0x8243E6F8)) return false;
  if(velocity) {
    scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=velocity;
    scratch.Call(0x8243E948);
  }
  return true;
}

// Gives a human a native "move to" order (82441670, the call behind the
// move_to_do script function): it walks (1), runs (2) or sprints (3) there
// with its normal locomotion and animations.
bool OrderMove(GuestScratch& scratch,uint32_t obj,uint32_t destination,int mode) {
  if(!(api->read_u32(obj+216)&0x4000)) return false;
  const uint32_t flags=scratch.data()+1600;
  scratch.ctx.r3.u64=flags; scratch.ctx.r4.u64=0;
  if(!scratch.Call(0x824A36F0)) return false;
  api->write_u8(flags,api->read_u8(flags)|0xC0);
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=destination;
  scratch.ctx.r5.u64=api->read_u32(obj+508); scratch.ctx.r6.u64=uint64_t(mode);
  scratch.ctx.r7.u64=~0ull; scratch.ctx.r8.u64=flags;
  scratch.ctx.r9.u64=0; scratch.ctx.r10.u64=0;
  return scratch.Call(0x82441670) && scratch.ctx.r3.u32!=0;
}

// Player-class characters. The game keeps 12 player objects (22128 bytes
// each at 0x83B57148); the local player is one, the rest are a free pool that
// multiplayer used for remote players. Only player objects have a clothing
// (customization) record, at +4484.
constexpr uint32_t kPlayerArray=0x83B57148, kPlayerSize=22128;

bool IsPlayerObject(uint32_t obj) {
  return obj>=kPlayerArray && obj<kPlayerArray+12*kPlayerSize && (obj-kPlayerArray)%kPlayerSize==0;
}

void EnsureComponents(GuestScratch& scratch,uint32_t slot);
void LogHex(const char* label,uint32_t address,uint32_t length);

// Body-model diagnostic: the per-slot body table at 0x8309ABF0 (8 bytes per
// slot: character definition, loaded body/morph object), the body-morph
// tables 8248FF10 uses (0x83117BBC/BEC/C1C per slot) and the name strings
// 8247E0E0 builds the per-slot definitions from.
void LogGuestText(const char* label,uint32_t address,uint32_t length) {
  if(!Readable(address,length)) return;
  std::string t;
  for(uint32_t i=0;i<length;++i) { const uint8_t c=api->read_u8(address+i); t+= (c>=32 && c<127) ? char(c) : '.'; }
  char head[120]; std::snprintf(head,sizeof(head),"%s @%08X: ",label,address);
  ProbeLog((head+t).c_str());
}
void DumpBodyTables(const char* when,uint32_t slot) {
  static bool strings_logged=false;
  if(!strings_logged) {
    strings_logged=true;
    LogGuestText("Body: slot 0 definition name",0x8203CFAC,48);
    LogGuestText("Body: per-slot name format",0x82077374,32);
  }
  if(slot>=12) return;
  char l[120];
  const uint32_t table=0x8309ABF0+slot*8;
  std::snprintf(l,sizeof(l),"Body %s slot %u: table %08X %08X, morph tables %08X %08X result %08X",when,slot,
                api->read_u32(table),api->read_u32(table+4),api->read_u32(0x83117BBC+slot*4),
                api->read_u32(0x83117BEC+slot*4),api->read_u32(0x83117C1C+slot*4));
  ProbeLog(l);
  const uint32_t def=api->read_u32(table), obj=api->read_u32(table+4);
  std::snprintf(l,sizeof(l),"Body %s slot %u definition",when,slot); LogGuestText(l,def,96);
  std::snprintf(l,sizeof(l),"Body %s slot %u definition hex",when,slot); LogHex(l,def,160);
  std::snprintf(l,sizeof(l),"Body %s slot %u object hex",when,slot); LogHex(l,obj,160);
}

// How the new player's body-table entry is set up (experiment, see
// DumpBodyTables): 0 = copy the local player's definition and body object
// (as before: one shared body), 1 = copy only the definition, 2 = keep the
// slot's own entry.
int body_table_mode=0;

// Own body for the other player. In single player every player slot uses the
// local player's character definition (StyleTest_PC), i.e. ONE body model
// ("pc_body"): the game bakes each customization record's body sliders into
// the model of the character it belongs to (824904B0 -> 8248EFB8), so the
// last record applied set the shape of both characters (both skinny or both
// muscular). Multiplayer gave each slot its own definition,
// StyleTest_PC_MP<slot+1> (8247E0E0; character.xtbl has MP2..MP12, same
// pc_body.cmeshx), so each remote player had their own body model. Its model
// is streamed in first (stream id at definition +608).
int StreamState(uint32_t id,char* info,size_t n);
uint32_t LookupModel(GuestScratch& scratch,const char* name,uint32_t buffer);
bool own_body=true;                 // mod.ini own_body = 0 turns it off
// Stream group 3 loads the multiplayer player bodies; in single player it has
// no memory (its 11 load slots are empty). Multiplayer's 8247E0E0 gives it
// 11 x (the local player model's stream size + 51200, 256-aligned) from the
// mp_remote_players heap (which overlays single-player memory) and stores
// base/slot size at group 3 +36/+32 (kStreamTable+648/+644). This gives it a
// block of its own instead (82716618, as for the clothing components).
bool EnsurePlayerBodyStreamMemory(GuestScratch& scratch) {
  constexpr uint32_t kGroup3Base=0x83E876B0+648, kGroup3Slot=0x83E876B0+644;
  if(api->read_u32(kGroup3Base)) return true;
  static bool failed=false;
  if(failed) return false;
  const uint32_t def0=LookupModel(scratch,"styletest_pc",scratch.data()+1560);
  uint32_t size=0;
  if(Readable(def0,612)) {
    const uint32_t id0=api->read_u32(def0+608), group=0x83E876B0+(id0>>24)*204;
    const uint32_t head=api->read_u32(group+12);
    unsigned guard=0;
    for(uint32_t e=head;e && Readable(e,48) && guard++<256;) {
      if(api->read_u32(e+4)==id0) { size=api->read_u32(e+8); break; }
      e=api->read_u32(e+40); if(e==head) break;
    }
  }
  if(!size || size>0x2000000) { failed=true; ProbeLog("Own body: local player model stream size not found; no memory for other players' bodies"); return false; }
  const uint32_t slot=(size+51200+255)&~255u, total=slot*11;
  scratch.ctx.r3.u64=total; scratch.ctx.r4.u64=0xFFFFFFFFull; scratch.ctx.r5.u64=0x10000; scratch.ctx.r6.u64=0x20000004;
  const uint32_t block=scratch.Call(0x82716618) ? scratch.ctx.r3.u32 : 0;
  char line[200];
  if(!block) {
    failed=true;
    std::snprintf(line,sizeof(line),"Own body: %.1f MB for player body streaming not available",total/1048576.0);
    ProbeLog(line); return false;
  }
  api->write_u32(kGroup3Slot,slot);
  api->write_u32(kGroup3Base,block);
  std::snprintf(line,sizeof(line),"Own body: player body streaming memory %.1f MB at %08X (11 slots of %u bytes; model %u bytes)",
                total/1048576.0,block,slot,size);
  ProbeLog(line);
  return true;
}
bool last_spawn_own_body=false;     // the last SpawnPlayerObject got its own body model
bool avatar_own_body=false;         // the other player's character has its own body model
uint32_t OwnBodyDefinition(GuestScratch& scratch,uint32_t slot,bool request) {
  if(!own_body || slot<1 || slot>11) return 0;
  char name[32];
  std::snprintf(name,sizeof(name),"StyleTest_PC_MP%u",slot+1);
  const uint32_t def=LookupModel(scratch,name,scratch.data()+1560);
  if(!Readable(def,612)) {
    static uint32_t logged=0;
    if(!(logged&(1u<<slot))) { logged|=1u<<slot; ProbeLog((std::string("Own body: no character definition ")+name).c_str()); }
    return 0;
  }
  const uint32_t id=api->read_u32(def+608);
  // Loaded = in the group's resident list (8250F3C8, as the game checks).
  auto loaded=[&]() { scratch.ctx.r3.u64=id; return scratch.Call(0x8250F3C8) && (scratch.ctx.r3.u32&0xFF)!=0; };
  if(loaded()) return def;
  static uint32_t requested=0;
  static uint64_t last_try[12]={};
  const uint64_t now=MonoMs();
  if(request && now-last_try[slot]>=3000) {
    last_try[slot]=now;
    const bool first=!(requested&(1u<<slot));
    requested|=1u<<slot;
    const uint32_t group=kStreamTable+(id>>24)*204;
    EnsurePlayerBodyStreamMemory(scratch);
    if(first) {
      char l[80]; std::snprintf(l,sizeof(l),"Own body: stream group %u",id>>24); LogHex(l,group,204);
      // Its load slots (count +0, 56-byte slots at [+8]: +4 id, +36 state)
      // and the per-group memory budgets 8241BD40 works with.
      const uint32_t slots=api->read_u32(group+8), count=api->read_u32(group);
      if(count<=16) LogHex("Own body: group slots",slots,count*56);
      LogHex("Own body: budgets 827ACFFC",0x827ACFFC,56);
      LogHex("Own body: budgets 8309A40C",0x8309A40C,56);
      LogHex("Own body: group 0 head",kStreamTable,64);
      // Reference, as 8247DDF0 does for the local player's definition.
      scratch.ctx.r3.u64=group; scratch.ctx.r4.u64=id;
      scratch.ctx.r5.u64=5; scratch.ctx.r6.u64=0; scratch.ctx.r7.u64=1;
      scratch.Call(0x8250C750);
    }
    // Load it now (8250F160 pumps the loader until it is in; returns false
    // at once when it can't be scheduled).
    scratch.ctx.r3.u64=id; scratch.ctx.r4.u64=5; scratch.ctx.r5.u64=0;
    const bool ok=scratch.Call(0x8250F160) && (scratch.ctx.r3.u32&0xFF)!=0;
    char info[160];
    StreamState(id,info,sizeof(info));
    char line[300];
    std::snprintf(line,sizeof(line),"Own body: %s (definition %08X, stream %08X) load %s, now %s; %s",name,def,id,
                  ok?"ok":"refused",loaded()?"LOADED":"not loaded",info);
    ProbeLog(line);
    if(loaded()) return def;
  }
  return 0;
}
// Slot of the player object the pool will hand out next (free list 0x8309ABE8).
uint32_t NextPlayerSlot() {
  const uint32_t head=api->read_u32(0x8309ABE8);
  return head>=0x83B57148 && head<0x83B57148+12*22128 && (head-0x83B57148)%22128==0 ? (head-0x83B57148)/22128 : 99;
}

// Allocate a pool player and spawn it at pos with the local player's model,
// following the multiplayer remote-player creator 823AF420.
uint32_t SpawnPlayerObject(GuestScratch& scratch,uint32_t player,uint32_t pos,uint32_t matrix,const char* tag) {
  char line[300];
  if(!IsPlayerObject(player)) { ProbeLog("Player object: local player not in the pool array"); return 0; }
  const uint32_t local_index=(player-kPlayerArray)/kPlayerSize;
  scratch.ctx.r3.u64=0;
  if(!scratch.Call(0x8247F630)) return 0;
  const uint32_t obj=scratch.ctx.r3.u32;
  if(!IsPlayerObject(obj) || obj==player) {
    std::snprintf(line,sizeof(line),"%s: player pool returned %08X",tag,obj); ProbeLog(line); return 0;
  }
  const uint32_t index=(obj-kPlayerArray)/kPlayerSize, table=0x8309ABF0;
  DumpBodyTables("before (local)",local_index);
  DumpBodyTables("before (new)",index);
  EnsureComponents(scratch,index);
  const uint32_t own_def=OwnBodyDefinition(scratch,index,true);
  last_spawn_own_body=own_def!=0;
  if(own_def) {
    api->write_u32(table+index*8,own_def);
    std::snprintf(line,sizeof(line),"%s: own body model (StyleTest_PC_MP%u, definition %08X)",tag,index+1,own_def);
  } else {
    api->write_u32(table+index*8,api->read_u32(table+local_index*8));
    api->write_u32(table+index*8+4,api->read_u32(table+local_index*8+4));
    std::snprintf(line,sizeof(line),"%s: SHARED body model with the local player (own one not loaded)",tag);
  }
  ProbeLog(line);
  api->write_u32(obj+2324,0);
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=0;
  scratch.Call(0x82435228);
  api->write_u32(obj+2320,0);
  api->write_u8(obj+3593,api->read_u8(obj+3593)&0x7F);
  api->write_u32(obj+3704,0); api->write_u32(obj+3700,0xFFFFFFFF);
  api->write_u32(obj+3728,0xFFFFFFFF); api->write_u32(obj+3732,api->read_u32(0x820144B4));
  api->write_u32(obj+3736,0xFFFFFFFF); api->write_u32(obj+3744,0);
  api->write_u32(obj+568,0);
  scratch.ctx.r3.u64=obj;
  if(!scratch.Call(0x8247E248)) { ProbeLog("Player object: respawn call failed"); return 0; }
  for(unsigned i=0;i<9;++i) api->write_f32(obj+32+i*4,api->read_f32(matrix+i*4));
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=pos; scratch.ctx.r5.u64=0; scratch.ctx.r6.u64=~0ull;
  scratch.Call(0x8243E6F8);
  std::snprintf(line,sizeof(line),"%s: player slot %u object %08X handle %08X customization %08X",
                tag,index,obj,api->read_u32(obj+68),api->read_u32(obj+4484));
  ProbeLog(line);
  DumpBodyTables("after (new)",index);
  if(!last_spawn_own_body) {
    // Shared body model: its sliders must be the local player's, or the game
    // bakes them into the shared model and reshapes this game's player too.
    const uint32_t mine=api->read_u32(player+4484), theirs=api->read_u32(obj+4484);
    if(Readable(mine,12) && Readable(theirs,12)) {
      const uint32_t a=api->read_u32(mine+4), b=api->read_u32(theirs+4);
      if(Readable(a,128*12) && Readable(b,128*12)) {
        std::memcpy(api->guest_pointer(b),api->guest_pointer(a),128*12);
        ProbeLog("Shared body model: the new character's body sliders set to this player's");
      }
    }
  }
  return obj;
}

// Clothing components. Worn items are drawn from "player component data"
// objects: one list per clothing category (54 categories, 36 bytes each at
// 0x8310CF78: +8 count per player, +12 buffer size, +32 ring of components
// linked at +136/+140). Multiplayer gave each remote player its own set
// (8248DAA0: count*11 components with buffers from the "mp_remote_players"
// heap at 0x827DD8CC). Single player only creates the local player's set, so
// a second player's clothing loads find no free component and never finish.
// This adds one more component to every category, once per game session,
// following 8248DAA0.
// The buffers come from a block of their own (82716618, the physical
// allocation the game makes for its "player" heap), not from the
// "mp_remote_players" heap: the game's heap setup (82184260) lays the
// multiplayer heaps over memory single player uses for other heaps, so
// clothing there was overwritten (the other player's torso and legs went
// invisible) and overwrote other data (garbled ground textures).
bool components_added=false;

void EnsureComponents(GuestScratch& scratch,uint32_t slot) {
  if(components_added) return;
  components_added=true;
  constexpr uint32_t kCategories=0x8310CF78;
  unsigned added=0, failed=0;
  uint64_t bytes=0;
  uint32_t total=0;
  for(unsigned c=0;c<54;++c) {
    const uint32_t cat=kCategories+c*36, size=api->read_u32(cat+12);
    if(api->read_u32(cat+8) && size && size<=0x800000) total+=(size+4095)&~4095u;
  }
  // 82716618(size, max address -1, 64 KB alignment, 0x20000004) as in 82184260.
  scratch.ctx.r3.u64=total; scratch.ctx.r4.u64=0xFFFFFFFFull; scratch.ctx.r5.u64=0x10000; scratch.ctx.r6.u64=0x20000004;
  const uint32_t block=(total && scratch.Call(0x82716618)) ? scratch.ctx.r3.u32 : 0;
  if(!block) {
    char line[120]; std::snprintf(line,sizeof(line),"Clothing memory: %.1f MB block not available; the other player can't be dressed",total/1048576.0);
    ProbeLog(line); return;
  }
  uint32_t cursor=block;
  for(unsigned c=0;c<54;++c) {
    const uint32_t cat=kCategories+c*36;
    const uint32_t size=api->read_u32(cat+12);
    if(!api->read_u32(cat+8) || !size || size>0x800000) continue;
    const uint32_t buffer=cursor;
    cursor+=(size+4095)&~4095u;
    if(!scratch.Call(0x82488A30) || !scratch.ctx.r3.u32) { ++failed; continue; }
    const uint32_t comp=scratch.ctx.r3.u32;
    const uint32_t head=api->read_u32(cat+32);
    if(!head) {
      api->write_u32(comp+136,comp); api->write_u32(comp+140,comp); api->write_u32(cat+32,comp);
    } else {
      const uint32_t tail=api->read_u32(head+140);
      api->write_u32(comp+140,tail); api->write_u32(comp+136,head);
      api->write_u32(tail+136,comp); api->write_u32(head+140,comp);
    }
    api->write_u32(comp+12,buffer);
    api->write_u8(comp+132,api->read_u8(comp+132)|0x20);
    api->write_u32(comp+24,slot);
    ++added; bytes+=size;
  }
  char line[160];
  std::snprintf(line,sizeof(line),"Clothing components added: %u (%u failed), %.1f MB in their own block at %08X",
                added,failed,bytes/1048576.0,block);
  ProbeLog(line);
}

// Body shape: the customization record's +4 array holds 128 morph entries of
// 12 bytes ({0, value, value}). Values are sent as floats and written back,
// then the slot's component morphs are reloaded (8248FF10, 8248D928) and the
// look rebuilt (8248A900), as the wardrobe does for the local player.
constexpr unsigned kMorphs=128;
unsigned ReadMorphs(uint32_t human,float* out) {
  const uint32_t custom=api->read_u32(human+4484);
  if(!Readable(custom,12)) return 0;
  const uint32_t morphs=api->read_u32(custom+4);
  if(!Readable(morphs,kMorphs*12)) return 0;
  for(unsigned i=0;i<kMorphs;++i) out[i]=api->read_f32(morphs+i*12+4);
  return kMorphs;
}
bool ApplyMorphs(GuestScratch& scratch,uint32_t human,const float* values) {
  const uint32_t custom=api->read_u32(human+4484);
  if(!Readable(custom,12)) return false;
  const uint32_t morphs=api->read_u32(custom+4);
  if(!Readable(morphs,kMorphs*12)) return false;
  // Each entry is {?, wanted (+4), baked (+8)}: the game's body update
  // (824904B0) bakes the difference into the character's mesh when they
  // differ and then sets baked = wanted. Only the wanted value is written
  // (writing both made the game see nothing to do: the copy stayed thin).
  for(unsigned i=0;i<kMorphs;++i) {
    if(!std::isfinite(values[i]) || values[i]<-4 || values[i]>4) return false;
  }
  for(unsigned i=0;i<kMorphs;++i) api->write_f32(morphs+i*12+4,values[i]);
  return true;
}

// Body slider summary for the log: how many differ from the default 0.5 and
// the first few of them (index:value).
std::string MorphSummary(const float* v,unsigned n) {
  unsigned off=0; std::string first;
  for(unsigned i=0;i<n;++i) {
    if(std::fabs(v[i]-0.5f)<0.001f) continue;
    if(++off<=8) { char b[24]; std::snprintf(b,sizeof(b)," %u:%.3f",i,v[i]); first+=b; }
  }
  return std::to_string(off)+" of "+std::to_string(n)+" not default"+first;
}
// Compares a character's slider entries with the values it should have:
// wanted (+4) and baked into the model (+8).
std::string MorphState(uint32_t human,const std::vector<float>& values) {
  const uint32_t custom=api->read_u32(human+4484);
  if(!Readable(custom,12)) return "no customization record";
  const uint32_t morphs=api->read_u32(custom+4);
  if(!Readable(morphs,kMorphs*12) || values.size()!=kMorphs) return "unreadable";
  unsigned wanted=0, baked=0;
  for(unsigned i=0;i<kMorphs;++i) {
    wanted+=std::fabs(api->read_f32(morphs+i*12+4)-values[i])<0.001f;
    baked+=std::fabs(api->read_f32(morphs+i*12+8)-values[i])<0.001f;
  }
  char b[160];
  std::snprintf(b,sizeof(b),"wanted matches %u/128, baked into the model %u/128 (record %08X, sliders %08X)",wanted,baked,custom,morphs);
  return b;
}

// Colour choices (skin tone, hair colour, ...): the customization slot's
// list at 0x8304D160 (count per slot, 5 entries of 12 bytes at +48+slot*60),
// packed by the game's multiplayer code as (key, option index) pairs of 8
// bytes (82321F38) and applied with 82322050(record, bytes, length), which
// sets each option through 82322328 - the same calls multiplayer used when a
// remote player's customization arrived (82376BD0).
unsigned ReadExtras(GuestScratch& scratch,uint32_t human,uint8_t* out) {
  const uint32_t custom=api->read_u32(human+4484);
  if(!Readable(custom,12) || api->read_u32(custom+8)>=12) return 0;
  const uint32_t size=scratch.data()+1620, buffer=scratch.data()+1624;
  std::memset(api->guest_pointer(buffer),0xFF,40);
  api->write_u32(size,0);
  scratch.ctx.r3.u64=buffer; scratch.ctx.r4.u64=size; scratch.ctx.r5.u64=human; scratch.ctx.r6.u64=40;
  if(!scratch.Call(0x82321F38)) return 0;
  const uint32_t length=api->read_u32(size);
  if(!length || length>40 || (length&7)) return 0;
  std::memcpy(out,api->guest_pointer(buffer),length);
  return length;
}
bool ApplyExtras(GuestScratch& scratch,uint32_t human,const uint8_t* bytes,unsigned length,bool rebuild=true) {
  const uint32_t custom=api->read_u32(human+4484);
  if(!Readable(custom,12) || api->read_u32(custom+8)>=12 || !length || length>40 || (length&7)) return false;
  const uint32_t buffer=scratch.data()+1624;
  std::memcpy(api->guest_pointer(buffer),bytes,length);
  scratch.ctx.r3.u64=custom; scratch.ctx.r4.u64=buffer; scratch.ctx.r5.u64=length;
  if(!scratch.Call(0x82322050)) return false;
  if(rebuild) { scratch.ctx.r3.u64=custom; scratch.Call(0x8248A900); } // rebuild the look
  return true;
}
std::string HexBytes(const uint8_t* bytes,unsigned length) {
  std::string out;
  char b[4];
  for(unsigned i=0;i<length;++i) { std::snprintf(b,sizeof(b),"%02X",bytes[i]); out+=b; if((i&7)==7 && i+1<length) out+=' '; }
  return out;
}
// Body-shape diagnostic (player copy, mod.ini body_test = 1): which morph
// values differ between two characters, first few listed.
std::string MorphDiff(uint32_t a,uint32_t b) {
  float x[128], y[128];
  if(ReadMorphs(a,x)!=128 || ReadMorphs(b,y)!=128) return "unreadable";
  std::string out;
  unsigned n=0;
  for(unsigned i=0;i<128;++i) if(x[i]!=y[i]) {
    if(++n<=12) { char t[40]; std::snprintf(t,sizeof(t)," %u:%.3f/%.3f",i,x[i],y[i]); out+=t; }
  }
  char h[40]; std::snprintf(h,sizeof(h),"%u differ",n);
  return h+out;
}

// Worn items as multiplayer customization sync packs them (8248DCA8):
// a count byte, then 4 bytes per item.
// For the local player 8248DCA8 reads a wardrobe copy (8236DC00) that isn't
// refreshed when another save is loaded; the generic branch (823186A0) reads
// what the character actually wears. It picks the branch by comparing with
// the local player's record, so that pointer is hidden for the call.
unsigned ReadWorn(GuestScratch& scratch,uint32_t obj,uint8_t* out,unsigned capacity) {
  const uint32_t custom=api->read_u32(obj+4484);
  if(!Readable(custom,12)) return 0;
  const uint32_t writer=scratch.data()+1700, buffer=scratch.data()+1712;
  api->write_u32(writer,0); api->write_u32(writer+4,buffer);
  const uint32_t local=api->read_u32(0x8309ABEC);
  const bool hide=Readable(local,4488) && api->read_u32(local+4484)==custom;
  if(hide) api->write_u32(local+4484,0);
  scratch.ctx.r3.u64=writer; scratch.ctx.r4.u64=custom;
  const bool called=scratch.Call(0x8248DCA8);
  if(hide) api->write_u32(local+4484,custom);
  if(!called) return 0;
  const uint32_t length=api->read_u32(writer);
  if(!length || length>capacity) return 0;
  std::memcpy(out,api->guest_pointer(buffer),length);
  return length;
}

// Worn-item list of a customization slot: count at 0x83036140+13056+slot*4,
// 20-byte entries (item first) at 0x83036140+13120+slot*1080.
constexpr uint32_t kSlotTables=0x83036140;
unsigned WornCount(uint32_t custom) {
  const uint32_t slot=api->read_u32(custom+8);
  return slot<12 ? api->read_u32(kSlotTables+13056+slot*4) : 0;
}

// Takes every item off (8248D198 per item, then 8248D340), like the first
// half of 82318B38. Its second half wipes the slot's tables, which leaves the
// character unable to show clothing, so it is not used.
void UnwearAll(GuestScratch& scratch,uint32_t custom) {
  const uint32_t slot=api->read_u32(custom+8);
  if(slot>=12) return;
  for(uint32_t i=0;i<54 && i<api->read_u32(kSlotTables+13056+slot*4);++i) {
    scratch.ctx.r3.u64=custom;
    scratch.ctx.r4.u64=api->read_u32(kSlotTables+13120+slot*1080+i*20);
    scratch.Call(0x8248D198);
  }
  scratch.Call(0x8248CFC8);
  scratch.ctx.r3.u64=custom; scratch.Call(0x8248D340);
  scratch.Call(0x8248CFC8);
}

// Clothing requests the game still has to process (8248CFC8 loops until this
// is empty; the game's frame update works through it too).
bool ClothingQueueIdle() { return api->read_u32(0x830B71D4)==0; }

// Wear the given items (8248DE28) without waiting for them to stream.
bool WearItems(GuestScratch& scratch,uint32_t custom,const uint8_t* worn,unsigned length) {
  if(!Readable(custom,12) || !length || length>256) return false;
  const uint32_t reader=scratch.data()+1700, buffer=scratch.data()+1712, cursor=scratch.data()+1690;
  std::memcpy(api->guest_pointer(buffer),worn,length);
  api->write_u32(reader,length); api->write_u32(reader+4,buffer); api->write_u32(cursor,0);
  scratch.ctx.r3.u64=custom; scratch.ctx.r4.u64=reader; scratch.ctx.r5.u64=cursor; scratch.ctx.r6.u64=0;
  return scratch.Call(0x8248DE28) && api->read_u32(cursor)==length;
}

// Take off what the slot still lists, wear the given items (8248DE28) and
// wait for them to stream (8248CFC8).
bool DressPlayerObject(GuestScratch& scratch,uint32_t obj,const uint8_t* worn,unsigned length) {
  const uint32_t custom=api->read_u32(obj+4484);
  if(!Readable(custom,12) || !length || length>256) return false;
  const unsigned before=WornCount(custom);
  if(before) UnwearAll(scratch,custom);
  const unsigned cleared=WornCount(custom);
  const uint32_t reader=scratch.data()+1700, buffer=scratch.data()+1712, cursor=scratch.data()+1690;
  std::memcpy(api->guest_pointer(buffer),worn,length);
  api->write_u32(reader,length); api->write_u32(reader+4,buffer); api->write_u32(cursor,0);
  scratch.ctx.r3.u64=custom; scratch.ctx.r4.u64=reader; scratch.ctx.r5.u64=cursor; scratch.ctx.r6.u64=0;
  if(!scratch.Call(0x8248DE28)) return false;
  scratch.Call(0x8248CFC8);
  char line[160];
  std::snprintf(line,sizeof(line),"Dress slot %u: listed %u, after taking off %u, now %u (read %u of %u bytes)",
                api->read_u32(custom+8),before,cleared,WornCount(custom),api->read_u32(cursor),length);
  ProbeLog(line);
  return api->read_u32(cursor)==length;
}

void ReleasePlayerObject(GuestScratch& scratch,uint32_t obj) {
  if(!IsPlayerObject(obj)) return;
  const uint32_t custom=api->read_u32(obj+4484);
  if(Readable(custom,12) && WornCount(custom)) UnwearAll(scratch,custom); // leave the slot clean
  scratch.ctx.r3.u64=obj;
  scratch.Call(0x8247DBB0);
}

// Combat and action events.
// - Actions: every animation action the local player plays goes through
//   82444F88(human, action, r5, r6, r7): punches (0xCA/0xCC/0xCE), jump
//   (0x20 start, 0x34 air, 0x2A land), stances (0xB3-0xB5, 0xD9, 0xDA), hit
//   reactions (0xE0) and so on. They are sent to the peer and played on the
//   character that shows us there.
// - Damage: 824470D0(victim, attacker, r5-r10, f1 amount, stack bytes at
//   caller sp+87/+95) is the game's damage entry (npc_kill uses it). When the
//   local player damages the peer's character, the damage isn't applied to
//   that stand-in; it is sent, and the peer's game applies it to the real
//   player with our stand-in as the attacker, so it reacts and loses health.
struct Event { uint32_t kind, a[8]; float f; };
enum : uint32_t { kEventAction=1, kEventDamage=2, kEventCall=3, kEventPedAction=4,
                  kEventPedDamage=5,      // joiner hit a copy of a host person: a[0] host handle, a[1..6] r5-r10, a[7] bytes
                  kEventPlayerHitByPed=6, // host: a shared person hit the joiner's stand-in: a[0] attacker host handle
                  kEventPedCall=7,        // host: knockdown / hit reaction on a shared person: a[0] fn, a[1] host handle, a[2..5] r4-r7, a[6] arg kinds
                  kEventCarImpact=8,      // our car hit a car the receiver drives: a[0] its handle as the receiver knows it, a[1..3] velocity change
                  kEventEnterCar=9,       // our player's seat request (8257B2C0): a[0] car handle, a[1] 1 = the receiver's handle, a[2] mode, a[3] seat, a[4..6] flag bytes
                  kEventExitCar=10,       // our player's exit request (8257B8E0): a[0..7] request words +4..+32
                  kEventCarAction=11,     // an action of our player's getting in (door, pulling out, climbing in): a[0..3] r4-r7
                  kEventCarDoor=12,       // our player's car door (825352E0): a[0] car id, a[1] theirs, a[2] door, a[3..5] f1-f3
                  kEventVictimAction=13,  // the driver our player pulls out plays a move: a[0] car id, a[1] theirs, a[2..5] r4-r7, a[6..7] f1-f2
                  kEventVictimExit=14,    // that driver's exit request (thrown out): a[0..7] request words +4..+32
                  kEventHostModels=15,    // host: people models loaded there: a[0] count, a[1..7] two 16-bit model ids each
                  kEventPedEffect=16,     // host: a shared person has an in-game effect (the arrow over who to attack): a[0] host handle, a[1..4] r3,r5,r6,r7 of 82403C38
                  kEventPedReact=17       // host: hit reaction 82458EB8 on a shared person: a[0] host handle, a[1] attacker kind (2 host player, 3 joiner, 4 shared person a[2]), a[3..5] hit position, a[6] r6, a[7] r7 | r8<<8 | 1<<16 position given
};
// In-game effects on people: the arrow over the characters a mission wants
// attacked. ingame_effect_add_npc (824CF198) calls 82403C38(1, handle, 4,
// effect, 0x828398C4); ingame_effect_remove_npc (824CF710) 82403F58(handle, 0).
// The joiner's copies are made by the mod, not by its mission, so they had
// none (only the joiner's own characters taken over had one: the first few).
// The host sends the effects of its shared people every second; the joiner
// puts them on the copies and takes them off when the host's go.
struct PedEffect { uint32_t kind=0, r5=0, effect=0, data=0; uint64_t seen=0; };
std::unordered_map<uint32_t,PedEffect> host_effects;    // host: handle -> effect (game thread)
std::unordered_map<uint32_t,PedEffect> joiner_effects;  // joiner: host handle -> effect (game thread)
int applying_effect=0;
unsigned effects_sent=0, effects_added=0, effects_removed=0;
WmlGuestFunction original_effect_add=nullptr, original_effect_remove=nullptr;
void EffectAddHook(WmlContext* c,uint8_t* b) {
  if(hosting.load() && !applying_effect && uint32_t(api->get_r(c,3))==1) {
    const uint32_t data=uint32_t(api->get_r(c,7));
    if(!data || (data>=0x82000000u && data<0x83000000u))
      host_effects[uint32_t(api->get_r(c,4))]=PedEffect{1,uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),data,0};
  }
  original_effect_add(c,b);
}
void EffectRemoveHook(WmlContext* c,uint8_t* b) {
  if(hosting.load() && !applying_effect) host_effects.erase(uint32_t(api->get_r(c,3)));
  original_effect_remove(c,b);
}
bool WatchImpact(uint32_t vehicle,ImpactWatch& w,uint32_t peer_handle,uint64_t now) {
  const uint32_t v=VehicleVelocityAddress(vehicle);
  if(!v) { w.has=false; return false; }
  if(now<w.quiet_until) return true;   // let the hit play out here first
  if(!w.has) return false;
  float dv[3], m=0;
  for(unsigned i=0;i<3;++i) { dv[i]=api->read_f32(v+i*4)-w.cmd[i]; m+=dv[i]*dv[i]; }
  w.has=false;
  if(m<3.0f*3.0f || m>60.0f*60.0f || now<w.next_send) return false;
  const uint32_t player=api->read_u32(0x8309ABEC);
  const uint32_t mine=ResolveVehicle(api->read_u32(player+2496));
  if(!mine || mine==vehicle) return false;
  const float dx=api->read_f32(mine+20)-api->read_f32(vehicle+20), dz=api->read_f32(mine+28)-api->read_f32(vehicle+28);
  if(dx*dx+dz*dz>9.0f*9.0f) return false;
  // Not sent any more: both games already have a moving copy of the other
  // car, so each resolves the hit on its own car; sending it as well pushed
  // cars twice (the joiner's car flew off and the two games disagreed).
  // The hit is only left to play here for a moment before steering resumes.
  (void)peer_handle;
  ++impacts_sent;
  w.quiet_until=now+300; w.next_send=now+400;
  return true;
}
int applying_ped_call=0; // joiner: a ped call from the host is being applied
int ped_react_depth=0;    // host: inside a shared person's hit reaction (its falls / states go with it)
unsigned ped_reacts_sent=0, ped_reacts_played=0, joiner_hit_reactions=0;
void ApplyPedReact(struct GuestScratch& scratch,const Event& e,uint64_t now);
bool IsSentPed(uint32_t handle);                 // host: this person is shared with the other game
void MakePassive(uint32_t obj,bool targetable=false);
void MatchCrouch(struct GuestScratch& scratch,uint32_t obj,bool want);
void SetCharacterHidden(struct GuestScratch& scratch,uint32_t obj,bool hide);
uint32_t SharedPedHostHandle(uint32_t local);    // joiner: host handle of a copy (0 if not a copy)
uint32_t SharedPedLocalHandle(uint32_t host);    // joiner: copy of a host person (0 if none)
std::atomic<uint32_t> ped_hits_sent{0}, ped_hits_applied{0}, hits_from_people{0};
void ApplyPedAction(GuestScratch& scratch,const struct Event& e,uint64_t now);
void ApplyPedCall(GuestScratch& scratch,const struct Event& e,uint64_t now);
bool forward_damage=false; // mod.ini [settings] damage_forward = 1
#pragma pack(push,1)
struct EventPacket { char magic[4]; uint32_t sequence; uint32_t kind, a[8]; float f; };
#pragma pack(pop)
std::mutex event_mutex;
std::vector<Event> events_out, events_in;
std::atomic<uint32_t> ped_actions_sent{0}, ped_actions_played{0}, ped_calls_sent{0}, ped_calls_played{0};
uint32_t own_actions_blocked=0; // game thread (joiner): actions a copy's own AI tried
std::atomic<uint32_t> actions_sent{0}, actions_played{0}, hits_sent{0}, hits_taken{0};
bool avatar_on_foot=false; // game thread: the other player's character is on foot here
uint64_t avatar_action_until=0; // game thread: no move orders while an action plays
bool avatar_jumping=false;      // between jump start (state 4/1) and landing (4/0xE)
bool avatar_reorder=false;      // give the character a fresh move order (after landing)
bool avatar_climbing=false;     // a climb plays: its animation moves the character over the wall
Event pending_action{};         // an action the character refused, retried after the next state change
uint64_t pending_action_until=0;

void QueueEvent(const Event& e) {
  if(!connected.load()) return;
  std::lock_guard lock(event_mutex);
  if(events_out.size()<64) events_out.push_back(e);
}

// Calls made inside a replicated call are replayed by that call on the other
// side; sending them too plays them twice and restarts the animation.
thread_local int replicated_depth=0;
// Our player's own getting in / out: the other game replays the request
// itself, which plays the walk to the door, the door and the seat there. The
// states and car actions it causes here are not sent on top of it.
uint64_t own_car_move_until=0;
inline bool CarMoveAction(uint32_t lr) { return (lr>=0x82570000u && lr<0x82580000u) || lr==0x8252ED04u; }
WmlGuestFunction original_play_action=nullptr;
// F9: 15 s trace of what our player and the other player's character do
// (actions, movement states, seat and exit requests, the character's state
// each frame), for working out climbing and getting in / out of cars.
std::atomic<uint64_t> trace_until{0};
bool Tracing() { return MonoMs()<trace_until.load(); }
uint32_t trace_driver=0;   // handle of the driver being pulled out (F9 trace)
const char* TraceWho(uint32_t obj) {
  if(obj && obj==api->read_u32(0x8309ABEC)) return "me";
  if(obj && avatar_local && obj==ResolveHuman(avatar_local)) return "other";
  if(obj && trace_driver && obj==ResolveHuman(trace_driver)) return "driver";
  return nullptr;
}
void TraceLine(const char* who,const char* what,uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t lr,bool played) {
  char line[180];
  std::snprintf(line,sizeof(line),"TRACE %s %s %X %X %X %X from %08X%s%s",who,what,a,b,c,d,lr,replicated_depth?" (replayed)":"",played?"":" refused");
  ProbeLog(line);
}
// Play-action's f1-f3 (blend and speed) go along: without them the other
// game played the animation with whatever was left in those registers.
constexpr uint32_t kFloatsMark=0xF1F2F300u;
void PackActionFloats(WmlContext* c,Event& e) {
  for(int i=0;i<3;++i) { const float f=float(api->get_f(c,1+i)); std::memcpy(&e.a[4+i],&f,4); }
  e.a[7]=kFloatsMark;
}
void UnpackActionFloats(GuestScratch& scratch,const Event& e) {
  double f[3]={0,0,0};
  if(e.a[7]==kFloatsMark) for(int i=0;i<3;++i) { float v; std::memcpy(&v,&e.a[4+i],4); if(std::isfinite(v)) f[i]=v; }
  scratch.ctx.f1.f64=f[0]; scratch.ctx.f2.f64=f[1]; scratch.ctx.f3.f64=f[2];
}
uint32_t CarEventId(uint32_t h,uint32_t& theirs);
void PlayActionHook(WmlContext* c,uint8_t* b) {
  {
    // The driver our player pulls out of a car: its moves go to the other
    // game, where the same driver plays them (it only stepped out there).
    const uint32_t who=uint32_t(api->get_r(c,3)), me=api->read_u32(0x8309ABEC);
    if(who && who!=me && !replicated_depth && connected.load() && CarMoveAction(api->get_lr(c)) && Readable(who,4252) && me) {
      const uint32_t mycar=api->read_u32(me+2496);
      const uint32_t av=avatar_local ? ResolveHuman(avatar_local) : 0;
      if(mycar && who!=av && api->read_u32(who+2496)==mycar) {
        uint32_t theirs=0; const uint32_t id=CarEventId(mycar,theirs);
        Event e{kEventVictimAction,{id,theirs,uint32_t(api->get_r(c,4)),uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7))},0};
        for(int i=0;i<2;++i) { const float f=float(api->get_f(c,1+i)); std::memcpy(&e.a[6+i],&f,4); }
        QueueEvent(e);
      }
    }
  }
  if(Tracing()) if(const char* who=TraceWho(uint32_t(api->get_r(c,3))))
    TraceLine(who,"action",uint32_t(api->get_r(c,4)),uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7)),api->get_lr(c),true);
  const uint32_t human=uint32_t(api->get_r(c,3));
  // Shared people (host): what they do (sit on a bench, lean, talk...) is
  // played on their copies too.
  if(human && hosting.load() && connected.load() && share_peds && !replicated_depth && Readable(human,4252) &&
     human!=api->read_u32(0x8309ABEC) && IsSentPed(api->read_u32(human+68))) {
    Event e{kEventPedAction,{uint32_t(api->get_r(c,4)),uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7)),
            api->read_u32(human+68)},0};
    QueueEvent(e); ++ped_actions_sent;
  }
  // Joiner: copies of the host's people only do what the host's do. Their
  // own AI picked its own idles (phone, smoking, looking around) and
  // reactions, so the same person moved differently on each side.
  if(human && !hosting.load() && connected.load() && share_peds && !replicated_depth && Readable(human,4252) &&
     human!=api->read_u32(0x8309ABEC) && SharedPedHostHandle(api->read_u32(human+68))) {
    ++own_actions_blocked;
    api->set_r(c,3,0);
    return;
  }
  // Our getting in (door, pulling a driver out, climbing in): sent as car
  // actions and played on our character there.
  if(human && human==api->read_u32(0x8309ABEC) && !replicated_depth && connected.load() && CarMoveAction(api->get_lr(c)) &&
     ResolveVehicle(api->read_u32(human+2496)) && api->read_u32(human+2556)!=0x12) {
    Event e{kEventCarAction,{uint32_t(api->get_r(c,4)),uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7))},0};
    PackActionFloats(c,e); QueueEvent(e); ++actions_sent;
  }
  if(human && human==api->read_u32(0x8309ABEC) && !replicated_depth &&
     !(CarMoveAction(api->get_lr(c)) || (MonoMs()<own_car_move_until && api->get_r(c,4)==0x160))) {
    Event e{kEventAction,{uint32_t(api->get_r(c,4)),uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7))},0};
    PackActionFloats(c,e); QueueEvent(e); ++actions_sent;
  }
  original_play_action(c,b);
}

// Calls on the local player that are repeated on our character in the peer's
// game. Arguments that point into the local player object are sent as an
// offset and translated to the character there.
constexpr uint32_t kTranslate=0x80000000u; // arg flag bit in a[7]
void QueueCall(uint32_t fn,WmlContext* c) {
  const uint32_t player=api->read_u32(0x8309ABEC);
  Event e{kEventCall,{fn},0};
  uint32_t flags=0;
  for(int i=0;i<4;++i) {
    uint32_t v=uint32_t(api->get_r(c,4+i));
    const uint32_t avatar_obj=avatar_local ? ResolveHuman(avatar_local) : 0;
    if(v>=player && v<player+22128) { v-=player; flags|=1u<<i; }
    else if(avatar_obj && v>=avatar_obj && v<avatar_obj+22128) { v-=avatar_obj; flags|=0x10u<<i; } // the peer, seen here
    e.a[1+i]=v;
  }
  e.a[7]=flags;
  QueueEvent(e);
}
// Host: a knockdown or hit reaction on one of the shared people, repeated on
// its copy in the joiner's game. Pointer arguments are translated: into the
// person itself (1), our player (2: the joiner's copy of us), the joiner's
// stand-in here (3: the joiner's own player), another shared person (4).
void QueuePedCall(uint32_t fn,WmlContext* c,uint32_t victim) {
  const uint32_t player=api->read_u32(0x8309ABEC);
  const uint32_t avatar_obj=avatar_local ? ResolveHuman(avatar_local) : 0;
  Event e{kEventPedCall,{fn,api->read_u32(victim+68)},0};
  uint32_t kinds=0;
  for(int i=0;i<4;++i) {
    uint32_t v=uint32_t(api->get_r(c,4+i)), k=0;
    if(v>=victim && v<victim+4252) { v-=victim; k=1; }
    else if(v>=player && v<player+4252) { v-=player; k=2; }
    else if(avatar_obj && v>=avatar_obj && v<avatar_obj+4252) { v-=avatar_obj; k=3; }
    else if(v>=0x83000000u && v<0x84000000u && Readable(v,4252) && api->read_u32(v+72)==1 && ResolveHuman(api->read_u32(v+68))==v) {
      v=api->read_u32(v+68); k=4;
    }
    e.a[2+i]=v; kinds|=k<<(i*3);
  }
  e.a[6]=kinds;
  QueueEvent(e); ++ped_calls_sent;
}
WmlGuestFunction original_swing=nullptr, original_state=nullptr, original_melee_hit=nullptr, original_attack=nullptr;
void LogLocalCall(const char* what,WmlContext* c) {
  static unsigned logged=0;
  if(logged++>=80) return;
  char line[160];
  std::snprintf(line,sizeof(line),"Local %s(%08X %08X %08X %08X) from %08X",what,uint32_t(api->get_r(c,4)),uint32_t(api->get_r(c,5)),
                uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7)),api->get_lr(c));
  ProbeLog(line);
}
void SwingHook(WmlContext* c,uint8_t* b) {
  if(uint32_t(api->get_r(c,3))==api->read_u32(0x8309ABEC) && connected.load()) LogLocalCall("melee request",c);
  original_swing(c,b);
}
// 82459430(human, attack): a punch/kick from the controls (air punches take
// this path; it plays the attack and runs the hit search 824541F8).
WmlGuestFunction original_punch=nullptr;
void PunchHook(WmlContext* c,uint8_t* b) {
  const bool mine=uint32_t(api->get_r(c,3))==api->read_u32(0x8309ABEC) && connected.load() && !replicated_depth;
  if(mine) { LogLocalCall("punch",c); QueueCall(0x82459430,c); ++actions_sent; }
  // Only our own punch is sent whole; people's punches and kicks go out as
  // the actions they play (their attacks weren't shown in the other game).
  if(mine) { ++replicated_depth; original_punch(c,b); --replicated_depth; } else original_punch(c,b);
}
// 824497F0(human, duration, r5, ...): knockdown / ragdoll fall (used by the
// melee hit 82454A30 and many others). Repeated on our character there so a
// fall on one screen is a fall on the other.
WmlGuestFunction original_knockdown=nullptr;
// Knockdowns and hit reactions of shared people: the host's game decides them
// (sent to the joiner); on the joiner a copy only has the ones the host sent.
// Returns true when the call must be skipped.
bool SharedPersonCall(uint32_t fn,WmlContext* c) {
  if(!connected.load() || !share_peds) return false;
  const uint32_t victim=uint32_t(api->get_r(c,3));
  if(!victim || victim==api->read_u32(0x8309ABEC) || !Readable(victim,4252)) return false;
  if(hosting.load()) {
    if(IsSentPed(api->read_u32(victim+68)) && !ped_react_depth) QueuePedCall(fn,c,victim);
    return false;
  }
  if(!applying_ped_call && SharedPedHostHandle(api->read_u32(victim+68))) { api->set_r(c,3,0); return true; }
  return false;
}
// 82463100 (movement) knocks a character down after 500 ms of no vertical
// movement while not on the ground (call site 82463558): that's standing on
// top of another character. With the other player's character and the copies
// of the host's people placed every frame, players ended up on top of them
// and fell over every half second ("breakdancing"). Instead: no fall, and
// the player steps off (UpdateBody, UnstickPlayer).
uint32_t StandingOnSharedCharacter(uint32_t player);   // the character under the player, or 0
std::atomic<uint32_t> unstick_from{0};
void KnockdownHook(WmlContext* c,uint8_t* b) {
  if(connected.load() && !replicated_depth && api->get_lr(c)==0x82463558u &&
     uint32_t(api->get_r(c,3))==api->read_u32(0x8309ABEC)) {
    if(const uint32_t under=StandingOnSharedCharacter(uint32_t(api->get_r(c,3)))) {
      unstick_from=under;
      api->set_r(c,3,0);
      return;
    }
    // Standing on something else that isn't ground (1.32: on one of this
    // game's hidden mission characters, the joiner fell over every 0.5 s for
    // a minute, on both screens): no fall in co-op.
    static unsigned logged=0;
    if(logged++<5) ProbeLog("Player standing on something that isn't ground: no fall");
    api->set_r(c,3,0);
    return;
  }
  // The victim's game decides falls. A fall our game decides for the peer's
  // stand-in (our punch landing here) is dropped; the peer's game sends the
  // real one back if its player goes down.
  const uint32_t avatar=avatar_local ? ResolveHuman(avatar_local) : 0;
  if(avatar && uint32_t(api->get_r(c,3))==avatar && connected.load() && !replicated_depth) { api->set_r(c,3,0); return; }
  if(SharedPersonCall(0x824497F0,c)) return;
  const bool mine=uint32_t(api->get_r(c,3))==api->read_u32(0x8309ABEC) && connected.load() && !replicated_depth;
  if(mine) { LogLocalCall("knockdown",c); QueueCall(0x824497F0,c); ++actions_sent; }
  ++replicated_depth; original_knockdown(c,b); --replicated_depth;
}
// 82458EB8(human, ...): starts a melee attack (plays the punch action and
// arms the hit window). Repeated on our character in the peer's game.
void AttackHook(WmlContext* c,uint8_t* b) {
  // Hit reactions of the host's people: the host's game decides them and the
  // joiner plays the same one (only a few falls came across; the joiner's hits
  // made nobody flinch on the host, and its own copies reacted on their own).
  if(connected.load() && share_peds) {
    const uint32_t victim=uint32_t(api->get_r(c,3)), player=api->read_u32(0x8309ABEC);
    if(victim && victim!=player && Readable(victim,4252) && api->read_u32(victim+72)==1) {
      if(hosting.load() && IsSentPed(api->read_u32(victim+68))) {
        const uint32_t attacker=uint32_t(api->get_r(c,4)), avatar=avatar_local ? ResolveHuman(avatar_local) : 0;
        Event e{kEventPedReact,{api->read_u32(victim+68)},0};
        if(attacker && attacker==player) e.a[1]=2;
        else if(attacker && attacker==avatar) e.a[1]=3;
        else if(attacker && Readable(attacker,4252) && api->read_u32(attacker+72)==1 && IsSentPed(api->read_u32(attacker+68))) { e.a[1]=4; e.a[2]=api->read_u32(attacker+68); }
        const uint32_t hit=uint32_t(api->get_r(c,5));
        if(hit && Readable(hit,12)) { for(int i=0;i<3;++i) e.a[3+i]=api->read_u32(hit+i*4); e.a[7]|=1u<<16; }
        e.a[6]=uint32_t(api->get_r(c,6));
        e.a[7]|=(uint32_t(api->get_r(c,7))&0xFF)|((uint32_t(api->get_r(c,8))&0xFF)<<8);
        QueueEvent(e); ++ped_reacts_sent;
        ++ped_react_depth; original_attack(c,b); --ped_react_depth;
        return;
      }
      if(!hosting.load() && !applying_ped_call && SharedPedHostHandle(api->read_u32(victim+68))) { api->set_r(c,3,0); return; }
    }
  }
  const bool mine=uint32_t(api->get_r(c,3))==api->read_u32(0x8309ABEC) && connected.load() && !replicated_depth;
  if(mine) { LogLocalCall("melee attack",c); QueueCall(0x82458EB8,c); ++actions_sent; }
  if(mine) { ++replicated_depth; original_attack(c,b); --replicated_depth; } else original_attack(c,b);
}
void StateHook(WmlContext* c,uint8_t* b) {
  if(Tracing()) if(const char* who=TraceWho(uint32_t(api->get_r(c,3))))
    TraceLine(who,"state",uint32_t(api->get_r(c,4)),uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7)),api->get_lr(c),true);
  const uint32_t state=uint32_t(api->get_r(c,4));
  // Hit reactions on the peer's stand-in come from the peer's game only.
  const uint32_t avatar=avatar_local ? ResolveHuman(avatar_local) : 0;
  if(avatar && state==8 && uint32_t(api->get_r(c,3))==avatar && connected.load() && !replicated_depth) return;
  if(state==8 && SharedPersonCall(0x82441308,c)) return;
  if(uint32_t(api->get_r(c,3))==api->read_u32(0x8309ABEC) && connected.load() && !replicated_depth) {
    // Jumps (4) and hit reactions (8) always; the other movement states
    // (climbing, vaulting, ledges...) when they change, so the other game's
    // character climbs too instead of floating over the wall.
    static uint32_t last_state=~0u, last_sub=~0u; static uint64_t last_sent=0;
    const uint32_t sub=uint32_t(api->get_r(c,5));
    const uint64_t t=MonoMs();
    const bool car_move=state==3 || (t<own_car_move_until && state<=1) || CarMoveAction(api->get_lr(c));
    if(car_move) {}
    else if(state==4 || state==8 || state!=last_state || sub!=last_sub || t-last_sent>500) {
      QueueCall(0x82441308,c);
      last_state=state; last_sub=sub; last_sent=t;
    }
  }
  original_state(c,b);
}
// 82454A30(victim, attacker, ..., f1): a melee hit (damage and knockdown).
// Our character of the peer never takes melee hits itself; the peer's game
// decides hits from our character's replicated punches.
void MeleeHitHook(WmlContext* c,uint8_t* b) {
  const uint32_t avatar=avatar_local ? ResolveHuman(avatar_local) : 0;
  if(avatar && uint32_t(api->get_r(c,3))==avatar && connected.load()) {
    if(forward_damage) {} // damage forwarding (below) handles it if enabled
    else { api->set_r(c,3,1); return; }
  }
  original_melee_hit(c,b);
}
WmlGuestFunction original_damage=nullptr;
void DamageHook(WmlContext* c,uint8_t* b) {
  const uint32_t victim=uint32_t(api->get_r(c,3)), attacker=uint32_t(api->get_r(c,4));
  const uint32_t avatar=avatar_local ? ResolveHuman(avatar_local) : 0;
  const uint32_t sp=uint32_t(api->get_r(c,1));
  // Joiner: copies of the host's people take no damage here. Our player's
  // hits on them go to the host, whose game applies them to the real person
  // (a kill there comes back as a death).
  // Joiner: this game's own mission characters are hidden while the host's
  // are shown; hidden ones don't hurt anyone.
  if(connected.load() && !hosting.load() && attacker && Readable(attacker,4252) && api->read_u32(attacker+72)==1 &&
     (api->read_u32(attacker+120)&0x08000000u)) { api->set_f(c,1,0.0); return; }
  if(connected.load() && !hosting.load() && !replicated_depth && victim && Readable(victim,4252)) {
    if(const uint32_t host_handle=SharedPedHostHandle(api->read_u32(victim+68))) {
      if(attacker && attacker==api->read_u32(0x8309ABEC)) {
        Event e{kEventPedDamage,{host_handle,uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7)),
                                 uint32_t(api->get_r(c,8)),uint32_t(api->get_r(c,9)),uint32_t(api->get_r(c,10)),
                                 uint32_t(api->read_u8(sp+87))|uint32_t(api->read_u8(sp+95))<<8},float(api->get_f(c,1))};
        QueueEvent(e); ++ped_hits_sent;
      }
      api->set_f(c,1,0.0);
      return;
    }
  }
  if(avatar && victim==avatar && connected.load()) {
    // Host: one of the shared people (a mission enemy...) hit the joiner's
    // stand-in. The joiner's game applies it to the real player.
    if(hosting.load() && attacker && Readable(attacker,4252) && api->read_u32(attacker+72)==1 &&
       IsSentPed(api->read_u32(attacker+68))) {
      Event e{kEventPlayerHitByPed,{api->read_u32(attacker+68),uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7)),
                                    uint32_t(api->get_r(c,8)),uint32_t(api->get_r(c,9)),uint32_t(api->get_r(c,10)),
                                    uint32_t(api->read_u8(sp+87))|uint32_t(api->read_u8(sp+95))<<8},float(api->get_f(c,1))};
      QueueEvent(e); ++hits_from_people;
      api->set_f(c,1,0.0);
      return;
    }
    if(forward_damage && attacker==api->read_u32(0x8309ABEC)) {
      Event e{kEventDamage,{uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7)),
                            uint32_t(api->get_r(c,8)),uint32_t(api->get_r(c,9)),uint32_t(api->get_r(c,10)),
                            api->read_u8(sp+87),api->read_u8(sp+95)},float(api->get_f(c,1))};
      QueueEvent(e); ++hits_sent;
    }
    api->set_f(c,1,0.0); // the stand-in never takes damage itself
    return;
  }
  original_damage(c,b);
}

// 8248DBB8 (from the multiplayer cleanup 82370E10, which also runs when a
// mission starts) unlinks every clothing piece flagged 0x20, the remote
// players' pieces, and wipes their buffers: the other player's torso and legs
// vanished then. Single player has no such pieces of its own, so skipping it
// during a session changes nothing else.
WmlGuestFunction original_strip_remote=nullptr;
void StripRemotePiecesHook(WmlContext* c,uint8_t* b) {
  if(running.load() && components_added) {
    static unsigned logged=0;
    if(logged++<5) ProbeLog("Kept the other player's clothing pieces (game cleanup skipped)");
    return;
  }
  original_strip_remote(c,b);
}

// Getting in and out of cars: our player's own requests are sent and the
// same requests are made for our character in the other game, so it walks to
// the door, opens it, pulls the driver out and gets out the same way (placing
// it straight in the seat showed none of that).
WmlGuestFunction original_seat_request=nullptr, original_exit_request=nullptr;
uint32_t enter_hold_car=0; uint64_t enter_hold_until=0, exit_hold_until=0; // game thread
uint64_t local_enter_time=0;  // our player's last seat request
uint32_t passenger_watch_car=0; uint64_t passenger_watch_since=0;  // our passenger getting-in, forced if it stalls
uint32_t pending_victim=0; uint64_t victim_deadline=0, victim_exit_at=0;  // driver the other player pulls out here
uint64_t avatar_car_anim_until=0;  // getting in / out plays: the on-foot follower leaves the character alone
uint32_t AdoptSharedCar(GuestScratch& scratch,uint32_t host_id,uint64_t now);
uint32_t SharedCarHostHandle(uint32_t local);   // joiner: host handle of a traffic copy (0 if none)
uint32_t CarEventId(uint32_t h,uint32_t& theirs) {
  theirs=0;
  if(proxy_local && h==proxy_local) { theirs=1; return proxy_peer; }
  if(auto t=taken_cars.find(h); t!=taken_cars.end()) { theirs=1; return t->second; }
  if(const uint32_t host=SharedCarHostHandle(h)) { theirs=1; return host; }
  return OwnId(h);
}
void SeatRequestHook(WmlContext* c,uint8_t* b) {
  const uint32_t r=uint32_t(api->get_r(c,3));
  if(Tracing() && Readable(r,28)) if(const char* who=TraceWho(api->read_u32(r)))
    TraceLine(who,"seat-request",api->read_u32(r+8),api->read_u32(r+12),api->read_u32(r+16),api->read_u32(r+20),api->get_lr(c),true);
  const uint32_t player=api->read_u32(0x8309ABEC);
  const bool mine=connected.load() && !replicated_depth && Readable(r,28) && player && api->read_u32(r)==player;
  // The player's own enter (E) asks without a car (+4 = 0): the game picks the
  // nearest one, and the player's car handle (+2496) is set by the call.
  // The other player drives the car we walk up to: ask for the passenger
  // seat (the game pulled them out of the driver's seat as if a stranger).
  bool already=false;
  if(mine && api->read_u32(r+12)==0xFFFFFFFFu && api->read_u32(r+8)==0) if(const uint32_t av=avatar_local ? ResolveHuman(avatar_local) : 0) {
    const uint32_t acar=ResolveVehicle(api->read_u32(av+2496));
    const uint32_t asked=api->read_u32(r+4);
    if(acar && api->read_u32(av+2500)==0 && (asked==0 || asked==acar)) {
      const float dx=api->read_f32(acar+20)-api->read_f32(player+20), dz=api->read_f32(acar+28)-api->read_f32(player+28);
      if(dx*dx+dz*dz<6.0f*6.0f) {
        uint32_t saved[7]; for(unsigned i=0;i<7;++i) saved[i]=api->read_u32(r+i*4);
        // Straight into the passenger seat (the game's own getting in took
        // the player to the driver's seat whatever seat was asked, and each
        // game then had a different driver).
        api->write_u32(r+4,acar); api->write_u32(r+8,3); api->write_u32(r+12,1);
        // Last flag (+24) off: it put the player straight in the seat (no
        // walk, door or climb in); the game's own passenger requests leave it off.
        static const uint8_t direct[12]={1,0,0,1,0,0,1,1,0,0,0,0};
        for(unsigned i=0;i<12;++i) api->write_u8(r+16+i,direct[i]);
        original_seat_request(c,b);
        if(api->read_u32(player+2496)==api->read_u32(acar+68)) { ProbeLog("Getting in where the other player drives: passenger seat"); already=true; passenger_watch_car=api->read_u32(acar+68); passenger_watch_since=MonoMs(); }
        else {
          // Refused (it did nothing and the player couldn't get in at all):
          // the game's own choice, the drivers' seat is sorted out after.
          for(unsigned i=0;i<7;++i) api->write_u32(r+i*4,saved[i]);
          ProbeLog("Passenger seat refused: getting in the usual way");
        }
      }
    }
  }
  uint32_t req[7]={};
  if(mine) for(unsigned i=0;i<7;++i) req[i]=api->read_u32(r+i*4);
  const uint32_t requester=Readable(r,28) ? api->read_u32(r) : 0;
  if(!already) original_seat_request(c,b);
  if(Tracing() && requester && TraceWho(requester) && Readable(requester,4252)) {
    if(const uint32_t car=ResolveVehicle(api->read_u32(requester+2496))) {
      const uint32_t d=api->read_u32(car+52320);
      if(ResolveHuman(d) && d!=api->read_u32(requester+68)) {
        trace_driver=d;
        char line[80]; std::snprintf(line,sizeof(line),"TRACE driver in that car: %08X",d); ProbeLog(line);
      }
    }
  }
  if(!mine) return;
  uint32_t v=req[1];
  if(!Readable(v,kVehicleSize) || api->read_u32(v+72)!=5) v=ResolveVehicle(api->read_u32(player+2496));
  if(!v || api->read_u32(v+72)!=5) {
    static unsigned missed=0;
    if(missed++<10) ProbeLog("Local seat request: no car found to send");
    return;
  }
  const uint32_t h=api->read_u32(v+68);
  uint32_t theirs=0; const uint32_t id=CarEventId(h,theirs);
  Event e{kEventEnterCar,{id,theirs,req[2],req[3],req[4],req[5],req[6]},0};
  QueueEvent(e);
  own_car_move_until=MonoMs()+5000;
  local_enter_time=MonoMs();
  static unsigned logged=0;
  if(logged++<20) { char line[140]; std::snprintf(line,sizeof(line),"Local seat request: car %08X mode %u seat %d (from %08X)",h,req[2],int32_t(req[3]),api->get_lr(c)); ProbeLog(line); }
}
void ExitRequestHook(WmlContext* c,uint8_t* b) {
  const uint32_t r=uint32_t(api->get_r(c,3));
  if(Tracing() && Readable(r,36)) if(const char* who=TraceWho(api->read_u32(r)))
    TraceLine(who,"exit-request",api->read_u32(r+4),api->read_u32(r+12),api->read_u32(r+28),api->read_u32(r+32),api->get_lr(c),true);
  if(connected.load() && !replicated_depth && Readable(r,36) && api->read_u32(r)==api->read_u32(0x8309ABEC)) {
    Event e{kEventExitCar,{},0};
    for(unsigned i=0;i<8;++i) e.a[i]=api->read_u32(r+4+i*4);
    QueueEvent(e);
    own_car_move_until=MonoMs()+2500;
    static unsigned logged=0;
    if(logged++<20) ProbeLog("Local exit request sent");
  }
  // The driver our player pulls out gets thrown out with the game's own
  // exit (from the pull-out, 8257D77C, flags +32 = 0x20000000): the same
  // request goes to the other game for its copy of that driver.
  else if(connected.load() && !replicated_depth && Readable(r,36)) {
    const uint32_t who=api->read_u32(r), me=api->read_u32(0x8309ABEC);
    const uint32_t av=avatar_local ? ResolveHuman(avatar_local) : 0;
    if(who && who!=me && who!=av && Readable(who,4252) && me && api->read_u32(me+2496) && api->read_u32(who+2496)==api->read_u32(me+2496)) {
      Event e{kEventVictimExit,{},0};
      for(unsigned i=0;i<8;++i) e.a[i]=api->read_u32(r+4+i*4);
      QueueEvent(e);
      static unsigned logged=0;
      if(logged++<20) { char line[140]; std::snprintf(line,sizeof(line),"Pulled-out driver's exit sent (%08X %08X %08X %08X)",e.a[0],e.a[2],e.a[6],e.a[7]); ProbeLog(line); }
    }
  }
  original_exit_request(c,b);
}
void DetonateHook(WmlContext* c,uint8_t* b);
extern WmlGuestFunction original_detonate;
// 82444270(human, ...): starts a climb or vault when a ledge is in front.
// The other player's character here never starts one itself: it was pushed
// into walls by its move orders and began its own climb 0.3 s before theirs
// arrived, then climbed again (two climbs fighting). Their climb is replayed.
WmlGuestFunction original_climb_start=nullptr;
unsigned own_climbs_blocked=0;
void ClimbStartHook(WmlContext* c,uint8_t* b) {
  const uint32_t avatar=avatar_local ? ResolveHuman(avatar_local) : 0;
  if(avatar && uint32_t(api->get_r(c,3))==avatar && connected.load() && !replicated_depth) {
    ++own_climbs_blocked; api->set_r(c,3,0); return;
  }
  original_climb_start(c,b);
}
// 823ACF50(handle): object removal. Logs who removes a car copy shown for the
// other game (copies vanished 20-700 ms after they were made).
WmlGuestFunction original_destroy=nullptr;
int mod_destroying=0;
bool IsSharedCarHandle(uint32_t handle);
void DestroyHook(WmlContext* c,uint8_t* b) {
  const uint32_t h=uint32_t(api->get_r(c,3));
  if(!mod_destroying && connected.load() && IsSharedCarHandle(h)) {
    static unsigned logged=0;
    if(logged++<20) { char line[120]; std::snprintf(line,sizeof(line),"Car copy %08X removed by the game (from %08X)",h,api->get_lr(c)); ProbeLog(line); }
  }
  original_destroy(c,b);
}
// 824789A8(human, flag): a person's removal (the human class's destroy). The
// joiner's game removed copies of the host's people by itself, 10-18 in 10 s
// whatever kind they were made as, and they were made again at once: people
// popping in and out. Copies are kept; this mod removes them itself.
WmlGuestFunction original_human_destroy=nullptr;
bool OurCharacter(uint32_t handle);
unsigned copies_kept=0, copies_let_go=0; uint32_t copies_kept_from=0;
std::unordered_map<uint32_t,uint64_t> destroy_asks;
bool keep_copies=true;
void HumanDestroyHook(WmlContext* c,uint8_t* b) {
  const uint32_t obj=uint32_t(api->get_r(c,3));
  const uint32_t lr=uint32_t(api->get_lr(c));
  // Never kept: people in a car (the car's own removal, 825324D0, takes
  // its occupants with it; one kept in a removed car froze the game), and
  // one the game has asked for 200 times.
  if(keep_copies && !mod_destroying && connected.load() && Readable(obj,4252) && OurCharacter(api->read_u32(obj+68))) {
    const bool in_car=api->read_u32(obj+2496)!=0 || (lr>=0x825324D0u && lr<0x82533000u);
    // The game asks again every frame: 200 asks were 2 s, after which the
    // copy was removed and made again (people changing model and clothes in
    // front of the player). Let through only after 60 s of asking.
    if(destroy_asks.size()>512) destroy_asks.clear();
    uint64_t& since=destroy_asks[api->read_u32(obj+68)];
    const uint64_t t=MonoMs();
    if(!since) since=t;
    if(!in_car && t-since<60000) { ++copies_kept; copies_kept_from=lr; return; }
    ++copies_let_go; destroy_asks.erase(api->read_u32(obj+68));
  }
  original_human_destroy(c,b);
}
// 82575438(car, entering human, occupant): gets the occupant out of the
// seat the human is getting into. Players never pull each other out: the one
// getting in ended up driving and the other was thrown to the passenger seat.
WmlGuestFunction original_eject=nullptr;
void EjectHook(WmlContext* c,uint8_t* b) {
  const uint32_t who=uint32_t(api->get_r(c,4)), occ=uint32_t(api->get_r(c,5));
  const uint32_t player=api->read_u32(0x8309ABEC), av=avatar_local ? ResolveHuman(avatar_local) : 0;
  if(connected.load() && av && occ && ((who==player && occ==av) || (who==av && occ==player))) {
    static unsigned logged=0;
    if(logged++<10) ProbeLog("Kept the other player in their seat (not pulled out)");
    api->set_r(c,3,0); return;
  }
  original_eject(c,b);
}
// 825352E0(car, door, f1-f3): a door opening or closing. Our player's car
// door is sent (the other game showed the climb-in with a closed door).
WmlGuestFunction original_door=nullptr;
uint32_t CarEventId(uint32_t h,uint32_t& theirs);
void DoorHook(WmlContext* c,uint8_t* b) {
  const uint32_t v=uint32_t(api->get_r(c,3));
  const uint32_t player=api->read_u32(0x8309ABEC);
  if(connected.load() && !replicated_depth && player && Readable(v,kVehicleSize) && api->read_u32(v+72)==5 && api->read_u32(v+68)==api->read_u32(player+2496)) {
    uint32_t theirs=0; const uint32_t id=CarEventId(api->read_u32(v+68),theirs);
    Event e{kEventCarDoor,{id,theirs,uint32_t(api->get_r(c,4))},0};
    for(int i=0;i<3;++i) { const float f=float(api->get_f(c,1+i)); std::memcpy(&e.a[3+i],&f,4); }
    QueueEvent(e);
  }
  original_door(c,b);
}
void InstallEventHooks() {
  api->hook(0x82575438,EjectHook,&original_eject);
  api->hook(0x825352E0,DoorHook,&original_door);
  api->hook(0x82444270,ClimbStartHook,&original_climb_start);
  api->hook(0x823ACF50,DestroyHook,&original_destroy);
  api->hook(0x824789A8,HumanDestroyHook,&original_human_destroy);
  api->hook(0x8257B2C0,SeatRequestHook,&original_seat_request);
  api->hook(0x825397E0,DetonateHook,&original_detonate);
  api->hook(0x8257B8E0,ExitRequestHook,&original_exit_request);
  api->hook(0x8248DBB8,StripRemotePiecesHook,&original_strip_remote);
  api->hook(0x82444F88,PlayActionHook,&original_play_action);
  api->hook(0x824470D0,DamageHook,&original_damage);
  api->hook(0x82454160,SwingHook,&original_swing);
  api->hook(0x82458EB8,AttackHook,&original_attack);
  api->hook(0x82459430,PunchHook,&original_punch);
  api->hook(0x824497F0,KnockdownHook,&original_knockdown);
  api->hook(0x82441308,StateHook,&original_state);
  api->hook(0x82454A30,MeleeHitHook,&original_melee_hit);
  api->hook(0x82403C38,EffectAddHook,&original_effect_add);
  api->hook(0x82403F58,EffectRemoveHook,&original_effect_remove);
}

// Game thread: play received events.
uint32_t pending_enter_car=0;  // (unused since 0.91)
uint32_t SharedCarLocal(uint32_t host_id);
// Who sits in a seat, by every person's car (+2496) and seat (+2500): the
// car's seat slots are empty for most drivers, so a pulled-out driver was
// never found (it stayed in and the player sat on top of it).
uint32_t OccupantHandle(uint32_t vehicle,uint32_t seat);
uint32_t SeatedIn(uint32_t car_handle,uint32_t seat) {
  if(!car_handle) return 0;
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t h=api->read_u32(kObjectTable+12+index*16);
    if(!Readable(h,4252) || api->read_u32(h+72)!=1 || (api->read_u32(h+68)&0xffff)!=index) continue;
    if(api->read_u32(h+2496)==car_handle && api->read_u32(h+2500)==seat) return api->read_u32(h+68);
  }
  const uint32_t v=ResolveVehicle(car_handle);
  return v ? OccupantHandle(v,seat) : 0;
}
std::vector<uint32_t> host_models; uint64_t host_models_time=0;
bool pin_host_models=false;  // mod.ini preload_host_models: asking for models no copy here needs yet (new stream entries) came in with the joiner's freezes (1.11 on)  // joiner: people models the host's game has loaded (game thread)
uint32_t remote_enter_car=0; uint64_t remote_enter_until=0, remote_enter_time=0;  // their getting in is playing (car actions)
uint32_t last_enter_car=0; uint64_t last_enter_time=0;
void OrderExitVehicle(GuestScratch& scratch,uint32_t human);
void DestroyObject(GuestScratch& scratch,uint32_t handle);
uint32_t OccupantHandle(uint32_t vehicle,uint32_t seat);
// The other player's moves (jumps, climbs, punches) arrive at once, but
// their position is shown ~50-130 ms behind: a jump started before the
// character reached the take-off spot. These are held back by that much.
int avatar_show_delay=50;
void ApplyEvents(GuestScratch& scratch,uint64_t now) {
  std::vector<Event> in;
  { std::lock_guard lock(event_mutex); in.swap(events_in); }
  {
    static std::vector<std::pair<uint64_t,Event>> held;
    std::vector<Event> ready;
    for(auto& [due,e]:held) if(due<=now) ready.push_back(e);
    held.erase(std::remove_if(held.begin(),held.end(),[now](auto& h){ return h.first<=now; }),held.end());
    for(const Event& e:in) {
      if((e.kind==kEventAction || e.kind==kEventCall || e.kind==kEventCarAction) && (!held.empty() || avatar_show_delay>0)) {
        if(held.empty() && avatar_show_delay<=0) ready.push_back(e);
        else held.push_back({now+uint64_t(std::clamp(avatar_show_delay,0,200)),e});
      } else ready.push_back(e);
    }
    in.swap(ready);
  }
  if(pending_victim) {
    const uint32_t v=ResolveHuman(pending_victim);
    // The car stays where it is while the other player walks up and pulls
    // the driver out (it drove off meanwhile and the two games differed).
    if(v) if(const uint32_t vc=ResolveVehicle(api->read_u32(v+2496))) if(const uint32_t vel=VehicleVelocityAddress(vc))
      for(unsigned i=0;i<3;++i) { api->write_f32(vel+i*4,0); api->write_f32(vel+16+i*4,0); }
    if(!v) pending_victim=0;
    else if((victim_exit_at && now>=victim_exit_at) || now>=victim_deadline) {
      OrderExitVehicle(scratch,v);
      ProbeLog(victim_exit_at?"Pulled-out driver gets out after its move":"Pulled-out driver ordered out (no move came)");
      pending_victim=0;
    }
  }

  const uint32_t avatar=avatar_local ? ResolveHuman(avatar_local) : 0;
  const uint32_t player=api->read_u32(0x8309ABEC);
  for(const Event& e:in) {
    if(e.kind==kEventPedAction) { ApplyPedAction(scratch,e,now); continue; }
    if(e.kind==kEventPedCall) { ApplyPedCall(scratch,e,now); continue; }
    if(e.kind==kEventPedReact) { ApplyPedReact(scratch,e,now); continue; }
    if(e.kind==kEventEnterCar || e.kind==kEventExitCar) {
      if(!avatar || !(api->read_u32(avatar+216)&0x4000)) continue;
      if(e.kind==kEventExitCar) {
        const uint32_t order=scratch.data()+1760;
        api->write_u32(order,avatar);
        for(unsigned i=0;i<8;++i) api->write_u32(order+4+i*4,e.a[i]);
        scratch.ctx.r3.u64=order;
        ++replicated_depth; scratch.Call(0x8257B8E0); --replicated_depth;
        exit_hold_until=now+4000;   // the pose-driven exit waits for this one
        avatar_car_anim_until=now+2200;
        ProbeLog("Other player gets out (their exit played here)");
        continue;
      }
      uint32_t car=0;
      if(e.a[1]) {                          // our handle (their copy of our car, or a car they took)
        for(auto& [local,host]:taken_cars) if(host==e.a[0]) { car=ResolveVehicle(local); break; }
        if(!car) car=ResolveVehicle(e.a[0]);
        if(!car && own_copy_id==e.a[0]) car=ResolveVehicle(own_copy_local);
      } else {                              // their own car: our copy of it
        if(proxy_peer==e.a[0]) car=ResolveVehicle(proxy_local);
        if(!car) for(auto& [local,host]:taken_cars) if(host==e.a[0]) { car=ResolveVehicle(local); break; }  // one we took
        if(!car) car=AdoptSharedCar(scratch,e.a[0],now);
      }
      if(!car) { ProbeLog("Other player gets in a car not shown here yet (placed when it is)"); continue; }
      // Their getting in is shown with their own moves (door, pulling the
      // driver out, climbing in: sent as car actions) and the character is
      // put in the seat once they sit. Replaying the request here stopped at
      // the door or gave up for this character.
      const uint32_t car_handle=api->read_u32(car+68);
      remote_enter_car=car_handle; remote_enter_until=now+8000; remote_enter_time=now;
      if(int32_t(e.a[3])<=0 && !(api->read_u32(player+2496)==car_handle && api->read_u32(player+2500)==0))
        if(const uint32_t occ=SeatedIn(car_handle,0)) if(occ!=api->read_u32(avatar+68) && occ!=api->read_u32(player+68)) if(const uint32_t d=ResolveHuman(occ)) {
          // The game's own "pulled out of the seat" for that driver, with
          // our copy of them as the one getting in (an exit order was
          // ignored: the driver stayed in and was removed).
          // (Calling the game's pull-out 82575438 here removed the whole car.)
          // The driver waits for the other player's pull-out: its own "being
          // pulled out" move is sent and played on it, then it gets out.
          (void)d;
          pending_victim=occ; victim_deadline=now+4000; victim_exit_at=0;
          ProbeLog("Other player takes a car with a driver: driver waits for the pull-out");
        }
      char line[120]; std::snprintf(line,sizeof(line),"Other player gets in %08X (mode %u, seat %d): their moves shown, seated when they sit",car_handle,e.a[2],int32_t(e.a[3]));
      ProbeLog(line);
      continue;
    }
    if(e.kind==kEventCarImpact) {
      // The car by the other game's handle first (our copy of their car, or a
      // host car the joiner took), then our own handle.
      uint32_t car=0;
      if(proxy_peer==e.a[0]) car=ResolveVehicle(proxy_local);
      if(!car) for(auto& [local,host]:taken_cars) if(host==e.a[0]) { car=ResolveVehicle(local); break; }
      if(!car) car=ResolveVehicle(e.a[0]);
      float dv[3]; bool ok=car!=0;
      for(unsigned i=0;i<3;++i) { std::memcpy(&dv[i],&e.a[1+i],4); ok=ok && std::isfinite(dv[i]) && std::abs(dv[i])<60; }
      const uint32_t v=ok ? VehicleVelocityAddress(car) : 0;
      if(v) { for(unsigned i=0;i<3;++i) api->write_f32(v+i*4,api->read_f32(v+i*4)+dv[i]); ++impacts_applied; }
      static unsigned logged=0;
      if(logged++<30) { char line[120]; std::snprintf(line,sizeof(line),"Car hit from the other game: %.1f %.1f %.1f m/s %s",dv[0],dv[1],dv[2],v?"applied":"(car not here)"); ProbeLog(line); }
      continue;
    }
    if(e.kind==kEventPedDamage || e.kind==kEventPlayerHitByPed) {
      if(!std::isfinite(e.f) || e.f<-2 || e.f>100000) continue;
      uint32_t victim=0, attacker=0;
      if(e.kind==kEventPedDamage) {   // host: the joiner hit one of our people
        if(!hosting.load() || !IsSentPed(e.a[0])) continue;
        victim=ResolveHuman(e.a[0]); attacker=avatar;
      } else {                        // joiner: one of the host's people hit us
        if(hosting.load() || !Readable(player,4488)) continue;
        victim=player; attacker=ResolveHuman(SharedPedLocalHandle(e.a[0]));
      }
      if(!victim) continue;
      scratch.ctx.r3.u64=victim; scratch.ctx.r4.u64=attacker;
      scratch.ctx.r5.u64=e.a[1]; scratch.ctx.r6.u64=e.a[2]; scratch.ctx.r7.u64=e.a[3];
      scratch.ctx.r8.u64=e.a[4]; scratch.ctx.r9.u64=e.a[5]; scratch.ctx.r10.u64=e.a[6];
      scratch.ctx.f1.f64=e.f;
      api->write_u8(scratch.ctx.r1.u32+87,uint8_t(e.a[7])); api->write_u8(scratch.ctx.r1.u32+95,uint8_t(e.a[7]>>8));
      ++replicated_depth;
      const bool ok=scratch.Call(0x824470D0);
      --replicated_depth;
      if(ok) ++ped_hits_applied;
      // The joiner's punch lands here as damage only: the person didn't flinch
      // on the host (nor, as the host decides reactions, on the joiner). Play
      // the game's own hit reaction from the joiner's side when close enough
      // for a melee hit; it goes to the joiner like any other reaction.
      if(ok && e.kind==kEventPedDamage && attacker && api->read_f32(victim+1912)>0.0f) {
        const float dx=api->read_f32(victim+20)-api->read_f32(attacker+20), dz=api->read_f32(victim+28)-api->read_f32(attacker+28);
        if(dx*dx+dz*dz<3.5f*3.5f) {
          const uint32_t hit=scratch.data()+2200;
          for(int i=0;i<3;++i) api->write_f32(hit+i*4,api->read_f32(attacker+20+i*4));
          scratch.ctx.r3.u64=victim; scratch.ctx.r4.u64=attacker; scratch.ctx.r5.u64=hit;
          scratch.ctx.r6.u64=e.a[1]; scratch.ctx.r7.u64=e.a[4]; scratch.ctx.r8.u64=1;
          if(scratch.Call(0x82458EB8)) ++joiner_hit_reactions;
        }
      }
      static unsigned logged=0;
      if(logged++<30) {
        char line[140];
        std::snprintf(line,sizeof(line),e.kind==kEventPedDamage?"The other player hit person %08X: %.1f damage%s":"Hit by person %08X of the host: %.1f damage%s",
                      e.a[0],e.f,attacker?"":" (attacker not here)");
        ProbeLog(line);
      }
      continue;
    }
    if(e.kind==kEventPedEffect) {
      joiner_effects[e.a[0]]=PedEffect{e.a[1],e.a[2],e.a[3],e.a[4],now};
      continue;
    }
    if(e.kind==kEventHostModels) {
      std::vector<uint32_t> ids;
      const unsigned count=std::min<unsigned>(e.a[0],14);
      for(unsigned i=0;i<count;++i) ids.push_back((e.a[1+i/2]>>((i&1)?0:16))&0xFFFF);
      host_models.swap(ids); host_models_time=now;
      continue;
    }
    if(e.kind==kEventVictimExit) {
      uint32_t victim=pending_victim ? ResolveHuman(pending_victim) : 0;
      if(!victim && remote_enter_car) {
        const uint32_t occ=SeatedIn(remote_enter_car,0);
        if(occ && (!avatar || occ!=api->read_u32(avatar+68)) && occ!=api->read_u32(player+68)) victim=ResolveHuman(occ);
      }
      if(victim) {
        const uint32_t order=scratch.data()+1760;
        api->write_u32(order,victim);
        for(unsigned i=0;i<8;++i) api->write_u32(order+4+i*4,e.a[i]);
        scratch.ctx.r3.u64=order;
        ++replicated_depth; scratch.Call(0x8257B8E0); --replicated_depth;
        pending_victim=0;
      }
      ProbeLog(victim?"Pulled-out driver thrown out here too (their exit)":"Pulled-out driver's exit: no driver here");
      continue;
    }
    if(e.kind==kEventVictimAction) {
      uint32_t car=0;
      if(e.a[1]) {
        for(auto& [local,host]:taken_cars) if(host==e.a[0]) { car=ResolveVehicle(local); break; }
        if(!car) car=ResolveVehicle(e.a[0]);
        if(!car && own_copy_id==e.a[0]) car=ResolveVehicle(own_copy_local);
      } else {
        if(proxy_peer==e.a[0]) car=ResolveVehicle(proxy_local);
        if(!car) for(auto& [local,host]:taken_cars) if(host==e.a[0]) { car=ResolveVehicle(local); break; }
        if(!car) car=SharedCarLocal(e.a[0]);
      }
      uint32_t victim=pending_victim ? ResolveHuman(pending_victim) : 0;
      if(!victim && car) {
        const uint32_t occ=SeatedIn(api->read_u32(car+68),0);
        if(occ && (!avatar || occ!=api->read_u32(avatar+68)) && occ!=api->read_u32(player+68)) victim=ResolveHuman(occ);
      }
      bool played=false;
      if(victim) {
        scratch.ctx.r3.u64=victim; scratch.ctx.r4.u64=e.a[2]; scratch.ctx.r5.u64=e.a[3]; scratch.ctx.r6.u64=e.a[4]; scratch.ctx.r7.u64=e.a[5];
        float f[2]; for(int i=0;i<2;++i) std::memcpy(&f[i],&e.a[6+i],4);
        scratch.ctx.f1.f64=std::isfinite(f[0])?f[0]:0; scratch.ctx.f2.f64=std::isfinite(f[1])?f[1]:0; scratch.ctx.f3.f64=0;
        ++replicated_depth; played=scratch.Call(0x82444F88) && scratch.ctx.r3.u32; --replicated_depth;
        if(pending_victim && ResolveHuman(pending_victim)==victim && !victim_exit_at) victim_exit_at=now+2500;  // (their exit normally comes first)
      }
      static unsigned logged=0;
      if(logged++<20) { char line[120]; std::snprintf(line,sizeof(line),"Pulled-out driver's move %02X: %s",e.a[2],victim?(played?"playing":"refused"):"no driver here"); ProbeLog(line); }
      continue;
    }
    if(e.kind==kEventCarDoor) {
      uint32_t car=0;
      if(e.a[1]) {
        for(auto& [local,host]:taken_cars) if(host==e.a[0]) { car=ResolveVehicle(local); break; }
        if(!car) car=ResolveVehicle(e.a[0]);
        if(!car && own_copy_id==e.a[0]) car=ResolveVehicle(own_copy_local);
      } else {
        if(proxy_peer==e.a[0]) car=ResolveVehicle(proxy_local);
        if(!car) for(auto& [local,host]:taken_cars) if(host==e.a[0]) { car=ResolveVehicle(local); break; }
        if(!car) car=SharedCarLocal(e.a[0]);
      }
      if(car && e.a[2]<16) {
        scratch.ctx.r3.u64=car; scratch.ctx.r4.u64=e.a[2];
        float f[3]; for(int i=0;i<3;++i) std::memcpy(&f[i],&e.a[3+i],4);
        scratch.ctx.f1.f64=std::isfinite(f[0])?f[0]:0; scratch.ctx.f2.f64=std::isfinite(f[1])?f[1]:0; scratch.ctx.f3.f64=std::isfinite(f[2])?f[2]:0;
        ++replicated_depth; scratch.Call(0x825352E0); --replicated_depth;
      }
      static unsigned logged=0;
      if(logged++<20) { char line[120]; std::snprintf(line,sizeof(line),"Car door %u of the other player's car: %s",e.a[2],car?"moved":"car not here"); ProbeLog(line); }
      continue;
    }
    if(e.kind==kEventCarAction && avatar && (api->read_u32(avatar+216)&0x4000)) {
      scratch.ctx.r3.u64=avatar; scratch.ctx.r4.u64=e.a[0]; scratch.ctx.r5.u64=e.a[1];
      scratch.ctx.r6.u64=e.a[2]; scratch.ctx.r7.u64=e.a[3];
      UnpackActionFloats(scratch,e);
      ++replicated_depth;
      const bool played=scratch.Call(0x82444F88) && scratch.ctx.r3.u32;
      --replicated_depth;
      avatar_action_until=std::max(avatar_action_until,now+2500);   // no move orders over it
      static unsigned logged=0;
      if(logged++<40) { char line[100]; std::snprintf(line,sizeof(line),"Car action %02X on the other player: %s",e.a[0],played?"playing":"refused"); ProbeLog(line); }
      continue;
    }
    if(e.kind==kEventAction && avatar && (api->read_u32(avatar+216)&0x4000)) {
      scratch.ctx.r3.u64=avatar; scratch.ctx.r4.u64=e.a[0]; scratch.ctx.r5.u64=e.a[1];
      scratch.ctx.r6.u64=e.a[2]; scratch.ctx.r7.u64=e.a[3];
      UnpackActionFloats(scratch,e);
      ++replicated_depth;
      const bool played=scratch.Call(0x82444F88) && scratch.ctx.r3.u32;
      --replicated_depth;
      if(played) ++actions_played;
      else { pending_action=e; pending_action_until=now+400; }
      if(!avatar_jumping) avatar_action_until=std::max(avatar_action_until,now+600);
      static unsigned logged=0;
      if(logged++<40) {
        char line[120];
        std::snprintf(line,sizeof(line),"Action %02X on the other player: %s",e.a[0],played?"playing":"refused");
        ProbeLog(line);
      }
    } else if(e.kind==kEventCall && avatar && (api->read_u32(avatar+216)&0x4000)) {
      // A punch is refused while the character's current action (+756, an
      // index into the action table) is marked busy, e.g. a walk/run cycle
      // from a move order. The original was allowed to punch, so clear it.
      const uint32_t busy_action=api->read_u32(avatar+756);
      if(e.a[0]==0x82459430) api->write_u32(avatar+756,0xFFFFFFFF);
      uint64_t r[4];
      for(int i=0;i<4;++i)
        r[i]=(e.a[7]>>i)&1 ? uint64_t(avatar+e.a[1+i]) : (e.a[7]>>(4+i))&1 ? uint64_t(player+e.a[1+i]) : uint64_t(e.a[1+i]);
      scratch.ctx.r3.u64=avatar; scratch.ctx.r4.u64=r[0]; scratch.ctx.r5.u64=r[1];
      scratch.ctx.r6.u64=r[2]; scratch.ctx.r7.u64=r[3];
      ++replicated_depth;
      const bool ok=scratch.Call(e.a[0]);
      --replicated_depth;
      if(e.a[0]==0x82441308 && e.a[1]==4) {
        if(e.a[2]==1) { avatar_jumping=true; avatar_action_until=now+3000; }   // take-off: until landing
        else if(e.a[2]==0xE) { // landed: hand back quickly, with a fresh move order (the jump left
          avatar_jumping=false; avatar_action_until=now+120;  // the old one stale: sliding, walking on the spot)
          avatar_reorder=true;
        }
      } else if(e.a[0]==0x824497F0) avatar_action_until=now+2500;
      else if(e.a[0]==0x82441308 && e.a[1]==3) avatar_car_anim_until=now+4000; // getting in / out of a car (state 3)
      else if(e.a[0]==0x82441308 && e.a[1]!=8 && !avatar_jumping) {
        // Climbing and other movement states: let the animation carry the
        // character (positions still follow the path) until the next state.
        // 0/1: back to normal movement; 5 climbing, 6 dropping/landing,
        // others: hold until the next state arrives (at most 3 s).
        // 6/E is the landing after a climb or drop (like 4/E after a jump):
        // hand back at once, it held the character for 3 s after the climb.
        // (state 1 also comes up in the middle of a climb, between the climb
        // and the drop: not an end then.)
        const bool normal=(e.a[1]==0) || (e.a[1]==1 && !avatar_climbing) || (e.a[1]==6 && e.a[2]==0xE);
        if(!(e.a[1]==1 && avatar_climbing)) {
          avatar_action_until=now+(normal ? 120 : 3000);
          if(normal) avatar_reorder=true;
          avatar_climbing=e.a[1]==5;
        }
        // An action the character refused just before (the sender played it
        // a moment before this state change): play it now that the state fits.
        if(pending_action.kind && now<pending_action_until) {
          const Event pa=pending_action; pending_action.kind=0;
          scratch.ctx.r3.u64=avatar; scratch.ctx.r4.u64=pa.a[0]; scratch.ctx.r5.u64=pa.a[1];
          scratch.ctx.r6.u64=pa.a[2]; scratch.ctx.r7.u64=pa.a[3];
          UnpackActionFloats(scratch,pa);
          ++replicated_depth;
          const bool played=scratch.Call(0x82444F88) && scratch.ctx.r3.u32;
          --replicated_depth;
          static unsigned logged=0;
          if(logged++<30) { char line[100]; std::snprintf(line,sizeof(line),"Action %02X on the other player, after its state change: %s",pa.a[0],played?"playing":"refused"); ProbeLog(line); }
        }
      }
      else if(!avatar_jumping) avatar_action_until=std::max(avatar_action_until,now+600);
      static unsigned logged=0;
      if(logged++<60) {
        char line[140];
        std::snprintf(line,sizeof(line),"Call %08X(%08X %08X %08X %08X) on the other player: %s, r3=%X, action was %X",e.a[0],e.a[1],e.a[2],e.a[3],e.a[4],ok?"done":"failed",scratch.ctx.r3.u32,busy_action);
        ProbeLog(line);
      }
    } else if(e.kind==kEventDamage && Readable(player,4488)) {
      if(!std::isfinite(e.f) || e.f<0 || e.f>100000) continue;
      scratch.ctx.r3.u64=player; scratch.ctx.r4.u64=avatar; // our stand-in of the attacker
      scratch.ctx.r5.u64=e.a[0]; scratch.ctx.r6.u64=e.a[1]; scratch.ctx.r7.u64=e.a[2];
      scratch.ctx.r8.u64=e.a[3]; scratch.ctx.r9.u64=e.a[4]; scratch.ctx.r10.u64=e.a[5];
      scratch.ctx.f1.f64=e.f;
      api->write_u8(scratch.ctx.r1.u32+87,uint8_t(e.a[6])); api->write_u8(scratch.ctx.r1.u32+95,uint8_t(e.a[7]));
      if(scratch.Call(0x824470D0)) ++hits_taken;
      char line[120];
      std::snprintf(line,sizeof(line),"Hit by the other player: %.1f damage",e.f);
      ProbeLog(line);
    }
  }
}

// Remote characters (the peer's player and the copied NPC) follow the path
// the original took. Snapshots carry the sender's clock; the character is
// placed 120 ms behind on the interpolated path with the game's teleport
// every update, so it moves smoothly whatever the frame rates. While the
// original moves, the character also gets a native move order a little
// ahead on the path, so its locomotion plays walk/run/sprint.
struct Snapshot { uint32_t time; float pos[3]; float rows[9]; };

void Normalize(float* v) {
  const float n=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
  if(n>1e-4f) for(unsigned i=0;i<3;++i) v[i]/=n;
}

struct Follower {
  Snapshot snaps[32];
  unsigned count=0;
  int64_t offset=INT64_MAX; // local ms minus sender ms (see Add)
  int64_t recent[48]; unsigned recent_count=0, recent_next=0;
  uint64_t next_order=0, next_node=0, next_gap_log=0;
  int order_mode=0; float order_heading=0;
  float gap_sum=0, gap_max=0; unsigned gap_count=0;
  float last_gap=0;  // this update's distance from the path
  bool moving=false;
  uint32_t failures=0;
  int delay=50;   // ms behind the newest snapshot (more for people sent at a lower rate)
  unsigned airborne_placed=0;  // updates placed on the path while in the air (log)

  int base_delay=-1; // the configured delay; the working one grows with network jitter
  // One position far off the path (the other player's hit reaction sent
  // one 742 m away: they vanished for a moment) is held until the next one
  // confirms it; a real teleport is only one packet late.
  bool suspect=false; float suspect_pos[3]{};
  void Reset() { count=0; offset=INT64_MAX; recent_count=0; recent_next=0; next_order=0; moving=false; if(base_delay>=0) delay=base_delay; }

  void Add(uint32_t time,const float* pos,const float* rows,uint64_t now) {
    if(base_delay<0) base_delay=delay;
    if(count && int32_t(time-snaps[count-1].time)<=0) {
      // The sender's clock went back (its game restarted): start over.
      if(int32_t(time-snaps[count-1].time)<-2000) Reset(); else return;
    }
    if(count && int32_t(time-snaps[count-1].time)<600) {
      const float* l=snaps[count-1].pos;
      const float jx=pos[0]-l[0], jy=pos[1]-l[1], jz=pos[2]-l[2];
      if(jx*jx+jy*jy+jz*jz>40.0f*40.0f) {
        const float cx=pos[0]-suspect_pos[0], cy=pos[1]-suspect_pos[1], cz=pos[2]-suspect_pos[2];
        if(!suspect || cx*cx+cy*cy+cz*cz>10.0f*10.0f) { suspect=true; std::memcpy(suspect_pos,pos,sizeof(suspect_pos)); return; }
      }
    }
    suspect=false;
    if(count==32) { std::memmove(snaps,snaps+1,sizeof(Snapshot)*31); --count; }
    Snapshot& s=snaps[count++];
    s.time=time;
    std::memcpy(s.pos,pos,sizeof(s.pos));
    std::memcpy(s.rows,rows,sizeof(s.rows));
    const int64_t o=int64_t(now)-int64_t(time);
    // The clock offset is the smallest delay over the last ~1.5 s of
    // packets. When packets come in sooner than that (after a lag spike the
    // offset had crept up) it drops at once: easing it down kept the path
    // ahead of the packets for seconds (negative delays in the log: the
    // character ran on past its data and slid back). Rising (packets
    // later) is eased, so the character never jumps back along its path.
    recent[recent_next]=o; recent_next=(recent_next+1)%48; if(recent_count<48) ++recent_count;
    int64_t target=o;
    for(unsigned i=0;i<recent_count;++i) target=std::min(target,recent[i]);
    if(offset==INT64_MAX || target<offset || target-offset>1500) offset=target;
    else offset+=std::clamp<int64_t>((target-offset)/6,1,25);
    // Jitter buffer: stay behind by the spread of recent packet delays, so
    // the path rarely runs past the newest snapshot (the character stopped
    // and then jumped, or walked on the spot).
    if(recent_count>=8) {
      int64_t spread[48]; unsigned n=0;
      for(unsigned i=0;i<recent_count;++i) spread[n++]=recent[i]-target;
      std::nth_element(spread,spread+n*4/5,spread+n);
      const int want=int(std::clamp<int64_t>(spread[n*4/5]+20,base_delay,300));
      delay+=std::clamp(want-delay,-2,8);
    }
  }
  // Sender time of the newest snapshot minus the time being shown.
  int32_t Lead(uint64_t now) const { return count ? int32_t(int64_t(snaps[count-1].time)-(int64_t(now)-offset-delay)) : 0; }

  // Pose at sender time t. Returns horizontal speed; fills velocity.
  float Sample(uint32_t t,float* pos,float* rows,float* velocity) const {
    const Snapshot& last=snaps[count-1];
    unsigned i=count-1;
    while(i>0 && int32_t(snaps[i].time-t)>0) --i;
    const Snapshot& a=snaps[i];
    const Snapshot& b=snaps[std::min(i+1,count-1)];
    float f=0;
    if(&a!=&b) f=std::clamp(float(int32_t(t-a.time))/float(std::max<int32_t>(1,int32_t(b.time-a.time))),0.0f,1.0f);
    for(unsigned k=0;k<3;++k) pos[k]=a.pos[k]+(b.pos[k]-a.pos[k])*f;
    for(unsigned k=0;k<9;++k) rows[k]=a.rows[k]+(b.rows[k]-a.rows[k])*f;
    Normalize(rows); Normalize(rows+3); Normalize(rows+6);
    unsigned j=count-1;
    while(j>0 && int32_t(last.time-snaps[j].time)<150) --j;
    const float dt=std::max(0.001f,float(int32_t(last.time-snaps[j].time))/1000.0f);
    for(unsigned k=0;k<3;++k) velocity[k]=j==count-1 ? 0 : (last.pos[k]-snaps[j].pos[k])/dt;
    if(int32_t(t-last.time)>0) { // beyond the newest: extrapolate up to 150 ms
      if(int32_t(t-last.time)>300) { // no data for a while: stand at the last spot
        for(unsigned k=0;k<3;++k) { pos[k]=last.pos[k]; velocity[k]=0; }
        std::memcpy(rows,last.rows,sizeof(last.rows));
        return 0;
      }
      const float ahead=std::min(0.15f,float(int32_t(t-last.time))/1000.0f);
      for(unsigned k=0;k<3;++k) pos[k]=last.pos[k]+velocity[k]*ahead;
    }
    return std::sqrt(velocity[0]*velocity[0]+velocity[2]*velocity[2]);
  }

  // Moves obj along the path. Uses scratch data+1300..1640.
  bool Drive(GuestScratch& scratch,uint32_t obj,uint64_t now,const char* tag) {
    if(!count) return false;
    const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316, velocity=data+1360;
    float path_pos[3], rows[9], vel[3];
    const float path_speed=Sample(uint32_t(int64_t(now)-offset-delay),path_pos,rows,vel);
    // How far the character is from where it should be (before this update's
    // move). Teleports usually close this; locomotion lag shows here.
    const float gx=path_pos[0]-api->read_f32(obj+20), gz=path_pos[2]-api->read_f32(obj+28);
    const float gap=std::sqrt(gx*gx+gz*gz);
    gap_sum+=gap; gap_max=std::max(gap_max,gap); ++gap_count; last_gap=gap;
    // Thrown far off by something played here (the other player's hit
    // reaction moved their character ~740 m for a moment: they vanished):
    // back on the path at once.
    if(gap>25.0f && obj==ResolveHuman(avatar_local) && count>=2) {
      const Snapshot& a=snaps[count-2]; const Snapshot& b=snaps[count-1];
      const float jx=b.pos[0]-a.pos[0], jz=b.pos[2]-a.pos[2];
      if(jx*jx+jz*jz<10.0f*10.0f) {
        for(unsigned i=0;i<3;++i) api->write_f32(obj+20+i*4,path_pos[i]);
        static unsigned logged=0;
        if(logged++<10) { char line[100]; std::snprintf(line,sizeof(line),"Other player's character was %.0f m off its path here: put back",gap); ProbeLog(line); }
      }
    }
    if(tag && now>=next_gap_log) {
      if(gap_count) {
        char line[220];
        std::snprintf(line,sizeof(line),"%s follow gap: avg %.2f m, max %.2f m, newest packet %lld ms old, shown %d ms behind, %u in-air updates on the path",tag,gap_sum/gap_count,gap_max,
                      (long long)(int64_t(now)-offset-int64_t(snaps[count-1].time)),delay,airborne_placed);
        airborne_placed=0;
        ProbeLog(line);
      }
      gap_sum=0; gap_max=0; gap_count=0; next_gap_log=now+3000;
    }
    // Run faster than the original while behind, so locomotion catches up;
    // but when the original stands still, so does this one (a copy that
    // couldn't reach its spot walked on the spot).
    // Getting in / out of a car plays: no move orders, no placing.
    if(obj==ResolveHuman(avatar_local) && now<avatar_car_anim_until && gap<6.0f) return true;
    const float speed=path_speed<0.3f ? 0.0f : path_speed+(gap>1.0f ? gap*1.5f : 0.0f);
    for(unsigned i=0;i<3;++i) { api->write_f32(pos+i*4,path_pos[i]); api->write_f32(velocity+i*4,vel[i]); }
    for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,rows[i]);
    const bool is_avatar=obj==ResolveHuman(avatar_local);
    if(is_avatar && now<avatar_action_until && !avatar_jumping && path_speed>2.0f) avatar_action_until=0;
    // In the air (jumping, falling, thrown): the game refuses move orders and
    // its own jump physics drifted up to ~2 m from where the other player
    // really is. Put the character on the path every update until it lands.
    if(is_avatar && gap<6.0f && std::abs(vel[1])>2.0f && !avatar_climbing) {  // stairs stay below 2 m/s vertical
      for(unsigned i=0;i<3;++i) api->write_f32(obj+20+i*4,path_pos[i]);
      for(unsigned i=0;i<9;++i) api->write_f32(obj+32+i*4,rows[i]);
      ++airborne_placed;
      if(moving) { moving=false; next_order=0; order_mode=0; }
      return true;
    }
    if(speed>0.6f && !(is_avatar && now<avatar_action_until)) {
      // Keep one steady order: re-aim only every 400 ms, or sooner when the
      // direction or gait changes, so the walk cycle doesn't stop and start.
      int mode=speed<2.2f ? 1 : speed<5.5f ? 2 : 3;
      if(order_mode) { // hysteresis: keep the current gait near its band edges
        const float lo=order_mode==1?0.0f:order_mode==2?2.2f:5.5f, hi=order_mode==1?2.2f:order_mode==2?5.5f:1e9f;
        if(speed>lo-0.4f && speed<hi+0.4f) mode=order_mode;
      }
      const float heading=std::atan2(vel[0],vel[2]);
      float turn=std::abs(heading-order_heading); if(turn>3.14159f) turn=6.28318f-turn;
      if(now>=next_order || mode!=order_mode || turn>0.45f) {
        order_mode=mode; order_heading=heading;
        const uint32_t destination=data+1620;
        for(unsigned i=0;i<3;++i) api->write_f32(destination+i*4,path_pos[i]+vel[i]*0.8f);
        if(!OrderMove(scratch,obj,destination,mode)) {
          if(tag && failures++<5) { char line[100]; std::snprintf(line,sizeof(line),"%s: move order refused",tag); ProbeLog(line); }
          // Refused (landing, getting up ...): keep it on the path this update
          // instead of leaving it where it is.
          if(is_avatar && gap<6.0f) {
            for(unsigned i=0;i<3;++i) api->write_f32(obj+20+i*4,path_pos[i]);
            for(unsigned i=0;i<9;++i) api->write_f32(obj+32+i*4,rows[i]);
            next_order=now+100;
            return true;
          }
        }
        next_order=now+400; moving=true;
      }
    } else if(moving && !(is_avatar && now<avatar_action_until)) {
      // (not during a replicated action: the stop order cut climbs and jumps
      // short and the character went idle while still moving over the wall)
      OrderMove(scratch,obj,pos,1); // stop on the spot
      moving=false; next_order=0; order_mode=0;
    }
    // While a replicated action (punch, jump, reaction) plays, let the
    // animation own the character unless the original has moved away.
    if(now<avatar_action_until && obj==ResolveHuman(avatar_local) && gap<3.0f) {
      // Climbing: the animation carries the character up and over; placing
      // it every update broke the climb (it went idle while floating over).
      if(avatar_climbing && gap<2.5f) return true;
      for(unsigned i=0;i<3;++i) api->write_f32(obj+20+i*4,path_pos[i]);
      for(unsigned i=0;i<9;++i) api->write_f32(obj+32+i*4,rows[i]);
      return true;
    }
    const bool find_node=now>=next_node;
    if(find_node) next_node=now+200;
    return MoveHuman(scratch,obj,pos,matrix,velocity,find_node);
  }
};
Follower avatar_follow, npc_follow; // game thread
std::vector<uint8_t> npc_pending_look; // applied once setup completes
void SetPendingLook(const uint8_t* look,unsigned length) { npc_pending_look.assign(look,look+length); }
void ResetReplicaFollow() { npc_follow.Reset(); }
void FeedReplica(uint32_t time,const float* pos,const float* rows,uint64_t now) { npc_follow.Add(time,pos,rows,now); }

// The peer's vehicle: a copy created here with the same model, moved to the
// peer's vehicle pose every update, with the avatar sitting in it.
// Cars are shared: each game has its real cars, and a copy ("proxy") of the
// car the other player uses. Whoever sits in the driver's seat is in charge
// of the car: the other game moves its instance along the driver's path.
// The other player's character is seated in the matching seat of the
// matching instance, so getting in behind a driver makes you a passenger,
// like with homies.
struct RemoteCar {
  int32_t type=-1;
  Follower follow;
  uint32_t last_sequence=0, followed=0;
  uint64_t next_spawn=0, next_enter=0, next_exit=0, next_log=0, next_swap=0;
  unsigned enter_tries=0, snaps=0, snap_nophys=0, snap_turn=0, snap_far=0;
  ImpactWatch impact;
  uint64_t want_since=0;     // the other player's car was first reported (enter events arrive with it)
  uint64_t enter_ordered=0;  // the character was told to get in (walk to the door, open it, pull a driver out)
  uint64_t moving_since=0;   // their car started moving while our copy of them was still getting in
  uint32_t enter_target=0;
};
RemoteCar remote_car; // game thread

void DestroyRemoteCar(GuestScratch& scratch) {
  const uint32_t player=api->read_u32(0x8309ABEC);
  if(const uint32_t av=avatar_local ? ResolveHuman(avatar_local) : 0) if(proxy_local && api->read_u32(av+2496)==proxy_local) {
    OrderExitVehicle(scratch,av);   // not removed with the character in it; next time
    return;
  }
  if(ResolveVehicle(proxy_local) && api->read_u32(player+2496)!=proxy_local) {
    scratch.ctx.r3.u64=proxy_local;
    scratch.Call(0x823ACF50); // generic object destroy (vtable +16)
    ProbeLog("Remote vehicle copy removed");
  }
  proxy_local=0; proxy_peer=0; remote_car.type=-1; remote_car.follow.Reset();
}

std::vector<uint8_t> ReadCarLook(GuestScratch& scratch,uint32_t vehicle) {
  std::vector<uint8_t> look;
  const uint32_t state=api->read_u32(vehicle+54688);
  if(!Readable(state,4)) return look;
  const uint32_t buffer=scratch.data()+2048;
  scratch.ctx.r3.u64=state; scratch.ctx.r4.u64=buffer; scratch.ctx.r5.u64=320;
  if(!scratch.Call(0x8254DAF8)) return look;
  const uint32_t n=scratch.ctx.r3.u32;
  if(n && n<=320) look.assign(static_cast<uint8_t*>(api->guest_pointer(buffer)),static_cast<uint8_t*>(api->guest_pointer(buffer))+n);
  return look;
}
bool ApplyCarLook(GuestScratch& scratch,uint32_t vehicle,const std::vector<uint8_t>& look) {
  const uint32_t state=api->read_u32(vehicle+54688);
  if(look.empty() || look.size()>320 || !Readable(state,4)) return false;
  const uint32_t buffer=scratch.data()+2048;
  std::memcpy(api->guest_pointer(buffer),look.data(),look.size());
  scratch.ctx.r3.u64=state; scratch.ctx.r4.u64=buffer;
  return scratch.Call(0x8254DCC0);
}
void ShareCarLook(GuestScratch& scratch,uint32_t vehicle,uint64_t now) {
  const uint32_t handle=api->read_u32(vehicle+68);
  uint64_t& last=car_look_read[handle];
  if(last && now-last<2000) return;
  last=now;
  std::vector<uint8_t> look=ReadCarLook(scratch,vehicle);
  if(look.empty()) return;
  std::lock_guard lock(car_look_mutex);
  car_looks_out[handle]=std::move(look);
}

uint32_t CreateVehicle(GuestScratch& scratch,int32_t type,const float* pos,const float* rows) {
  if(type<0 || uint32_t(type)>=api->read_u32(kVehicleInfoCount)) return 0;
  const uint32_t data=scratch.data(), p=data+1700, m=data+1716;
  for(unsigned i=0;i<3;++i) api->write_f32(p+i*4,pos[i]);
  api->write_f32(p+12,0);
  for(unsigned i=0;i<9;++i) api->write_f32(m+i*4,rows[i]);
  // Same arguments as the network vehicle creator 823BA888.
  const uint32_t sp=scratch.ctx.r1.u32;
  api->write_u32(sp+84,0); api->write_u8(sp+95,0); api->write_u32(sp+100,0);
  scratch.ctx.r3.u64=3; scratch.ctx.r4.u64=2; scratch.ctx.r5.u64=uint32_t(type);
  scratch.ctx.r6.u64=p; scratch.ctx.r7.u64=m; scratch.ctx.r8.u64=0;
  scratch.ctx.r9.u64=0; scratch.ctx.r10.u64=0; scratch.ctx.f1.f64=0.0;
  if(!scratch.Call(0x82569700)) return 0;
  const uint32_t handle=scratch.ctx.r3.u32;
  const uint32_t vehicle=ResolveVehicle(handle);
  if(!vehicle) return 0;
  // Like the network vehicle creator 823BA888: take a streaming reference on
  // the model (id in the info entry's first word), or the car stays unloaded
  // and invisible, then place it.
  const uint32_t id=api->read_u32(kVehicleInfo+uint32_t(type)*kVehicleInfoSize);
  scratch.ctx.r3.u64=kStreamTable+(id>>24)*204; scratch.ctx.r4.u64=id;
  scratch.ctx.r5.u64=5; scratch.ctx.r6.u64=0; scratch.ctx.r7.u64=1;
  scratch.Call(0x8250C750);
  scratch.ctx.r3.u64=vehicle; scratch.ctx.r4.u64=p; scratch.ctx.r5.u64=m; scratch.ctx.r6.u64=0;
  scratch.Call(0x82535C68);
  return handle;
}

// Puts a human straight into a seat, the way character_add_vehicle (824C9550)
// and the multiplayer player code do: 8257B2C0 with a 28-byte request.
// The AI enter order (820F2420) is ignored by player-class characters.
// animated: the way the game's own enter (820F2858) asks when next to the
// car: request byte +24 clear (set, the character is put straight in).
bool SeatInVehicle(GuestScratch& scratch,uint32_t human,uint32_t vehicle,uint32_t seat,bool animated=false) {
  const uint32_t r=scratch.data()+1800;
  api->write_u32(r,human); api->write_u32(r+4,vehicle); api->write_u32(r+8,3); api->write_u32(r+12,seat);
  uint8_t flags[12]={1,0,0,1,0,0,1,1,1,0,0,0};
  if(animated) { // as the player's own E (8247FC68): mode 0, walk to the door
    static const uint8_t walk[12]={0,0,0,1,0,0,1,0,0,0,0,0};
    std::memcpy(flags,walk,12); api->write_u32(r+8,0);
  }
  for(unsigned i=0;i<12;++i) api->write_u8(r+16+i,flags[i]);
  scratch.ctx.r3.u64=r;
  return scratch.Call(0x8257B2C0) && scratch.ctx.r3.u32;
}

void OrderEnterVehicle(GuestScratch& scratch,uint32_t human,uint32_t vehicle_handle,uint32_t seat) {
  scratch.ctx.r3.u64=human; scratch.ctx.r4.u64=vehicle_handle;
  scratch.ctx.r5.u64=1; scratch.ctx.r6.u64=seat; scratch.ctx.r7.u64=1;
  scratch.Call(0x820F2420);
}

void OrderExitVehicle(GuestScratch& scratch,uint32_t human) {
  const uint32_t order=scratch.data()+1760;
  for(unsigned i=0;i<40;i+=4) api->write_u32(order+i,0);
  api->write_u32(order,human); api->write_u32(order+4,0xFFFFFFFFu);
  scratch.ctx.r3.u64=order;
  scratch.Call(0x8257B8E0);
}

// Puts the vehicle exactly at a pose the way teleport_vehicle (824DCD50)
// does: settle the wheels, then move body and physics.
void PlaceVehicle(GuestScratch& scratch,uint32_t vehicle,uint32_t pos,uint32_t matrix) {
  scratch.ctx.r3.u64=vehicle;
  if(scratch.Call(0x82534AE8)) {
    const int wheels=std::min<int>(int(scratch.ctx.r3.u32),16);
    for(int i=0;i<wheels;++i) {
      scratch.ctx.r3.u64=vehicle; scratch.ctx.r4.u64=uint32_t(i);
      if(!scratch.Call(0x82534EC8)) break;
      scratch.ctx.r4.u64=scratch.ctx.r3.u32; scratch.ctx.r3.u64=vehicle; scratch.ctx.f1.f64=0.0;
      scratch.Call(0x825353C0);
    }
  }
  scratch.ctx.r3.u64=vehicle; scratch.ctx.r4.u64=pos; scratch.ctx.r5.u64=matrix; scratch.ctx.r6.u64=1;
  scratch.Call(0x82535C68);
}

// Moves a car along the driver's path by steering its physics: velocity
// towards the path, the driver's spin, and a snap only when far off.
void FollowVehicle(GuestScratch& scratch,uint32_t vehicle,const PlayerPose& p,uint64_t now) {
  RemoteCar& car=remote_car;
  if(car.followed!=vehicle) { car.follow.Reset(); car.followed=vehicle; }
  if(p.sequence!=car.last_sequence) { car.last_sequence=p.sequence; car.follow.Add(p.time,p.vpos,p.vrows,now); }
  if(!car.follow.count) return;
  const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316;
  float path_pos[3], rows[9], vel[3];
  car.follow.Sample(uint32_t(int64_t(now)-car.follow.offset-30),path_pos,rows,vel);
  if(p.vvel[0]||p.vvel[1]||p.vvel[2]) std::memcpy(vel,p.vvel,sizeof(vel));
  float e[3], dist2=0, align=0, cur[9];
  for(unsigned i=0;i<3;++i) { e[i]=path_pos[i]-api->read_f32(vehicle+20+i*4); dist2+=e[i]*e[i]; }
  for(unsigned i=0;i<9;++i) { cur[i]=api->read_f32(vehicle+32+i*4); align+=rows[i]*cur[i]; } // 3 when identical
  const float speed=std::sqrt(vel[0]*vel[0]+vel[1]*vel[1]+vel[2]*vel[2]);
  const float snap=8.0f+speed*0.35f;
  const uint32_t v=VehicleVelocityAddress(vehicle);
  // Teleport only when far off (a teleport resets the car and shows as a
  // flicker); otherwise steer it with velocity and spin towards the path.
  if(!v || dist2>snap*snap || align<2.0f) { // ~60 degrees
    for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,path_pos[i]);
    api->write_f32(pos+12,0);
    for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,rows[i]);
    PlaceVehicle(scratch,vehicle,pos,matrix);
    if(const uint32_t v2=VehicleVelocityAddress(vehicle))
      for(unsigned i=0;i<3;++i) { api->write_f32(v2+i*4,vel[i]); api->write_f32(v2+16+i*4,p.vang[i]); }
    ++car.snaps;
    if(!v) ++car.snap_nophys; else if(align<2.0f) ++car.snap_turn; else ++car.snap_far;
    return;
  }
  // Rotation error as a small angular velocity: half the sum of the cross
  // products of the car's axes with the wanted axes.
  float w[3]={0,0,0};
  for(unsigned a=0;a<3;++a) {
    const float* c=cur+a*3; const float* t=rows+a*3;
    w[0]+=0.5f*(c[1]*t[2]-c[2]*t[1]);
    w[1]+=0.5f*(c[2]*t[0]-c[0]*t[2]);
    w[2]+=0.5f*(c[0]*t[1]-c[1]*t[0]);
  }
  for(unsigned i=0;i<3;++i) {
    api->write_f32(v+i*4,vel[i]+std::clamp(e[i]*5.0f,-15.0f,15.0f));
    api->write_f32(v+16+i*4,p.vang[i]+std::clamp(w[i]*5.0f,-3.0f,3.0f));
  }
}

// While the other player's character drives, the local player's "leader"
// (human +4128) is set to it. The game's enter code (82575438) then leaves a
// driver who is the entering human's leader in place instead of pulling them
// out, so walking up to the car gives the local player a passenger seat.
void SetRideAlong(uint32_t avatar,bool on) {
  const uint32_t player=api->read_u32(0x8309ABEC);
  const uint32_t handle=api->read_u32(avatar+68);
  const uint32_t current=api->read_u32(player+4128);
  if(on && current!=handle && (current==0 || !ResolveHuman(current))) api->write_u32(player+4128,handle);
  else if(!on && current==handle) api->write_u32(player+4128,0);
}

// Returns true while a vehicle owns the avatar (the on-foot follower must
// not move it then).
uint32_t AdoptSharedCar(GuestScratch& scratch,uint32_t host_id,uint64_t now);
void PinModel(GuestScratch& scratch,uint32_t id,uint64_t now);
uint32_t OccupantHandle(uint32_t vehicle,uint32_t seat);
bool UpdateRemoteCar(GuestScratch& scratch,uint32_t avatar,const PlayerPose& p,uint64_t now) {
  RemoteCar& car=remote_car;
  // Our getting in as a passenger (their car) stalled before (the player
  // stood there and never sat): after 3.5 s the seat is taken directly.
  if(passenger_watch_car) {
    const uint32_t me=api->read_u32(0x8309ABEC);
    const uint32_t pc=ResolveVehicle(passenger_watch_car);
    if(!pc) passenger_watch_car=0;
    else if(api->read_u32(me+2496)==passenger_watch_car && api->read_u32(me+2500)==1 && api->read_u32(me+2556)==0x12) passenger_watch_car=0;
    else if(now-passenger_watch_since>3500) {
      const bool ok=SeatInVehicle(scratch,me,pc,1);
      ProbeLog(ok?"Passenger getting-in stalled: seated directly":"Passenger getting-in stalled: seat refused");
      passenger_watch_car=0;
    }
  }
  // Getting in here stops at the door: the enter task (+2556 = 3) waits in
  // state 3/13 for 8244D128 to say the walk to the door is done, which it
  // never does for this character (the player's own is done in ~0.1 s). The
  // game's next step (82465578, as 8257F0E8 does) is called for it then,
  // and the door, climb-in and seat play.
  {
    static uint64_t stuck_since=0, next_push=0;
    if(now<enter_hold_until && api->read_u32(avatar+2556)==3 && api->read_u32(avatar+516)==3 && api->read_u32(avatar+520)==0x13) {
      if(!stuck_since) stuck_since=now;
      if(now-stuck_since>250 && now>=next_push) {
        next_push=now+700;
        scratch.ctx.r3.u64=avatar; scratch.ctx.r4.u64=0;
        ++replicated_depth; scratch.Call(0x82465578); --replicated_depth;
        enter_hold_until=std::max(enter_hold_until,now+4000); avatar_car_anim_until=std::max(avatar_car_anim_until,now+4000);
        static unsigned logged=0;
        if(logged++<20) ProbeLog("Remote player: at the door, getting in carried on");
      }
    } else stuck_since=0;
  }
  // The other player's own enter/exit is playing here: no placing meanwhile.
  if(now<enter_hold_until) { car.enter_target=enter_hold_car; car.next_enter=std::max(car.next_enter,enter_hold_until); }
  if(now<exit_hold_until) car.next_exit=std::max(car.next_exit,exit_hold_until);
  if(!ResolveVehicle(proxy_local)) { proxy_local=0; proxy_peer=0; }
  const uint32_t avatar_car=api->read_u32(avatar+2496);
  const bool avatar_in=ResolveVehicle(avatar_car)!=0;
  const bool sitting=p.vehicle>=0 && (p.flags&0x10);
  if(!sitting) {
    // Still getting in there: the character follows them on foot (their
    // car actions play on it) and is put in the seat once they sit.
    if(p.vehicle>=0 && !avatar_in) return false;
    if(p.vehicle>=0) return avatar_in;   // seated here already: never taken out while their car is set
    if(now<enter_hold_until) return avatar_in;
    static uint64_t exit_first=0;
    if(avatar_in) {
      SetRideAlong(avatar,false);
      if(!exit_first) exit_first=now;
      // Exits refused (their car was wrecked: 30+ orders in a row, the
      // character stayed in the wreck while they walked away): after 4.5 s
      // the character is removed and made again where they are.
      if(now-exit_first>4500) {
        static unsigned logged=0;
        if(!avatar_force_remake && logged++<10) ProbeLog("Remote player still in the car 4.5 s after leaving it there: made again");
        avatar_force_remake=true;
      } else if(now>=car.next_exit) {
        OrderExitVehicle(scratch,avatar); car.next_exit=now+1500;
        ProbeLog("Remote player left the vehicle; exit ordered");
      }
    } else exit_first=0;
    car.enter_tries=0; car.want_since=0;
    return avatar_in;
  }
  if(!car.want_since) car.want_since=now;
  // Which car here is the one the other player sits in?
  uint32_t target=0;
  if(!p.vowner) target=ResolveVehicle(p.vid); // our own car
  if(!p.vowner && !target && own_copy_id==p.vid) target=ResolveVehicle(own_copy_local);
  if(!p.vowner && !target) {
    // Our car they took is gone here (this game removed it far away), so
    // they drove up in an invisible car: a copy is made as for theirs.
    const float dx=p.vpos[0]-local_x.load(), dz=p.vpos[2]-local_z.load();
    if(dx*dx+dz*dz>100.0f*100.0f || now<car.next_spawn) return avatar_in;
    car.next_spawn=now+2000;
    const uint32_t made=CreateVehicle(scratch,p.vehicle,p.vpos,p.vrows); if(made && p.vehicle>=0) PinModel(scratch,api->read_u32(kVehicleInfo+uint32_t(p.vehicle)*kVehicleInfoSize),now);
    char line[160];
    std::snprintf(line,sizeof(line),"Remote vehicle type %d: %s (local %08X; our car %08X is gone here)",p.vehicle,made?"created":"not created",made,p.vid);
    ProbeLog(line);
    if(!made) return avatar_in;
    own_copy_local=made; own_copy_id=p.vid; car.enter_tries=0; car.next_enter=0;
    target=ResolveVehicle(made);
  } else if(p.vowner) {
    // proxy_local is a handle; the car object is looked up from it.
    if(proxy_peer==p.vid) target=ResolveVehicle(proxy_local);
    if(!target) for(auto& [local,host]:taken_cars) if(host==p.vid) { target=ResolveVehicle(local); break; }
    if(!target) target=AdoptSharedCar(scratch,p.vid,now); // a traffic car shown here: that one, not a second car
    if(!target) {
      const float dx=p.vpos[0]-local_x.load(), dz=p.vpos[2]-local_z.load();
      if(dx*dx+dz*dz>100.0f*100.0f || now<car.next_spawn) return avatar_in;
      car.next_spawn=now+2000;
      if(proxy_local) DestroyRemoteCar(scratch);
      const uint32_t made=CreateVehicle(scratch,p.vehicle,p.vpos,p.vrows); if(made && p.vehicle>=0) PinModel(scratch,api->read_u32(kVehicleInfo+uint32_t(p.vehicle)*kVehicleInfoSize),now);
      char line[140];
      std::snprintf(line,sizeof(line),"Remote vehicle type %d: %s (local %08X, theirs %08X)",p.vehicle,made?"created":"not created",made,p.vid);
      ProbeLog(line);
      if(!made) return avatar_in;
      proxy_local=made; proxy_peer=p.vid; car.type=p.vehicle; car.enter_tries=0; car.next_enter=0;
      target=ResolveVehicle(made);
    }
  }
  if(!target) return avatar_in;
  if(p.vowner) { // the other player's own car: same colours as theirs
    static std::vector<uint8_t> applied; static uint32_t applied_to=0;
    std::vector<uint8_t> look;
    { std::lock_guard lock(car_look_mutex); auto l=car_looks_in.find(p.vid); if(l!=car_looks_in.end()) look=l->second; }
    const uint32_t th=api->read_u32(target+68);
    if(!look.empty() && (look!=applied || applied_to!=th) && ApplyCarLook(scratch,target,look)) {
      applied=look; applied_to=th;
      char line[100]; std::snprintf(line,sizeof(line),"Remote vehicle colours applied (%zu bytes)",look.size()); ProbeLog(line);
    }
  }
  const uint32_t target_handle=api->read_u32(target+68);
  // Both want the driver's seat: the game that owns the car keeps its
  // driver. When that is the other game, this player moves over to the
  // passenger seat; when it is this game, the other player's character sits
  // in the passenger seat here (their game moves them over too).
  uint32_t want_seat=p.seat;
  const uint32_t player=api->read_u32(0x8309ABEC);
  const bool local_driving=api->read_u32(player+2496)==target_handle && api->read_u32(player+2500)==0 && api->read_u32(player+2556)==0x12;
  if(p.seat==0 && local_driving) {
    // Both in the driver's seat: whoever got in first keeps it (the game
    // put the one getting in at the wheel even when asked for the passenger
    // seat, and both sat there: the car couldn't be driven).
    const bool they_first=remote_enter_car==target_handle && remote_enter_time && remote_enter_time<local_enter_time;
    if(they_first) {
      if(now>=car.next_swap) {
        car.next_swap=now+1000;
        const bool ok=SeatInVehicle(scratch,player,target,1);
        if(!ok) car.next_swap=now+5000;   // (tried every second, and each try was sent to the other game)
        ProbeLog(ok?"Local player moved to the passenger seat (the other player was there first)":"Local player: passenger seat refused");
      }
      return true;
    }
    want_seat=1;
  }
  // The driver's game is in charge of the car.
  // Their car drives off while their getting in still plays here (it was
  // left behind and put in the seat 6-10 s later): hold the car a moment,
  // then seat the character at once.
  if(want_seat==0 && now<enter_hold_until) {
    const bool seated=OccupantHandle(target,0)==api->read_u32(avatar+68);
    const float sp=std::sqrt(p.vvel[0]*p.vvel[0]+p.vvel[1]*p.vvel[1]+p.vvel[2]*p.vvel[2]);
    if(seated) car.moving_since=0;
    else if(sp>1.5f) {
      if(!car.moving_since) {
        car.moving_since=now;
        char line[140]; std::snprintf(line,sizeof(line),"Their car moves (%.1f m/s) while their getting in plays here; seat %u holds %08X",sp,want_seat,OccupantHandle(target,0));
        ProbeLog(line);
      }
      if(now<car.moving_since+700) return true;   // car waits
      const bool ok=SeatInVehicle(scratch,avatar,target,0);
      enter_hold_until=0; avatar_car_anim_until=0; car.moving_since=0;
      ProbeLog(ok?"Remote player: seated at once (their car drove off before getting in finished here)":"Remote player: seating at once refused");
    }
  } else car.moving_since=0;
  if(want_seat==0) {
    static uint64_t next_damage=0;
    if(now>=next_damage) { next_damage=now+250; ApplyCarDamage(scratch,target,uint8_t((p.flags>>1)&7)); }
    // (the handle the other game knows this car by)
    const uint32_t peer_handle=p.vid;
    if(!WatchImpact(target,car.impact,peer_handle,now)) { FollowVehicle(scratch,target,p,now); AfterSteer(target,car.impact); }
  }
  SetRideAlong(avatar,want_seat==0 && avatar_car==target_handle);
  // Their own enter is replayed here (kEventEnterCar), and a replayed exit
  // must not be undone while their game still shows them seated: the pose
  // only moves the character when neither did it.
  const bool car_hold=now<exit_hold_until || now<enter_hold_until || now<car.want_since+100;
  if(car_hold && !avatar_in) {}
  else if(avatar_car!=target_handle || api->read_u32(avatar+2500)!=want_seat) {
    if(avatar_in && avatar_car!=target_handle && now>=car.next_exit) { OrderExitVehicle(scratch,avatar); car.next_exit=now+1000; }
    else if(!avatar_in && false && car.enter_target!=target_handle && now>=car.next_enter) {
      // Get in the way the player did: walk to the door, open it, pull out
      // a driver if there is one (the game's enter order 820F2420(human,
      // vehicle, 1, seat, 1), as vehicle_enter_do uses). Placed straight in
      // the seat if that doesn't work within 5 s.
      const float dx=api->read_f32(target+20)-api->read_f32(avatar+20), dz=api->read_f32(target+28)-api->read_f32(avatar+28);
      if(dx*dx+dz*dz<12.0f*12.0f) {
        // (The AI enter order 820F2420 is ignored by player-class
        // characters: it walked for 3.5 s and was then placed.)
        ++replicated_depth;
        const bool ok=SeatInVehicle(scratch,avatar,target,want_seat,true);
        --replicated_depth;
        car.enter_target=target_handle; car.enter_ordered=now; car.next_enter=now+3500;
        char line[120]; std::snprintf(line,sizeof(line),"Remote player: getting in %08X seat %u %s",target_handle,want_seat,ok?"(walking to the door)":"refused, placing");
        ProbeLog(line);
        if(!ok) car.next_enter=now;
      } else car.enter_target=target_handle;
    }
    else if(now>=car.next_enter) {
      if(const uint32_t occ=SeatedIn(target_handle,want_seat)) if(occ!=api->read_u32(avatar+68) && ResolveHuman(occ) && occ!=api->read_u32(player+68)) {
        DestroyObject(scratch,occ);   // the driver they pulled out is still sitting here
        ProbeLog("Driver still in the seat the other player took: removed");
      }
      const bool ok=SeatInVehicle(scratch,avatar,target,want_seat);
      ++car.enter_tries; car.next_enter=now+1000;
      char line[120]; std::snprintf(line,sizeof(line),"Remote player: seat %u in %08X %s (try %u)",want_seat,target_handle,ok?"ok":"refused",car.enter_tries); ProbeLog(line);
    }
  } else {
    if(now>=car.next_log) {
      car.next_log=now+5000;
      char line[220];
      std::snprintf(line,sizeof(line),"Remote vehicle: seated %u, %s car, pos %.1f %.1f %.1f, physics %s, teleports %u (far %u, turn %u, no physics %u)",p.seat,p.vowner?"their":"our",
                    api->read_f32(target+20),api->read_f32(target+24),api->read_f32(target+28),VehicleVelocityAddress(target)?"active":"inactive",car.snaps,car.snap_far,car.snap_turn,car.snap_nophys);
      car.snaps=car.snap_far=car.snap_turn=car.snap_nophys=0;
      ProbeLog(line);
    }
  }
  return true;
}

// The peer's player, shown as a mod-owned actor with the same model when this
// game has it, otherwise a stand-in.
void SetCharacterHidden(GuestScratch& scratch,uint32_t obj,bool hide) {
  const uint32_t flags=api->read_u32(obj+120);
  if(hide==((flags&0x08000000u)!=0)) return;
  if(!hide) { scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=obj+20; scratch.ctx.r5.u64=1; scratch.Call(0x8243F260); }
  api->write_u32(obj+120,hide ? (flags|0x08000000u) : (flags&~0x08000000u));
  if(flags&0x04000000u) {
    const uint32_t vtable=api->read_u32(obj);
    if(Readable(vtable+28,4)) {
      scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=hide?0:1; scratch.ctx.r5.u64=1;
      scratch.Call(api->read_u32(vtable+28));
    }
  }
}

void UpdateAvatar(GuestScratch& scratch,uint64_t now) {
  static uint32_t last_sequence=0;
  static uint64_t next_item_check=0;
  PlayerPose p; bool fresh;
  { std::lock_guard lock(pose_mutex); p=remote_pose; fresh=remote_pose_new; remote_pose_new=false; }
  if(!p.sequence) return;
  if(fresh) avatar_seen=now;
  if(now-avatar_seen>3000 && !HostPauseHold(now)) { if(avatar_local) DestroyAvatar(scratch); return; }
  // No other player's character during a cutscene here, and for 2 s after:
  // both games play it with their own player on the same spot, so the
  // character stood inside this player (who then got stuck and kept falling
  // over). The joiner steps aside when it ends; the character comes back
  // dressed.
  {
    static uint64_t cutscene_seen=0;
    if(CutscenePlaying() || (!hosting.load() && host_cutscene.load())) cutscene_seen=now;
    // Hidden like character_hide / character_show (824C92E0 / 824C9488) do:
    // not removed (removing takes the clothes off and waits for the game,
    // which never finished while a cutscene loads: the game froze).
    static bool hidden=false;
    const uint32_t shown_obj=ResolveHuman(avatar_local);
    if(cutscene_seen && now-cutscene_seen<2000) {
      if(shown_obj && !hidden) { SetCharacterHidden(scratch,shown_obj,true); hidden=true; ProbeLog("Cutscene: other player's character hidden until it ends"); }
      avatar_follow.Reset();
      return;
    }
    if(hidden) {
      hidden=false;
      if(shown_obj) { SetCharacterHidden(scratch,shown_obj,false); ProbeLog("Other player's character shown again"); }
    }
  }
  if(p.sequence!=last_sequence) { last_sequence=p.sequence; avatar_follow.Add(p.time,p.pos,p.rows,now); }
  {
    // Out of this game's streaming range the character's clothing is unloaded
    // for good. Remove the character (and its car) and make a fresh one when
    // the other player comes back; a new character is dressed like the first.
    const float dx=p.pos[0]-local_x.load(), dz=p.pos[2]-local_z.load();
    if(dx*dx+dz*dz>130.0f*130.0f && (avatar_local || proxy_local)) {
      if(avatar_local) { DestroyAvatar(scratch); if(!avatar_local) ProbeLog("Remote player out of range; character removed until they come back"); }
      if(!avatar_local && proxy_local) DestroyRemoteCar(scratch);   // (the car after the character is out of it)
      avatar_follow.Reset();
      return;
    }
  }
  const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316, name=data+1400;
  uint32_t obj=ResolveHuman(avatar_local);
  if(!obj) {
    avatar_local=0; avatar_count=0;
    WritePose(p,pos,matrix);
    const float dx=p.pos[0]-local_x.load(),dy=p.pos[1]-local_y.load(),dz=p.pos[2]-local_z.load();
    // Only create close by: a character made far from the camera is built
    // without its body and clothing meshes and never gets them back.
    if(dx*dx+dy*dy+dz*dz>60*60) return;
    static uint64_t next_attempt=0;
    if(now<next_attempt) return;
    next_attempt=now+1000;
    const char* used=p.model;
    char local_model[64];
    { std::lock_guard lock(pose_mutex); std::memcpy(local_model,local_pose.model,sizeof(local_model)); }
    const uint32_t player=api->read_u32(0x8309ABEC);
    // Own body model for the other player: ask for it and wait for it (up to
    // 8 s, then the shared one as before).
    static uint64_t own_body_wait_start=0;
    if(use_player_avatar && own_body && !OwnBodyDefinition(scratch,NextPlayerSlot(),true)) {
      if(!own_body_wait_start) own_body_wait_start=now;
      if(now-own_body_wait_start<8000) return;
    }
    own_body_wait_start=0;
    if(use_player_avatar && p.model[0] && !std::strcmp(p.model,local_model)) {
      if(const uint32_t made=SpawnPlayerObject(scratch,player,pos,matrix,"Remote player")) {
        avatar_local=api->read_u32(made+68); avatar_player=true; avatar_own_body=last_spawn_own_body;
        if(ResolveHuman(avatar_local)!=made) {
          ProbeLog("Remote player: player object not in the object table, returning it");
          ReleasePlayerObject(scratch,made); avatar_local=0; avatar_player=false;
        }
      }
    }
    if(!avatar_local) {
      uint32_t definition=LookupModel(scratch,p.model,name);
      if(!definition) { used=kFallbackModel; definition=LookupModel(scratch,kFallbackModel,name); }
      if(!definition) { ProbeLog("Remote player: no usable model in this game"); return; }
      avatar_local=CreateActor(scratch,definition,pos,matrix,"Remote player");
    }
    obj=ResolveHuman(avatar_local);
    if(!obj) { avatar_local=0; avatar_player=false; return; }
    avatar_count=1; next_item_check=0; applied_worn.clear(); dress_step=0;
    // A character made again (back in range, after a gap in updates) starts
    // with nothing on: dress it from the last clothing received, the same way
    // as the first one. It was left undressed until the other player changed
    // clothes (the missing torso and legs: F8 showed "0 items listed").
    { std::lock_guard lock(pose_mutex); if(!remote_worn.empty()) remote_worn_new=true; }
    dressed_record=0;
    avatar_follow.next_order=0; avatar_follow.moving=false;
    { std::lock_guard lock(pose_mutex); avatar_model=used; }
    char line[200];
    std::snprintf(line,sizeof(line),"Remote player avatar uses model %s (peer model '%s')",used,p.model);
    ProbeLog(line);
  }
  // Pedestrians pick up props (bottles, phones) by themselves; the peer's
  // held item isn't synchronized yet, so the character carries nothing.
  if(now>=next_item_check) {
    scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=0;
    scratch.Call(0x82446E50);
    next_item_check=now+500;
  }
  if(avatar_player && p.look_length) {
    static std::vector<uint8_t> applied_look_block;
    static uint32_t look_handle=0;
    std::vector<uint8_t> look(p.look,p.look+p.look_length);
    if(look!=applied_look_block || look_handle!=avatar_local) {
      const bool ok=ApplyLook(scratch,avatar_local,p.look,p.look_length);
      char line[120];
      std::snprintf(line,sizeof(line),"Remote player look block %u bytes (%02X %02X) %s",p.look_length,p.look[0],p.look_length>1?p.look[1]:0,ok?"applied":"not applied");
      ProbeLog(line);
      if(ok) { applied_look_block=look; look_handle=avatar_local; }
    }
  }
  if(avatar_player) {
    std::vector<uint8_t> worn;
    { std::lock_guard lock(pose_mutex); if(remote_worn_new) { worn=remote_worn; remote_worn_new=false; } }
    // Some game events (e.g. taking a hit) strip or replace the character's
    // clothing. Once a second, check the slot still lists what was put on.
    static uint64_t next_check=0;
    // dressed_record: file level (reset when the character is made again)
    if(clothing_dump_request.exchange(false)) {
      // F8: what the other player's clothing looks like right now (press it
      // while the torso/legs are missing): each worn item's node and piece,
      // then the same for this game's own player, which looks right.
      for(int who=0;who<2;++who) {
      const uint32_t subject=who==0 ? obj : api->read_u32(0x8309ABEC);
      if(!Readable(subject,4488)) continue;
      const uint32_t custom=api->read_u32(subject+4484);
      char line[240];
      std::snprintf(line,sizeof(line),"F8 clothing (%s): object %08X record %08X slot %u, %u items listed, flags216 %08X, cutscene %d/%d, queue %08X",
                    who==0?"other player":"own player",subject,custom,Readable(custom,12)?api->read_u32(custom+8):99,Readable(custom,12)?WornCount(custom):0,
                    api->read_u32(subject+216),CutscenePlaying(),host_cutscene.load(),api->read_u32(0x830B71D4));
      ProbeLog(line);
      if(Readable(custom,12)) {
        const uint32_t head=api->read_u32(custom);
        uint32_t node=head;
        for(unsigned n=0;node && n<64 && Readable(node,24);++n) {
          const uint32_t item=api->read_u32(node), comp=api->read_u32(node+12);
          const bool cr=comp && Readable(comp,144);
          std::snprintf(line,sizeof(line),"  node %08X item %08X cat %d hidden %u comp %08X flags %02X buffer %08X slot %d refs %d next %08X",
                        node,item,Readable(item,36)?int(api->read_u32(item+32)):-1,api->read_u8(node+4),comp,
                        cr?api->read_u8(comp+132):0,cr?api->read_u32(comp+12):0,cr?int(api->read_u32(comp+24)):-1,
                        cr?int(api->read_u32(comp+20)):-1,cr?api->read_u32(comp+136):0);
          ProbeLog(line);
          node=api->read_u32(node+16);
          if(node==head) break;
        }
      }
      // Look/render state of the record (+4 morphs, +12, +16..+27).
      if(Readable(custom,28)) {
        std::snprintf(line,sizeof(line),"  record: %08X %08X %08X %08X %08X %08X %08X",api->read_u32(custom),api->read_u32(custom+4),
                      api->read_u32(custom+8),api->read_u32(custom+12),api->read_u32(custom+16),api->read_u32(custom+20),api->read_u32(custom+24));
        ProbeLog(line);
      }
      }
    }
    if(false) {  // F9 (8248A900 look rebuild) froze the game: removed
      // F9: rebuild how the character is drawn from what it wears (8248A900,
      // which the game's own clothing update 8248CBB0 calls after loads). No
      // taking off and putting on: that crashed.
      const uint32_t custom=api->read_u32(obj+4484);
      if(Readable(custom,12) && !CutscenePlaying()) {
        ProbeLog("F9: rebuilding the other player's look");
        scratch.ctx.r3.u64=custom;
        const bool ok=scratch.Call(0x8248A900);
        ProbeLog(ok?"F9: done":"F9: call failed");
      }
    }
    if(now>=next_check && !applied_worn.empty() && !dress_step) {
      next_check=now+1000;
      const uint32_t custom=api->read_u32(obj+4484);
      // Clothing pieces can also be unloaded while the list stays (e.g. after
      // driving far away and back): count listed nodes without a component.
      // Single player hands a free clothing piece (+132 without 0x10/0x40)
      // to whoever needs one, without checking whose it was (8248D5F0), so
      // a released piece of this character can be reloaded with someone
      // else's clothing (cutscene characters...) while still listed here:
      // count those as missing too.
      unsigned missing=0, released=0;
      static unsigned missing_for=0;
      if(Readable(custom,12)) {
        const uint32_t head=api->read_u32(custom);
        uint32_t node=head;
        for(unsigned n=0;node && n<64 && Readable(node,24);++n) {
          const uint32_t comp=api->read_u32(node+12);
          if(api->read_u32(node) && !comp) ++missing;
          else if(comp && Readable(comp,144) && (!(api->read_u8(comp+132)&0x50) || !api->read_u32(comp+12))) ++released;
          node=api->read_u32(node+16);
          if(node==head) break;
        }
      }
      missing+=released;
      missing_for=missing ? missing_for+1 : 0;
      // No dressing again just because a cutscene ended: doing that right
      // after one froze (0.51) or crashed (0.52) the game.
      if(CutscenePlaying() || host_cutscene.load()) missing_for=0;
      // Far from the camera the game unloads clothing and may hand the pieces
      // to something else. Dress again when the character comes back close.
      if(redress_for_colours && !CutscenePlaying() && !host_cutscene.load()) {
        redress_for_colours=false;
        ProbeLog("Remote player dressed again for their colours (hair colour is set when the hair goes on)");
        std::lock_guard lock(pose_mutex); worn=remote_worn;
        applied_worn.clear();
      } else
      if(custom!=dressed_record || WornCount(custom)!=applied_worn[0] || missing_for>=3) {
        missing_for=0;
        char line[160];
        std::snprintf(line,sizeof(line),"Remote player clothing lost (record %08X, %u items listed, %u expected, %u pieces unloaded, %u taken over); dressing again",
                      custom,Readable(custom,12)?WornCount(custom):0,applied_worn[0],missing-released,released);
        ProbeLog(line);
        std::lock_guard lock(pose_mutex); worn=remote_worn;
        applied_worn.clear();
      }
    }
    // The first dressing of a new character waits for the clothing to load,
    // as before. Dressing again happens over several frames without waiting:
    // take off (queued), let the game's frame update finish that, then wear.
    // Waiting inside the update right after a cutscene never returned (the
    // game froze), so nothing starts during a cutscene or while the game
    // still has clothing requests queued.
    static std::vector<uint8_t> pending_worn;
    static uint64_t dress_started=0;
    if(!worn.empty()) pending_worn=worn;
    const bool first=applied_worn.empty() && dressed_record!=api->read_u32(obj+4484);
    static uint64_t last_cutscene=0;
    if(CutscenePlaying() || host_cutscene.load()) last_cutscene=now;
    const bool quiet=now-last_cutscene>10000 && ClothingQueueIdle();
    if(dress_step==0 && !pending_worn.empty() && pending_worn!=applied_worn && quiet) {
      if(first) {
        // Colours first, as multiplayer does, so the hair goes on in its colour.
        { std::vector<uint8_t> ex; { std::lock_guard lock(pose_mutex); ex=remote_extras; }
          extras_at_dress.clear();
          if(!ex.empty() && ApplyExtras(scratch,obj,ex.data(),unsigned(ex.size()),false)) extras_at_dress=ex; }
        const bool ok=DressPlayerObject(scratch,obj,pending_worn.data(),unsigned(pending_worn.size()));
        char line[120];
        std::snprintf(line,sizeof(line),"Remote player clothing: %zu bytes %s",pending_worn.size(),ok?"worn":"not fully applied");
        ProbeLog(line);
        applied_worn=pending_worn; pending_worn.clear();
        dressed_record=api->read_u32(obj+4484);
      } else {
        const uint32_t custom=api->read_u32(obj+4484);
        const uint32_t slot=Readable(custom,12) ? api->read_u32(custom+8) : 99;
        { std::vector<uint8_t> ex; { std::lock_guard lock(pose_mutex); ex=remote_extras; }
          if(slot<12 && !ex.empty() && ApplyExtras(scratch,obj,ex.data(),unsigned(ex.size()),false)) extras_at_dress=ex; }
        if(slot<12) {
          for(uint32_t i=0;i<54 && i<api->read_u32(kSlotTables+13056+slot*4);++i) {
            scratch.ctx.r3.u64=custom; scratch.ctx.r4.u64=api->read_u32(kSlotTables+13120+slot*1080+i*20);
            scratch.Call(0x8248D198);
          }
          dress_step=1; dress_started=now;
        }
      }
    } else if(dress_step==1 && ClothingQueueIdle()) {
      const uint32_t custom=api->read_u32(obj+4484);
      scratch.ctx.r3.u64=custom; scratch.Call(0x8248D340);
      dress_step=2;
    } else if(dress_step==2 && ClothingQueueIdle() && !CutscenePlaying()) {
      const uint32_t custom=api->read_u32(obj+4484);
      const bool ok=WearItems(scratch,custom,pending_worn.data(),unsigned(pending_worn.size()));
      char line[140];
      std::snprintf(line,sizeof(line),"Remote player dressed again: %zu bytes %s, %u items listed (%.1f s)",pending_worn.size(),
                    ok?"worn":"not fully applied",WornCount(custom),(now-dress_started)/1000.0);
      ProbeLog(line);
      applied_worn=pending_worn; pending_worn.clear();
      dressed_record=custom; dress_step=0;
    }
    if(dress_step && now-dress_started>20000) {
      ProbeLog("Remote player dressing gave up: the game's clothing requests didn't finish in 20 s; trying again later");
      dress_step=0;
    }
    std::vector<float> morphs;
    { std::lock_guard lock(pose_mutex); if(remote_morphs_new) { morphs=remote_morphs; remote_morphs_new=false; } }
    // Only with its own body model: with the shared one the other player's
    // sliders would reshape this game's own player too.
    static uint64_t morph_check_at=0; static int morph_checks=0;
    if(morphs.size()==128 && morphs!=applied_morphs) {
      ProbeLog(("Remote player body sliders received: "+MorphSummary(morphs.data(),128)+
                (sync_body?"":" (mod.ini body = 0: not applied)")+(avatar_own_body?"":" (shared body model: not applied)")).c_str());
    }
    if(sync_body && avatar_own_body && morphs.size()==128 && morphs!=applied_morphs) {
      ProbeLog(ApplyMorphs(scratch,obj,morphs.data()) ? "Remote player body shape applied" : "Remote player body shape not applied");
      applied_morphs=morphs;
      morph_check_at=now+3000; morph_checks=0;
    }
    // Check that the game baked them into the model, and put them back if
    // something (dressing, a look rebuild) reset them.
    if(morph_check_at && now>=morph_check_at && applied_morphs.size()==128) {
      ProbeLog(("Remote player body after "+std::to_string(morph_checks==0?3:15)+" s: "+MorphState(obj,applied_morphs)).c_str());
      morph_check_at=++morph_checks<2 ? now+12000 : 0;
    }
    {
      static uint64_t next_recheck=0;
      if(sync_body && avatar_own_body && applied_morphs.size()==128 && now>=next_recheck) {
        next_recheck=now+2000;
        const uint32_t custom=api->read_u32(obj+4484);
        const uint32_t sliders=Readable(custom,12)?api->read_u32(custom+4):0;
        if(Readable(sliders,kMorphs*12)) {
          unsigned wrong=0;
          for(unsigned i=0;i<kMorphs;++i) wrong+=std::fabs(api->read_f32(sliders+i*12+4)-applied_morphs[i])>=0.001f;
          if(wrong) {
            static int resets=0;
            if(resets++<10) ProbeLog(("Remote player body sliders were reset ("+std::to_string(wrong)+" changed): applied again").c_str());
            ApplyMorphs(scratch,obj,applied_morphs.data());
          }
        }
      }
    }
    // Colours (skin tone, hair colour): once dressed, and again when they
    // change or the character was made again.
    {
      const uint32_t custom=api->read_u32(obj+4484);
      std::vector<uint8_t> extras;
      { std::lock_guard lock(pose_mutex); extras=remote_extras; remote_extras_new=false; }
      static uint32_t extras_record=0;
      if(avatar_own_body && !extras.empty() && dress_step==0 && dressed_record==custom && ClothingQueueIdle() &&
         (extras!=applied_extras || extras_record!=custom)) {
        const bool ok=ApplyExtras(scratch,obj,extras.data(),unsigned(extras.size()));
        ProbeLog(("Remote player colours "+std::string(ok?"applied":"not applied")+" ("+
                  std::to_string(extras.size())+" bytes): "+HexBytes(extras.data(),unsigned(extras.size()))).c_str());
        if(ok) {
          // Put on before these colours were set: dress again once so the hair takes them.
          if(extras!=extras_at_dress && !applied_worn.empty()) { redress_for_colours=true; extras_at_dress=extras; }
          applied_extras=extras; extras_record=custom;
        }
      }
    }
  }
  // Stuck in a car the other player left (see UpdateRemoteCar): made again.
  if(avatar_force_remake) {
    DestroyAvatar(scratch);
    if(!ResolveHuman(avatar_local)) { avatar_force_remake=false; avatar_follow.Reset(); }
    return;
  }
  if(UpdateRemoteCar(scratch,obj,p,now)) { ++avatar_updates; avatar_follow.Reset(); avatar_on_foot=false; return; }
  avatar_on_foot=true;
  remote_car.enter_target=0;
  // A landing that never arrived kept the character in "jumping" for good
  // (no hold after later actions, move orders fighting the animation).
  if(avatar_jumping && now>avatar_action_until) { avatar_jumping=false; avatar_reorder=true; }
  if(avatar_climbing && now>avatar_action_until) avatar_climbing=false;
  if(avatar_reorder) { avatar_reorder=false; avatar_follow.next_order=0; avatar_follow.order_mode=0; avatar_follow.moving=true; }
  MakePassive(obj,true);
  {
    static uint64_t next_crouch=0;
    if(now>=next_crouch) { next_crouch=now+250; MatchCrouch(scratch,obj,(p.flags&1)!=0); }
  }
  avatar_show_delay=avatar_follow.delay;
  avatar_follow.last_gap=0;
  if(avatar_follow.Drive(scratch,obj,now,"Remote player")) ++avatar_updates;
  // Stuck off the path (knocked down just before a cutscene hid it: after the
  // cutscene it lay flat 11-18 m behind for a whole mission, every move order
  // refused): remove it; a fresh one is made where the other player is.
  {
    static uint64_t off_since=0, last_remake=0;
    const bool busy=now<avatar_car_anim_until || avatar_climbing || avatar_jumping;
    if(avatar_follow.last_gap>5.0f && !busy) { if(!off_since) off_since=now; } else off_since=0;
    if(off_since && now-off_since>3000 && now-last_remake>10000) {
      char line[120]; std::snprintf(line,sizeof(line),"Other player's character stuck %.0f m off its path for 3 s: made again",avatar_follow.last_gap);
      ProbeLog(line);
      off_since=0; last_remake=now;
      DestroyAvatar(scratch); avatar_follow.Reset();
    }
  }
}

// The update hook above runs only 7-25 times a second, so on its own the
// other player moved in steps. Every frame, before the game builds it, the
// character is also put where the path says it is at that moment (plain
// memory writes; the update hook keeps giving the walk/run orders).
void SmoothSharedPeds(uint64_t now);
void TraceFrame() {
  if(!Tracing()) return;
  static uint32_t last[3][9]{};
  const uint32_t objs[3]={api->read_u32(0x8309ABEC), avatar_local ? ResolveHuman(avatar_local) : 0, trace_driver ? ResolveHuman(trace_driver) : 0};
  for(int i=0;i<3;++i) {
    const uint32_t o=objs[i];
    if(!o) continue;
    const uint32_t v[9]={api->read_u32(o+508),api->read_u32(o+516),api->read_u32(o+520),api->read_u32(o+756),api->read_u32(o+2496),
                         api->read_u32(o+2500),api->read_u32(o+2556),api->read_u32(o+276),api->read_u32(o+224)};
    if(std::memcmp(v,last[i],sizeof(v))) {
      std::memcpy(last[i],v,sizeof(v));
      char line[240];
      const uint32_t car=ResolveVehicle(v[4]);
      std::snprintf(line,sizeof(line),"TRACE %s now: move %X, state %X/%X, action %X, car %08X seat %d task %X, 276=%X 224=%X, in seat0 %08X, pos %.1f %.1f %.1f",i==2?"driver":i?"other":"me",v[0],v[1],v[2],v[3],v[4],int32_t(v[5]),v[6],v[7],v[8],
                    car?OccupantHandle(car,0):0,api->read_f32(o+20),api->read_f32(o+24),api->read_f32(o+28));
      ProbeLog(line);
    }
  }
}
void GameFrame(void*) {
  TraceFrame();
  if(!connected.load() || !running.load()) return;
  std::unique_lock game_lock(game_mutex, std::try_to_lock);
  if(!game_lock.owns_lock()) return;
  if(!hosting.load()) SmoothSharedPeds(MonoMs());
  if(!avatar_local || !avatar_on_foot) return;
  const uint32_t obj=ResolveHuman(avatar_local);
  if(!obj || !avatar_follow.count || avatar_follow.offset==INT64_MAX) return;
  const uint64_t now=MonoMs();
  if(now<avatar_action_until) return;  // jumps, punches: the animation owns it
  float pos[3], rows[9], vel[3];
  avatar_follow.Sample(uint32_t(int64_t(now)-avatar_follow.offset-avatar_follow.delay),pos,rows,vel);
  // Only small corrections: a big difference means a teleport or a
  // transition the update hook handles.
  const float dx=pos[0]-api->read_f32(obj+20), dy=pos[1]-api->read_f32(obj+24), dz=pos[2]-api->read_f32(obj+28);
  if(dx*dx+dy*dy+dz*dz>4.0f) return;
  for(unsigned i=0;i<3;++i) api->write_f32(obj+20+i*4,pos[i]);
  for(unsigned i=0;i<9;++i) api->write_f32(obj+32+i*4,rows[i]);
}

// The copied NPC follows the host's NPC the same way, every update.
void UpdateReplica(GuestScratch& scratch,uint64_t now) {
  const uint32_t obj=ResolveHuman(replica_local);
  if(!obj) return;
  if(!npc_pending_look.empty() && (api->read_u32(obj+216)&0x4000)) {
    const bool ok=ApplyLook(scratch,replica_local,npc_pending_look.data(),unsigned(npc_pending_look.size()));
    char line[100];
    std::snprintf(line,sizeof(line),"NPC replica appearance %zu bytes %s (after setup)",npc_pending_look.size(),ok?"applied":"failed");
    ProbeLog(line);
    npc_pending_look.clear();
  }
  if(npc_follow.Drive(scratch,obj,now,"NPC replica")) ++replica_updates;
}

// Once per session: facts needed for clothing sync. Player objects come from
// a pool (free list 0x8309ABE8, active list 0x8309AC54, links +4100); each
// has a customization record at +4484 (worn items). 8248DCA8 writes the
// worn-item list the way multiplayer customization sync does.
void LogPlayerDiagnostics(GuestScratch& scratch,uint32_t player) {
  auto count=[](uint32_t head) {
    unsigned n=0;
    for(uint32_t p=head; p && n<32 && Readable(p,4108);) { ++n; p=api->read_u32(p+4100); if(p==head) break; }
    return n;
  };
  const uint32_t free_head=api->read_u32(0x8309ABE8), active_head=api->read_u32(0x8309AC54);
  const uint32_t custom=api->read_u32(player+4484);
  char line[400];
  std::snprintf(line,sizeof(line),"Players: local %08X, free pool %08X (%u), active list %08X (%u), customization %08X slot %d",
                player,free_head,count(free_head),active_head,count(active_head),custom,
                Readable(custom,12) ? int(api->read_u32(custom+8)) : -1);
  ProbeLog(line);
  if(!Readable(custom,12)) return;
  const uint32_t writer=scratch.data()+1700, buffer=scratch.data()+1712;
  std::memset(api->guest_pointer(buffer),0,512);
  api->write_u32(writer,0); api->write_u32(writer+4,buffer);
  scratch.ctx.r3.u64=writer; scratch.ctx.r4.u64=custom;
  if(!scratch.Call(0x8248DCA8)) return;
  const uint32_t length=std::min<uint32_t>(api->read_u32(writer),256);
  std::string hex;
  const uint8_t* bytes=static_cast<const uint8_t*>(api->guest_pointer(buffer));
  for(uint32_t i=0;i<length;++i) { char b[4]; std::snprintf(b,sizeof(b),"%02x",bytes[i]); hex+=b; }
  ProbeLog(("Worn items ("+std::to_string(length)+" bytes): "+hex).c_str());
}

// Diagnostics for clothing: hex of the customization record and the
// per-slot tables 82318B38 clears, for a slot.
void LogHex(const char* label,uint32_t address,uint32_t length) {
  if(!Readable(address,length)) return;
  const uint8_t* b=static_cast<const uint8_t*>(api->guest_pointer(address));
  uint32_t end=length; while(end && !b[end-1]) --end;
  std::string hex;
  for(uint32_t i=0;i<end;++i) { char t[4]; std::snprintf(t,sizeof(t),"%02x",b[i]); hex+=t; }
  char head[120]; std::snprintf(head,sizeof(head),"%s @%08X (%u of %u bytes): ",label,address,end,length);
  ProbeLog((head+hex).c_str());
}
void DumpSlot(const char* who,uint32_t custom) {
  if(!Readable(custom,28)) return;
  const uint32_t slot=api->read_u32(custom+8);
  char l[80];
  std::snprintf(l,sizeof(l),"%s record",who); LogHex(l,custom,28);
  if(slot>=12) return;
  std::snprintf(l,sizeof(l),"%s A count",who); LogHex(l,kSlotTables+slot*4,4);
  std::snprintf(l,sizeof(l),"%s A block",who); LogHex(l,kSlotTables+48+slot*1080,1080);
  std::snprintf(l,sizeof(l),"%s B count",who); LogHex(l,kSlotTables+13056+slot*4,4);
  std::snprintf(l,sizeof(l),"%s B block",who); LogHex(l,kSlotTables+13104+slot*1080,1080);
  std::snprintf(l,sizeof(l),"%s C word",who); LogHex(l,0x8304CDA0+slot*4,4);
  std::snprintf(l,sizeof(l),"%s C 12",who); LogHex(l,0x8304CDA0+48+slot*12,12);
  std::snprintf(l,sizeof(l),"%s C 192",who); LogHex(l,0x8304CDA0+192+slot*4,4);
  std::snprintf(l,sizeof(l),"%s C 60",who); LogHex(l,0x8304CDA0+240+slot*60,60);
  std::snprintf(l,sizeof(l),"%s stream state",who); LogHex(l,0x830B1270+slot*656,656);
  const uint32_t morphs=api->read_u32(custom+4);
  std::snprintf(l,sizeof(l),"%s morphs",who); LogHex(l,morphs,384);
}

// Overlay key 5 (one game): a dressed player-object copy of you 3 m ahead.
// Press 5 again to return it to the pool.
uint32_t player_copy=0;

void TogglePlayerCopy(GuestScratch& scratch,uint32_t player) {
  if(player_copy) {
    DumpSlot("copy (later)",api->read_u32(player_copy+4484));
    DumpSlot("local",api->read_u32(player+4484));
    ReleasePlayerObject(scratch,player_copy);
    ProbeLog("Player copy returned to the pool");
    player_copy=0; return;
  }
  PlayerPose p;
  { std::lock_guard lock(pose_mutex); p=local_pose; }
  for(unsigned i=0;i<3;++i) p.pos[i]+=p.rows[6+i]*3.0f;
  const uint32_t pos=scratch.data()+1300, matrix=scratch.data()+1316;
  WritePose(p,pos,matrix);
  // Own body model if it's loaded (asked for here); otherwise the shared one
  // with this player's sliders (log "Own body" / "SHARED").
  player_copy=SpawnPlayerObject(scratch,player,pos,matrix,"Player copy");
  if(!player_copy) return;
  uint8_t worn[256];
  const unsigned length=ReadWorn(scratch,player,worn,sizeof(worn));
  char line[120];
  std::snprintf(line,sizeof(line),"Player copy: %u bytes of worn items %s",length,
                DressPlayerObject(scratch,player_copy,worn,length)?"applied":"not applied");
  ProbeLog(line);
  uint8_t extras[40];
  const unsigned extras_length=ReadExtras(scratch,player,extras);
  std::snprintf(line,sizeof(line),"Player copy: %u bytes of colours %s",extras_length,
                extras_length && ApplyExtras(scratch,player_copy,extras,extras_length)?"applied":"not applied");
  ProbeLog(line);
  ProbeLog(("Player copy body before: copy vs you: "+MorphDiff(player_copy,player)).c_str());
  float morphs[128];
  if((sync_body || body_test) && last_spawn_own_body && ReadMorphs(player,morphs)) {
    float before[128];
    ReadMorphs(player,before);
    ProbeLog(ApplyMorphs(scratch,player_copy,morphs) ? "Player copy: body shape applied" : "Player copy: body shape not applied");
    float after[128];
    unsigned changed=0;
    if(ReadMorphs(player,after)==128) for(unsigned i=0;i<128;++i) changed+=after[i]!=before[i];
    std::snprintf(line,sizeof(line),"Player copy body after: your own values changed: %u; copy vs you: %s",changed,MorphDiff(player_copy,player).c_str());
    ProbeLog(line);
  }
}

// Call tracing for reverse engineering (debug command "trace 1" / "trace 0"):
// logs arguments, caller and result of selected game functions.
std::atomic<bool> trace_on{false};
std::atomic<uint32_t> trace_filter{0}; // only calls whose r3 is this (0 = all)
std::atomic<uint32_t> trace_skip_self{0}; // drop repeats of the same call site
bool TraceWanted(uint32_t r3) { const uint32_t f=trace_filter.load(); return !f || r3==f; }
std::atomic<int> trace_budget{0};
template<uint32_t A> struct Trace {
  static inline WmlGuestFunction original=nullptr;
  static void Hook(WmlContext* c,uint8_t* b) {
    uint32_t a[5];
    for(int i=0;i<5;++i) a[i]=uint32_t(api->get_r(c,3+i));
    const uint32_t lr=api->get_lr(c);
    original(c,b);
    if(trace_on.load() && TraceWanted(a[0]) && trace_budget.fetch_sub(1)>0) {
      char line[160];
      std::snprintf(line,sizeof(line),"trace %08X(%08X %08X %08X %08X %08X) from %08X -> %08X",
                    A,a[0],a[1],a[2],a[3],a[4],lr,uint32_t(api->get_r(c,3)));
      ProbeLog(line);
    }
  }
  static void Install() { api->hook(A,Hook,&original); }
};
void InstallTraces() {
  Trace<0x8248D088>::Install(); Trace<0x8248B588>::Install(); Trace<0x8248A100>::Install();
  Trace<0x82312B10>::Install(); Trace<0x82312DF0>::Install(); Trace<0x82489518>::Install();
  Trace<0x8248B760>::Install(); Trace<0x8248B9F8>::Install(); Trace<0x8248AD88>::Install();
  Trace<0x82488780>::Install(); Trace<0x8248D198>::Install();
}

// Runtime trace slots: debug command "hook ADDR" attaches the next free slot
// to any game function (16 slots).
template<int N> struct TraceSlot {
  static inline uint32_t address=0;
  static inline WmlGuestFunction original=nullptr;
  static void Hook(WmlContext* c,uint8_t* b) {
    uint32_t a[5];
    for(int i=0;i<5;++i) a[i]=uint32_t(api->get_r(c,3+i));
    const uint32_t lr=api->get_lr(c);
    original(c,b);
    if(trace_on.load() && TraceWanted(a[0]) && trace_budget.fetch_sub(1)>0) {
      // Consecutive identical calls from the same site are logged once.
      static thread_local uint32_t last[7];
      const uint32_t now_call[7]={a[0],a[1],a[2],a[3],lr,0,0};
      if(!std::memcmp(last,now_call,sizeof(last))) { trace_budget.fetch_add(1); return; }
      std::memcpy(last,now_call,sizeof(last));
      char line[160];
      std::snprintf(line,sizeof(line),"trace %08X(%08X %08X %08X %08X %08X) from %08X -> %08X",
                    address,a[0],a[1],a[2],a[3],a[4],lr,uint32_t(api->get_r(c,3)));
      ProbeLog(line);
    }
  }
  static bool Attach(uint32_t a) {
    if(address) return false;
    address=a;
    return api->hook(a,Hook,&original)==0;
  }
};
template<int N> bool AttachSlot(uint32_t a) {
  if constexpr(N<16) { return TraceSlot<N>::Attach(a) || AttachSlot<N+1>(a); }
  else return false;
}

// Developer console through a file: debug_cmd.txt in the mod folder is read
// on the game thread (checked 4 times a second), executed line by line, then
// deleted. Output goes to the coop log. Numbers are hex; tokens $player,
// $copy, $lrec, $crec (customization records) stand for addresses.
//   spawn | release | dump A N | write32 A V | copy SRC DST N |
//   find32 A N V | call F [r3 r4 r5 r6 r7] | apply A N (look-up only)
uint32_t DebugValue(GuestScratch& scratch,const std::string& token,uint32_t player) {
  (void)scratch;
  if(token=="$player") return player;
  if(token=="$copy") return player_copy;
  if(token=="$lrec") return api->read_u32(player+4484);
  if(token=="$crec") return player_copy ? api->read_u32(player_copy+4484) : 0;
  if(token=="$data") return scratch.data();
  if(token=="$veh") return api->read_u32(player+2496);
  if(token=="$test") return test_local;
  return uint32_t(std::strtoul(token.c_str(),nullptr,16));
}

uint32_t debug_car=0;
void RunDebugCommands(GuestScratch& scratch,uint32_t player) {
  const std::string path=std::string(self->folder)+"\\debug_cmd.txt";
  std::ifstream file(path);
  if(!file) return;
  std::vector<std::string> lines;
  for(std::string line; std::getline(file,line);) if(!line.empty() && line[0]!='#') lines.push_back(line);
  file.close();
  DeleteFileA(path.c_str());
  for(const std::string& line:lines) {
    std::vector<std::string> t;
    { std::string word; for(char c:line) { if(c==' '||c=='\t'||c=='\r') { if(!word.empty()) t.push_back(word), word.clear(); } else word+=c; }
      if(!word.empty()) t.push_back(word); }
    if(t.empty()) continue;
    ProbeLog(("> "+line).c_str());
    auto v=[&](size_t i) { return i<t.size() ? DebugValue(scratch,t[i],player) : 0u; };
    char out[200];
    if(t[0]=="trace") { trace_on=v(1)!=0; trace_budget=v(2) ? int(v(2)) : 600; trace_filter=v(3); }
    else if(t[0]=="hook") ProbeLog(AttachSlot<0>(v(1)) ? "  hooked" : "  hook failed (no free slot?)");
    else if(t[0]=="spawn") { if(!player_copy) TogglePlayerCopy(scratch,player); }
    else if(t[0]=="release") { if(player_copy) TogglePlayerCopy(scratch,player); }
    else if(t[0]=="dump") LogHex("dump",v(1),std::min(v(2),4096u));
    else if(t[0]=="write32") { if(Readable(v(1),4)) api->write_u32(v(1),v(2)); }
    else if(t[0]=="copy") { const uint32_t n=std::min(v(3),65536u);
      if(Readable(v(1),n) && Readable(v(2),n)) std::memmove(api->guest_pointer(v(2)),api->guest_pointer(v(1)),n); }
    else if(t[0]=="find32") {
      const uint32_t a=v(1), n=std::min(v(2),0x100000u), want=v(3);
      unsigned hits=0;
      for(uint32_t o=0;o+4<=n && hits<64;o+=4) if(Readable(a+o,4) && api->read_u32(a+o)==want) {
        std::snprintf(out,sizeof(out),"  found at +%X (%08X)",o,a+o); ProbeLog(out); ++hits; }
      std::snprintf(out,sizeof(out),"  %u hits",hits); ProbeLog(out);
    }
    else if(t[0]=="call") {
      scratch.ctx.r3.u64=v(2); scratch.ctx.r4.u64=v(3); scratch.ctx.r5.u64=v(4);
      scratch.ctx.r6.u64=v(5); scratch.ctx.r7.u64=v(6);
      const bool ok=scratch.Call(v(1));
      std::snprintf(out,sizeof(out),"  call %08X %s, r3=%08X",v(1),ok?"ok":"failed",scratch.ctx.r3.u32); ProbeLog(out);
    }
    else if(t[0]=="car" || t[0]=="carmove") {
      static const float zero[3]{};
      float pos[3], rows[9];
      for(unsigned i=0;i<9;++i) rows[i]=api->read_f32(player+32+i*4);
      for(unsigned i=0;i<3;++i) pos[i]=api->read_f32(player+20+i*4)+rows[6+i]*6.0f;
      pos[1]+=0.5f; (void)zero;
      if(t[0]=="car") {
        debug_car=CreateVehicle(scratch,int32_t(v(1)),pos,rows);
        std::snprintf(out,sizeof(out),"  car type %u -> handle %08X object %08X",v(1),debug_car,ResolveVehicle(debug_car)); ProbeLog(out);
      } else if(const uint32_t veh=ResolveVehicle(debug_car)) {
        const uint32_t data=scratch.data(), a=data+1300, m=data+1316;
        for(unsigned i=0;i<3;++i) api->write_f32(a+i*4,pos[i]);
        for(unsigned i=0;i<9;++i) api->write_f32(m+i*4,rows[i]);
        scratch.ctx.r3.u64=veh; scratch.ctx.r4.u64=a; scratch.ctx.r5.u64=m; scratch.ctx.r6.u64=1;
        ProbeLog(scratch.Call(0x82535C68) ? "  moved" : "  move failed");
      }
    }
    else if(t[0]=="carinfo") {
      const uint32_t h=t.size()>1 ? v(1) : debug_car, veh=ResolveVehicle(h);
      std::snprintf(out,sizeof(out),"  car %08X obj %08X type %d pos %.1f %.1f %.1f f120 %08X sim %u vel %08X player veh %08X",h,veh,veh?VehicleType(veh):-1,
        veh?api->read_f32(veh+20):0,veh?api->read_f32(veh+24):0,veh?api->read_f32(veh+28):0,veh?api->read_u32(veh+120):0,
        veh?api->read_u8(veh+50300):0,veh?VehicleVelocityAddress(veh):0,api->read_u32(player+2496));
      ProbeLog(out);
    }
    else if(t[0]=="enter") {
      const uint32_t who=ResolveHuman(t.size()>2 ? v(2) : test_local);
      if(who) { OrderEnterVehicle(scratch,who,debug_car,v(1)); ProbeLog("  ordered"); } else ProbeLog("  no test actor (key 4)");
    }
    else if(t[0]=="carfree") { if(ResolveVehicle(debug_car)) { scratch.ctx.r3.u64=debug_car; scratch.Call(0x823ACF50); } debug_car=0; }
    else ProbeLog("  unknown command");
  }
}

// Overlay key 4: create (or remove) a test actor 3 m in front of the player,
// using the player's own model and appearance. Needs no second game.
void ToggleTestActor(GuestScratch& scratch) {
  if(ResolveHuman(test_local)) { DestroyActor(scratch,test_local,"test"); return; }
  PlayerPose p;
  { std::lock_guard lock(pose_mutex); p=local_pose; }
  for(unsigned i=0;i<3;++i) p.pos[i]+=p.rows[6+i]*3.0f;
  const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316, name=data+1400;
  WritePose(p,pos,matrix);
  bool own=true;
  uint32_t definition=LookupModel(scratch,p.model,name);
  if(!definition) { own=false; definition=LookupModel(scratch,kFallbackModel,name); }
  if(!definition) { ProbeLog("Test actor: no usable model"); return; }
  test_local=CreateActor(scratch,definition,pos,matrix,"Test");
  if(const uint32_t obj=ResolveHuman(test_local)) {
    if(own && p.look_length) {
      char line[100];
      std::snprintf(line,sizeof(line),"Test actor appearance %u bytes %s",p.look_length,
                    ApplyLook(scratch,test_local,p.look,p.look_length)?"applied":"not applied");
      ProbeLog(line);
    }
    WritePose(p,pos,matrix);
    MoveHuman(scratch,obj,pos,matrix,0);
  } else test_local=0;
}

// ---------------------------------------------------------------------------
// Shared traffic. While the two players are close, the host's game is in
// charge of the ambient traffic: it sends its traffic cars (model, pose,
// speed, driver model) ten times a second, and the client removes its own
// traffic, stops spawning more, and shows copies of the host's cars (with a
// copy of each driver) that follow the host's cars the way a shared car does.
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct TrafficCar {
  uint32_t id;          // handle in the host's game
  int16_t type;         // vehicle info index
  uint16_t driver;      // 1-based index into the driver model names, 0 none
  float pos[3], rows[9], vel[3], ang[3];
  uint8_t damage;       // bit 0 smoking, 1 on fire, 2 destroyed (vehicle +53976)
  uint8_t pad[3];
  uint16_t pass[3];     // passengers in seats 1-3: 1-based model name index, 0 none
  uint16_t pad2;
};
constexpr unsigned kTrafficPerPacket=12;  // 1120 bytes: fits an EOS P2P packet (1170)
struct TrafficPacket {
  char magic[4];        // WTR1
  uint32_t sequence, time;
  uint16_t count, part; // part 0 or 1 of this sequence
  TrafficCar cars[kTrafficPerPacket];
};
struct TrafficName { uint16_t index; char model[46]; };
struct TrafficNamesPacket {
  char magic[4];        // WTN1
  uint16_t count, pad;
  TrafficName names[16];
};
#pragma pack(pop)

constexpr uint32_t kTrafficList=0x8309A374;     // first node; node+0 next, node+8 handle
constexpr uint32_t kTrafficDensity=0x827AD060;  // set_traffic_density
constexpr unsigned kTrafficParts=9;             // WTR1 parts per update: up to 108 cars
constexpr unsigned kMaxShared=kTrafficParts*kTrafficPerPacket;

std::mutex traffic_mutex;
std::vector<TrafficCar> traffic_out;              // host: latest capture
uint32_t traffic_out_time=0, traffic_out_sequence=0;
std::vector<std::string> traffic_names;           // host: driver model names
struct TrafficIn { uint32_t sequence=0, time=0; uint64_t received=0; std::vector<TrafficCar> cars; };
TrafficIn traffic_in[kTrafficParts];              // client: latest parts
std::unordered_map<uint16_t,std::string> traffic_in_names;

uint32_t OccupantHandle(uint32_t vehicle,uint32_t seat) {
  const uint32_t v=api->read_u32(vehicle+52320+seat*108);
  if(ResolveHuman(v)) return v;
  if(Readable(v,4252) && api->read_u32(v+72)==1) return api->read_u32(v+68);
  return 0;
}

bool ModelName(uint32_t human,char* out,size_t size) {
  const uint32_t desc=api->read_u32(human+228);
  if(!Readable(desc,12)) return false;
  const uint32_t name=api->read_u32(desc+8);
  if(!Readable(name,46)) return false;
  const char* text=static_cast<const char*>(api->guest_pointer(name));
  size_t i=0;
  for(;i+1<size && text[i]>=32 && text[i]<=126;++i) out[i]=text[i];
  out[i]=0;
  return i>0;
}

// Host: the traffic cars nearest the other player.
// Who sits where: every person's car (+2496) and seat (+2500). The car's
// own seat slots (+52320) were empty for most traffic drivers, so none were
// sent and the other game showed empty cars.
std::unordered_map<uint32_t,std::array<uint32_t,4>> CarOccupants() {
  std::unordered_map<uint32_t,std::array<uint32_t,4>> m;
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t h=api->read_u32(kObjectTable+12+index*16);
    if(!Readable(h,4252) || api->read_u32(h+72)!=1 || (api->read_u32(h+68)&0xffff)!=index) continue;
    const uint32_t car=api->read_u32(h+2496);
    if(!car || !ResolveVehicle(car)) continue;
    const uint32_t seat=api->read_u32(h+2500);
    if(seat>=4) continue;
    auto& a=m[car];
    if(!a[seat]) a[seat]=h;
  }
  return m;
}
void CaptureTraffic(GuestScratch& scratch,uint32_t player,uint64_t now) {
  const auto occupants=CarOccupants();
  auto seated=[&](uint32_t v,unsigned seat)->uint32_t {
    auto it=occupants.find(api->read_u32(v+68));
    if(it!=occupants.end() && it->second[seat]) return it->second[seat];
    return ResolveHuman(OccupantHandle(v,seat));
  };
  static uint64_t next=0;
  if(now<next) return;
  next=now+100;
  struct Found { float d2; TrafficCar car; };
  std::vector<Found> found;
  const uint32_t own=api->read_u32(player+2496);
  const float rx=remote_x.load(), rz=remote_z.load();
  const uint32_t head=api->read_u32(kTrafficList);
  uint32_t node=head;
  // The list is circular: it ends when it comes back to the first node.
  for(unsigned guard=0;node && guard<64 && Readable(node,12);++guard) {
    const uint32_t handle=api->read_u32(node+8);
    node=api->read_u32(node);
    if(node==head) node=0;
    if(handle==own || handle==proxy_local) continue;
    if(const uint32_t av=ResolveHuman(avatar_local)) if(api->read_u32(av+2496)==handle) continue; // the other player's car now
    if(handle==remote_enter_car && MonoMs()<remote_enter_until) continue;  // the other player is getting in
    const uint32_t v=ResolveVehicle(handle);
    if(!v) continue;
    const int32_t type=VehicleType(v);
    if(type<0) continue;
    TrafficCar c{};
    c.id=OwnId(handle); c.type=int16_t(type); c.damage=CarDamage(v);
    for(unsigned i=0;i<3;++i) c.pos[i]=api->read_f32(v+20+i*4);
    for(unsigned i=0;i<9;++i) c.rows[i]=api->read_f32(v+32+i*4);
    if(const uint32_t vel=VehicleVelocityAddress(v))
      for(unsigned i=0;i<3;++i) { c.vel[i]=api->read_f32(vel+i*4); c.ang[i]=api->read_f32(vel+16+i*4); }
    const float dx=c.pos[0]-rx, dz=c.pos[2]-rz;
    const float d2=dx*dx+dz*dz;
    if(d2>250.0f*250.0f) continue;
    if(const uint32_t driver=seated(v,0)) {
      char name[46];
      if(ModelName(driver,name,sizeof(name))) {
        std::lock_guard lock(traffic_mutex);
        auto it=std::find(traffic_names.begin(),traffic_names.end(),name);
        if(it==traffic_names.end() && traffic_names.size()<4000) { traffic_names.push_back(name); it=traffic_names.end()-1; }
        if(it!=traffic_names.end()) c.driver=uint16_t(it-traffic_names.begin()+1);
      }
    }
    // Passengers too (only the driver was shown on the joiner's side).
    for(unsigned seat=1;seat<=3;++seat) if(const uint32_t who=seated(v,seat)) {
      char name[46];
      if(ModelName(who,name,sizeof(name))) {
        std::lock_guard lock(traffic_mutex);
        auto it=std::find(traffic_names.begin(),traffic_names.end(),name);
        if(it==traffic_names.end() && traffic_names.size()<4000) { traffic_names.push_back(name); it=traffic_names.end()-1; }
        if(it!=traffic_names.end()) c.pass[seat-1]=uint16_t(it-traffic_names.begin()+1);
      }
    }
    found.push_back({d2,c});
  }
  // Every other car near the joiner as well (parked ones aren't in the
  // traffic list: they were missing or a different car on the joiner's
  // side). Not during a mission: its cars come from each game's own copy of
  // the mission.
  if(!MissionActive()) {
    const uint32_t av=ResolveHuman(avatar_local);
    const uint32_t avatar_car=av ? api->read_u32(av+2496) : 0;
    for(uint32_t index=0;index<4096;++index) {
      const uint32_t v=api->read_u32(kObjectTable+12+index*16);
      if(!Readable(v,kVehicleSize) || api->read_u32(v+72)!=5 || (api->read_u32(v+68)&0xffff)!=index) continue;
      const uint32_t handle=api->read_u32(v+68);
      if(handle==own || handle==proxy_local || handle==avatar_car || (handle==remote_enter_car && MonoMs()<remote_enter_until)) continue;
      if(std::any_of(found.begin(),found.end(),[&](const Found& f){return f.car.id==handle;})) continue;
      const int32_t type=VehicleType(v);
      if(type<0) continue;
      TrafficCar c{};
      c.id=OwnId(handle); c.type=int16_t(type); c.damage=CarDamage(v);
      for(unsigned i=0;i<3;++i) c.pos[i]=api->read_f32(v+20+i*4);
      for(unsigned i=0;i<9;++i) c.rows[i]=api->read_f32(v+32+i*4);
      if(const uint32_t vel=VehicleVelocityAddress(v))
        for(unsigned i=0;i<3;++i) { c.vel[i]=api->read_f32(vel+i*4); c.ang[i]=api->read_f32(vel+16+i*4); }
      const float dx=c.pos[0]-rx, dz=c.pos[2]-rz, d2=dx*dx+dz*dz;
      if(d2>200.0f*200.0f) continue;
      found.push_back({d2*1.5f,c});  // traffic first when there are too many
    }
  }
  std::sort(found.begin(),found.end(),[](const Found& a,const Found& b){return a.d2<b.d2;});
  if(found.size()>kMaxShared) found.resize(kMaxShared);
  for(auto& f:found) if(const uint32_t v=ResolveVehicle(f.car.id)) ShareCarLook(scratch,v,now);
  {
    // Only the cars still sent keep their looks in the outgoing set.
    std::lock_guard lock(car_look_mutex);
    const uint32_t own_car=api->read_u32(player+2496);
    for(auto it=car_looks_out.begin();it!=car_looks_out.end();)
      it=(it->first==own_car || std::any_of(found.begin(),found.end(),[&](const Found& f){return f.car.id==it->first;})) ? std::next(it) : car_looks_out.erase(it);
  }
  std::lock_guard lock(traffic_mutex);
  traffic_out.clear();
  for(auto& f:found) traffic_out.push_back(f.car);
  traffic_out_time=uint32_t(now); ++traffic_out_sequence;
}

struct SharedCar {
  uint32_t car=0, driver=0;
  uint32_t pass[3]={};     // passenger copies (seats 1-3)
  uint64_t seat_since[4]={}; bool seat_standin[4]={};  // driver / passengers whose model hasn't loaded
  uint64_t next_pass=0;
  std::vector<uint8_t> applied_look;
  int16_t type=-1;
  uint16_t driver_index=0;
  uint64_t seen=0, next_place=0;
  float vel[3]{}, ang[3]{};
  Follower follow;
  ImpactWatch impact;
  uint8_t damage=0; uint64_t next_damage=0, made_at=0;
};
std::unordered_map<uint32_t,SharedCar> shared_cars; // client, game thread
bool traffic_shared=false;
float saved_traffic_density=-1;
unsigned car_looks_applied=0, drivers_removed=0, passengers_made=0, drivers_made=0, drivers_seat_refused=0, drivers_loading=0, cars_with_driver_in=0;
std::unordered_map<uint32_t,uint64_t> quick_gone; // joiner: host car id -> not made again until
std::vector<std::pair<uint32_t,uint64_t>> orphan_drivers; // joiner: pulled-out drivers to remove
unsigned shared_snaps=0, shared_made=0, shared_removed=0, own_removed=0, gone_count=0, stale_count=0, far_count=0;

extern int mod_destroying;
void DestroyObject(GuestScratch& scratch,uint32_t handle) {
  scratch.ctx.r3.u64=handle;
  ++mod_destroying; scratch.Call(0x823ACF50); --mod_destroying;
}
bool IsSharedCarHandle(uint32_t handle) {
  if(!handle) return false;
  if(handle==proxy_local) return true;
  for(auto& [id,sc]:shared_cars) if(sc.car==handle) return true;
  return false;
}

void RemoveSharedCar(GuestScratch& scratch,SharedCar& s,uint32_t player) {
  (void)player;
  if(s.driver && ResolveHuman(s.driver)) DestroyObject(scratch,s.driver);
  for(uint32_t& h:s.pass) { if(h && ResolveHuman(h)) DestroyObject(scratch,h); h=0; }
  if(ResolveVehicle(s.car) && api->read_u32(player+2496)!=s.car) DestroyObject(scratch,s.car);
  s.car=0; s.driver=0;
  ++shared_removed;
}

// Joiner: the host got into one of its traffic cars that is shown here as a
// copy: that copy becomes the host's car here (their driver is pulled out by
// our copy of the host, as in their game).
uint32_t SharedCarLocal(uint32_t host_id) {
  auto it=shared_cars.find(host_id);
  return it==shared_cars.end() ? 0 : ResolveVehicle(it->second.car);
}
uint32_t AdoptSharedCar(GuestScratch& scratch,uint32_t host_id,uint64_t now) {
  (void)scratch;
  auto it=shared_cars.find(host_id);
  if(it==shared_cars.end() || !ResolveVehicle(it->second.car)) return 0;
  if(ResolveVehicle(proxy_local) && proxy_local!=it->second.car) DestroyRemoteCar(scratch);
  proxy_local=it->second.car; proxy_peer=host_id; remote_car.enter_tries=0; remote_car.next_enter=0;
  if(it->second.driver) orphan_drivers.push_back({it->second.driver,now+4000});
  for(uint32_t h:it->second.pass) if(h) orphan_drivers.push_back({h,now+4000});
  shared_cars.erase(it);
  ProbeLog("Remote vehicle: the host took a car shown here; using that one");
  return ResolveVehicle(proxy_local);
}
uint32_t SharedCarHostHandle(uint32_t local) {
  if(!local) return 0;
  for(auto& [id,sc]:shared_cars) if(sc.car==local) return id;
  return 0;
}
int applying_car_damage=0;
void ApplyCarDamage(GuestScratch& scratch,uint32_t vehicle,uint8_t want) {
  if(!vehicle) return;
  const uint8_t have=CarDamage(vehicle);
  if((have&3)!=(want&3)) {
    scratch.ctx.r3.u64=api->read_u32(vehicle+68); scratch.ctx.r4.u64=(want&1)?1:0; scratch.ctx.r5.u64=(want&2)?1:0;
    ++applying_car_damage; scratch.Call(0x8253CD70); --applying_car_damage;
    ++car_damage_applied;
  }
  static std::unordered_map<uint32_t,uint64_t> blown;   // handle -> when (once per car)
  const uint32_t vh=api->read_u32(vehicle+68);
  if((want&4) && !(have&4) && !blown.count(vh)) {   // wrecked there: blow it up here too
    blown[vh]=MonoMs();
    if(blown.size()>256) blown.clear();
    const uint32_t sp=scratch.ctx.r1.u32, pos=scratch.data()+1700, zero=scratch.data()+1720;
    for(unsigned i=0;i<3;++i) { api->write_f32(pos+i*4,api->read_f32(vehicle+20+i*4)); api->write_f32(zero+i*4,0); }
    api->write_u32(sp+84,1); api->write_u32(sp+92,zero); api->write_u8(sp+103,0); api->write_u8(sp+119,0);
    scratch.ctx.r3.u64=vehicle; scratch.ctx.r4.u64=0; scratch.ctx.r5.u64=0; scratch.ctx.r6.u64=0;
    scratch.ctx.r7.u64=pos; scratch.ctx.r8.u64=0; scratch.ctx.r9.u64=0; scratch.ctx.r10.u64=0x7FFFFFFF;
    scratch.ctx.f1.f64=api->read_f32(0x820875F0); scratch.ctx.f2.f64=api->read_f32(0x820875EC);
    ++applying_car_damage; scratch.Call(0x825397E0); --applying_car_damage;
    ProbeLog("Car wrecked in the other game: blown up here too");
  }
}
// A car the other game drives (our copy of its traffic, the other player's
// car here, or one of ours the other player drives) is only wrecked when the
// other game says so: it blew up here from local knocks while it was fine
// there, and killed the player riding in it.
bool DrivenFromOtherGame(uint32_t vehicle) {
  const uint32_t handle=api->read_u32(vehicle+68);
  if(proxy_local && handle==proxy_local) return true;
  if(!hosting.load() && SharedCarHostHandle(handle)) return true;
  if(const uint32_t av=ResolveHuman(avatar_local)) if(api->read_u32(av+2496)==handle && api->read_u32(av+2500)==0) return true;
  return false;
}
WmlGuestFunction original_detonate=nullptr;
unsigned detonations_blocked=0;
void DetonateHook(WmlContext* c,uint8_t* b) {
  const uint32_t v=uint32_t(api->get_r(c,3));
  if(connected.load() && !applying_car_damage && Readable(v,kVehicleSize) && api->read_u32(v+72)==5 && DrivenFromOtherGame(v)) {
    if(detonations_blocked++<20) ProbeLog("Car explosion here skipped: the game that drives it decides");
    return;
  }
  original_detonate(c,b);
}
// Steers a car along a followed path: velocity and spin towards the path, a
// teleport only when far off or without physics.
void SteerAlongPath(GuestScratch& scratch,uint32_t vehicle,Follower& follow,const float* sent_vel,const float* sent_ang,uint64_t now,uint64_t& next_place,unsigned& snaps) {
  if(!follow.count) return;
  const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316;
  float path_pos[3], rows[9], vel[3];
  follow.Sample(uint32_t(int64_t(now)-follow.offset-30),path_pos,rows,vel);
  if(sent_vel && (sent_vel[0]||sent_vel[1]||sent_vel[2])) std::memcpy(vel,sent_vel,sizeof(vel));
  float e[3], dist2=0, align=0, cur[9];
  for(unsigned i=0;i<3;++i) { e[i]=path_pos[i]-api->read_f32(vehicle+20+i*4); dist2+=e[i]*e[i]; }
  for(unsigned i=0;i<9;++i) { cur[i]=api->read_f32(vehicle+32+i*4); align+=rows[i]*cur[i]; }
  const float speed=std::sqrt(vel[0]*vel[0]+vel[1]*vel[1]+vel[2]*vel[2]);
  const float snap=8.0f+speed*0.35f;
  const uint32_t v=VehicleVelocityAddress(vehicle);
  if(!v || dist2>snap*snap || align<2.0f) {
    (void)next_place; // without physics the car is placed on its path every update
    for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,path_pos[i]);
    api->write_f32(pos+12,0);
    for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,rows[i]);
    PlaceVehicle(scratch,vehicle,pos,matrix);
    if(const uint32_t v2=VehicleVelocityAddress(vehicle))
      for(unsigned i=0;i<3;++i) { api->write_f32(v2+i*4,vel[i]); api->write_f32(v2+16+i*4,sent_ang?sent_ang[i]:0.0f); }
    ++snaps;
    return;
  }
  float w[3]={0,0,0};
  for(unsigned a=0;a<3;++a) {
    const float* c=cur+a*3; const float* t=rows+a*3;
    w[0]+=0.5f*(c[1]*t[2]-c[2]*t[1]);
    w[1]+=0.5f*(c[2]*t[0]-c[0]*t[2]);
    w[2]+=0.5f*(c[0]*t[1]-c[1]*t[0]);
  }
  for(unsigned i=0;i<3;++i) {
    api->write_f32(v+i*4,vel[i]+std::clamp(e[i]*5.0f,-15.0f,15.0f));
    api->write_f32(v+16+i*4,(sent_ang?sent_ang[i]:0.0f)+std::clamp(w[i]*5.0f,-3.0f,3.0f));
  }
}

// Client: removes this game's own traffic the way the game trims traffic
// (82410F38 on the list node, as 82411E30 does). The list is circular.
void RemoveOwnTraffic(GuestScratch& scratch,uint32_t player) {
  const uint32_t own=api->read_u32(player+2496);
  for(unsigned removed=0;removed<32;++removed) {
    const uint32_t head=api->read_u32(kTrafficList);
    uint32_t node=head, target=0;
    for(unsigned guard=0;node && guard<64 && Readable(node,12);++guard) {
      const uint32_t handle=api->read_u32(node+8);
      bool ours=false;
      for(auto& [id,sc]:shared_cars) if(sc.car==handle) ours=true;
      if(handle && !ours && handle!=own && handle!=proxy_local) { target=node; break; }
      node=api->read_u32(node);
      if(node==head) break;
    }
    if(!target) return;
    scratch.ctx.r3.u64=target;
    if(!scratch.Call(0x82410F38)) return;
    ++own_removed;
    if(api->read_u32(kTrafficList)==head && api->read_u32(head+8)==api->read_u32(target+8) && target==head) return; // not removed
  }
}

// Joiner: this game's own parked cars near the player go (the host's are
// shown instead, the same models in the same spots). Not during a mission
// (its script may own them), never occupied ones or ones a player took.
unsigned own_parked_removed=0, car_type_mismatch=0;
void PinModel(GuestScratch& scratch,uint32_t id,uint64_t now);
void RemoveOwnParked(GuestScratch& scratch,uint32_t player) {
  const uint32_t own=api->read_u32(player+2496);
  const float px=local_x.load(), pz=local_z.load();
  std::vector<uint32_t> remove;
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t v=api->read_u32(kObjectTable+12+index*16);
    if(!Readable(v,kVehicleSize) || api->read_u32(v+72)!=5 || (api->read_u32(v+68)&0xffff)!=index) continue;
    const uint32_t handle=api->read_u32(v+68);
    if(handle==own || handle==proxy_local || taken_cars.count(handle)) continue;
    bool shared=false;
    for(auto& [id,sc]:shared_cars) if(sc.car==handle) { shared=true; break; }
    if(shared) continue;
    const float dx=api->read_f32(v+20)-px, dz=api->read_f32(v+28)-pz;
    if(dx*dx+dz*dz>160.0f*160.0f) continue;
    bool occupied=false;
    for(uint32_t seat=0;seat<4;++seat) if(OccupantHandle(v,seat)) { occupied=true; break; }
    if(occupied) continue;
    remove.push_back(handle);
  }
  for(uint32_t h:remove) { DestroyObject(scratch,h); ++own_parked_removed; }
}
uint32_t LoadedLookalike(GuestScratch& scratch,const char* want,uint32_t player);
unsigned seat_standins=0;
// A driver or passenger whose model doesn't load here in 2.5 s (this game
// only keeps a few people models at once) is swapped for a lookalike with a
// loaded model: they sat there invisible.
void StandInSeated(GuestScratch& scratch,uint32_t& handle,uint64_t& since,bool& standin,uint32_t v,unsigned seat,const std::string& model,uint32_t player,uint64_t now) {
  const uint32_t d=ResolveHuman(handle);
  if(!d || standin || (api->read_u32(d+216)&0x4000)) { since=0; return; }
  if(!since) { since=now; return; }
  if(now-since<2500 || now<actors_blocked_until) return;
  since=now;
  const uint32_t definition=LoadedLookalike(scratch,model.c_str(),player);
  if(!definition) return;
  const uint32_t pos=scratch.data()+1300, matrix=scratch.data()+1316;
  for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,api->read_f32(v+20+i*4));
  api->write_f32(pos+12,0);
  for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,api->read_f32(v+32+i*4));
  const uint32_t made=CreateActor(scratch,definition,pos,matrix,"Traffic stand-in",true);
  const uint32_t m=ResolveHuman(made);
  if(!m) return;
  DestroyObject(scratch,handle);
  if(!SeatInVehicle(scratch,m,v,seat)) { DestroyObject(scratch,made); handle=0; return; }
  handle=made; standin=true; ++seat_standins;
}
void UpdateSharedTraffic(GuestScratch& scratch,uint32_t player,uint64_t now) {
  const float dx=remote_x.load()-local_x.load(), dz=remote_z.load()-local_z.load();
  const float d2=dx*dx+dz*dz;
  const bool want=connected.load() && share_traffic && (traffic_shared ? d2<200.0f*200.0f : d2<120.0f*120.0f);
  if(!want) {
    if(traffic_shared) {
      for(auto& [id,s]:shared_cars) RemoveSharedCar(scratch,s,player);
      shared_cars.clear();
      if(saved_traffic_density>=0) api->write_f32(kTrafficDensity,saved_traffic_density);
      saved_traffic_density=-1; traffic_shared=false;
      ProbeLog("Shared traffic off: this game spawns its own traffic again");
    }
    return;
  }
  static uint64_t next_density=0, next_log=0;
  if(!traffic_shared) {
    traffic_shared=true;
    saved_traffic_density=api->read_f32(kTrafficDensity);
    ProbeLog("Shared traffic on: showing the host's traffic");
    next_density=0;
  }
  if(now>=next_density) {
    next_density=now+100;  // this game keeps spawning its own (density mods raise it again): remove them before they show
    const float d=api->read_f32(kTrafficDensity);
    if(d>0) { saved_traffic_density=d; api->write_f32(kTrafficDensity,0); }
    RemoveOwnTraffic(scratch,player);
    static uint64_t next_parked=0;
    if(now>=next_parked && !MissionActive() && !mirror_mission) { next_parked=now+500; RemoveOwnParked(scratch,player); }
  }
  // Latest host data.
  std::vector<TrafficCar> cars;
  uint32_t time=0;
  std::unordered_map<uint16_t,std::string> names;
  {
    std::lock_guard lock(traffic_mutex);
    uint32_t seq=traffic_in[0].sequence;
    for(auto& part:traffic_in) if(int32_t(part.sequence-seq)>0) seq=part.sequence;
    for(auto& part:traffic_in)
      if(part.sequence==seq && (now-part.received<1000 || HostPauseHold(now))) { cars.insert(cars.end(),part.cars.begin(),part.cars.end()); time=part.time; }
    names=traffic_in_names;
  }
  const uint32_t own=api->read_u32(player+2496);
  for(auto o=orphan_drivers.begin();o!=orphan_drivers.end();) {
    if(now<o->second) { ++o; continue; }
    // Still sitting (its exit waits for the pull-out move): checked again,
    // for up to 10 s (it stayed for good next to the host's own copy of the
    // same person).
    if(const uint32_t d=ResolveHuman(o->first)) {
      if(ResolveVehicle(api->read_u32(d+2496)) && now<o->second+10000) { ++o; continue; }
      DestroyObject(scratch,o->first);
    }
    o=orphan_drivers.erase(o);
  }
  unsigned made_this_update=0;   // drivers made this update (at most 2)
  for(const TrafficCar& c:cars) {
    auto it=shared_cars.find(c.id);
    if(it==shared_cars.end()) {
      // A car one of the players took, back in the host's traffic list once
      // they got out: keep the same car here (a new copy popped up before).
      uint32_t reuse=0;
      for(auto t=taken_cars.begin();t!=taken_cars.end();++t) if(t->second==c.id) {
        if(own==t->first) { reuse=~0u; break; }       // still ours
        reuse=t->first; taken_cars.erase(t); break;
      }
      if(reuse==~0u) continue;
      if(!reuse && proxy_peer==c.id && ResolveVehicle(proxy_local)) {
        const uint32_t av=ResolveHuman(avatar_local);
        if(av && api->read_u32(av+2496)==proxy_local) continue;  // the other player still sits in it
        if((remote_enter_car==proxy_local && now<remote_enter_until) || now<enter_hold_until) continue;  // their getting in is playing
        reuse=proxy_local; proxy_local=0; proxy_peer=0; remote_car.type=-1; remote_car.follow.Reset();
      }
      if(reuse && ResolveVehicle(reuse)) {
        SharedCar s; s.car=reuse; s.type=c.type; it=shared_cars.emplace(c.id,std::move(s)).first;
      }
    }
    if(it==shared_cars.end()) {
      const float ex=c.pos[0]-local_x.load(), ez=c.pos[2]-local_z.load();
      if(ex*ex+ez*ez>200.0f*200.0f || shared_cars.size()>=kMaxShared) continue;
      if(auto q=quick_gone.find(c.id); q!=quick_gone.end()) { if(now<q->second) continue; quick_gone.erase(q); }
      SharedCar s;
      s.car=CreateVehicle(scratch,c.type,c.pos,c.rows);
      if(!s.car) continue;
      s.made_at=now;
      if(const uint32_t v=ResolveVehicle(s.car)) {
        if(VehicleType(v)!=c.type) ++car_type_mismatch;
        // Ask for its model at the top priority (people's models waited
        // behind this game's own set; cars did the same).
        PinModel(scratch,api->read_u32(kVehicleInfo+uint32_t(c.type)*kVehicleInfoSize),now);
      }
      s.type=c.type;
      ++shared_made;
      it=shared_cars.emplace(c.id,std::move(s)).first;
    }
    SharedCar& s=it->second;
    s.seen=now; s.damage=c.damage;
    std::memcpy(s.vel,c.vel,sizeof(s.vel)); std::memcpy(s.ang,c.ang,sizeof(s.ang));
    s.follow.Add(time,c.pos,c.rows,now);
    // The host's car has no driver any more (they got out and walk on as a
    // shared person): remove ours, it kept sitting and "drove" on here.
    if(!c.driver && s.driver && ResolveHuman(s.driver) && own!=s.car) { DestroyObject(scratch,s.driver); s.driver=0; ++drivers_removed; }
    // Driver: a copy of the host's driver model, seated once the name is known.
    if(c.driver && !ResolveHuman(s.driver) && now>=s.next_pass && made_this_update<2) {
      auto n=names.find(c.driver);
      const uint32_t v=ResolveVehicle(s.car);
      if(n!=names.end() && v && own!=s.car) {
        ++made_this_update;
        const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316, name=data+1400;
        for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,c.pos[i]);
        api->write_f32(pos+12,0);
        for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,c.rows[i]);
        if(const uint32_t definition=LookupModel(scratch,n->second.c_str(),name)) {
          s.driver=CreateActor(scratch,definition,pos,matrix,"Traffic driver",true);
          if(const uint32_t d=ResolveHuman(s.driver)) {
            // Its model at the top priority (this game's own people models
            // were let go: drivers stayed invisible).
            const uint32_t desc=api->read_u32(d+228);
            if(Readable(desc,612)) if(const uint32_t mid=api->read_u32(desc+608)) PinModel(scratch,mid,now);
            ++drivers_made;
            // Refused: removed and not tried again for this car for 3 s (it
            // was made again every update, 86 in 10 s, and the game froze).
            if(!SeatInVehicle(scratch,d,v,0)) { ++drivers_seat_refused; DestroyObject(scratch,s.driver); s.driver=0; s.next_pass=now+3000; }
          }
        }
      }
      if(!ResolveHuman(s.driver)) s.driver=0;
    }
    if(c.driver && own!=s.car) if(const uint32_t v=ResolveVehicle(s.car)) {
      auto n=names.find(c.driver);
      if(n!=names.end()) StandInSeated(scratch,s.driver,s.seat_since[0],s.seat_standin[0],v,0,n->second,player,now);
    }
    if(!c.driver) s.seat_standin[0]=false;
    // Passengers: copies of the host's, seated the same way; removed when
    // the host's get out.
    if(own!=s.car) for(unsigned i=0;i<3;++i) {
      const uint32_t v=ResolveVehicle(s.car);
      if(!c.pass[i]) {
        if(s.pass[i] && ResolveHuman(s.pass[i])) { DestroyObject(scratch,s.pass[i]); ++drivers_removed; }
        s.pass[i]=0; continue;
      }
      if(ResolveHuman(s.pass[i])) {
        auto n=names.find(c.pass[i]);
        if(v && n!=names.end()) StandInSeated(scratch,s.pass[i],s.seat_since[i+1],s.seat_standin[i+1],v,i+1,n->second,player,now);
        continue;
      }
      s.seat_standin[i+1]=false;
      if(!v || now<s.next_pass) continue;
      s.pass[i]=0;
      if(OccupantHandle(v,i+1)) continue;           // seat taken here (a player)
      auto n=names.find(c.pass[i]);
      if(n==names.end()) continue;
      const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316, name=data+1400;
      for(unsigned k=0;k<3;++k) api->write_f32(pos+k*4,c.pos[k]);
      api->write_f32(pos+12,0);
      for(unsigned k=0;k<9;++k) api->write_f32(matrix+k*4,c.rows[k]);
      if(const uint32_t definition=LookupModel(scratch,n->second.c_str(),name)) {
        s.pass[i]=CreateActor(scratch,definition,pos,matrix,"Traffic passenger",true);
        if(const uint32_t d=ResolveHuman(s.pass[i])) {
          const uint32_t desc=api->read_u32(d+228);
          if(Readable(desc,612)) if(const uint32_t mid=api->read_u32(desc+608)) PinModel(scratch,mid,now);
          if(!SeatInVehicle(scratch,d,v,i+1)) { DestroyObject(scratch,s.pass[i]); s.pass[i]=0; s.next_pass=now+3000; }
          else ++passengers_made;
        } else s.pass[i]=0;
      }
    }
  }
  for(auto it=shared_cars.begin();it!=shared_cars.end();) {
    SharedCar& s=it->second;
    const uint32_t v=ResolveVehicle(s.car);
    if(v && own==s.car) { // the player took it: it's theirs now
      // The driver they pulled out is the host's to show (it walks on there
      // as a shared person): ours goes once it's out of the car.
      if(s.driver) orphan_drivers.push_back({s.driver,now+4000});
      for(uint32_t h:s.pass) if(h) orphan_drivers.push_back({h,now+4000});
      taken_cars[s.car]=it->first;
      it=shared_cars.erase(it); continue;
    }
    const float ex=v?api->read_f32(v+20)-local_x.load():0, ez=v?api->read_f32(v+28)-local_z.load():0;
    const bool stale=now-s.seen>1500 && !HostPauseHold(now);
    if(!v || stale || ex*ex+ez*ez>250.0f*250.0f) {
      if(!v) ++gone_count; else if(stale) ++stale_count; else ++far_count;
      // Removed by this game right after it was made (hundreds a minute in
      // the last logs): don't make that one again for a while.
      if(!v && now-s.made_at<3000) {
        quick_gone[it->first]=now+15000;
        static unsigned logged=0;
        if(logged++<15) { char line[120]; std::snprintf(line,sizeof(line),"Car copy (type %d) was removed by this game %u ms after it was made",s.type,unsigned(now-s.made_at)); ProbeLog(line); }
      }
      RemoveSharedCar(scratch,s,player); it=shared_cars.erase(it); continue;
    }
    {
      std::vector<uint8_t> look;
      { std::lock_guard lock(car_look_mutex); auto l=car_looks_in.find(it->first); if(l!=car_looks_in.end()) look=l->second; }
      if(!look.empty() && look!=s.applied_look && ApplyCarLook(scratch,v,look)) { s.applied_look=look; ++car_looks_applied; }
    }
    if(!WatchImpact(v,s.impact,it->first,now)) { SteerAlongPath(scratch,v,s.follow,s.vel,s.ang,now,s.next_place,shared_snaps); AfterSteer(v,s.impact); }
    if(now>=s.next_damage) { s.next_damage=now+250; ApplyCarDamage(scratch,v,s.damage); }
    ++it;
  }
  if(now>=next_log) {
    next_log=now+10000;
    drivers_loading=0;
    for(auto& [id,sc]:shared_cars) if(const uint32_t d=ResolveHuman(sc.driver)) if(!(api->read_u32(d+216)&0x4000)) ++drivers_loading;
    cars_with_driver_in=0;
    for(const TrafficCar& c:cars) if(c.driver) ++cars_with_driver_in;
    char line[260];
    std::snprintf(line,sizeof(line),"Shared traffic: %zu cars (host sent %zu), made %u, removed %u (gone %u, not sent %u, far %u), own removed %u (parked %u), placed/teleports %u, colours applied %u, passengers seated %u",
                  shared_cars.size(),cars.size(),shared_made,shared_removed,gone_count,stale_count,far_count,own_removed,own_parked_removed,shared_snaps,car_looks_applied,passengers_made);
    car_looks_applied=0; own_parked_removed=0; passengers_made=0;
    ProbeLog(line);
    std::snprintf(line,sizeof(line),"Traffic people: drivers made %u, seat refused %u, still loading %u, stand-ins %u; host cars with a driver %u",drivers_made,drivers_seat_refused,drivers_loading,seat_standins,cars_with_driver_in);
    seat_standins=0;
    ProbeLog(line);
    drivers_made=drivers_seat_refused=0;
    if(car_damage_applied || detonations_blocked) { std::snprintf(line,sizeof(line),"Car damage: %u smoke/fire changes applied, %u local explosions skipped",car_damage_applied,detonations_blocked); ProbeLog(line); car_damage_applied=0; }
    if(car_type_mismatch) { std::snprintf(line,sizeof(line),"Cars made with a different model than asked: %u",car_type_mismatch); ProbeLog(line); car_type_mismatch=0; }
    ProbeLog(line);
    shared_made=shared_removed=own_removed=shared_snaps=gone_count=stale_count=far_count=0;
  }
}

// ---------------------------------------------------------------- pedestrians
// Shared pedestrians work like shared traffic: the host sends the ambient
// people near the other player; the other game hides its own ambient people
// (script density 0, and removes the ones it has) and shows copies of the
// host's, walking along the host's paths.
#pragma pack(push, 1)
struct PedState {
  uint32_t id;          // handle in the host's game
  uint16_t model;       // 1-based index into the shared model names
  uint8_t flags;        // bit 0: right-hand orientation sign, 1: mission character, 2: dead, 3: crouching
  uint8_t anim;         // animation set (+952, as set_animation_state sets it); 0xFF none
  float pos[3];
  float yaw;
  uint16_t action;      // the action it is playing (+756; newspaper, tying shoes, cane walk), 0xFFFF none
  uint16_t spare;
};
constexpr unsigned kPedsPerPacket=32;  // 912 bytes: fits an EOS P2P packet (1170)
constexpr unsigned kPedParts=3;
constexpr unsigned kPedsMax=96;   // three packets (pad = part 0-2) with the same sequence
struct PedPacket {
  char magic[4];        // WPD1
  uint32_t sequence, time;
  uint16_t count, pad;
  PedState peds[kPedsPerPacket];
};
// A person's appearance (the variant block the game's own network code sends:
// clothing, colours, hair...). The model name alone gave copies a random look.
struct PedLookPacket {
  char magic[4];        // WPL1
  uint32_t id;
  uint8_t length;
  uint8_t look[127];
};
#pragma pack(pop)

constexpr uint32_t kAmbientPeds=0x8309ABB4;  // first ambient person; next at +3600 (circular)
constexpr uint32_t kPedDensity=0x827AD064;   // set_ped_density

std::vector<PedState> peds_out;                // host: latest capture
uint32_t peds_out_time=0, peds_out_sequence=0;
struct PedsIn { uint32_t sequence=0, time=0; uint64_t received=0; std::vector<PedState> peds; } peds_in;
std::unordered_map<uint32_t,std::vector<uint8_t>> ped_looks_out, ped_looks_in; // by host handle (traffic_mutex)

// People the ambient spawner owns (the same test as its counter 8240DB70):
// none of the mission / persona flags at +120.
// No hit points left (+1912, as get_current_hit_points reads them).
bool PersonDead(uint32_t obj) {
  return api->read_f32(obj+1912)<=0.0f;
}
bool AmbientPerson(uint32_t obj) {
  return (api->read_u32(obj+120)&0xD0000000u)==0;
}

uint16_t ModelIndex(uint32_t human) {
  char name[46];
  if(!ModelName(human,name,sizeof(name))) return 0;
  std::lock_guard lock(traffic_mutex);
  auto it=std::find(traffic_names.begin(),traffic_names.end(),name);
  if(it==traffic_names.end()) {
    if(traffic_names.size()>=4000) return 0;
    traffic_names.push_back(name); it=traffic_names.end()-1;
  }
  return uint16_t(it-traffic_names.begin()+1);
}

// Host: ambient people on foot near the other player.
// A person's animation set (walk, idle and stand styles: +952, an index
// into the game's list of 227 sets; set_animation_state 824DA2A0 writes it
// and applies it with 82440200(human, -1, -1, 0)). Copies made here picked
// their own, so the same person idled and walked differently on each side.
uint16_t CurrentPedAction(uint32_t obj) {
  const uint32_t a=api->read_u32(obj+756);
  return a<0xFFFF ? uint16_t(a) : 0xFFFF;
}
uint8_t AnimSet(uint32_t obj) {
  const uint32_t v=api->read_u32(obj+952);
  return v<227 ? uint8_t(v) : 0xFF;
}
void CapturePeds(uint32_t player,uint64_t now) {
  static uint64_t next=0;
  if(now<next) return;
  next=now+50;   // every update (the update runs 7-25 times a second)
  struct Found { float d2; PedState ped; };
  std::vector<Found> found;
  const float rx=remote_x.load(), rz=remote_z.load();
  const uint32_t avatar=ResolveHuman(avatar_local), replica=ResolveHuman(replica_local);
  const uint32_t head=api->read_u32(kAmbientPeds);
  uint32_t obj=head;
  std::vector<uint32_t> listed;  // everyone in the ambient list
  static unsigned ambient_far=0, ambient_named=0; static uint64_t next_far_log=0;
  if(now>=next_far_log) {
    next_far_log=now+10000;
    if(ambient_far || ambient_named) { char line[140]; std::snprintf(line,sizeof(line),"Pedestrians not sent: %u over 100 m from this player, %u story characters (in 10 s)",ambient_far,ambient_named); ProbeLog(line); }
    ambient_far=0; ambient_named=0;
  }
  for(unsigned guard=0;obj && guard<128 && Readable(obj,4252);++guard) {
    const uint32_t cur=obj;
    obj=api->read_u32(cur+3600);
    if(obj==head) obj=0;
    if(cur==player || cur==avatar || cur==replica || api->read_u32(cur+72)!=1) continue;
    listed.push_back(cur);
    // Everyone in the ambient list, whatever the +120 flags say: a pedestrian
    // that turns on a player (punched) gets flags there, stopped being sent
    // and vanished from the other screen.
    if(api->read_u32(cur+2496)) continue; // people in cars
    PedState p{};
    p.id=api->read_u32(cur+68);
    for(unsigned i=0;i<3;++i) p.pos[i]=api->read_f32(cur+20+i*4);
    const float fx=api->read_f32(cur+56), fz=api->read_f32(cur+64);
    p.yaw=std::atan2(fx,fz);
    const float r0x=api->read_f32(cur+32), r0z=api->read_f32(cur+40);
    if(r0x*fz-r0z*fx>=0) p.flags|=1;
    if(PersonDead(cur)) p.flags|=4;
    if(api->read_u32(cur+216)&0x20000000u) p.flags|=8;  // crouching
    p.anim=AnimSet(cur);
    p.action=CurrentPedAction(cur);
    const float dx=p.pos[0]-rx, dz=p.pos[2]-rz, d2=dx*dx+dz*dz;
    if(d2>90.0f*90.0f) continue;
    // Far from this player: people there aren't animated here (T-pose on the
    // joiner) and this player can't see them. After a mission the game turns
    // the leftover gang members into pedestrians; once the host was moved to
    // the next mission they were sent to the joiner still at the old spot.
    { const float hx=p.pos[0]-api->read_f32(player+20), hz=p.pos[2]-api->read_f32(player+28);
      if(hx*hx+hz*hz>100.0f*100.0f) { ++ambient_far; continue; } }
    // Named story characters (Johnny Gat, Dex, Julius, Troy: xx_A_/xx_B_/xx_W_)
    // are never sent as pedestrians: after the mission's end prompt the game
    // keeps them at the church stairs, unseen here, in this list, and the
    // joiner showed them.
    {
      char name[46];
      if(ModelName(cur,name,sizeof(name)) && std::strncmp(name,"PD_",3) && name[0] && name[1] && name[2]=='_' && name[3] && name[3]!='X') { ++ambient_named; continue; }
    }
    p.model=ModelIndex(cur);
    if(!p.model) continue;
    found.push_back({d2,p});
    uint8_t look[127];
    if(const unsigned n=ReadLook(cur,look,sizeof(look))) {
      std::lock_guard lock(traffic_mutex);
      ped_looks_out[p.id].assign(look,look+n);
    }
  }
  // During a mission, its characters (enemies, homies, targets) too: they
  // aren't in the ambient list, so look through all objects.
  static unsigned mission_people_seen=0, mission_people_hidden=0, mission_people_far=0;
  static uint64_t next_mission_log=0;
  if(share_missions && MissionActive()) {
    for(uint32_t index=0;index<4096;++index) {
      const uint32_t cur=api->read_u32(kObjectTable+12+index*16);
      if(!Readable(cur,4252) || api->read_u32(cur+72)!=1 || (api->read_u32(cur+68)&0xffff)!=index) continue;
      // Not the +120 flags: mission characters have 0x20000000 there only
      // during a cutscene, the same flags as ambient people after it.
      if(cur==player || cur==avatar || cur==replica) continue;
      if(std::find(listed.begin(),listed.end(),cur)!=listed.end()) continue;
      // Cutscene actors (0x20000000 at +120 only while one plays): the
      // joiner's game plays the same cutscene with its own.
      if(share_cutscenes && (api->read_u32(cur+120)&0x20000000u)) continue;
      // Hidden in this game (0x08000000): a finished mission's characters are
      // hidden here, not removed at once, and the next mission's are made
      // hidden ahead. Sent anyway, the joiner showed them (people only the
      // joiner saw after the mission-passed prompts, many T-posing).
      if(api->read_u32(cur+120)&0x08000000u) { ++mission_people_hidden; continue; }
      // Far from this player: after a mission is passed the host is moved to
      // the next one (~1.3 km) while the joiner still stands at the old spot
      // for ~10 s; the finished mission's people there (out of the host's
      // sight) were sent and shown only on the joiner.
      {
        const float hx=api->read_f32(cur+20)-api->read_f32(player+20), hz=api->read_f32(cur+28)-api->read_f32(player+28);
        if(hx*hx+hz*hz>100.0f*100.0f) { ++mission_people_far; continue; }
      }
      // +2496 is the vehicle for people in one; for mission characters it can
      // hold other values, which skipped all of them.
      if(ResolveVehicle(api->read_u32(cur+2496))) continue;
      ++mission_people_seen;
      PedState p{};
      p.id=api->read_u32(cur+68);
      for(unsigned i=0;i<3;++i) p.pos[i]=api->read_f32(cur+20+i*4);
      const float fx=api->read_f32(cur+56), fz=api->read_f32(cur+64);
      p.yaw=std::atan2(fx,fz);
      if(api->read_f32(cur+32)*fz-api->read_f32(cur+40)*fx>=0) p.flags|=1;
      p.flags|=2;  // mission character
      if(PersonDead(cur)) p.flags|=4;
      if(api->read_u32(cur+216)&0x20000000u) p.flags|=8;  // crouching
      p.anim=AnimSet(cur);
      p.action=CurrentPedAction(cur);
      const float dx=p.pos[0]-rx, dz=p.pos[2]-rz, d2=dx*dx+dz*dz;
      if(d2>90.0f*90.0f) continue;
      p.model=ModelIndex(cur);
      if(!p.model) continue;
      uint8_t look[127];
      if(const unsigned n=ReadLook(cur,look,sizeof(look))) {
        std::lock_guard lock(traffic_mutex);
        ped_looks_out[p.id].assign(look,look+n);
      }
      found.push_back({d2*0.25f,p});  // mission characters first
    }
  }
  if(now>=next_mission_log && MissionActive()) {
    next_mission_log=now+10000;
    char line[180];
    std::snprintf(line,sizeof(line),"Mission characters seen: %u in 10 s (not sent: %u hidden here, %u over 100 m from this player); sending %zu people",mission_people_seen,mission_people_hidden,mission_people_far,std::min<size_t>(found.size(),kPedsMax));
    ProbeLog(line);
    mission_people_seen=0; mission_people_hidden=0; mission_people_far=0;
  }
  std::sort(found.begin(),found.end(),[](const Found& a,const Found& b){return a.d2<b.d2;});
  if(found.size()>kPedsMax) found.resize(kPedsMax);
  std::lock_guard lock(traffic_mutex);
  peds_out.clear();
  for(auto& f:found) peds_out.push_back(f.ped);
  peds_out_time=uint32_t(now); ++peds_out_sequence;
  if(ped_looks_out.size()>256) { // forget people no longer sent
    for(auto it=ped_looks_out.begin();it!=ped_looks_out.end();)
      it=std::any_of(peds_out.begin(),peds_out.end(),[&](const PedState& p){return p.id==it->first;}) ? std::next(it) : ped_looks_out.erase(it);
  }
}

struct SharedPed {
  bool look_hidden=false; uint64_t ready_since=0;  // hidden until the host's look is on (a hat appeared, then went)
  uint32_t handle=0;
  uint16_t model=0;
  uint8_t flags=0;           // latest flags from the host
  bool dead=false;           // killed here to match the host
  bool adopted=false;        // this game's own mission character, following the host's (hidden, not removed, when dropped)
  uint64_t next_adopt=0;     // a copy still loading tries again to take over one of this game's own
  uint64_t loading_since=0;  // (diagnostics) when it was first seen not set up
  bool stuck_logged=false;
  uint32_t pending=0;        // the copy with the host's model, still loading, while a stand-in shows the person
  bool pending_adopted=false;
  bool standin=false;
  uint32_t effect_on=0;      // the copy (local handle) the host's in-game effect (attack arrow) was put on
  bool cut_hidden=false;     // hidden for a cutscene, shown again once the host sends it again after it
  uint64_t sent_at=0;        // last time the host sent this person
  bool rerequested=false;
  uint64_t next_standin=0;
  uint64_t pending_since=0;
  uint64_t next_crouch=0;
  bool look_applied=false;
  uint8_t anim=0xFF;         // the host's animation set for this person
  uint8_t anim_seen=0xFF, anim_applied=0xFF; uint64_t anim_since=0;
  uint64_t next_anim=0;
  uint64_t action_until=0;   // an action from the host plays; don't walk it around
  uint16_t host_action=0xFFFF;  // what the host's person is playing now
  uint32_t action_args[3]={0,1,0}; uint16_t action_args_for=0xFFFF;  // r5-r7 of the last action event
  uint64_t next_action_sync=0;
  unsigned action_misses=0;
  uint64_t seen=0;
  Follower follow;
};
std::unordered_map<uint32_t,SharedPed> shared_peds; // client, game thread
// Every frame, like the other player: each copy is put where its path says it
// is at that moment (small corrections only; the update hook keeps giving
// the walk/run orders and handles bigger moves).
void SmoothSharedPeds(uint64_t now) {
  for(auto& [id,sp]:shared_peds) {
    if(sp.dead || now<sp.action_until || !sp.follow.count || sp.follow.offset==INT64_MAX) continue;
    const uint32_t obj=ResolveHuman(sp.handle);
    if(!obj || !(api->read_u32(obj+216)&0x4000)) continue;
    float pos[3], rows[9], vel[3];
    sp.follow.Sample(uint32_t(int64_t(now)-sp.follow.offset-sp.follow.delay),pos,rows,vel);
    const float dx=pos[0]-api->read_f32(obj+20), dy=pos[1]-api->read_f32(obj+24), dz=pos[2]-api->read_f32(obj+28);
    if(dx*dx+dy*dy+dz*dz>4.0f) continue;
    for(unsigned i=0;i<3;++i) api->write_f32(obj+20+i*4,pos[i]);
    for(unsigned i=0;i<9;++i) api->write_f32(obj+32+i*4,rows[i]);
  }
}
uint32_t StandingOnSharedCharacter(uint32_t player) {
  if(!Readable(player,4252)) return 0;
  const float px=api->read_f32(player+20), py=api->read_f32(player+24), pz=api->read_f32(player+28);
  auto on=[&](uint32_t obj) {
    if(!obj || obj==player) return false;
    const float dx=api->read_f32(obj+20)-px, dz=api->read_f32(obj+28)-pz, dy=py-api->read_f32(obj+24);
    return dx*dx+dz*dz<1.3f*1.3f && dy>-0.5f && dy<2.6f;
  };
  if(const uint32_t avatar=ResolveHuman(avatar_local); on(avatar)) return avatar;
  for(auto& [id,sp]:shared_peds) { const uint32_t obj=ResolveHuman(sp.handle); if(on(obj)) return obj; }
  return 0;
}
// Step off the character under the player: 1.4 m away from it, on the ground
// next to it.
void UnstickPlayer(GuestScratch& scratch,uint32_t player) {
  const uint32_t under=unstick_from.exchange(0);
  if(!under || !Readable(under,4252) || !Readable(player,4252)) return;
  float dx=api->read_f32(player+20)-api->read_f32(under+20), dz=api->read_f32(player+28)-api->read_f32(under+28);
  float n=std::sqrt(dx*dx+dz*dz);
  if(n<0.05f) { dx=api->read_f32(under+32); dz=api->read_f32(under+40); n=std::sqrt(dx*dx+dz*dz); if(n<0.05f) { dx=1; dz=0; n=1; } }
  const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316;
  api->write_f32(pos,api->read_f32(under+20)+dx/n*1.4f);
  api->write_f32(pos+4,api->read_f32(under+24)+0.1f);
  api->write_f32(pos+8,api->read_f32(under+28)+dz/n*1.4f);
  api->write_f32(pos+12,0);
  for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,api->read_f32(player+32+i*4));
  const bool ok=MoveHuman(scratch,player,pos,matrix,0,true);
  static unsigned logged=0;
  if(logged++<20) ProbeLog(ok?"Player was standing on a shared character: stepped off (no fall)":"Player was standing on a shared character: step off refused");
}
bool OurCharacter(uint32_t handle) {
  if(!handle) return false;
  for(auto& [id,sp]:shared_peds) if(sp.handle==handle || sp.pending==handle) return true;
  for(auto& [id,sc]:shared_cars) { if(sc.driver==handle) return true; for(uint32_t ph:sc.pass) if(ph==handle) return true; }
  return false;
}
uint32_t SharedPedHostHandle(uint32_t local) {
  if(!local) return 0;
  for(auto& [id,sp]:shared_peds) if(sp.handle==local || sp.pending==local) return id;
  return 0;
}
uint32_t SharedPedLocalHandle(uint32_t host) {
  auto it=shared_peds.find(host);
  return it==shared_peds.end() ? 0 : it->second.handle;
}
// Kill a copy the way npc_kill does (824D4DA8).
void KillCopy(GuestScratch& scratch,uint32_t obj) {
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=0; scratch.ctx.r5.u64=0; scratch.ctx.r6.u64=0;
  scratch.ctx.r7.u64=0; scratch.ctx.r8.u64=0; scratch.ctx.r9.u64=0; scratch.ctx.r10.u64=0;
  scratch.ctx.f1.f64=api->read_f32(0x827AC214);
  api->write_u8(scratch.ctx.r1.u32+87,0); api->write_u8(scratch.ctx.r1.u32+95,0);
  ++replicated_depth; scratch.Call(0x824470D0); --replicated_depth;
}

// A character driven from the other game: its own AI must not fight, crouch
// in cover or run off (combat_disable 824C9CD0 sets +3692 bit 0x08;
// set_attack_enemies / _peds / _player_flag are +3697 bits 0x40 / 0x20 /
// 0x10). It showed as crouching for good, or shot and ran where the
// original just stood watching.
void MakePassive(uint32_t obj,bool targetable) {
  if(!Readable(obj,4252)) return;
  // The other player's character must stay a fighter the game's people react
  // to (with combat off, people it hit didn't know what to do and attackers
  // only went for the host); it just never starts attacks of its own.
  const uint8_t c=api->read_u8(obj+3692);
  if(targetable) { if(c&0x08) api->write_u8(obj+3692,uint8_t(c&~0x08)); }
  else if(!(c&0x08)) api->write_u8(obj+3692,uint8_t(c|0x08));
  const uint8_t a=api->read_u8(obj+3697);
  if(a&0x70) api->write_u8(obj+3697,uint8_t(a&~0x70));
}
// Crouch like the original: crouch_start (82461F58) / crouch_stop
// (82462108(obj, 0) then 82440200(obj, -1, -1, 1)), as the script
// functions do. +216 bit 0x20000000 is set while crouched.
void MatchCrouch(GuestScratch& scratch,uint32_t obj,bool want) {
  if(!Readable(obj,4252) || !(api->read_u32(obj+216)&0x4000)) return;
  const bool crouched=(api->read_u32(obj+216)&0x20000000u)!=0;
  if(crouched==want) return;
  ++replicated_depth;
  scratch.ctx.r3.u64=obj;
  if(want) scratch.Call(0x82461F58);
  else { scratch.ctx.r4.u64=0; scratch.Call(0x82462108); }
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=uint64_t(int64_t(-1)); scratch.ctx.r5.u64=uint64_t(int64_t(-1)); scratch.ctx.r6.u64=1;
  scratch.Call(0x82440200);
  --replicated_depth;
}
bool IsSentPed(uint32_t handle) {
  std::lock_guard lock(traffic_mutex);
  return std::any_of(peds_out.begin(),peds_out.end(),[&](const PedState& p){return p.id==handle;});
}

void ApplyPedAction(GuestScratch& scratch,const Event& e,uint64_t now) {
  auto it=shared_peds.find(e.a[4]);
  if(it==shared_peds.end()) return;
  const uint32_t obj=ResolveHuman(it->second.handle);
  if(!obj || !(api->read_u32(obj+216)&0x4000)) return;
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=e.a[0]; scratch.ctx.r5.u64=e.a[1];
  scratch.ctx.r6.u64=e.a[2]; scratch.ctx.r7.u64=e.a[3];
  ++replicated_depth;
  const bool played=scratch.Call(0x82444F88) && scratch.ctx.r3.u32;
  --replicated_depth;
  it->second.action_args_for=uint16_t(e.a[0]); it->second.action_args[0]=e.a[1]; it->second.action_args[1]=e.a[2]; it->second.action_args[2]=e.a[3];
  if(played) { ++ped_actions_played; it->second.action_until=now+8000; it->second.next_action_sync=now+2000; }
  static unsigned logged=0;
  if(logged++<60) {
    char line[120];
    std::snprintf(line,sizeof(line),"Person action %02X (%X %X %X): %s",e.a[0],e.a[1],e.a[2],e.a[3],played?"playing":"refused");
    ProbeLog(line);
  }
}
void ApplyPedCall(GuestScratch& scratch,const Event& e,uint64_t now) {
  if(hosting.load() || (e.a[0]!=0x824497F0u && e.a[0]!=0x82441308u)) return;
  auto it=shared_peds.find(e.a[1]);
  if(it==shared_peds.end() || it->second.dead) return;
  const uint32_t obj=ResolveHuman(it->second.handle);
  if(!obj || !(api->read_u32(obj+216)&0x4000)) return;
  const uint32_t player=api->read_u32(0x8309ABEC), avatar=avatar_local ? ResolveHuman(avatar_local) : 0;
  uint64_t r[4];
  for(int i=0;i<4;++i) {
    const uint32_t k=(e.a[6]>>(i*3))&7, v=e.a[2+i];
    r[i]=k==1 ? uint64_t(obj+v) : k==2 ? (avatar ? uint64_t(avatar+v) : 0) : k==3 ? uint64_t(player+v)
        : k==4 ? uint64_t(ResolveHuman(SharedPedLocalHandle(v))) : uint64_t(v);
  }
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=r[0]; scratch.ctx.r5.u64=r[1]; scratch.ctx.r6.u64=r[2]; scratch.ctx.r7.u64=r[3];
  ++applying_ped_call; ++replicated_depth;
  const bool ok=scratch.Call(e.a[0]);
  --replicated_depth; --applying_ped_call;
  if(ok) ++ped_calls_played;
  // Let the fall / reaction play instead of walking the copy along its path.
  it->second.action_until=std::max(it->second.action_until,now+(e.a[0]==0x824497F0u ? 3000 : 700));
  static unsigned logged=0;
  if(logged++<30) {
    char line[140];
    std::snprintf(line,sizeof(line),"Person %s from the host (%08X %08X %08X %08X): %s",e.a[0]==0x824497F0u?"knockdown":"hit reaction",
                  e.a[2],e.a[3],e.a[4],e.a[5],ok?"played":"failed");
    ProbeLog(line);
  }
}
void ApplyPedReact(GuestScratch& scratch,const Event& e,uint64_t now) {
  if(hosting.load()) return;
  auto it=shared_peds.find(e.a[0]);
  if(it==shared_peds.end() || it->second.dead) return;
  const uint32_t obj=ResolveHuman(it->second.handle);
  if(!obj || !(api->read_u32(obj+216)&0x4000)) return;
  const uint32_t player=api->read_u32(0x8309ABEC), avatar=avatar_local ? ResolveHuman(avatar_local) : 0;
  const uint32_t attacker=e.a[1]==2 ? avatar : e.a[1]==3 ? player : e.a[1]==4 ? ResolveHuman(SharedPedLocalHandle(e.a[2])) : 0;
  uint32_t hit=0;
  if(e.a[7]&(1u<<16)) { hit=scratch.data()+2200; for(int i=0;i<3;++i) api->write_u32(hit+i*4,e.a[3+i]); }
  else if(attacker) { hit=scratch.data()+2200; for(int i=0;i<3;++i) api->write_f32(hit+i*4,api->read_f32(attacker+20+i*4)); }
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=attacker; scratch.ctx.r5.u64=hit;
  scratch.ctx.r6.u64=e.a[6]; scratch.ctx.r7.u64=e.a[7]&0xFF; scratch.ctx.r8.u64=(e.a[7]>>8)&0xFF;
  ++applying_ped_call; ++replicated_depth;
  const bool ok=scratch.Call(0x82458EB8);
  --replicated_depth; --applying_ped_call;
  if(ok) ++ped_reacts_played;
  it->second.action_until=std::max(it->second.action_until,now+1200);
  static unsigned logged=0;
  if(logged++<40) {
    char line[140];
    std::snprintf(line,sizeof(line),"Person hit reaction (full) from the host, attacker kind %u%s: %s",e.a[1],attacker?"":" (not here)",ok?"played":"failed");
    ProbeLog(line);
  }
}
bool peds_shared=false;
float saved_ped_density=-1;
unsigned peds_gone=0, peds_unsent=0, peds_far=0, ped_actions_synced=0, peds_still=0;
bool sync_idle=false;  // mod.ini sync_idle: +756 isn't what the person visibly does (a newspaper showed here only)
unsigned peds_made=0, peds_removed=0, own_peds_removed=0, peds_failed=0, peds_looks=0, peds_anims=0;

void RemoveOwnPeds(GuestScratch& scratch,uint32_t player) {
  std::vector<uint32_t> ours;
  // Every character this mod made: the copy waiting for its model behind a
  // stand-in and the traffic drivers and passengers too (the waiting copies
  // were removed as this game's own, 40 in 10 s: people popped in and out).
  for(auto& [id,sp]:shared_peds) { ours.push_back(sp.handle); if(sp.pending) ours.push_back(sp.pending); }
  for(auto& [id,sc]:shared_cars) { if(sc.driver) ours.push_back(sc.driver); for(uint32_t ph:sc.pass) if(ph) ours.push_back(ph); }
  const uint32_t avatar=ResolveHuman(avatar_local), replica=ResolveHuman(replica_local);
  std::vector<uint32_t> remove;
  const uint32_t head=api->read_u32(kAmbientPeds);
  uint32_t obj=head;
  for(unsigned guard=0;obj && guard<128 && Readable(obj,4252);++guard) {
    const uint32_t cur=obj;
    obj=api->read_u32(cur+3600);
    if(obj==head) obj=0;
    if(cur==player || cur==avatar || cur==replica || api->read_u32(cur+72)!=1) continue;
    if(!AmbientPerson(cur) || api->read_u32(cur+2496)) continue;
    const uint32_t handle=api->read_u32(cur+68);
    if(std::find(ours.begin(),ours.end(),handle)!=ours.end()) continue;
    remove.push_back(handle);
  }
  for(uint32_t h:remove) { if(ResolveHuman(h)) { DestroyObject(scratch,h); ++own_peds_removed; } }
}

// A copy leaves: made here -> removed; this game's own mission character
// taken over -> hidden again (its mission script still knows it).
// One of this game's own characters that co-op doesn't need: a generic gang
// member or passer-by (TS_X_*, PD_*), not a named one (TS_W_* homies and
// story characters), not in a car, not a player, not a copy of the host's.
bool OwnDisposable(uint32_t obj,uint32_t player) {
  if(!obj || obj==player || obj==ResolveHuman(avatar_local)) return false;
  if(SharedPedHostHandle(api->read_u32(obj+68))) return false;
  if(ResolveVehicle(api->read_u32(obj+2496))) return false;
  char name[46];
  if(!ModelName(obj,name,sizeof(name))) return false;
  // PD_* passers-by and any generic xx_X_* (TS_X_ gang, LC_X_ other gangs,
  // LW_X_ police...); named ones are xx_A_/xx_B_/xx_W_ (Johnny Gat, Dex, Julius, Troy).
  return !std::strncmp(name,"PD_",3) || (name[0] && name[1] && name[2]=='_' && name[3]=='X' && name[4]=='_');
}
// Any of this game's own characters (named ones too), not a player, a copy
// or someone in a car.
bool OwnRemovable(uint32_t obj,uint32_t player) {
  if(!obj || obj==player || obj==ResolveHuman(avatar_local)) return false;
  if(SharedPedHostHandle(api->read_u32(obj+68))) return false;
  return !ResolveVehicle(api->read_u32(obj+2496));
}
void DropCopy(GuestScratch& scratch,SharedPed& sp) {
  if(sp.pending && ResolveHuman(sp.pending)) {
    if(sp.pending_adopted) SetCharacterHidden(scratch,ResolveHuman(sp.pending),true); else DestroyObject(scratch,sp.pending);
  }
  sp.pending=0;
  const uint32_t obj=ResolveHuman(sp.handle);
  if(!obj) return;
  if(sp.adopted) SetCharacterHidden(scratch,obj,true); else DestroyObject(scratch,sp.handle);
}
// While this game runs the host's mission it has its own mission characters,
// with their models loaded (copies of the host's often never finished loading
// here). Take over the nearest one with the same model instead of making a
// copy: it then follows the host's.
uint32_t AdoptOwnCharacter(GuestScratch& scratch,const char* model,const float* pos,uint32_t player) {
  std::vector<uint32_t> listed;
  { const uint32_t head=api->read_u32(kAmbientPeds); uint32_t o=head;
    for(unsigned g=0;o && g<128 && Readable(o,4252);++g) { listed.push_back(o); o=api->read_u32(o+3600); if(o==head) break; } }
  const uint32_t avatar=ResolveHuman(avatar_local);
  uint32_t best=0; float best_d2=80.0f*80.0f;  // (they move away from where they spawned)
  char name[46];
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t cur=api->read_u32(kObjectTable+12+index*16);
    if(!Readable(cur,4252) || api->read_u32(cur+72)!=1 || (api->read_u32(cur+68)&0xffff)!=index) continue;
    if(cur==player || cur==avatar || (api->read_u32(cur+120)&0xF0000000u)) continue;
    if(SharedPedHostHandle(api->read_u32(cur+68))) continue;
    if(std::find(listed.begin(),listed.end(),cur)!=listed.end()) continue;
    if(ResolveVehicle(api->read_u32(cur+2496))) continue;
    if(!ModelName(cur,name,sizeof(name)) || (model && std::strcmp(name,model))) continue;
    if(!model && !(api->read_u32(cur+216)&0x4000)) continue;  // a stand-in: only one that is set up
    const float dx=api->read_f32(cur+20)-pos[0], dz=api->read_f32(cur+28)-pos[2], d2=dx*dx+dz*dz;
    if(d2<best_d2) { best_d2=d2; best=cur; }
  }
  if(!best) return 0;
  SetCharacterHidden(scratch,best,false);
  return api->read_u32(best+68);
}
std::unordered_map<uint16_t,uint64_t> unloadable_models; // joiner: model index -> until when copies use a lookalike
unsigned copies_dropped=0;
unsigned peds_adopted=0, standins_made=0, standins_replaced=0, streams_rerequested=0, standins_kept=0;

// A model's streaming entry (group kStreamTable+(id>>24)*204: +48 list of
// entries, +52 free entries, linked at +28; entry +0 the loaded data, +4 id,
// +64 references of kind 5). 0: no entry (the request was dropped),
// 1: requested, not loaded, 2: loaded.
uint32_t FindStreamEntry(uint32_t id) {
  const uint32_t group=kStreamTable+(id>>24)*204;
  if(!Readable(group,204)) return 0;
  const uint32_t head=api->read_u32(group+48);
  unsigned guard=0;
  for(uint32_t e=head;e && Readable(e,72) && guard++<4096;) {
    if(api->read_u32(e+4)==id) return e;
    e=api->read_u32(e+28); if(e==head) break;
  }
  return 0;
}
// Streaming requests carry a priority level (8250C750 r6: counts per level
// at entry+20, entry+8 levels; the group queues entries by their highest
// level, +56). The people's copies asked at level 0 like everything else and
// several models never loaded while this game's own sets held the memory.
// A model a copy waits for gets one extra reference at the top level, held
// while copies need it.
struct ModelPin { int level=0; uint64_t used=0; };
bool pin_top=true;
bool keep_worn_models=true;  // mod.ini keep_worn_models: worn models keep their top-level request
std::unordered_map<uint32_t,ModelPin> model_pins; // model id -> pin (joiner, game thread)
unsigned pins_made=0, pins_released=0;
void PinModel(GuestScratch& scratch,uint32_t id,uint64_t now) {
  auto it=model_pins.find(id);
  if(it!=model_pins.end()) { it->second.used=now; return; }
  const uint32_t entry=FindStreamEntry(id);
  int levels=entry ? int(api->read_u32(entry+8)) : ((id>>24)==0 ? 14 : 0);  // not asked for yet: people models have 14 levels
  if(levels<=1 || levels>32) { model_pins[id]=ModelPin{0,now}; return; } // nothing higher to ask for
  // mod.ini pin_top: 1 = top level as before. 0 = the lowest (the joiner fit
  // 8 models against the host's 11 with the top level asked for).
  const int level=pin_top ? levels-1 : 1;
  scratch.ctx.r3.u64=kStreamTable+(id>>24)*204; scratch.ctx.r4.u64=id;
  scratch.ctx.r5.u64=5; scratch.ctx.r6.u64=uint64_t(level); scratch.ctx.r7.u64=1;
  scratch.Call(0x8250C750);
  model_pins[id]=ModelPin{level,now};
  ++pins_made;
}
void ReleasePins(GuestScratch& scratch,uint64_t now,bool all) {
  for(auto it=model_pins.begin();it!=model_pins.end();) {
    if(!all && now-it->second.used<4000) { ++it; continue; }
    if(it->second.level>0) {
      scratch.ctx.r3.u64=kStreamTable+(it->first>>24)*204; scratch.ctx.r4.u64=it->first;
      scratch.ctx.r5.u64=5; scratch.ctx.r6.u64=uint64_t(it->second.level); scratch.ctx.r7.u64=0;
      scratch.Call(0x8250CA48);
      ++pins_released;
    }
    it=model_pins.erase(it);
  }
}
// Once per session: what the people-models group holds (who keeps memory).
void DumpStreamGroup(uint32_t id) {
  static bool done=false;
  if(done) return;
  done=true;
  const uint32_t group=kStreamTable+(id>>24)*204;
  const uint32_t head=api->read_u32(group+48);
  unsigned guard=0;
  for(uint32_t e=head;e && Readable(e,72) && guard++<64;) {
    char line[200];
    std::snprintf(line,sizeof(line),"  stream entry %08X: %s, refs by kind %u %u %u %u %u %u %u, levels %u",api->read_u32(e+4),
                  api->read_u32(e)?"loaded":"waiting",api->read_u32(e+44),api->read_u32(e+48),api->read_u32(e+52),api->read_u32(e+56),
                  api->read_u32(e+60),api->read_u32(e+64),api->read_u32(e+68),api->read_u32(e+8));
    ProbeLog(line);
    e=api->read_u32(e+28); if(e==head) break;
  }
}
int StreamState(uint32_t id,char* info,size_t n) {
  const uint32_t group=kStreamTable+(id>>24)*204;
  if(!Readable(group,204)) { std::snprintf(info,n,"group unreadable"); return 0; }
  unsigned entries=0, loaded=0, free_entries=0; uint32_t found=0;
  const uint32_t head=api->read_u32(group+48);
  for(uint32_t e=head;e && Readable(e,72) && entries<4096;) {
    ++entries; if(api->read_u32(e)) ++loaded;
    if(api->read_u32(e+4)==id) found=e;
    e=api->read_u32(e+28); if(e==head) break;
  }
  const uint32_t fhead=api->read_u32(group+52);
  for(uint32_t e=fhead;e && Readable(e,72) && free_entries<4096;) { ++free_entries; e=api->read_u32(e+28); if(e==fhead) break; }
  const int state=!found ? 0 : api->read_u32(found) ? 2 : 1;
  std::snprintf(info,n,"id %08X %s; group %u: %u entries (%u loaded), %u free, flags %02X",id,
                state==0?"not requested":state==1?"requested, not loaded":"loaded",id>>24,entries,loaded,free_entries,api->read_u8(group+200));
  return state;
}
// A model already loaded here, as like the given one as possible (same kind
// of person: "_M_"/"_F_" and prefix), from the people set up here.
uint32_t LoadedLookalike(GuestScratch& scratch,const char* want,uint32_t player) {
  const bool female=std::strstr(want,"_F_")!=nullptr;
  const bool host_set=MonoMs()-host_models_time<3000 && !host_models.empty();
  char best[46]={}; int best_score=-1; char name[46];
  auto consider=[&](uint32_t obj) {
    if(!obj || obj==player || !(api->read_u32(obj+216)&0x4000) || !ModelName(obj,name,sizeof(name))) return;
    int score=0;
    if((std::strstr(name,"_F_")!=nullptr)!=female) return;  // never a man for a woman (a girl there was a fat man here)
    auto big=[](const char* n){ return std::strstr(n,"Fat")||std::strstr(n,"Big")||std::strstr(n,"Heavy"); };
    if(big(name)==big(want)) score+=3;
    if(!std::strncmp(name,want,3)) score+=6;        // same kind: gang member for a gang member (TS_), not an old man
    if(!std::strncmp(name,want,5)) score+=2;
    if(std::strncmp(want,"TS_",3) && !std::strncmp(name,"TS_",3)) score-=10;  // no gang models for passers-by
    if(!std::strncmp(name,"StyleTest",9)) score-=20;
    // A model the host's game has loaded now: a stand-in with any other keeps
    // that model in memory here (1.29: the joiner held two models the host
    // didn't have, for stand-ins, and 28 of 42 of the host's people waited
    // invisible for their models).
    if(host_set) {
      const uint32_t desc=api->read_u32(obj+228);
      const uint32_t id=Readable(desc,612) ? api->read_u32(desc+608) : 0;
      if(std::find(host_models.begin(),host_models.end(),id)!=host_models.end()) score+=8;
    }
    if(score>best_score) { best_score=score; std::memcpy(best,name,sizeof(best)); }
  };
  for(auto& [id,sp]:shared_peds) if(!sp.standin) consider(ResolveHuman(sp.handle));
  { const uint32_t head=api->read_u32(kAmbientPeds); uint32_t o=head;
    for(unsigned g=0;o && g<128 && Readable(o,4252);++g) { consider(o); o=api->read_u32(o+3600); if(o==head) break; } }
  if(best_score<0) return 0;
  return LookupModel(scratch,best,scratch.data()+1400);
}

// This game's own pedestrian spawner, off while the host's people are shown
// (spawning_pedestrians_do(false) 824DBF80: flag byte 0x8309A365, 0x82832740
// = -1, 82413398(0,0,0); back on with 82413558(1)). Density 0 alone didn't
// stop it: about 100 people a minute were made and removed.
bool own_spawning_off=false;
void SetOwnPedSpawning(GuestScratch& scratch,bool on) {
  // A mission can turn the spawner back on (1.32 log: 80 of this game's own
  // people made and removed every 10 s during a mission: people popping in
  // and out, and their models kept the host's from loading): off again.
  if(!on && own_spawning_off && api->read_u8(0x8309A365)) {
    own_spawning_off=false;
    static unsigned again=0;
    if(again++<10) ProbeLog("This game's pedestrian spawning was turned back on (by its mission): off again");
  }
  if(!on && !own_spawning_off) {
    if(!api->read_u8(0x8309A365)) return;   // already off (a mission or script did it)
    api->write_u8(0x8309A365,0);
    api->write_u32(0x82832740,0xFFFFFFFFu);
    scratch.ctx.r3.u64=0; scratch.ctx.r4.u64=0; scratch.ctx.r5.u64=0;
    scratch.Call(0x82413398);
    own_spawning_off=true;
    ProbeLog("This game's pedestrian spawning turned off while the host's people are shown");
  } else if(on && own_spawning_off) {
    scratch.ctx.r3.u64=1;
    scratch.Call(0x82413558);
    own_spawning_off=false;
    ProbeLog("This game's pedestrian spawning back on");
  }
}
// The people models' memory held this game's own ambient set (references of
// kind 2 from its population code) and its pending wishes, so the host's
// models waited forever ("requested, not loaded", levels 14, 12 loaded).
// While the host's people are shown, those references are dropped for
// models no copy uses (8250CA48(group, id, 2, 0, 0) once per reference).
unsigned own_model_refs_dropped=0;
// People models whose references outlive every character using them (copies
// removed before their model loaded kept their reference): the model stays
// in memory or keeps waiting for room, and the host's current people's models
// never load (9 of 12 loaded, 13 waiting, one model with 64 references).
// A model no character here uses any more has all its references cleared.
unsigned stale_models_cleared=0;
void ClearUnusedModelRefs(GuestScratch& scratch) {
  const uint32_t group=kStreamTable;
  if(!Readable(group,204) || (api->read_u8(group+200)&0x20)) return;
  std::unordered_map<uint32_t,unsigned> used;
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t h=api->read_u32(kObjectTable+12+index*16);
    if(!Readable(h,4252) || api->read_u32(h+72)!=1 || (api->read_u32(h+68)&0xffff)!=index) continue;
    const uint32_t desc=api->read_u32(h+228);
    if(Readable(desc,612)) ++used[api->read_u32(desc+608)];
  }
  if(used.empty()) return;   // table not readable: leave everything alone
  { const uint64_t now=MonoMs(); for(auto& [id,pin]:model_pins) if(now-pin.used<4000) ++used[id];  // copies about to be made
    if(now-host_models_time<3000) for(uint32_t id:host_models) ++used[id]; }  // the host's current set
  std::vector<uint32_t> clear;
  const uint32_t head=api->read_u32(group+48);
  unsigned guard=0;
  for(uint32_t e=head;e && Readable(e,72) && guard++<256;) {
    const uint32_t id=api->read_u32(e+4);
    unsigned refs=0; for(unsigned k=0;k<7;++k) refs+=api->read_u32(e+44+k*4);
    if(refs && (id>>24)==0 && !used.count(id)) clear.push_back(id);
    e=api->read_u32(e+28); if(e==head) break;
  }
  for(uint32_t id:clear) {
    model_pins.erase(id);
    scratch.ctx.r3.u64=group; scratch.ctx.r4.u64=id; scratch.ctx.r5.u64=5; scratch.ctx.r6.u64=0; scratch.ctx.r7.u64=1;
    scratch.Call(0x8250CA48);
    ++stale_models_cleared;
  }
}
// A model loaded here that the host's game doesn't have loaded is used by no
// person the host has now (lookalikes, drivers of cars gone there): while
// the host's people's models wait for room (8 loaded, 4-5 waiting, those
// people missing here), its users here are removed so it can go. One model
// every 2 s; never the players' own.
unsigned models_evicted=0;
// Memory slots of the people-models group (8250C3D0: +0 count, +8 array of
// 56-byte slots, +36 state, 4 = free): where the memory goes on each side
// (the host held 11 models, the joiner only 8 in the same memory).
void LogPeopleSlots(const char* tag) {
  const uint32_t group=kStreamTable;
  if(!Readable(group,204)) return;
  const uint32_t count=api->read_u32(group+0), slots=api->read_u32(group+8);
  if(count>64 || !Readable(slots,count*56)) return;
  std::string l=tag; char t[64];
  for(uint32_t i=0;i<count;++i) {
    const uint32_t sl=slots+i*56;
    std::snprintf(t,sizeof(t)," [%u:%X %X %X %X %X]",api->read_u32(sl+36),api->read_u32(sl+0),api->read_u32(sl+4),api->read_u32(sl+8),api->read_u32(sl+12),api->read_u32(sl+16));
    l+=t;
  }
  ProbeLog(l.c_str());
}
// Slots still holding a model nothing uses (+0 entry 0, state 0: kept as a
// cache). The game only reuses one for the same model, so 4 of the 12 slots
// sat on models no one had while the host's people's models waited (the
// joiner fit 8 against the host's 11). While something waits, such a model
// is dropped with the game's own unload (8250CD88: frees its memory too;
// marking the slot free by hand in 1.15/1.16 left the memory taken and the
// streaming thrashed: 5 fps).
unsigned slots_freed=0;
bool free_orphan_slots=true;  // mod.ini free_slots
void FreeOrphanSlots(GuestScratch& scratch) {
  const uint32_t group=kStreamTable;
  if(!Readable(group,204) || (api->read_u8(group+200)&0x20)) return;
  const uint32_t count=api->read_u32(group+0), slots=api->read_u32(group+8);
  if(count>64 || !Readable(slots,count*56)) return;
  unsigned waiting=0, guard=0;
  const uint32_t head=api->read_u32(group+48);
  for(uint32_t e=head;e && Readable(e,72) && guard++<256;) { if(!api->read_u32(e)) ++waiting; e=api->read_u32(e+28); if(e==head) break; }
  if(!waiting) return;
  for(uint32_t i=0;i<count;++i) {
    const uint32_t sl=slots+i*56, id=api->read_u32(sl+4);
    if(api->read_u32(sl+36)!=0 || api->read_u32(sl+0)!=0 || id==0xFFFFFFFFu) continue;
    if(FindStreamEntry(id)) continue;   // asked for again: the game reuses it
    scratch.ctx.r3.u64=group; scratch.ctx.r4.u64=id;
    scratch.Call(0x8250CD88);
    ++slots_freed;
    return;   // one per second
  }
}
void EvictNonHostModels(GuestScratch& scratch,uint64_t now) {
  if(now-host_models_time>3000 || host_models.empty()) return;
  const uint32_t group=kStreamTable;
  if(!Readable(group,204) || (api->read_u8(group+200)&0x20)) return;
  std::vector<uint32_t> candidates; unsigned waiting=0, guard=0;
  const uint32_t head=api->read_u32(group+48);
  for(uint32_t e=head;e && Readable(e,72) && guard++<256;) {
    const uint32_t id=api->read_u32(e+4);
    if(!api->read_u32(e)) ++waiting;
    else if(std::find(host_models.begin(),host_models.end(),id)==host_models.end()) candidates.push_back(id);
    e=api->read_u32(e+28); if(e==head) break;
  }
  if(!waiting || candidates.empty()) return;
  // Models the host's people really wear here (copies with the host's model,
  // not stand-ins, and copies still loading it) are the host's even when its
  // list (at most 14, loaded ones only) leaves them out: dropping those copies
  // made them again at once - people going invisible and back, over and over.
  {
    std::vector<uint32_t> real;
    auto model_of=[](uint32_t handle)->uint32_t { const uint32_t o=ResolveHuman(handle); if(!o) return 0; const uint32_t d=api->read_u32(o+228); return Readable(d,612) ? api->read_u32(d+608) : 0; };
    for(auto& [hid,sp]:shared_peds) {
      if(sp.handle && !sp.standin) if(const uint32_t m=model_of(sp.handle)) real.push_back(m);
      if(sp.pending) if(const uint32_t m=model_of(sp.pending)) real.push_back(m);
    }
    const size_t before=candidates.size();
    candidates.erase(std::remove_if(candidates.begin(),candidates.end(),[&](uint32_t id){return std::find(real.begin(),real.end(),id)!=real.end();}),candidates.end());
    static unsigned logged=0;
    if(before!=candidates.size() && logged++<10) { char line[140]; std::snprintf(line,sizeof(line),"Model memory: %zu models the host's people wear here kept (not in the host's list)",before-candidates.size()); ProbeLog(line); }
    if(candidates.empty()) return;
  }
  const uint32_t player=api->read_u32(0x8309ABEC), avatar=avatar_local ? ResolveHuman(avatar_local) : 0;
  for(uint32_t id:candidates) {
    std::vector<uint32_t> users; bool blocked=false;
    for(uint32_t index=0;index<4096 && !blocked;++index) {
      const uint32_t h=api->read_u32(kObjectTable+12+index*16);
      if(!Readable(h,4252) || api->read_u32(h+72)!=1 || (api->read_u32(h+68)&0xffff)!=index) continue;
      const uint32_t desc=api->read_u32(h+228);
      if(!Readable(desc,612) || api->read_u32(desc+608)!=id) continue;
      if(h==player || h==avatar || api->read_u32(h+2496)) blocked=true;  // not people in cars (safer)
      else if(!OurCharacter(api->read_u32(h+68))) {
        // This game's own character (a hidden mission character, one taken
        // over and let go, a cutscene extra): removed too, outside cutscenes
        // (1.36: three models only the joiner's own characters used kept the
        // host's out; 32 of 44 people invisible in the fight).
        if(!CutscenePlaying() && OwnDisposable(h,player)) {
          static unsigned logged=0;
          if(logged++<20) { char mn[46]="?"; ModelName(h,mn,sizeof(mn)); char line[140]; std::snprintf(line,sizeof(line),"Model memory: this game's own %s (flags %08X) removed for the host's models",mn,api->read_u32(h+120)); ProbeLog(line); }
          DestroyObject(scratch,api->read_u32(h+68));
        } else {
          static unsigned logged=0;
          if(logged++<20) { char mn[46]="?"; ModelName(h,mn,sizeof(mn)); char line[140]; std::snprintf(line,sizeof(line),"Model memory: model %X kept by this game's own %s (flags %08X)",id,mn,api->read_u32(h+120)); ProbeLog(line); }
          blocked=true;
        }
      }
      else users.push_back(api->read_u32(h+68));
    }
    if(blocked) continue;
    for(uint32_t u:users) {
      bool done=false;
      for(auto it=shared_peds.begin();it!=shared_peds.end() && !done;++it) {
        SharedPed& sp=it->second;
        if(sp.pending==u) { DestroyObject(scratch,u); sp.pending=0; done=true; }
        else if(sp.handle==u && !sp.adopted) { DropCopy(scratch,sp); shared_peds.erase(it); done=true; break; }
      }
      if(done) continue;
      for(auto& [cid,sc]:shared_cars) {
        if(sc.driver==u) { DestroyObject(scratch,u); sc.driver=0; sc.next_pass=now+5000; break; }
        bool hit=false; for(uint32_t& ph:sc.pass) if(ph==u) { DestroyObject(scratch,u); ph=0; hit=true; }
        if(hit) break;
      }
    }
    model_pins.erase(id);
    ++models_evicted;
    return;   // one per call
  }
}
void DropOwnModelRefs(GuestScratch& scratch,uint32_t group_index) {
  const uint32_t group=kStreamTable+group_index*204;
  if(!Readable(group,204)) return;
  std::vector<std::pair<uint32_t,uint32_t>> drop;
  const uint32_t head=api->read_u32(group+48);
  unsigned guard=0;
  for(uint32_t e=head;e && Readable(e,72) && guard++<512;) {
    const uint32_t kind2=api->read_u32(e+52), kind5=api->read_u32(e+64);
    if(kind2 && kind2<64 && !kind5) drop.push_back({api->read_u32(e+4),kind2});
    e=api->read_u32(e+28); if(e==head) break;
  }
  for(auto [id,count]:drop)
    for(uint32_t i=0;i<count;++i) {
      scratch.ctx.r3.u64=group; scratch.ctx.r4.u64=id; scratch.ctx.r5.u64=2; scratch.ctx.r6.u64=0; scratch.ctx.r7.u64=0;
      scratch.Call(0x8250CA48);
      ++own_model_refs_dropped;
    }
}
uint64_t peds_cut_end=0;
void UpdatePedEffects(GuestScratch& scratch,uint64_t now) {
  static uint64_t next=0, next_log=0;
  if(now<next) return;
  next=now+250;
  for(auto& [id,sp]:shared_peds) {
    auto je=joiner_effects.find(id);
    const bool want=je!=joiner_effects.end() && now-je->second.seen<2500 && !CutscenePlaying();
    const uint32_t obj=ResolveHuman(sp.handle);
    if(sp.effect_on && sp.effect_on!=sp.handle) {   // copy swapped (stand-in <-> real): off the old one
      if(ResolveHuman(sp.effect_on)) { ++applying_effect; scratch.ctx.r3.u64=sp.effect_on; scratch.ctx.r4.u64=0; scratch.Call(0x82403F58); --applying_effect; }
      sp.effect_on=0;
    }
    if(want && !sp.effect_on && obj && (api->read_u32(obj+216)&0x4000) && !sp.look_hidden) {
      ++applying_effect;
      scratch.ctx.r3.u64=je->second.kind; scratch.ctx.r4.u64=sp.handle; scratch.ctx.r5.u64=je->second.r5;
      scratch.ctx.r6.u64=je->second.effect; scratch.ctx.r7.u64=je->second.data;
      scratch.Call(0x82403C38);
      --applying_effect;
      sp.effect_on=sp.handle; ++effects_added;
    } else if(!want && sp.effect_on) {
      if(obj) { ++applying_effect; scratch.ctx.r3.u64=sp.effect_on; scratch.ctx.r4.u64=0; scratch.Call(0x82403F58); --applying_effect; }
      sp.effect_on=0; ++effects_removed;
    }
  }
  for(auto it=joiner_effects.begin();it!=joiner_effects.end();) it=now-it->second.seen>10000 ? joiner_effects.erase(it) : std::next(it);
  if(now>=next_log && (effects_added || effects_removed)) {
    next_log=now+10000;
    char line[120]; std::snprintf(line,sizeof(line),"Attack arrows (in-game effects) from the host: %u put on copies, %u taken off, %zu listed now",effects_added,effects_removed,joiner_effects.size());
    ProbeLog(line); effects_added=0; effects_removed=0;
  }
}
void UpdateSharedPeds(GuestScratch& scratch,uint32_t player,uint64_t now) {
  const float dx=remote_x.load()-local_x.load(), dz=remote_z.load()-local_z.load();
  const float d2=dx*dx+dz*dz;
  // Not during a cutscene here: it brings its own characters (the copies
  // doubled them).
  const bool in_range=connected.load() && running.load() && share_peds &&
                      (peds_shared ? d2<200.0f*200.0f : d2<120.0f*120.0f);
  const bool want=in_range && !CutscenePlaying();
  // During a cutscene the copies are hidden, not removed (1.28 and earlier
  // removed them all and this game's own people spawned for the length of
  // the cutscene: people flashing in, then everyone made again afterwards -
  // stand-ins, wrong models, T-poses). Shown again when it ends.
  static bool hidden_for_cutscene=false;
  if(in_range && peds_shared && CutscenePlaying()) {
    if(!hidden_for_cutscene) {
      for(auto& [id,sp]:shared_peds) if(const uint32_t o=ResolveHuman(sp.handle)) SetCharacterHidden(scratch,o,true);
      hidden_for_cutscene=true;
      ProbeLog("Cutscene: the host's people hidden here until it ends (kept, not removed)");
    }
    return;
  }
  if(hidden_for_cutscene) {
    hidden_for_cutscene=false;
    unsigned shown=0;
    // Not shown at once: only those the host sends again (1.35: the enemies
    // of a finished mission all came back after its last cutscene until
    // they were removed as no longer sent).
    for(auto& [id,sp]:shared_peds) if(ResolveHuman(sp.handle)) { sp.cut_hidden=true; ++shown; sp.follow.Reset(); }
    peds_cut_end=now;
    char line[120]; std::snprintf(line,sizeof(line),"Cutscene over: %u of the host's people shown again when the host sends them",shown); ProbeLog(line);
  }
  if(!want) {
    if(peds_shared) {
      for(auto& [id,sp]:shared_peds) if(ResolveHuman(sp.handle)) { DropCopy(scratch,sp); ++peds_removed; }
      shared_peds.clear();
      ReleasePins(scratch,now,true);
      if(saved_ped_density>=0) api->write_f32(kPedDensity,saved_ped_density);
      SetOwnPedSpawning(scratch,true);
      saved_ped_density=-1; peds_shared=false;
      ProbeLog("Shared pedestrians off: this game spawns its own people again");
    }
    return;
  }
  static uint64_t next_density=0, next_log=0;
  if(!peds_shared) {
    peds_shared=true;
    saved_ped_density=api->read_f32(kPedDensity);
    ProbeLog("Shared pedestrians on: showing the host's people");
    next_density=0;
  }
  if(now>=next_density) {
    next_density=now+100;  // this game keeps spawning its own (density mods raise it again): remove them before they show
    const float d=api->read_f32(kPedDensity);
    if(d>0) { saved_ped_density=d; api->write_f32(kPedDensity,0); }
    SetOwnPedSpawning(scratch,false);
    RemoveOwnPeds(scratch,player);
  }
  std::vector<PedState> peds;
  uint32_t time=0;
  static unsigned copies_ready=0, copies_waiting=0, copies_near=0, copies_killed=0, copies_standin=0;
  copies_ready=copies_waiting=copies_near=copies_standin=0;
  std::unordered_map<uint16_t,std::string> names;
  {
    std::lock_guard lock(traffic_mutex);
    if(now-peds_in.received<1000 || HostPauseHold(now)) { peds=peds_in.peds; time=peds_in.time; }
    names=traffic_in_names;
  }
  // New people: at most 3 per update, nearest first (the host sends them
  // sorted), so creating them doesn't stall a frame.
  unsigned created=0;
  for(const PedState& p:peds) {
    float rows[9];
    const float fx=std::sin(p.yaw), fz=std::cos(p.yaw), sign=(p.flags&1)?1.0f:-1.0f;
    rows[0]=sign*fz; rows[1]=0; rows[2]=-sign*fx;
    rows[3]=0; rows[4]=1; rows[5]=0;
    rows[6]=fx; rows[7]=0; rows[8]=fz;
    auto it=shared_peds.find(p.id);
    if(it==shared_peds.end() || !ResolveHuman(it->second.handle)) {
      if(created>=8 || now<actors_blocked_until) continue;
      const float ex=p.pos[0]-local_x.load(), ez=p.pos[2]-local_z.load();
      if(ex*ex+ez*ez>100.0f*100.0f) continue;
      auto n=names.find(p.model);
      if(n==names.end()) continue;
      const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316, name=data+1400;
      for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,p.pos[i]);
      api->write_f32(pos+12,0);
      for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,rows[i]);
      uint32_t handle=0;
      bool adopted=false;
      // A driver pulled out here (our copy of the host's traffic driver) is
      // the same person the host now shares on foot: that copy carries on
      // (it was removed and a new one appeared standing, skipping the fall
      // and getting up).
      for(auto o=orphan_drivers.begin();o!=orphan_drivers.end();++o) {
        const uint32_t d=ResolveHuman(o->first);
        if(!d || ResolveVehicle(api->read_u32(d+2496))) continue;
        const float ox=api->read_f32(d+20)-p.pos[0], oz=api->read_f32(d+28)-p.pos[2];
        char mn[46];
        if(ox*ox+oz*oz<5.0f*5.0f && ModelName(d,mn,sizeof(mn)) && n->second==mn) {
          handle=o->first; orphan_drivers.erase(o);
          ProbeLog("Pulled-out driver copy carries on as the host's person (no second copy)");
          break;
        }
      }
      if(!handle && mirror_mission && (p.flags&2)) {
        handle=AdoptOwnCharacter(scratch,n->second.c_str(),p.pos,player);
        // This model doesn't load here: take over one of this game's own
        // mission characters of any model instead.
        if(!handle) { auto u=unloadable_models.find(p.model); if(u!=unloadable_models.end() && now<u->second) handle=AdoptOwnCharacter(scratch,nullptr,p.pos,player); }
        adopted=handle!=0;
        if(adopted) ++peds_adopted;
      }
      bool lookalike=false;
      if(!handle) {
        uint32_t definition=0;
        auto u=unloadable_models.find(p.model);
        if(u!=unloadable_models.end() && now<u->second) {
          definition=LoadedLookalike(scratch,n->second.c_str(),player);
          lookalike=definition!=0;
        }
        if(!definition) definition=LookupModel(scratch,n->second.c_str(),name);
        if(!definition) { ++peds_failed; continue; }
        handle=CreateActor(scratch,definition,pos,matrix,"Shared person",true);
      }
      ++created;
      if(!ResolveHuman(handle)) { ++peds_failed; continue; }
      if(it==shared_peds.end()) it=shared_peds.emplace(p.id,SharedPed{}).first;
      it->second.handle=handle; it->second.model=p.model; it->second.look_applied=false; it->second.follow.Reset();
      it->second.adopted=adopted; it->second.dead=false;
      it->second.standin=lookalike; it->second.pending=0; it->second.loading_since=0; it->second.rerequested=false;
      if(lookalike) it->second.look_applied=true;  // the host's look is for the other model
      it->second.follow.delay=130;  // 1-2 snapshots of margin: no running past the newest and snapping back
      ++peds_made;
    }
    SharedPed& sp=it->second;
    sp.seen=now; sp.sent_at=now; sp.flags=p.flags; sp.anim=p.anim; sp.host_action=p.action;
    sp.follow.Add(time,p.pos,rows,now);
  }
  for(auto it=shared_peds.begin();it!=shared_peds.end();) {
    SharedPed& sp=it->second;
    const uint32_t obj=ResolveHuman(sp.handle);
    const float ex=obj?api->read_f32(obj+20)-local_x.load():0, ez=obj?api->read_f32(obj+28)-local_z.load():0;
    // The host sends nobody during its cutscenes: keep the copies until 3 s
    // after (all were removed and made again one by one afterwards).
    if(host_cutscene.load() || CutscenePlaying()) sp.seen=std::max(sp.seen,now-1500+3000);
    if(!obj || (now-sp.seen>1500 && !HostPauseHold(now)) || ex*ex+ez*ez>150.0f*150.0f) {
      if(!obj) ++peds_gone; else if(now-sp.seen>1500) ++peds_unsent; else ++peds_far;
      DropCopy(scratch,sp);
      ++peds_removed; it=shared_peds.erase(it); continue;
    }
    if(sp.cut_hidden && sp.sent_at>peds_cut_end) {
      sp.cut_hidden=false;
      if(!sp.look_hidden) SetCharacterHidden(scratch,obj,false);
      sp.follow.Reset();
    }
    // A copy of a mission character still loading: this game's own one may
    // have appeared since (it spawns a little after the host's); take it over.
    if(mirror_mission && (sp.flags&2) && !sp.adopted && !(api->read_u32(obj+216)&0x4000) && now>=sp.next_adopt) {
      sp.next_adopt=now+1000;
      auto n=names.find(sp.model);
      float here[3]={api->read_f32(obj+20),api->read_f32(obj+24),api->read_f32(obj+28)};
      if(n!=names.end())
        if(const uint32_t own=AdoptOwnCharacter(scratch,n->second.c_str(),here,player)) {
          DestroyObject(scratch,sp.handle);
          sp.handle=own; sp.adopted=true; sp.look_applied=false; ++peds_adopted;
          ++it; continue;
        }
    }
    // A stand-in shows the person while the copy with the host's model
    // loads; the real one takes over as soon as it is set up.
    if(sp.pending) {
      ++copies_standin;
      const uint32_t real=ResolveHuman(sp.pending);
      if(!real) sp.pending=0;
      else if(!(api->read_u32(real+216)&0x4000) && now-sp.pending_since>45000) {
        // Its model isn't going to load here (the game doesn't stream it in
        // while its memory for people is full): drop the waiting copy, which
        // holds one of the game's limited person slots; the stand-in stays.
        if(sp.pending_adopted) SetCharacterHidden(scratch,real,true); else DestroyObject(scratch,sp.pending);
        sp.pending=0; ++standins_kept;
      }
      else if(api->read_u32(real+216)&0x4000) {
        const uint32_t standin=sp.handle; const bool standin_adopted=sp.adopted;
        sp.handle=sp.pending; sp.adopted=sp.pending_adopted; sp.pending=0; sp.standin=false;
        sp.look_applied=false; sp.follow.next_order=0; sp.follow.moving=false; sp.loading_since=0;
        { const uint32_t pos=scratch.data()+1300, matrix=scratch.data()+1316;
          for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,api->read_f32(obj+20+i*4));
          api->write_f32(pos+12,0);
          for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,api->read_f32(obj+32+i*4));
          MoveHuman(scratch,real,pos,matrix,0,true); }
        if(standin_adopted) SetCharacterHidden(scratch,obj,true); else DestroyObject(scratch,standin);
        ++standins_replaced;
        ++it; continue;
      } else {
        // Keep it where the person is, so it streams in there.
        for(unsigned i=0;i<3;++i) api->write_f32(real+20+i*4,api->read_f32(obj+20+i*4));
      }
    }
    // Still not set up after 10 s and no stand-in could be made (the pool of
    // people was full): remove it, and make this model's people with a
    // loaded lookalike for a while (it held a slot and showed nothing).
    if(!sp.pending && !sp.adopted && !(api->read_u32(obj+216)&0x4000) && sp.loading_since &&
       now-sp.loading_since>((sp.flags&2)?10000u:20000u)) {
      if(sp.flags&2) unloadable_models[sp.model]=now+30000;  // passers-by: just try again
      DestroyObject(scratch,sp.handle);
      ++copies_dropped;
      it=shared_peds.erase(it); continue;
    }
    {
      const uint32_t waiting=sp.pending ? ResolveHuman(sp.pending) : (!(api->read_u32(obj+216)&0x4000) ? obj : 0);
      if(waiting && !(api->read_u32(waiting+216)&0x4000)) {
        const uint32_t desc=api->read_u32(waiting+228);
        if(Readable(desc,612)) if(const uint32_t mid=api->read_u32(desc+608)) PinModel(scratch,mid,now);
      }
      // Kept at the top level while a copy wears it, not only while it loads:
      // released 4 s after loading, the next waiting model (top level) pushed
      // it out while ~30 people wore it; they went invisible, asked for it
      // again and pushed out another - people blinking all through fights.
      if(!waiting && !sp.standin && keep_worn_models) {
        const uint32_t desc=api->read_u32(obj+228);
        if(Readable(desc,612)) if(const uint32_t mid=api->read_u32(desc+608)) PinModel(scratch,mid,now);
      }
    }
    if(!sp.pending && !(api->read_u32(obj+216)&0x4000) && sp.loading_since) {
      const uint32_t desc=api->read_u32(obj+228);
      const uint32_t id=Readable(desc,612) ? api->read_u32(desc+608) : 0;
      // The streaming request can be dropped when the game had no free
      // entry at the time; ask again (once) if it isn't there.
      if(id && !sp.rerequested && now-sp.loading_since>2000) {
        sp.rerequested=true;
        char info[160];
        if(StreamState(id,info,sizeof(info))==0) {
          scratch.ctx.r3.u64=kStreamTable+(id>>24)*204; scratch.ctx.r4.u64=id;
          scratch.ctx.r5.u64=5; scratch.ctx.r6.u64=0; scratch.ctx.r7.u64=1;
          scratch.Call(0x8250C750);
          ++streams_rerequested;
        }
      }
      // Stand-ins only for mission characters (enemies must be there);
      // passers-by wait for the host's model (random models looked wrong).
      // (Stand-ins for passers-by were tried in 0.96/0.97: people switched
      // models in front of the player and most didn't match; mission
      // characters only again.)
      if((sp.flags&2) && now-sp.loading_since>3000 && now>=sp.next_standin) {
        sp.next_standin=now+1500;
        uint32_t standin=0; bool standin_adopted=false;
        float here[3]={api->read_f32(obj+20),api->read_f32(obj+24),api->read_f32(obj+28)};
        if(mirror_mission && (sp.flags&2)) { standin=AdoptOwnCharacter(scratch,nullptr,here,player); standin_adopted=standin!=0; }
        if(!standin && now>=actors_blocked_until) {
          auto n=names.find(sp.model);
          if(const uint32_t definition=LoadedLookalike(scratch,n!=names.end()?n->second.c_str():"",player)) {
            const uint32_t pos=scratch.data()+1300, matrix=scratch.data()+1316;
            for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,here[i]);
            api->write_f32(pos+12,0);
            for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,api->read_f32(obj+32+i*4));
            standin=CreateActor(scratch,definition,pos,matrix,"Stand-in",true);
          }
        }
        if(ResolveHuman(standin)) {
          sp.pending=sp.handle; sp.pending_adopted=sp.adopted; sp.pending_since=now;
          sp.handle=standin; sp.adopted=standin_adopted; sp.standin=true;
          sp.look_applied=true;  // the host's look is for the other model
          sp.follow.next_order=0; sp.follow.moving=false;
          ++standins_made;
          ++it; continue;
        }
      }
    }
    if(api->read_u32(obj+216)&0x4000) { ++copies_ready; sp.loading_since=0; }
    else {
      ++copies_waiting;
      if(!sp.loading_since) sp.loading_since=now;
      else if(now-sp.loading_since>8000 && !sp.stuck_logged) {
        sp.stuck_logged=true;
        static unsigned logged=0;
        if(logged++<25) {
          auto n=names.find(sp.model);
          char line[160];
          std::snprintf(line,sizeof(line),"Copy never finished loading: %s (%s, host %08X, %s)",n!=names.end()?n->second.c_str():"?",
                        (sp.flags&2)?"mission character":"pedestrian",it->first,sp.adopted?"taken over":"made here");
          ProbeLog(line);
          const uint32_t desc=api->read_u32(obj+228);
          if(Readable(desc,612)) {
            char info[160]; StreamState(api->read_u32(desc+608),info,sizeof(info));
            std::snprintf(line,sizeof(line),"  its model's streaming: %s",info); ProbeLog(line);
            DumpStreamGroup(api->read_u32(desc+608));
          }
        }
      }
    }
    if(ex*ex+ez*ez<40.0f*40.0f) ++copies_near;
    // Died in the host's game: die here too, and lie where it fell.
    if((sp.flags&4) && !sp.dead && (api->read_u32(obj+216)&0x4000)) {
      KillCopy(scratch,obj); sp.dead=true; ++copies_killed;
    }
    if(sp.dead) { ++it; continue; }
    // The appearance goes on once the person's setup has finished.
    // A new copy shows the model's own random clothes (a hat) until the
    // host's look is put on a moment later: hidden until then (at most 3 s).
    if(!sp.look_applied && !sp.look_hidden && !sp.standin && !sp.adopted) { SetCharacterHidden(scratch,obj,true); sp.look_hidden=true; }
    if(api->read_u32(obj+216)&0x4000) { if(!sp.ready_since) sp.ready_since=now; } else sp.ready_since=0;
    if(!sp.look_applied && (api->read_u32(obj+216)&0x4000)) {
      std::vector<uint8_t> look;
      { std::lock_guard lock(traffic_mutex); auto l=ped_looks_in.find(it->first); if(l!=ped_looks_in.end()) look=l->second; }
      if(!look.empty()) { sp.look_applied=ApplyLook(scratch,sp.handle,look.data(),unsigned(look.size())); if(sp.look_applied) ++peds_looks; }
    }
    if(sp.look_hidden && (sp.look_applied || (sp.ready_since && now-sp.ready_since>3000))) { if(!sp.cut_hidden) SetCharacterHidden(scratch,obj,false); sp.look_hidden=false; }
    if(!sp.dead) {
      // Passers-by stay targetable (the joiner's punches land and count in
      // the host's game); mission characters, who carry guns, stay out of
      // combat here so they only shoot when the host's do.
      MakePassive(obj,!(sp.flags&2));
      if(now>=sp.next_crouch) { sp.next_crouch=now+250; MatchCrouch(scratch,obj,(sp.flags&8)!=0); }
    }
    // What the host's person is doing right now (reading a newspaper, tying
    // shoes, walking with a cane): started on the copy too when it isn't
    // playing it (it began before the copy was made, or the start was missed).
    if(sync_idle && sp.host_action!=0xFFFF && now>=sp.next_action_sync && (api->read_u32(obj+216)&0x4000) && !sp.dead &&
       api->read_u32(obj+756)!=sp.host_action && sp.action_misses<4) {
      sp.next_action_sync=now+1500;
      const bool known=sp.action_args_for==sp.host_action;
      scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=sp.host_action;
      scratch.ctx.r5.u64=known?sp.action_args[0]:0; scratch.ctx.r6.u64=known?sp.action_args[1]:1; scratch.ctx.r7.u64=known?sp.action_args[2]:0;
      ++replicated_depth;
      const bool played=scratch.Call(0x82444F88) && scratch.ctx.r3.u32;
      --replicated_depth;
      if(played) { ++ped_actions_synced; sp.action_until=now+4000; sp.action_misses=0; } else ++sp.action_misses;
    }
    if(sp.host_action==0xFFFF) sp.action_misses=0;
    // The host's animation set (idle, walk and stand styles).
    // Applied once per change of the host's value (it was forced again every
    // second and flickered against the copy's own: an old lady switched between
    // a cane and an invisible wheelchair).
    if(sp.anim!=sp.anim_seen) { sp.anim_seen=sp.anim; sp.anim_since=now; }
    // Also when the copy drifted off it (its own walk style) for 6 s.
    const bool drifted=sp.anim==sp.anim_applied && api->read_u32(obj+952)!=sp.anim && now>=sp.next_anim+5000 && sp.follow.moving;
    if(sp.anim!=0xFF && (sp.anim!=sp.anim_applied || drifted) && now-sp.anim_since>=1500 && (api->read_u32(obj+216)&0x4000) && now>=sp.action_until) {
      sp.anim_applied=sp.anim;
      sp.next_anim=now+1000;
      api->write_u32(obj+952,sp.anim);
      scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=uint64_t(int64_t(-1)); scratch.ctx.r5.u64=uint64_t(int64_t(-1)); scratch.ctx.r6.u64=0;
      ++replicated_depth; scratch.Call(0x82440200); --replicated_depth;
      sp.follow.next_order=0; sp.follow.moving=true;
      ++peds_anims;
    }
    if(now<sp.action_until && sp.follow.count) {
      // Hand back to the path as soon as the host's person moves off.
      float pp[3], rr[9], vv[3];
      const float speed=sp.follow.Sample(uint32_t(int64_t(now)-sp.follow.offset-sp.follow.delay),pp,rr,vv);
      const float gx=pp[0]-api->read_f32(obj+20), gz=pp[2]-api->read_f32(obj+28);
      if(speed<0.5f && gx*gx+gz*gz<1.0f) { ++it; continue; }
      sp.action_until=0;
    }
    // The host's person stands still (sitting on a bench, leaning, an idle):
    // no walk orders, which stood the copy up and let it sit down again.
    if(sp.follow.count) {
      float pp[3], rr[9], vv[3];
      const float speed=sp.follow.Sample(uint32_t(int64_t(now)-sp.follow.offset-sp.follow.delay),pp,rr,vv);
      const float gx=pp[0]-api->read_f32(obj+20), gz=pp[2]-api->read_f32(obj+28);
      if(speed<0.3f && gx*gx+gz*gz<0.8f*0.8f) { ++peds_still; ++it; continue; }
    }
    sp.follow.Drive(scratch,obj,now,nullptr);
    ++it;
  }
  { static uint64_t next_pins=0; if(now>=next_pins) { next_pins=now+1000; if(pin_host_models && now-host_models_time<3000) for(uint32_t id:host_models) PinModel(scratch,id,now); mod_stage.store("model memory"); ReleasePins(scratch,now,false); ClearUnusedModelRefs(scratch); if(free_orphan_slots) FreeOrphanSlots(scratch); }
    static uint64_t next_evict=0; if(now>=next_evict) { next_evict=now+2000; mod_stage.store("model room"); EvictNonHostModels(scratch,now); } mod_stage.store("shared people"); }
  {
    static uint64_t next_drop=0;
    if(now>=next_drop) {
      next_drop=now+2000;
      std::vector<uint32_t> groups;
      for(auto& [id,sp]:shared_peds) {
        const uint32_t o=ResolveHuman(sp.pending ? sp.pending : sp.handle);
        const uint32_t desc=o ? api->read_u32(o+228) : 0;
        if(Readable(desc,612)) { const uint32_t g=api->read_u32(desc+608)>>24; if(std::find(groups.begin(),groups.end(),g)==groups.end()) groups.push_back(g); }
      }
      for(uint32_t g:groups) if(g<64) DropOwnModelRefs(scratch,g);
    }
  }
  if(now>=next_log) {
    next_log=now+10000;
    char line[220];
    {
      const uint32_t group=kStreamTable;
      unsigned loaded=0, waiting=0, guard=0;
      if(Readable(group,204)) { const uint32_t head=api->read_u32(group+48);
        for(uint32_t e=head;e && Readable(e,72) && guard++<256;) { if(api->read_u32(e)) ++loaded; else ++waiting; e=api->read_u32(e+28); if(e==head) break; } }
      std::snprintf(line,sizeof(line),"People models here: %u loaded, %u waiting for room; %u unused models let go, %u not in the host's set made room, %u stuck slots freed",loaded,waiting,stale_models_cleared,models_evicted,slots_freed); ProbeLog(line); stale_models_cleared=0; models_evicted=0; slots_freed=0;
      { std::string l="  loaded:", w="  waiting:", h="  host has:"; char t[16];
        if(Readable(group,204)) { const uint32_t head=api->read_u32(group+48); unsigned g2=0;
          for(uint32_t e=head;e && Readable(e,72) && g2++<256;) { unsigned refs=0; for(unsigned k=0;k<7;++k) refs+=api->read_u32(e+44+k*4);
            std::snprintf(t,sizeof(t)," %X(%u)",api->read_u32(e+4),refs); (api->read_u32(e)?l:w)+=t; e=api->read_u32(e+28); if(e==head) break; } }
        for(uint32_t id:host_models) { std::snprintf(t,sizeof(t)," %X",id); h+=t; }
        ProbeLog((l+w+h).c_str()); LogPeopleSlots("  slots here:"); }
    }
    if(pins_made || pins_released) {
      std::snprintf(line,sizeof(line),"Model priority: %u models asked for at the top level, %u released, %zu held",pins_made,pins_released,model_pins.size());
      ProbeLog(line); pins_made=pins_released=0;
    }
    if(own_model_refs_dropped) {
      std::snprintf(line,sizeof(line),"Model memory: %u of this game's own people-model references dropped",own_model_refs_dropped);
      ProbeLog(line); own_model_refs_dropped=0;
    }
    std::snprintf(line,sizeof(line),"Shared pedestrians: %zu people (host sent %zu), made %u, failed %u, looks applied %u, animation sets matched %u, removed %u, own removed %u, actions %u",
                  shared_peds.size(),peds.size(),peds_made,peds_failed,peds_looks,peds_anims,peds_removed,own_peds_removed,ped_actions_played.exchange(0));
    ProbeLog(line);
    std::snprintf(line,sizeof(line),"Copies: %u set up, %u still loading, %u within 40 m, %u killed, %u own mission characters taken over, %u stand-ins; hits sent %u, hits taken from people %u, own actions blocked %u",
                  copies_ready,copies_waiting,copies_near,copies_killed,peds_adopted,copies_standin,ped_hits_sent.exchange(0),ped_hits_applied.exchange(0),own_actions_blocked);
    own_actions_blocked=0;
    peds_adopted=0;
    ProbeLog(line);
    if(standins_made || standins_replaced || streams_rerequested || standins_kept || actors_refused || copies_dropped) {
      std::snprintf(line,sizeof(line),"Models not loaded here in time: %u stand-ins made (a loaded model, until the host's loads), %u replaced by the real one, %u kept for good, %u model requests asked again; %u people refused by a full pool, %u stuck copies remade with a loaded model",
                    standins_made,standins_replaced,standins_kept,streams_rerequested,actors_refused,copies_dropped);
      ProbeLog(line);
      standins_made=standins_replaced=streams_rerequested=standins_kept=actors_refused=copies_dropped=0;
    }
    copies_killed=0;
    std::snprintf(line,sizeof(line),"Copies removed: %u gone here, %u no longer sent by the host, %u far; made as kind %u (%u fell back to 3); %u removals by this game refused (last from %08X), %u let through; %u ongoing actions of the host's people started here",peds_gone,peds_unsent,peds_far,copy_kind,actors_kind_fallback,copies_kept,copies_kept_from,copies_let_go,ped_actions_synced); ProbeLog(line); actors_kind_fallback=0; copies_kept=0; copies_let_go=0; ped_actions_synced=0;
    peds_gone=peds_unsent=peds_far=0;
    peds_made=peds_failed=peds_removed=own_peds_removed=peds_looks=peds_anims=0;
  }
}

std::atomic<uint32_t> packets_in{0}, packets_out{0};
void UpdateBody(WmlContext* raw);

// Chains the game's update, runs the mod's work and every 10 seconds logs its
// cost, so a slowdown can be told apart from the mod.
void TraceFrame();
// Crash and freeze reports: the game's own log just stops at a crash, so
// the mod writes what it can (exception, address, module) and a small dump
// of all threads next to its log. A game update missing for 20 s while
// connected (a freeze, or a long pause) writes the same once.
unsigned people_model_scale=1, vehicle_model_scale=1, people_memory_scale=1;
std::string mod_folder;
std::atomic<uint64_t> last_update_ms{0};
LPTOP_LEVEL_EXCEPTION_FILTER previous_crash_filter=nullptr;
void WriteReport(const char* why,EXCEPTION_POINTERS* ep) {
  const std::string base=mod_folder+"\\coop-crash-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
  if(FILE* f=std::fopen((base+".txt").c_str(),"w")) {
    std::fprintf(f,"%s\n",why);
    if(ep && ep->ExceptionRecord) {
      const auto* er=ep->ExceptionRecord;
      std::fprintf(f,"exception %08lX at %p",er->ExceptionCode,er->ExceptionAddress);
      if(er->NumberParameters>=2) std::fprintf(f," (%s %p)",er->ExceptionInformation[0]?"write":"read",(void*)er->ExceptionInformation[1]);
      HMODULE mod=nullptr; char name[MAX_PATH]{};
      if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)er->ExceptionAddress,&mod) && mod) {
        GetModuleFileNameA(mod,name,sizeof(name));
        std::fprintf(f," in %s +%llX",name,(unsigned long long)((uintptr_t)er->ExceptionAddress-(uintptr_t)mod));
      }
      std::fprintf(f,"\n");
    }
    std::fprintf(f,"stage: %s, inside game function %08X, last game function called %08X\n",mod_stage.load(),mod_in_call.load(),mod_last_call.load());
    std::fprintf(f,"connected %d hosting %d, last game update %llu ms ago\n",connected.load()?1:0,hosting.load()?1:0,(unsigned long long)(MonoMs()-last_update_ms.load()));
    std::fclose(f);
  }
  using DumpFn=BOOL(WINAPI*)(HANDLE,DWORD,HANDLE,int,void*,void*,void*);
  static HMODULE dbghelp=LoadLibraryA("dbghelp.dll");
  if(!dbghelp) return;
  auto dump=reinterpret_cast<DumpFn>(GetProcAddress(dbghelp,"MiniDumpWriteDump"));
  if(!dump) return;
  HANDLE file=CreateFileA((base+".dmp").c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE) return;
  struct { DWORD thread; EXCEPTION_POINTERS* ep; BOOL client; } info{GetCurrentThreadId(),ep,FALSE};
  dump(GetCurrentProcess(),GetCurrentProcessId(),file,0x1000 /*with thread info*/,ep?&info:nullptr,nullptr,nullptr);
  CloseHandle(file);
}
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
  static std::atomic<bool> once{false};
  if(!once.exchange(true)) WriteReport("Game crash",ep);
  return previous_crash_filter ? previous_crash_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}
void StartCrashReports(const char* folder) {
  mod_folder=folder;
  previous_crash_filter=SetUnhandledExceptionFilter(CrashFilter);
  std::thread([]{
    bool reported=false;
    for(;;) {
      Sleep(1000);
      const uint64_t last=last_update_ms.load();
      if(!last) continue;
      const uint64_t gap=MonoMs()-last;
      // A session running now or within the last minute counts: a frozen
      // game stops sending, the other side drops it after ~15 s, and the
      // 20 s check (connected only) never wrote a report for such a freeze.
      static uint64_t last_session=0; static unsigned reports=0;
      if(connected.load() || running.load()) last_session=MonoMs();
      const bool session=last_session && MonoMs()-last_session<60000;
      if(gap<3000) reported=false;
      else if(!reported && reports<3 && gap>8000 && (session || people_model_scale>1 || people_memory_scale>1)) {
        reported=true; ++reports;
        WriteReport("No game update for 8 s (frozen, or paused a long time)",nullptr);
        ProbeLog("FREEZE: no game update for 8 s - report written next to this log (coop-crash-*.txt / .dmp)");
      }
    }
  }).detach();
}
// Streaming groups are set up once at start (8250EEF8(config), 6 groups of
// 40 bytes: +0 memory, +4 memory size, +8 slots, +12 entries, +16 levels).
// Group 0 holds the people models: 12 slots in 3.4 MB, so only about 10
// people models fit at once - fewer than a busy street needs (with Living
// Stilwater even more so) and the other player's game couldn't show the
// people this game has. Its slots and memory are raised (mod.ini
// people_models, 3 = three times as many), in a block of its own.
WmlGuestFunction original_stream_setup=nullptr;
void StreamSetupHook(WmlContext* c,uint8_t* b) {
  const uint32_t cfg=uint32_t(api->get_r(c,3));
  for(unsigned g=0;g<6;++g) {
    const uint32_t e=cfg+g*40;
    char line[160]; std::snprintf(line,sizeof(line),"Streaming group %u: memory %08X, %u KB, %u slots, %u entries, %u levels",g,
      api->read_u32(e),api->read_u32(e+4)/1024,api->read_u32(e+8),api->read_u32(e+12),api->read_u32(e+16)); ProbeLog(line);
  }
  auto scale=[&](unsigned g,unsigned k,unsigned mem_k,const char* what) {
    mem_k=std::max(mem_k,k);
    if(mem_k<=1) return;
    const uint32_t e=cfg+g*40;
    const uint32_t size=api->read_u32(e+4), slots=api->read_u32(e+8), entries=api->read_u32(e+12);
    if(!size || size>0x1000000 || !slots || slots>64) return;
    uint32_t want=((size*mem_k)+0xFFFF)&~0xFFFFu;
    // Test: an exact size for the people-model memory (mod.ini people_memory_kb;
    // 0 = people_memory times the normal). To find a size limit.
    if(g==0) {
      const unsigned kb=GetPrivateProfileIntA("settings","people_memory_kb",0,(std::string(self->folder)+"\\mod.ini").c_str());
      if(kb>=1024 && kb<=65536) want=((kb*1024u)+0xFFFF)&~0xFFFFu;
    }
    GuestScratch scratch(c);
    scratch.ctx.r3.u64=want; scratch.ctx.r4.u64=0xFFFFFFFFull; scratch.ctx.r5.u64=0x10000; scratch.ctx.r6.u64=0x20000004;
    const uint32_t block=scratch.Call(0x82716618) ? scratch.ctx.r3.u32 : 0;
    char line[160];
    if(!block) { std::snprintf(line,sizeof(line),"Streaming group %u (%s): %u KB more not available, left as it is",g,what,(want-size)/1024); ProbeLog(line); return; }
    // Where the block landed, and whether it overlaps memory the other
    // streaming groups already use (a vehicle's parts were garbage with the
    // bigger people memory: suspected overlap).
    {
      char where[200]; int o=std::snprintf(where,sizeof(where),"Streaming group %u: new block %08X-%08X",g,block,block+want);
      for(unsigned h=0;h<6;++h) {
        const uint32_t m=api->read_u32(cfg+h*40), sz=api->read_u32(cfg+h*40+4);
        if(!m || !sz || h==g) continue;
        if(block<m+sz && m<block+want) o+=std::snprintf(where+o,sizeof(where)-o,"; OVERLAPS group %u (%08X-%08X)",h,m,m+sz);
      }
      ProbeLog(where);
    }
    // Test (mod.ini people_memory_test = 1): the block is taken but group 0
    // stays where it was - tells "the game runs short of memory" apart from
    // "group 0 in a new place breaks something".
    const int test=GetPrivateProfileIntA("settings","people_memory_test",0,(std::string(self->folder)+"\\mod.ini").c_str());
    if(test==1) { ProbeLog("Streaming group test 1: block taken, group left in place"); return; }
    std::memset(api->guest_pointer(block),0,want);
    if(test==2) { ProbeLog("Streaming group test 2: block taken and cleared, group left in place"); return; }
    api->write_u32(e,block); api->write_u32(e+4,want);
    api->write_u32(e+8,std::min<uint32_t>(slots*k,entries));
    std::snprintf(line,sizeof(line),"Streaming group %u (%s): %u slots in %u KB (was %u in %u KB)",g,what,api->read_u32(e+8),want/1024,slots,size/1024);
    ProbeLog(line);
  };
  scale(0,people_model_scale,people_memory_scale,"people models");
  scale(1,vehicle_model_scale,1,"group 1");
  original_stream_setup(c,b);
}
void UpdateHook(WmlContext* raw,uint8_t* base) {
  last_update_ms.store(MonoMs());
  mod_stage.store("game update");
  original_update(raw,base);
  mod_stage.store("mod update");
  TraceFrame();   // (the per-frame callback is never called by the loader)
  static LARGE_INTEGER frequency{};
  static double busy_ms=0, worst_ms=0;
  static uint32_t calls=0;
  static uint64_t window=0;
  if(!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
  LARGE_INTEGER a,b;
  QueryPerformanceCounter(&a);
  UpdateBody(raw);
  mod_stage.store("between updates");
  QueryPerformanceCounter(&b);
  const double ms=double(b.QuadPart-a.QuadPart)*1000.0/double(frequency.QuadPart);
  busy_ms+=ms; worst_ms=std::max(worst_ms,ms); ++calls;
  const uint64_t now=MonoMs();
  if(!window) window=now;
  if(now-window>=10000) {
    if(running.load()) {
      const double seconds=(now-window)/1000.0;
      char line[420];
      std::snprintf(line,sizeof(line),"Perf: %.0f updates/s, mod %.2f ms/s (worst %.2f ms), packets in %.0f/s out %.0f/s, actors %u, person actions sent %u, joiner hits applied %u, people's hits on joiner %u, knockdowns/reactions sent %u played %u, car hits sent %u applied %u, own climbs of the other player blocked %u",
                    calls/seconds,busy_ms/seconds,worst_ms,packets_in.exchange(0)/seconds,packets_out.exchange(0)/seconds,
                    unsigned((avatar_local?1:0)+(replica_local?1:0)+(test_local?1:0)),ped_actions_sent.exchange(0),
                    hosting.load()?ped_hits_applied.exchange(0):0u,hits_from_people.exchange(0),ped_calls_sent.exchange(0),ped_calls_played.exchange(0),impacts_sent.exchange(0),impacts_applied.exchange(0),own_climbs_blocked);
      own_climbs_blocked=0;
      ProbeLog(line);
      if(ped_reacts_sent || ped_reacts_played || joiner_hit_reactions) {
        std::snprintf(line,sizeof(line),"Hit reactions: %u sent, %u played here, %u from the other player's hits",ped_reacts_sent,ped_reacts_played,joiner_hit_reactions);
        ProbeLog(line); ped_reacts_sent=ped_reacts_played=joiner_hit_reactions=0;
      }
    }
    busy_ms=0; worst_ms=0; calls=0; window=now;
  }
}

// F7: everyone within 150 m, for working out which people a mission uses.
void DumpPeople(uint32_t player) {
  char line[260];
  std::snprintf(line,sizeof(line),"F7 dump: mission globals %08X %08X %08X, cutscene %d/%d, mission slots %04X %04X",
                api->read_u32(0x82B3106C),api->read_u32(0x82B31080),api->read_u32(0x827D578C),
                api->read_u8(0x8370D991),api->read_u8(0x8370D990),api->read_u16(0x82B30F3A+306),api->read_u16(0x82B30F3A+308));
  ProbeLog(line);
  std::vector<uint32_t> ambient;
  { const uint32_t head=api->read_u32(0x8309ABB4); uint32_t o=head;
    for(unsigned g=0;o && g<128 && Readable(o,4252);++g) { ambient.push_back(o); o=api->read_u32(o+3600); if(o==head) break; } }
  const float px=api->read_f32(player+20), pz=api->read_f32(player+28);
  unsigned n=0;
  for(uint32_t index=0;index<4096 && n<80;++index) {
    const uint32_t cur=api->read_u32(kObjectTable+12+index*16);
    if(!Readable(cur,4252) || api->read_u32(cur+72)!=1) continue;
    const float dx=api->read_f32(cur+20)-px, dz=api->read_f32(cur+28)-pz, d=std::sqrt(dx*dx+dz*dz);
    if(d>150) continue;
    char name[46]="?"; ModelName(cur,name,sizeof(name));
    const bool amb=std::find(ambient.begin(),ambient.end(),cur)!=ambient.end();
    std::snprintf(line,sizeof(line),"  %08X %s d=%.0f flags120=%08X flags216=%08X ambient=%d +2496=%08X vehicle=%d%s",
                  api->read_u32(cur+68),name,d,api->read_u32(cur+120),api->read_u32(cur+216),amb?1:0,
                  api->read_u32(cur+2496),ResolveVehicle(api->read_u32(cur+2496))?1:0,cur==player?" (player)":"");
    ProbeLog(line); ++n;
  }
}

// The host's "mission running" flag drops for a moment between mission stages
// (seen in logs: "Host's mission ended" then "started" again within a second).
// Each drop restarted the mirroring here: mission characters were remade
// (flickering) and the next cutscene counted as the first one (not played).
// A mission counts as over only after 5 s without the flag.
// A different mission (the next one starting right after a pass, 4 s later)
// is a new mission though: its hash changes, and that ends the old one here
// for one update so the start (bringing this player over, starting it here)
// runs again. 1.27 kept the old one going: the joiner stayed behind.
bool HostMissionActive(uint64_t now) {
  static uint64_t last_seen_active=0, gap_until=0;
  static uint32_t active_hash=0;
  const bool flag=host_mission.load() && now-host_state_seen.load()<3000;
  const uint32_t hash=host_mission_hash.load();
  if(flag && hash && active_hash && hash!=active_hash) gap_until=now+500;  // a new mission
  if(flag && hash) active_hash=hash;
  if(now<gap_until) { last_seen_active=0; return false; }
  if(flag) last_seen_active=now;
  return share_missions && last_seen_active && now-last_seen_active<5000 && now-host_state_seen.load()<3000;
}

// Joiner: brought next to the host when the host starts a mission (once the
// host's cutscene is over), and again if left far behind during it.
void UpdateMissionFollow(GuestScratch& scratch,uint32_t player,uint64_t now) {
  // This only runs while connected: a gap means a new session.
  static uint64_t last_run=0;
  static bool join_move=false;
  if(now-last_run>2000) join_move=share_missions;
  last_run=now;
  static bool was_active=false, pending=false;
  static uint64_t next_catch_up=0;
  const bool active=HostMissionActive(now);
  if(active && !was_active) { pending=true; ProbeLog("Host started a mission; bringing this player over after the cutscene"); }
  static bool host_cut_was=false;
  const bool host_cut=host_cutscene.load();
  if(active && host_cut_was && !host_cut) pending=true;  // after each of the host's cutscenes
  host_cut_was=host_cut;
  if(!active && was_active) ProbeLog("Host's mission ended");
  was_active=active;
  if(host_cutscene.load() || CutscenePlaying()) return;
  PlayerPose p;
  { std::lock_guard lock(pose_mutex); p=remote_pose; }
  if(!p.sequence) return;
  const float dx=p.pos[0]-local_x.load(), dy=p.pos[1]-local_y.load(), dz=p.pos[2]-local_z.load(), d=std::sqrt(dx*dx+dz*dz);
  {
    // The host's mission moved the host somewhere else (a jump of 30 m+
    // between two updates): follow.
    static float last[3]{}; static bool have=false;
    const float jx=p.pos[0]-last[0], jz=p.pos[2]-last[2];
    if(active && have && jx*jx+jz*jz>30.0f*30.0f && p.vehicle<0) { pending=true; ProbeLog("Host was moved by the mission: following"); }
    last[0]=p.pos[0]; last[1]=p.pos[1]; last[2]=p.pos[2]; have=true;
  }
  // Standing inside each other (a cutscene puts both players on the same
  // spot): the host's player got stuck on this character, fell over every
  // half second and couldn't move. Step aside, in or out of a mission.
  // Only when it lasts (3 s without a break): in fights both players come
  // that close all the time, and 1.27 and earlier moved the joiner every
  // few seconds (jumping around on the host's screen).
  static uint64_t overlap_since=0;
  const bool overlap=d<0.6f && std::abs(dy)<1.5f && p.vehicle<0 && !ResolveVehicle(api->read_u32(player+2496));
  if(!overlap) overlap_since=0; else if(!overlap_since) overlap_since=now;
  const bool step_aside=overlap_since && now-overlap_since>3000;
  // Joining: brought next to the host once (the host's character only
  // appears within 40 m, so a joiner far away saw nobody).
  const bool joining=join_move && d>6.0f;
  if(join_move && d<=6.0f) join_move=false;
  if(!active && !step_aside && !joining) { pending=false; return; }
  if(!pending && !step_aside && !joining && !(d>150.0f && now>=next_catch_up)) return;
  pending=false; next_catch_up=now+10000; overlap_since=0; join_move=false;
  if(d<6.0f && d>=0.6f) return;
  // 1.5 m to the host's right, facing the same way.
  const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316;
  float target[3]={p.pos[0]+p.rows[0]*1.5f, p.pos[1]+0.1f, p.pos[2]+p.rows[2]*1.5f};
  float rows[9]={p.rows[0],0,p.rows[2], 0,1,0, p.rows[6],0,p.rows[8]};
  if(p.vehicle>=0) { target[0]=p.vpos[0]+p.vrows[0]*4.0f; target[1]=p.vpos[1]+0.5f; target[2]=p.vpos[2]+p.vrows[2]*4.0f; }
  for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,target[i]);
  api->write_f32(pos+12,0);
  for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,rows[i]);
  bool ok=false;
  if(const uint32_t vehicle=ResolveVehicle(api->read_u32(player+2496))) { PlaceVehicle(scratch,vehicle,pos,matrix); ok=true; }
  else ok=MoveHuman(scratch,player,pos,matrix,0,true);
  char line[140];
  // After a long move this game still loads the new area: the host's people
  // made at once stood in a T-pose. Wait 4 s.
  if(ok && d>100.0f) actors_blocked_until=std::max<uint64_t>(actors_blocked_until,now+1000);
  std::snprintf(line,sizeof(line),"Moved next to the host (%.1f m away%s): %s",d,d<0.6f?", standing inside each other":"",ok?"done":"refused");
  ProbeLog(line);
}

// Joiner: the host's story is the one played, so this game doesn't start
// missions of its own while in a session.
// Joiner: the story missions' start markers (ground ring and map icon) are
// hidden while in a session. Mission table (82211080): count at 0x82B31084,
// 100-byte entries from 0x82B313A8; +52 is the handle of the mission's
// start trigger (82213E28). Hidden/shown with the game's trigger_disable /
// trigger_enable (824EA968 / 824EA900), and shown again when the session ends.
constexpr uint32_t kMissionCount=0x82B31084, kMissions=0x82B313A8;
std::vector<uint32_t> hidden_markers;
uint32_t ResolveObject(uint32_t handle,uint32_t size) {
  if(!handle || (handle&0xffff)>=4096) return 0;
  const uint32_t obj=api->read_u32(kObjectTable+12+(handle&0xffff)*16);
  return Readable(obj,size) && api->read_u32(obj+68)==handle ? obj : 0;
}
void HideMissionMarkers(GuestScratch& scratch,uint64_t now) {
  static uint64_t next=0;
  if(now<next) return;
  next=now+1000;
  const uint32_t count=std::min<uint32_t>(api->read_u32(kMissionCount),100);
  unsigned hidden=0;
  for(uint32_t i=0;i<count;++i) {
    const uint32_t handle=api->read_u32(kMissions+i*100+52);
    const uint32_t trigger=ResolveObject(handle,380);
    if(!trigger || (api->read_u8(trigger+284)&0x10)) continue;
    scratch.ctx.r3.u64=trigger;
    if(!scratch.Call(0x824EA968)) continue;
    if(std::find(hidden_markers.begin(),hidden_markers.end(),handle)==hidden_markers.end()) hidden_markers.push_back(handle);
    ++hidden;
  }
  static bool logged=false;
  if(hidden && !logged) {
    logged=true;
    char line[120]; std::snprintf(line,sizeof(line),"Mission markers hidden: %u (of %u missions)",hidden,count); ProbeLog(line);
  }
}
void ShowMissionMarkers(GuestScratch& scratch) {
  if(hidden_markers.empty()) return;
  unsigned shown=0;
  for(uint32_t handle:hidden_markers) {
    const uint32_t trigger=ResolveObject(handle,380);
    if(!trigger || !(api->read_u8(trigger+284)&0x10)) continue;
    scratch.ctx.r3.u64=trigger;
    if(scratch.Call(0x824EA900)) ++shown;
  }
  hidden_markers.clear();
  char line[80]; std::snprintf(line,sizeof(line),"Mission markers shown again: %u",shown); ProbeLog(line);
}

WmlGuestFunction original_cutscene_start=nullptr;
void CutsceneStartHook(WmlContext* c,uint8_t* b) {
  if(share_cutscenes && hosting.load() && connected.load()) {
    const uint32_t name=uint32_t(api->get_r(c,3));
    if(Readable(name,1)) {
      std::lock_guard lock(cutscene_mutex);
      CutsceneStart cs;
      cs.sequence=cutscene_out.sequence+1;
      cs.r4=uint32_t(api->get_r(c,4)); cs.r5=uint32_t(api->get_r(c,5));
      cs.r6=uint32_t(api->get_r(c,6)); cs.r7=uint32_t(api->get_r(c,7));
      for(unsigned i=0;i<63 && Readable(name+i,1);++i) { cs.name[i]=char(api->read_u8(name+i)); if(!cs.name[i]) break; }
      cutscene_out=cs; cutscene_out_time=MonoMs();
      char line[120]; std::snprintf(line,sizeof(line),"Cutscene started here: %s (shown to the other player too)",cs.name); ProbeLog(line);
    }
  }
  else if(connected.load() && !hosting.load() && mirror_mission && !applying_remote && MonoMs()-mirror_since>15000) {
    // This game's mission script is behind the host's (it waits for things
    // that happen in the host's game): its cutscenes come from the host.
    ProbeLog("This game's own mission cutscene held back: the host's cutscenes play here");
    return;
  }
  original_cutscene_start(c,b);
}
// Joiner: while the host's cutscene plays here and the host is still watching
// it, the skip buttons (Y, and A in some scenes; the mouse clicks map to them)
// are taken out of this player's controller state: only the host skips.
// 827166D0(user, state) is the game's XamInputGetState wrapper; buttons are
// the u16 at state+4.
bool joiner_cutscene=false;   // game thread: the host's cutscene plays here
uint64_t mission_end_applied_at=0; // game thread: when the host's mission end was replayed here
WmlGuestFunction original_input=nullptr;
void InputHook(WmlContext* c,uint8_t* b) {
  const uint32_t state=uint32_t(api->get_r(c,4));
  original_input(c,b);
  if(joiner_cutscene && connected.load() && !hosting.load() && host_cutscene.load() && CutscenePlaying() && Readable(state,8)) {
    const uint16_t buttons=api->read_u16(state+4);
    if(buttons&0x9000) api->write_u16(state+4,uint16_t(buttons&~0x9000));
  }
}

// Joiner: the host's cutscene plays here as well (the same name and
// arguments). When the host's ends, skipped or not, this one is ended too
// (821F9700, the game's own immediate cutscene end).
void UpdateSharedCutscene(GuestScratch& scratch,uint64_t now) {
  static bool host_seen=false;
  bool& ours=joiner_cutscene;
  static uint64_t started=0;
  CutsceneStart cs; bool fresh=false;
  { std::lock_guard lock(cutscene_mutex); if(cutscene_in_new) { cs=cutscene_in; fresh=true; cutscene_in_new=false; } }
  const bool mission_loaded=mirror_mission && now-mirror_since>15000;
  if(mirror_mission) {
    // Cutscenes here (this game's own first one, then the host's) end with
    // the host's and only the host can skip them.
    if(CutscenePlaying() && !ours) { ours=true; host_seen=false; started=now; ProbeLog("Mission cutscene here follows the host's"); }
  }
  // While the host runs a mission this game runs it too and plays its own
  // cutscenes once the mission has loaded (played from here earlier, their
  // characters weren't loaded yet and the cutscene was empty).
  // The first one (while the mission still loads here) is left to this game's
  // own mission; later ones come from the host.
  if(fresh && HostMissionActive(now) && !mission_loaded) {
    char line[120]; std::snprintf(line,sizeof(line),"Host's cutscene %s: left to this game's own mission",cs.name); ProbeLog(line);
    fresh=false;
  }
  if(fresh && share_cutscenes && !CutscenePlaying() && cs.name[0]) {
    const uint32_t name=scratch.data()+1400;
    const size_t n=strnlen(cs.name,63);
    std::memcpy(api->guest_pointer(name),cs.name,n); api->write_u8(name+uint32_t(n),0);
    scratch.ctx.r3.u64=name; scratch.ctx.r4.u64=cs.r4; scratch.ctx.r5.u64=cs.r5;
    scratch.ctx.r6.u64=cs.r6; scratch.ctx.r7.u64=cs.r7;
    ++applying_remote;
    const bool called=scratch.Call(0x821F9098);
    --applying_remote;
    ours=called && CutscenePlaying(); host_seen=false; started=now;
    char line[140]; std::snprintf(line,sizeof(line),"Host's cutscene %s: %s",cs.name,ours?"playing here too":called?"didn't start here":"call failed"); ProbeLog(line);
  }
  if(!ours) return;
  if(!CutscenePlaying()) { ours=false; ProbeLog("Shared cutscene finished here"); return; }
  if(host_cutscene.load()) host_seen=true;
  const bool host_done=(host_seen && !host_cutscene.load()) || (!host_seen && now-started>5000) || !connected.load();
  if(host_done && mission_end_applied_at && now-mission_end_applied_at<5000) {
    // The mission end already ran here and takes the cutscene down itself;
    // ending it again from outside froze the game (reward screen).
    ours=false;
    ProbeLog("Host's cutscene is over: the mission end here closes it");
    return;
  }
  if(host_done) {
    scratch.Call(0x821F9700);
    ours=false;
    ProbeLog("Host's cutscene is over: ended here too");
  }
}

void QueueHud(const HudEvent& e) {
  std::lock_guard lock(hud_mutex);
  if(hud_out.size()<64) hud_out.push_back(e);
  ++hud_sent;
}
bool HoldLocalHud() {
  if(connected.load() && !hosting.load() && mirror_mission && !applying_remote) { ++hud_blocked; return true; }
  return false;
}
WmlGuestFunction original_objective=nullptr, original_help=nullptr, original_mission_end=nullptr;
// Co-op messages (hosting, join code, joined, left, ended, errors) shown in
// the game's own help box, like mission help: 822E52C8(r3 = out handle,
// r4 = UTF-16 text, r5 = 8 words of settings; the game's defaults at
// 0x82820C4C, 7 s on screen, +12 a handle picked as 822E54B0 does).
std::mutex notice_mutex;
std::deque<std::string> notices;
thread_local bool showing_notice=false;
void Notify(const std::string& text) {
  if(text.empty()) return;
  std::lock_guard lock(notice_mutex);
  if(!notices.empty() && notices.back()==text) return;
  notices.push_back(text);
  while(notices.size()>4) notices.pop_front();
}
// Game thread, player in the world: one message at a time (each stays a few
// seconds), never over a cutscene or a menu.
void ShowNotices(GuestScratch& scratch) {
  static uint64_t next=0;
  const uint64_t now=GetTickCount64();
  if(now<next || CutscenePlaying() || api->read_u32(0x839E0DF8)) return;
  std::string line;
  { std::lock_guard lock(notice_mutex); if(notices.empty()) return; line=notices.front(); notices.pop_front(); }
  const uint32_t text=scratch.data()+2048, params=scratch.data()+3200, out=scratch.data()+3300;
  const size_t n=std::min<size_t>(line.size(),299);
  for(size_t i=0;i<n;++i) api->write_u16(text+uint32_t(i)*2,uint16_t(uint8_t(line[i])));
  api->write_u16(text+uint32_t(n)*2,0);
  for(unsigned i=0;i<8;++i) api->write_u32(params+i*4,api->read_u32(0x82820C4C+i*4));
  api->write_u32(params+12,api->read_u8(0x8370EAF6) ? api->read_u32(0x8370F1F4) : api->read_u32(0x82820C4C+12));
  api->write_u32(out,0);
  showing_notice=true; ++applying_remote;
  scratch.ctx.r3.u64=out; scratch.ctx.r4.u64=text; scratch.ctx.r5.u64=params;
  scratch.Call(0x822E52C8);
  --applying_remote; showing_notice=false;
  ProbeLog(("Message shown: "+line).c_str());
  next=now+3500;
}
void ObjectiveHook(WmlContext* c,uint8_t* b) {
  if(connected.load() && hosting.load() && share_missions) {
    HudEvent e; e.kind=kHudObjective;
    const uint32_t text=uint32_t(api->get_r(c,3));
    if(!text) e.a[0]=1; else e.length=ReadWide(text,e.text,300);
    QueueHud(e);
  } else if(HoldLocalHud()) return;
  original_objective(c,b);
}
void HelpHook(WmlContext* c,uint8_t* b) {
  if(showing_notice) { original_help(c,b); return; }  // our own co-op messages stay here
  // Only mission help: the host's own prompts ("press E to open the door")
  // were shown on the joiner's screen too and never went away there.
  if(connected.load() && hosting.load() && share_missions && MissionActive()) {
    HudEvent e; e.kind=kHudHelp;
    const uint32_t params=uint32_t(api->get_r(c,5));
    if(Readable(params,32)) for(unsigned i=0;i<8;++i) e.a[i]=api->read_u32(params+i*4);
    e.length=ReadWide(uint32_t(api->get_r(c,4)),e.text,300);
    QueueHud(e);
  } else if(HoldLocalHud()) return;
  original_help(c,b);
}
void MissionEndHook(WmlContext* c,uint8_t* b) {
  if(connected.load() && hosting.load() && share_missions) {
    HudEvent e; e.kind=kHudMissionEnd; e.a[0]=uint32_t(api->get_r(c,3));
    e.length=ReadWide(uint32_t(api->get_r(c,4)),e.text,300);
    QueueHud(e);
    char line[80]; std::snprintf(line,sizeof(line),"Mission ended here (%s)",e.a[0]?"passed":"failed"); ProbeLog(line);
  } else if(HoldLocalHud()) { ProbeLog("This game's own mission end held back: the host's mission decides"); return; }
  original_mission_end(c,b);
}
// Groups of mission characters (create_group / group_create_hidden /
// destroy_group): the joiner's mission script waits for things that only
// happen in the host's game, so the host's group changes are made there too;
// the characters then follow the host's (taken over) or stay hidden.
WmlGuestFunction original_group_create=nullptr, original_group_destroy=nullptr;
void GroupCreateHook(WmlContext* c,uint8_t* b) {
  if(connected.load() && hosting.load() && share_missions) {
    HudEvent e; e.kind=kGroupCreate; e.a[0]=uint32_t(api->get_r(c,3)); e.a[1]=uint32_t(api->get_r(c,4)); QueueHud(e);
  }
  original_group_create(c,b);
}
void GroupDestroyHook(WmlContext* c,uint8_t* b) {
  if(connected.load() && hosting.load() && share_missions) {
    HudEvent e; e.kind=kGroupDestroy; e.a[0]=uint32_t(api->get_r(c,3)); QueueHud(e);
  }
  original_group_destroy(c,b);
}
// Joiner: the host's HUD and mission end, replayed here.
void ApplyHostHud(GuestScratch& scratch) {
  std::vector<HudEvent> in;
  { std::lock_guard lock(hud_mutex); in.swap(hud_in); }
  const uint32_t text=scratch.data()+2048, params=scratch.data()+3200, out=scratch.data()+3300;
  static uint64_t end_waiting_since=0;
  for(size_t index=0;index<in.size();++index) {
    const HudEvent& e=in[index];
    // The host skipped a cutscene and its mission ended: here the cutscene
    // still plays. End it first (as in the host's game), the mission end
    // follows on a later update (both in one go froze the joiner's game).
    if(e.kind==kHudMissionEnd && api->read_u32(0x82B3106C) && CutscenePlaying()) {
      const uint64_t t=GetTickCount64();
      if(!end_waiting_since) end_waiting_since=t;
      if(t-end_waiting_since<5000) {
        if(joiner_cutscene) {
          scratch.Call(0x821F9700);
          joiner_cutscene=false;
          ProbeLog("Host's mission ended during a cutscene here: cutscene ended first, the mission end follows");
        }
        std::lock_guard lock(hud_mutex);
        hud_in.insert(hud_in.begin(),in.begin()+index,in.end());
        return;
      }
      ProbeLog("Cutscene here still playing after 5 s: the host's mission end applied anyway");
    }
    if(e.kind==kHudMissionEnd) end_waiting_since=0;
    const uint32_t n=std::min<uint32_t>(e.length,299);
    for(uint32_t i=0;i<n;++i) api->write_u16(text+i*2,e.text[i]);
    api->write_u16(text+n*2,0);
    ++applying_remote;
    if(e.kind==kHudObjective) {
      scratch.ctx.r3.u64=e.a[0] ? 0 : text;
      scratch.Call(0x822EF440);
    } else if(e.kind==kHudHelp) {
      for(unsigned i=0;i<8;++i) api->write_u32(params+i*4,e.a[i]);
      // +12 is a handle from this game (as 822E54B0 picks it).
      api->write_u32(params+12,api->read_u8(0x8370EAF6) ? api->read_u32(0x8370F1F4) : api->read_u32(0x82820C4C+12));
      api->write_u32(out,0);
      scratch.ctx.r3.u64=out; scratch.ctx.r4.u64=text; scratch.ctx.r5.u64=params;
      scratch.Call(0x822E52C8);
    } else if(e.kind==kGroupCreate && mirror_mission) {
      scratch.ctx.r3.u64=e.a[0]; scratch.ctx.r4.u64=e.a[1]; scratch.Call(0x824C43B0);
      static unsigned logged=0;
      if(logged++<30) { char line[80]; std::snprintf(line,sizeof(line),"Host's mission group %08X turned on here too",e.a[0]); ProbeLog(line); }
    } else if(e.kind==kGroupDestroy && mirror_mission) {
      scratch.ctx.r3.u64=e.a[0]; scratch.Call(0x824C4B58);
    } else if(e.kind==kHudMissionEnd && api->read_u32(0x82B3106C)) {
      // (Not "while mirroring": the host's mission has already ended by the
      // time this arrives, which ended the mirroring and dropped the ending,
      // so this game's copy never finished and showed no rewards.)
      scratch.ctx.r3.u64=e.a[0]; scratch.ctx.r4.u64=e.length ? text : 0;
      scratch.Call(0x82215360);
      mission_end_applied_at=GetTickCount64();
      ProbeLog(e.a[0]?"Host's mission passed: ended here too":"Host's mission failed: ended here too");
    }
    --applying_remote;
    ++hud_applied;
  }
}
// Joiner: start the host's mission here too (82212728 by the mission's name
// hash, as the script's mission_start does), and hide this game's own mission
// characters: the host's are shown as copies.
bool drop_own_mission=true;  // mod.ini drop_own_mission
// Diagnostics: every character near this player that isn't a copy of the
// host's (name, flags, distance, ambient or not) - who stands there that the
// host doesn't have.
void LogNearbyOwn(const char* when,uint32_t player) {
  static unsigned lines=0;
  if(lines>=60 || !Readable(player,4252)) return;
  std::vector<uint32_t> listed;
  { const uint32_t head=api->read_u32(kAmbientPeds); uint32_t o=head;
    for(unsigned g=0;o && g<128 && Readable(o,4252);++g) { listed.push_back(o); o=api->read_u32(o+3600); if(o==head) break; } }
  const uint32_t avatar=ResolveHuman(avatar_local);
  const float px=api->read_f32(player+20), pz=api->read_f32(player+28);
  std::string l=std::string("Near here (")+when+"), not the host's:"; unsigned n=0, copies=0;
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t cur=api->read_u32(kObjectTable+12+index*16);
    if(!Readable(cur,4252) || api->read_u32(cur+72)!=1 || (api->read_u32(cur+68)&0xffff)!=index) continue;
    if(cur==player || cur==avatar) continue;
    const float dx=api->read_f32(cur+20)-px, dz=api->read_f32(cur+28)-pz;
    if(dx*dx+dz*dz>60.0f*60.0f) continue;
    if(SharedPedHostHandle(api->read_u32(cur+68))) { ++copies; continue; }
    if(n++>=14) continue;
    char name[46]="?"; ModelName(cur,name,sizeof(name)); char t[120];
    std::snprintf(t,sizeof(t)," %s [%08X %.0fm%s%s]",name,api->read_u32(cur+120),std::sqrt(dx*dx+dz*dz),
                  std::find(listed.begin(),listed.end(),cur)!=listed.end()?" ped":"",ResolveVehicle(api->read_u32(cur+2496))?" car":"");
    l+=t;
  }
  char t[64]; std::snprintf(t,sizeof(t),"; %u in all, %u copies of the host's",n,copies); l+=t;
  ProbeLog(l.c_str()); ++lines;
}
void UpdateMissionMirror(GuestScratch& scratch,uint32_t player,uint64_t now) {
  const uint32_t hash=host_mission_hash.load();
  const bool host_active=HostMissionActive(now) && hash;
  const bool mine=api->read_u32(0x82B3106C)!=0;
  static uint32_t tried=0;
  static uint64_t next_try=0;
  if(!host_active) tried=0;
  if(host_active && !mine && tried!=hash && now>=next_try && !CutscenePlaying()) {
    tried=hash; next_try=now+2000;
    ++applying_remote;
    scratch.ctx.r3.u64=hash;
    const bool called=scratch.Call(0x82212728);
    --applying_remote;
    const bool started=called && api->read_u32(0x82B3106C)!=0;
    char line[120];
    std::snprintf(line,sizeof(line),"Host's mission %08X: %s",hash,started ? "started here too" : "didn't start here (trying again)");
    ProbeLog(line);
    if(!started) tried=0;
  }
  const bool was=mirror_mission;
  mirror_mission=mine && host_active;
  {
    static uint64_t watch_until=0, next_watch=0; static const char* watch_what="";
    if(was && !mirror_mission) { watch_until=now+24000; next_watch=now; watch_what="after the mission"; }
    if(!was && mirror_mission) { watch_until=now+12000; next_watch=now; watch_what="mission start"; }
    if(now<watch_until && now>=next_watch) { next_watch=now+3000; LogNearbyOwn(watch_what,player); }
  }
  if(mirror_mission && !was) { mirror_since=now; ProbeLog("Running the host's mission here: its HUD and characters come from the host"); }
  // This game's own mission characters the mod hid (shown again for the
  // mission's cutscenes). When the host's mission is over here they are
  // removed: the last cutscene showed them and nothing hid them again (1.34:
  // all the enemies of the finished mission came back on the joiner).
  static std::vector<uint32_t> own_mission_seen;
  if(!mirror_mission) {
    static uint64_t sweep_until=0, next_sweep=0;
    if(was) sweep_until=now+8000;
    if((!own_mission_seen.empty() || now<sweep_until) && !CutscenePlaying() && now>=next_sweep) {
      next_sweep=now+1000;
      unsigned removed=0;
      for(uint32_t h:own_mission_seen) if(const uint32_t o=ResolveHuman(h))
        if(!SharedPedHostHandle(h) && o!=player && !ResolveVehicle(api->read_u32(o+2496))) { DestroyObject(scratch,h); ++removed; }
      own_mission_seen.clear();
      // Also everything else of this game's own that the mission left behind
      // (let-go characters, cutscene extras): not in the ambient list.
      std::vector<uint32_t> listed;
      { const uint32_t head=api->read_u32(kAmbientPeds); uint32_t o=head;
        for(unsigned g=0;o && g<128 && Readable(o,4252);++g) { listed.push_back(o); o=api->read_u32(o+3600); if(o==head) break; } }
      for(uint32_t index=0;index<4096;++index) {
        const uint32_t cur=api->read_u32(kObjectTable+12+index*16);
        if(!Readable(cur,4252) || api->read_u32(cur+72)!=1 || (api->read_u32(cur+68)&0xffff)!=index) continue;
        if(std::find(listed.begin(),listed.end(),cur)!=listed.end() || !OwnDisposable(cur,player)) continue;
        DestroyObject(scratch,api->read_u32(cur+68)); ++removed;
      }
      if(removed) { char line[120]; std::snprintf(line,sizeof(line),"Mission over here: %u of this game's own mission characters removed",removed); ProbeLog(line); }
    }
    return;
  }
  // Hidden ones come back for a cutscene: they may be in it (hiding right as
  // it began left it empty). No hiding during one and for 5 s after.
  static std::vector<uint32_t> hidden_by_us;
  static std::unordered_map<uint32_t,uint64_t> first_seen;
  static uint64_t cutscene_at=0;
  if(CutscenePlaying()) {
    if(!hidden_by_us.empty()) {
      for(uint32_t h:hidden_by_us) if(const uint32_t o=ResolveHuman(h)) if(!SharedPedHostHandle(h)) SetCharacterHidden(scratch,o,false);
      hidden_by_us.clear();
      ProbeLog("Cutscene: this game's own mission characters shown for it");
    }
    first_seen.clear(); cutscene_at=now; return;
  }
  if(cutscene_at && now-cutscene_at<500) return;
  static uint64_t next_scan=0;
  if(now<next_scan) return;
  next_scan=now+250;
  // Mission characters this game made itself (not ambient, not copies, not
  // cutscene actors, not in cars): hidden.
  std::vector<uint32_t> listed;
  { const uint32_t head=api->read_u32(kAmbientPeds); uint32_t o=head;
    for(unsigned g=0;o && g<128 && Readable(o,4252);++g) { listed.push_back(o); o=api->read_u32(o+3600); if(o==head) break; } }
  const uint32_t avatar=ResolveHuman(avatar_local);
  static unsigned hidden_total=0;
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t cur=api->read_u32(kObjectTable+12+index*16);
    if(!Readable(cur,4252) || api->read_u32(cur+72)!=1 || (api->read_u32(cur+68)&0xffff)!=index) continue;
    if(cur==player || cur==avatar) continue;
    const uint32_t flags=api->read_u32(cur+120);
    // Any of this game's own cutscene actors still standing 2 s after the
    // cutscene, whatever its other flags (1.44 removed Troy, Dex and Johnny
    // Gat but Julius stayed on the stairs).
    if((flags&0x20000000u) && !(cutscene_at && now-cutscene_at<2000) && !SharedPedHostHandle(api->read_u32(cur+68)) &&
       std::find(listed.begin(),listed.end(),cur)==listed.end() && !ResolveVehicle(api->read_u32(cur+2496))) {
      static unsigned logged=0;
      if(logged++<30) { char name[46]="?"; ModelName(cur,name,sizeof(name)); char line[120]; std::snprintf(line,sizeof(line),"Own cutscene character left standing removed: %s (flags %08X)",name,flags); ProbeLog(line); }
      DestroyObject(scratch,api->read_u32(cur+68));
      continue;
    }
    // Cutscene actors (0x20000000/0x08000000) are left alone during a
    // cutscene and 2 s after; ones still standing there after that are this
    // game's own mission's (1.37: Johnny Gat, Dex, Julius and gang members on
    // the stairs at the next mission's start, which the host doesn't have).
    if((flags&0xD0000000u) || ((flags&0x28000000u) && cutscene_at && now-cutscene_at<2000)) continue;
    if(SharedPedHostHandle(api->read_u32(cur+68))) continue;
    if(std::find(listed.begin(),listed.end(),cur)!=listed.end()) continue;
    if(ResolveVehicle(api->read_u32(cur+2496))) continue;
    const uint32_t handle=api->read_u32(cur+68);
    // This game's own cutscene actors left standing (Johnny Gat, Dex, Julius,
    // gang members at the next mission's start, T-posing, not in the host's
    // game): their 0x08000000 comes from the cutscene and doesn't hide them,
    // so hiding them did nothing and they stood there until removed 5 s
    // later. Removed at once.
    if(flags&0x20000000u) {
      static unsigned logged=0;
      if(logged++<30) { char name[46]="?"; ModelName(cur,name,sizeof(name)); char line[120]; std::snprintf(line,sizeof(line),"Own cutscene character left standing removed: %s (flags %08X)",name,flags); ProbeLog(line); }
      DestroyObject(scratch,handle);
      continue;
    }
    // Hidden right away (a copy of the host's can still take it over later):
    // shown for a few seconds, the extra attackers weren't the host's.
    SetCharacterHidden(scratch,cur,true);
    hidden_by_us.push_back(handle);
    if(std::find(own_mission_seen.begin(),own_mission_seen.end(),handle)==own_mission_seen.end()) own_mission_seen.push_back(handle);
    if(hidden_total++<20) { char name[46]="?"; ModelName(cur,name,sizeof(name)); char line[100]; std::snprintf(line,sizeof(line),"Own mission character hidden: %s",name); ProbeLog(line); }
  }
  // This game's own mission characters stay hidden but keep their models in
  // the people-model memory (1.30 log: 3 of the 10 loaded models were only
  // theirs, while 4 of the host's waited and those people were invisible
  // through the fight). One hidden 6 s whose model the host doesn't have is
  // removed while the host's models wait (mod.ini drop_own_mission = 0: off).
  static std::unordered_map<uint32_t,uint64_t> hidden_at;
  static unsigned own_mission_removed=0;
  if(drop_own_mission && now-host_models_time<3000 && !host_models.empty()) {
    unsigned waiting=0, guard=0;
    const uint32_t group=kStreamTable, head=api->read_u32(group+48);
    for(uint32_t e=head;e && Readable(e,72) && guard++<256;) { if(!api->read_u32(e)) ++waiting; e=api->read_u32(e+28); if(e==head) break; }
    for(uint32_t h:hidden_by_us) {
      const uint32_t o=ResolveHuman(h);
      if(!o || SharedPedHostHandle(h)) continue;
      auto at=hidden_at.find(h);
      if(at==hidden_at.end()) { hidden_at[h]=now; continue; }
      // Generic ones after 1 s; named ones (story characters) only after 5 s
      // hidden and while the host's models wait for room.
      if(now-at->second<1000) continue;
      if(!OwnDisposable(o,player) && !(waiting && now-at->second>5000 && OwnRemovable(o,player))) continue;
      const uint32_t desc=api->read_u32(o+228);
      const uint32_t id=Readable(desc,612) ? api->read_u32(desc+608) : 0;
      if(!id || std::find(host_models.begin(),host_models.end(),id)!=host_models.end()) continue;
      (void)waiting;
      char name[46]="?"; ModelName(o,name,sizeof(name));
      DestroyObject(scratch,h);
      hidden_at.erase(at);
      if(own_mission_removed++<30) { char line[140]; std::snprintf(line,sizeof(line),"Own mission character removed: %s (hidden, its model kept the host's people's models from loading)",name); ProbeLog(line); }
      break;   // one per scan
    }
    if(hidden_at.size()>512) hidden_at.clear();
  }
}

// Joiner: its game never pauses the world during a session (the pause menu,
// prompts and objective screens still open). 8220C778 is the game's pause
// (a counter at 0x8370DAE4; the first pause stops the world), 8220C868 the
// unpause, which does nothing while the counter is 0, so a skipped pause
// leaves nothing to undo.
WmlGuestFunction original_pause=nullptr;
void PauseHook(WmlContext* c,uint8_t* b) {
  if(connected.load() && running.load() && !hosting.load()) {
    static unsigned logged=0;
    if(logged++<20) { char line[100]; std::snprintf(line,sizeof(line),"Pause skipped (the world keeps running for the other player), from %08X",api->get_lr(c)); ProbeLog(line); }
    return;
  }
  original_pause(c,b);
}

// Joiner: this game's copy of the mission waits on its intro prompt (the
// mission start sequence, [0x82B8BE88] state 3, confirmed with 822230B8 when
// the confirm action is pressed, see 82223610). Its mission only really began
// when the player pressed it, later than the host's, and it then reset its
// groups (the host's characters taken over were lost). Confirmed here at once.
void UpdateMissionIntro(GuestScratch& scratch) {
  const uint32_t intro=api->read_u32(0x82B8BE88);
  if(!Readable(intro,16) || api->read_u32(intro+12)!=3) return;
  scratch.ctx.r3.u64=intro;
  ++applying_remote;
  scratch.Call(0x822230B8);
  --applying_remote;
  static unsigned logged=0;
  if(logged++<10) ProbeLog("Mission intro prompt confirmed here (the host's mission is already running)");
}

bool HostPauseHold(uint64_t now) {
  return connected.load() && !hosting.load() && host_paused.load() && now-host_state_seen.load()<3000;
}
// Joiner: while the host has the game paused, this player can't move
// (player_controls_disable / _enable, 824D9228 / 824D92F0, which take no
// arguments); the host's traffic and people stand still because the host's
// world is stopped.
bool controls_held=false;
void UpdateHostPause(GuestScratch& scratch,uint64_t now) {
  // (diagnostics) the game's sound pause count (82100C18(-1) raises it, as
  // the pause does): sound went strange for the joiner after prompts.
  { static int32_t last=0; const int32_t v=int32_t(api->read_u32(0x8370CF84));
    if(v!=last) { static unsigned n=0; if(n++<30) { char line[100]; std::snprintf(line,sizeof(line),"Sound pause count now %d (was %d)",v,last); ProbeLog(line); } last=v; } }
  // Not while this player's own pause menu is open: switching the controls
  // on under the open menu left it stuck on screen (it couldn't be closed,
  // while the player walked around). Applied once the menu is closed.
  if(api->read_u32(0x839E0DF8)==0x82FFB84Cu) return;
  const bool hold=HostPauseHold(now);
  if(hold && !controls_held) {
    scratch.Call(0x824D9228); controls_held=true;
    ProbeLog("Host paused: controls off until the host resumes");
  } else if(!hold && controls_held) {
    scratch.Call(0x824D92F0); controls_held=false;
    ProbeLog("Host resumed: controls back on");
  }
}

void MissionStartHook(WmlContext* c,uint8_t* b) {
  if(share_missions && connected.load() && running.load() && !hosting.load() && !applying_remote) {
    static unsigned logged=0;
    if(logged++<10) ProbeLog("Mission start blocked: in co-op the host's missions are played");
    api->set_r(c,3,0);
    return;
  }
  original_mission_start(c,b);
}

void UpdateBody(WmlContext* raw) {
  std::unique_lock game_lock(game_mutex, std::try_to_lock);
  if (!game_lock.owns_lock()) return;
  if (update_ticks.fetch_add(1)==0) ProbeLog("Verified recurring game update hook reached: 82209E30");
  const bool toggle_test=test_toggle.exchange(false);
  const bool toggle_copy=copy_toggle.exchange(false);
  static uint64_t next_debug_check=0;
  bool debug=false;
  if(MonoMs()>=next_debug_check) {
    next_debug_check=MonoMs()+250;
    debug=GetFileAttributesA((std::string(self->folder)+"\\debug_cmd.txt").c_str())!=INVALID_FILE_ATTRIBUTES;
  }
  if (!running.load() && !cleanup_requested.load() && !toggle_test && !toggle_copy && !debug && watches.empty()) return;
  const uint32_t sp=static_cast<uint32_t>(api->get_r(raw,1));
  if (sp<0x5000 || !Readable(sp-0x5000,0x5000)) return;
  GuestScratch scratch(raw);
  const uint64_t now=MonoMs();
  if(cleanup_requested.exchange(false) || !connected.load()) {
    ShowMissionMarkers(scratch);
    if(controls_held) { scratch.Call(0x824D92F0); controls_held=false; ProbeLog("Session over: controls back on"); }
    if(replica_local) DestroyReplica(scratch);
    if(avatar_local) DestroyAvatar(scratch);
    if(!avatar_local && proxy_local) DestroyRemoteCar(scratch);
  }
  // The other player's character is a real player object (from the game's
  // player pool). After a cutscene the game can take it for the local player
  // (both players saw only the other one's character and couldn't move):
  // the local player is put back.
  {
    static uint32_t own_player=0;
    const uint32_t cur=api->read_u32(0x8309ABEC);
    const uint32_t av=avatar_local ? ResolveHuman(avatar_local) : 0;
    if(connected.load() && av && cur==av && own_player && own_player!=av && Readable(own_player,4252) && api->read_u32(own_player+72)==1) {
      api->write_u32(0x8309ABEC,own_player);
      static unsigned logged=0;
      if(logged++<10) { char line[140]; std::snprintf(line,sizeof(line),"The game took the other player's character for ours (%08X): our player %08X put back",cur,own_player); ProbeLog(line); }
    } else if(Readable(cur,4252) && cur!=av) own_player=cur;
    static uint32_t last_head=0; static bool was_cut=false;
    const bool cut=CutscenePlaying();
    const uint32_t head=api->read_u32(0x8309AC54);
    if(connected.load() && (head!=last_head || (was_cut && !cut))) {
      static unsigned logged=0;
      if(logged++<20) { char line[160]; std::snprintf(line,sizeof(line),"Players: local %08X, active list head %08X, other player %08X%s",api->read_u32(0x8309ABEC),head,av,(was_cut&&!cut)?" (cutscene just ended)":""); ProbeLog(line); }
    }
    last_head=head; was_cut=cut;
  }
  const uint32_t player=api->read_u32(0x8309ABEC);
  if(traffic_shared && (!connected.load() || !running.load()) && Readable(player,4252)) UpdateSharedTraffic(scratch,player,now);
  if(peds_shared && (!connected.load() || !running.load()) && Readable(player,4252)) UpdateSharedPeds(scratch,player,now);
  if (!Readable(player,4252) || api->read_u32(player+72)!=1) return;
  local_x=api->read_f32(player+20); local_y=api->read_f32(player+24); local_z=api->read_f32(player+28);
  CaptureLocalPose(player);
  // Host: which people models are loaded here, once a second. The joiner
  // loads the same set ahead (its copies waited for their models, showed
  // lookalikes until close by, and popped in).
  if(hosting.load() && connected.load()) {
    static uint64_t next_models=0;
    if(now>=next_models) {
      next_models=now+1000;
      const uint32_t group=kStreamTable;
      if(Readable(group,204)) {
        std::vector<uint32_t> ids; unsigned guard=0;
        const uint32_t head=api->read_u32(group+48);
        for(uint32_t e=head;e && Readable(e,72) && guard++<256;) {
          const uint32_t id=api->read_u32(e+4);
          if(api->read_u32(e) && id<0x10000 && ids.size()<14) ids.push_back(id);
          e=api->read_u32(e+28); if(e==head) break;
        }
        for(auto it=host_effects.begin();it!=host_effects.end();) {
          if(!ResolveHuman(it->first)) { it=host_effects.erase(it); continue; }
          if(IsSentPed(it->first)) {
            Event fx{kEventPedEffect,{it->first,it->second.kind,it->second.r5,it->second.effect,it->second.data},0};
            QueueEvent(fx); ++effects_sent;
          }
          ++it;
        }
        Event ev{kEventHostModels,{},0};
        ev.a[0]=uint32_t(ids.size());
        for(unsigned i=0;i<ids.size();++i) ev.a[1+i/2]|=(i&1)?ids[i]:(ids[i]<<16);
        QueueEvent(ev);
        static uint64_t next_models_log=0;
        if(now>=next_models_log) {
          next_models_log=now+10000;
          std::string l="People models here (host):"; char t[16]; unsigned g2=0;
          for(uint32_t e=head;e && Readable(e,72) && g2++<256;) { unsigned refs=0; for(unsigned k=0;k<7;++k) refs+=api->read_u32(e+44+k*4);
            std::snprintf(t,sizeof(t)," %X%s(%u)",api->read_u32(e+4),api->read_u32(e)?"":"w",refs); l+=t; e=api->read_u32(e+28); if(e==head) break; }
          ProbeLog(l.c_str()); LogPeopleSlots("  slots here:");
        }
      }
    }
  }
  if(toggle_test) ToggleTestActor(scratch);
  if(toggle_copy) TogglePlayerCopy(scratch,player);
  if(debug) RunDebugCommands(scratch,player);
  static uint64_t next_worn=0;
  if(running.load() && now>=next_worn) {
    next_worn=now+1000;
    uint8_t worn[256];
    const unsigned length=ReadWorn(scratch,player,worn,sizeof(worn));
    float morphs[128];
    const unsigned morph_count=ReadMorphs(player,morphs);
    std::lock_guard lock(pose_mutex);
    local_worn.assign(worn,worn+length);
    if(morph_count==128 && std::vector<float>(morphs,morphs+128)!=local_morphs)
      ProbeLog(("Local body sliders: "+MorphSummary(morphs,128)).c_str());
    local_morphs.assign(morphs,morphs+morph_count);
    uint8_t extras[40];
    const unsigned extras_length=ReadExtras(scratch,player,extras);
    if(extras_length && std::vector<uint8_t>(extras,extras+extras_length)!=local_extras) {
      ProbeLog(("Local appearance colours ("+std::to_string(extras_length)+" bytes): "+HexBytes(extras,extras_length)).c_str());
    }
    if(extras_length) local_extras.assign(extras,extras+extras_length);
  }
  ShowNotices(scratch);
  WatchActors(now);
  if(!running.load()) { diagnostics_done=false; return; }
  if(!diagnostics_done) { diagnostics_done=true; LogPlayerDiagnostics(scratch,player); }
  local_mission=MissionActive(); local_cutscene=CutscenePlaying();
  if(dump_request.exchange(false)) DumpPeople(player);
  {
    // Learning which of the mission globals really mark a mission: log changes.
    static uint32_t g1=0xFFFFFFFF, g2=0xFFFFFFFF, g3=0xFFFFFFFF;
    const uint32_t a=api->read_u32(0x82B3106C), b=api->read_u32(0x82B31080), c=api->read_u32(0x827D578C);
    if(a!=g1 || b!=g2 || c!=g3) {
      g1=a; g2=b; g3=c;
      char line[120]; std::snprintf(line,sizeof(line),"Mission globals now %08X %08X %08X",a,b,c); ProbeLog(line);
    }
  }
  {
    // Host: its clock goes out with the mission state. Joiner: takes the
    // host's clock, so the time of day and lighting match.
    std::lock_guard lock(time_mutex);
    std::memcpy(local_time,api->guest_pointer(kGameTime),16);
    if(host_time_new && connected.load() && !hosting.load()) {
      std::memcpy(api->guest_pointer(kGameTime),host_time,16);
      host_time_new=false;
    }
  }
  {
    static bool logged_mission=false;
    if(hosting.load() && connected.load() && local_mission.load()!=logged_mission) {
      logged_mission=local_mission.load();
      ProbeLog(logged_mission?"Mission started here; the other player is brought over":"Mission ended here");
    }
  }
  if(connected.load()) UpdateAvatar(scratch,now);
  if(connected.load() && !hosting.load()) UpdateMissionFollow(scratch,player,now);
  if(connected.load() && !hosting.load() && share_missions) HideMissionMarkers(scratch,now);
  if(!hosting.load()) UpdateHostPause(scratch,now);
  if(connected.load()) UnstickPlayer(scratch,player);
  if(connected.load() && !hosting.load() && share_missions && host_mission.load()) UpdateMissionIntro(scratch);
  if(connected.load() && !hosting.load()) ApplyHostHud(scratch);
  if(connected.load() && !hosting.load()) UpdateMissionMirror(scratch,player,now);
  if(connected.load() && !hosting.load()) UpdateSharedCutscene(scratch,now);
  if(connected.load() && hosting.load()) CaptureTraffic(scratch,player,now);
  // This player's own car: its look goes to the other game (whose copy of it
  // is made from the type alone).
  if(connected.load()) {
    const uint32_t own=ResolveVehicle(api->read_u32(player+2496));
    if(own && api->read_u32(own+68)!=proxy_local) ShareCarLook(scratch,own,now);
  }
  if(connected.load() && hosting.load() && share_peds) CapturePeds(player,now);
  mod_stage.store("shared traffic");
  if(!hosting.load()) UpdateSharedTraffic(scratch,player,now);
  mod_stage.store("shared people");
  if(!hosting.load()) { UpdateSharedPeds(scratch,player,now); UpdatePedEffects(scratch,now); }
  mod_stage.store("events");
  if(connected.load() && !hosting.load()) UpdateReplica(scratch,now);
  if(connected.load()) ApplyEvents(scratch,now);
  static uint64_t next=0;
  static uint32_t cursor=0;
  if (now<next) return;
  next=now+50;
  if(replica_local && now-replica_seen>3000) DestroyReplica(scratch);
  const uint32_t data=scratch.data(), length=data+1200;
  if (hosting.load() && !share_peds) {
    if(!ResolveHuman(tracked_host)) tracked_host=0;
    unsigned stages[4]{};
    // Check the tracked NPC first; search at most 512 slots per update.
    const uint32_t tracked_index=tracked_host&0xffff;
    for (unsigned scan=0;scan<(tracked_host?513u:512u);++scan) {
      if(tracked_host && scan>0) break; // tracked NPC is only checked directly
      const uint32_t index=tracked_host&&scan==0 ? tracked_index : cursor++%4096;
      const uint32_t object=api->read_u32(kObjectTable+12+index*16);
      if (!NpcReady(object,index,player,stages)) { if(scan==0) tracked_host=0; continue; }
      const uint32_t handle=api->read_u32(object+68);
      if(tracked_host && tracked_host!=handle) continue;
      const float dx=api->read_f32(object+20)-local_x.load();
      const float dy=api->read_f32(object+24)-local_y.load();
      const float dz=api->read_f32(object+28)-local_z.load();
      if(dx*dx+dy*dy+dz*dz>40*40) { if(tracked_host==handle)tracked_host=0; continue; }
      tracked_host=handle;
      std::memset(api->guest_pointer(data),0,1248);
      api->write_u16(length,0);
      const uint32_t flags=api->read_u32(object+120);
      scratch.ctx.r3.u64=api->read_u32(object+68);
      scratch.ctx.r4.u64=data; scratch.ctx.r5.u64=length; scratch.ctx.r6.u64=0;
      const bool called=scratch.Call(0x823A7AB0);
      // Native serializer sets the network-owned flag as a side effect.
      // The host must retain ownership after this readout.
      api->write_u32(object+120,flags);
      coop::NativePacket p;
      p.host_handle=api->read_u32(object+68);
      p.payload_size=api->read_u16(length);
      if (called && p.payload_size<=coop::kNativeCapacity) {
        std::memcpy(p.payload,api->guest_pointer(data),p.payload_size);
        if (coop::ValidateNpc(p,sizeof(p))) {
          p.sequence=native_encoded.fetch_add(1)+1;
          p.time=uint32_t(MonoMs());
          { std::lock_guard lock(probe_mutex); native_out=p; }
          if (p.sequence<=16 || p.sequence%50==0) {
            char line[400];
            std::snprintf(line,sizeof(line),"Native NPC encoded: handle=%08X bytes=%u model=%s",p.host_handle,p.payload_size,p.payload+28);
            ProbeLog(line);
          }
          if (p.sequence<=16) {
            const std::string path=std::string(self->folder)+"\\native-npc-"+
              std::to_string(GetCurrentProcessId())+"-"+std::to_string(p.sequence)+".bin";
            std::ofstream capture(path,std::ios::binary);
            capture.write(reinterpret_cast<const char*>(&p),sizeof(p));
          }
        } else {
          char line[120];
          std::snprintf(line,sizeof(line),"Native NPC invalid payload: handle=%08X bytes=%u",p.host_handle,p.payload_size);
          ProbeLog(line);
        }
      }
      return;
    }
    static uint64_t warn=0;
    if(now>=warn) {
      char line[180];
      std::snprintf(line,sizeof(line),"NPC eligibility: humans=%u named=%u skeleton=%u animation-ready=%u",stages[0],stages[1],stages[2],stages[3]);
      ProbeLog(line); warn=now+3000;
    }
  } else {
    coop::NativePacket p;
    { std::lock_guard lock(probe_mutex); if(!native_pending)return; p=native_in; native_pending=false; }
    // Resolve the model by name before constructing our own actor. Successful
    // 0.3 live captures verified the serializer and this lookup independently.
    std::memcpy(api->guest_pointer(data),p.payload,p.payload_size);
    // Disable the native lookup's default-model fallback: a nonzero result
    // must mean this exact host model exists, not that a substitute exists.
    scratch.ctx.r3.u64=data+28; scratch.ctx.r4.u64=0;
    const bool available=scratch.Call(0x82114928) && scratch.ctx.r3.u32!=0;
    const uint32_t definition=available?scratch.ctx.r3.u32:0;
    if(available) ++native_available;
    if(native_received.load()<=16 || p.sequence%50==0) {
      char line[400];
      std::snprintf(line,sizeof(line),"Native NPC received: handle=%08X bytes=%u model=%s definition=%08X",p.host_handle,p.payload_size,p.payload+28,available?scratch.ctx.r3.u32:0);
      ProbeLog(line);
    }
    if(available) ApplyReplica(scratch,p,definition);
  }
}

void SetStatus(const char* value) {
  std::lock_guard lock(status_mutex);
  status = value;
}

std::string GetStatus() {
  std::lock_guard lock(status_mutex);
  return status;
}

std::string ReadJoinIp() {
  std::ifstream file(std::string(self->folder) + "\\join_ip.txt");
  std::string value;
  std::getline(file, value);
  if (value.empty()) return "127.0.0.1";
  return value;
}

std::string LocalIp() {
  char name[256]{};
  if (gethostname(name, sizeof(name)) != 0) return "127.0.0.1";
  addrinfo hints{};
  hints.ai_family = AF_INET;
  addrinfo* result = nullptr;
  if (getaddrinfo(name, nullptr, &hints, &result) != 0 || !result) return "127.0.0.1";
  char text[INET_ADDRSTRLEN]{};
  auto* address = reinterpret_cast<sockaddr_in*>(result->ai_addr);
  inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
  freeaddrinfo(result);
  return text[0] ? text : "127.0.0.1";
}

void CloseSocket() {
  if (sock != INVALID_SOCKET) {
    closesocket(sock);
    sock = INVALID_SOCKET;
  }
}

// Transport: everything the session sends or receives goes through these two,
// so the direct UDP socket can be swapped for Epic Online Services P2P (the
// lobby / join-code path) without touching the session code. A remote player
// is always identified by a sockaddr_in; the EOS backend will hand out a
// stand-in address per remote player.
// EOS P2P packets are at most 1170 bytes: anything bigger is logged once per
// packet type so it can be split before EOS is switched on.
constexpr int kEosMaxPacket = 1170;
// Online session (Epic Online Services): set by StartSession before the
// network thread starts.
std::atomic<bool> use_eos{false};
std::string eos_join_code;
std::string EosDisplayName();
static_assert(sizeof(PedPacket) <= kEosMaxPacket && sizeof(TrafficPacket) <= kEosMaxPacket && sizeof(HudPacket) <= kEosMaxPacket &&
              sizeof(PosePacket) <= kEosMaxPacket && sizeof(TrafficNamesPacket) <= kEosMaxPacket && sizeof(PedLookPacket) <= kEosMaxPacket &&
              sizeof(EventPacket) <= kEosMaxPacket && sizeof(CutscenePacket) <= kEosMaxPacket && sizeof(CarLookPacket) <= kEosMaxPacket &&
              sizeof(MorphPacket) <= kEosMaxPacket && sizeof(WornPacket) <= kEosMaxPacket && sizeof(ExtrasPacket) <= kEosMaxPacket &&
              sizeof(coop::NativePacket) <= kEosMaxPacket, "a co-op packet is too big for EOS P2P (1170 bytes)");
int NetSend(const void* data, int length, const sockaddr_in& to) {
  if (length > kEosMaxPacket) {
    static std::mutex m; static std::vector<uint32_t> seen;
    uint32_t tag = 0; std::memcpy(&tag, data, length >= 4 ? 4 : 0);
    std::lock_guard lock(m);
    if (std::find(seen.begin(), seen.end(), tag) == seen.end()) {
      seen.push_back(tag);
      char line[128]; char t[5]{}; std::memcpy(t, &tag, 4);
      for (char& c : t) if (c && (c < 32 || c > 126)) c = '?';
      std::snprintf(line, sizeof(line), "Transport: packet '%s' is %d bytes (EOS limit %d)", t, length, kEosMaxPacket);
      ProbeLog(line);
    }
  }
#ifdef WHOMPAYS_EOS
  if (use_eos.load()) return eos::Send(data, length, to);
#endif
  return sendto(sock, reinterpret_cast<const char*>(data), length, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}
int NetRecv(char* buffer, int capacity, sockaddr_in& from) {
#ifdef WHOMPAYS_EOS
  if (use_eos.load()) return eos::Receive(buffer, capacity, from);
#endif
  int from_len = sizeof(from);
  return recvfrom(sock, buffer, capacity, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
}

constexpr char kHello[] = "WHOMPAYS_COOP_V3_HELLO";
constexpr char kAck[] = "WHOMPAYS_COOP_V3_ACK";
bool SamePeer(const sockaddr_in& a,const sockaddr_in& b) {
  return a.sin_addr.s_addr==b.sin_addr.s_addr && a.sin_port==b.sin_port;
}
void NetworkLoop(sockaddr_in target, bool host) {
  sockaddr_in peer=target;
  bool has_peer=!host;
  auto next_hello=std::chrono::steady_clock::now();
  auto next_snapshot=next_hello;
  auto next_worn=next_hello;
  auto next_traffic=next_hello;
  auto last_peer=next_hello;
  uint32_t last_sent=0,last_received=0,last_pose=0,last_event=0,event_sequence=0;
  struct Pending { EventPacket packet; int left; };
  struct HudPending { HudPacket packet; int left; };
  std::vector<HudPending> hud_resend;
  uint32_t last_hud=0;
  uint32_t last_chat_in=0, chat_seq_out=0;
  std::vector<Pending> resend;
  char packet[1500]{};
#ifdef WHOMPAYS_EOS
  std::string eos_shown;
  if (use_eos.load()) {
    eos::log_fn = [](const char* line) { ProbeLog(line); };
    eos::Begin(std::string(self->folder), host, eos_join_code, EosDisplayName(), 2);
  }
#endif
  while (!stop_thread.load()) {
#ifdef WHOMPAYS_EOS
    if (use_eos.load() && !connected.load()) {
      const std::string t = eos::Text();
      if (t != eos_shown) {
        eos_shown = t;
        if (!t.empty()) SetStatus(t.c_str());
        if (t.rfind("Online. Join code: ", 0) == 0)
          Notify("Co-op is online. Join code: " + eos::Code() + " (also under Pause > Options). Your friend picks Join Co-op and types it.");
        else if (t.rfind("Online failed: ", 0) == 0)
          Notify("Co-op couldn't go online: " + t.substr(15));
      }
    }
#endif
    sockaddr_in from{};
    int from_len=sizeof(from);
    (void)from_len;
    int count=NetRecv(packet,sizeof(packet),from);
    if(count>0) ++packets_in;
    auto now=std::chrono::steady_clock::now();
    if(count>0) {
      if(host && count==sizeof(kHello)-1 && !std::memcmp(packet,kHello,count) &&
         (!has_peer || SamePeer(peer,from))) {
        peer=from; has_peer=true; last_peer=now;
        NetSend(kAck,sizeof(kAck)-1,peer);
        if(!connected.exchange(true)) { ProbeLog("V3 client connected"); Notify("Co-op: your friend joined your game."); }
        SetStatus("Connected. Other player and one NPC replica (test build).");
      } else if(has_peer && SamePeer(peer,from)) {
        if(!host && count==sizeof(kAck)-1 && !std::memcmp(packet,kAck,count)) {
          last_peer=now;
          if(!connected.exchange(true)) { ProbeLog("V3 host acknowledged connection"); Notify("Co-op: connected. You're in your friend's game."); }
          SetStatus("Connected. Other player and one NPC replica (test build).");
        } else if(connected.load() && count==sizeof(ChatPacket) && !std::memcmp(packet,"WCH1",4)) {
          ChatPacket c; std::memcpy(&c,packet,sizeof(c));
          c.name[15]=0; c.text[159]=0;
          if(int32_t(c.sequence-last_chat_in)>0) {
            last_chat_in=c.sequence; last_peer=now;
            std::string name(c.name), text(c.text);
            for(char& ch:name) if(uint8_t(ch)<32) ch=' ';
            for(char& ch:text) if(uint8_t(ch)<32) ch=' ';
            if(name.empty()) name="Player";
            std::lock_guard lock(chat_mutex);
            chat_in.push_back(name+"\t"+text);
            while(chat_in.size()>50) chat_in.pop_front();
          }
        } else if(connected.load() && count==sizeof(PosePacket) && !std::memcmp(packet,"WPP1",4)) {
          PosePacket p; std::memcpy(&p,packet,sizeof(p));
          bool valid=p.sequence>last_pose;
          for(float v:p.pos) valid=valid && std::isfinite(v) && std::abs(v)<100000;
          for(float v:p.rows) valid=valid && std::isfinite(v) && std::abs(v)<=1.5f;
          valid=valid && p.look_length<=sizeof(p.look) && p.vehicle>=-1 && p.vehicle<256 && p.seat<16;
          for(float v:p.vpos) valid=valid && std::isfinite(v) && std::abs(v)<100000;
          for(float v:p.vrows) valid=valid && std::isfinite(v) && std::abs(v)<=1.5f;
          for(float v:p.vvel) valid=valid && std::isfinite(v) && std::abs(v)<1000;
          for(float v:p.vang) valid=valid && std::isfinite(v) && std::abs(v)<100;
          valid=valid && p.vowner<=1;
          if(valid) {
            last_pose=p.sequence; last_peer=now;
            p.model[63]=0;
            for(char& c:p.model) if(c && (c<32 || c>126)) c='_';
            PlayerPose pose; pose.sequence=p.sequence;
            std::memcpy(pose.pos,p.pos,sizeof(pose.pos));
            std::memcpy(pose.rows,p.rows,sizeof(pose.rows));
            std::memcpy(pose.model,p.model,sizeof(pose.model));
            pose.look_length=p.look_length; pose.time=p.time;
            std::memcpy(pose.look,p.look,sizeof(pose.look));
            pose.vehicle=p.vehicle; pose.seat=p.seat;
            std::memcpy(pose.vpos,p.vpos,sizeof(pose.vpos));
            std::memcpy(pose.vrows,p.vrows,sizeof(pose.vrows));
            std::memcpy(pose.vvel,p.vvel,sizeof(pose.vvel));
            std::memcpy(pose.vang,p.vang,sizeof(pose.vang));
            pose.vowner=p.vowner; pose.vid=p.vid; pose.flags=p.flags;
            { std::lock_guard lock(pose_mutex); remote_pose=pose; remote_pose_new=true; }
            remote_x=p.pos[0]; remote_y=p.pos[1]; remote_z=p.pos[2];
          }
        } else if(!host && connected.load() && count==sizeof(TrafficPacket) && !std::memcmp(packet,"WTR1",4)) {
          TrafficPacket t; std::memcpy(&t,packet,sizeof(t));
          if(t.part<kTrafficParts && t.count<=kTrafficPerPacket) {
            bool valid=true;
            for(unsigned i=0;i<t.count;++i) {
              for(float v:t.cars[i].pos) valid=valid && std::isfinite(v) && std::abs(v)<100000;
              for(float v:t.cars[i].rows) valid=valid && std::isfinite(v) && std::abs(v)<=1.5f;
              for(float v:t.cars[i].vel) valid=valid && std::isfinite(v) && std::abs(v)<1000;
              for(float v:t.cars[i].ang) valid=valid && std::isfinite(v) && std::abs(v)<100;
            }
            if(valid) {
              std::lock_guard lock(traffic_mutex);
              TrafficIn& in=traffic_in[t.part];
              if(int32_t(t.sequence-in.sequence)>0) {
                in.sequence=t.sequence; in.time=t.time; in.received=MonoMs();
                in.cars.assign(t.cars,t.cars+t.count);
              }
            }
          }
          last_peer=now;
        } else if(!host && connected.load() && count==sizeof(HudPacket) && !std::memcmp(packet,"WHU1",4)) {
          HudPacket hp; std::memcpy(&hp,packet,sizeof(hp));
          if(int32_t(hp.sequence-last_hud)>0 && hp.length<300) {
            last_hud=hp.sequence;
            HudEvent e; e.kind=hp.kind; std::memcpy(e.a,hp.a,sizeof(e.a)); e.length=hp.length; std::memcpy(e.text,hp.text,sizeof(e.text));
            std::lock_guard lock(hud_mutex);
            if(hud_in.size()<64) hud_in.push_back(e);
          }
          last_peer=now;
        } else if(!host && connected.load() && count==sizeof(MissionIdPacket) && !std::memcmp(packet,"WMN1",4)) {
          MissionIdPacket mi; std::memcpy(&mi,packet,sizeof(mi));
          host_mission_hash=mi.hash;
          last_peer=now;
        } else if(!host && connected.load() && count==sizeof(CutscenePacket) && !std::memcmp(packet,"WCS1",4)) {
          CutscenePacket cp; std::memcpy(&cp,packet,sizeof(cp));
          cp.name[63]=0;
          std::lock_guard lock(cutscene_mutex);
          if(cp.sequence!=cutscene_in.sequence) {
            cutscene_in.sequence=cp.sequence; cutscene_in.r4=cp.r4; cutscene_in.r5=cp.r5; cutscene_in.r6=cp.r6; cutscene_in.r7=cp.r7;
            std::memcpy(cutscene_in.name,cp.name,64); cutscene_in_new=true;
          }
          last_peer=now;
        } else if(!host && connected.load() && count==sizeof(MissionStatePacket) && !std::memcmp(packet,"WMS1",4)) {
          MissionStatePacket t; std::memcpy(&t,packet,sizeof(t));
          host_mission=t.mission!=0; host_cutscene=t.cutscene!=0; host_paused=t.paused!=0; host_state_seen=MonoMs();
          { std::lock_guard lock(time_mutex); std::memcpy(host_time,t.time,16); host_time_new=true; }
          last_peer=now;
        } else if(connected.load() && count==sizeof(CarLookPacket) && !std::memcmp(packet,"WVL1",4)) {
          CarLookPacket t; std::memcpy(&t,packet,sizeof(t));
          if(t.length && t.length<=sizeof(t.look)) {
            std::lock_guard lock(car_look_mutex);
            if(car_looks_in.size()>512) car_looks_in.clear();
            car_looks_in[t.id].assign(t.look,t.look+t.length);
          }
          last_peer=now;
        } else if(!host && connected.load() && count==sizeof(PedLookPacket) && !std::memcmp(packet,"WPL1",4)) {
          PedLookPacket t; std::memcpy(&t,packet,sizeof(t));
          if(t.length && t.length<=sizeof(t.look)) {
            std::lock_guard lock(traffic_mutex);
            if(ped_looks_in.size()>512) ped_looks_in.clear();
            ped_looks_in[t.id].assign(t.look,t.look+t.length);
          }
          last_peer=now;
        } else if(!host && connected.load() && count==sizeof(PedPacket) && !std::memcmp(packet,"WPD1",4)) {
          PedPacket t; std::memcpy(&t,packet,sizeof(t));
          if(t.count<=kPedsPerPacket) {
            bool valid=true;
            for(unsigned i=0;i<t.count;++i) {
              for(float v:t.peds[i].pos) valid=valid && std::isfinite(v) && std::abs(v)<100000;
              valid=valid && std::isfinite(t.peds[i].yaw) && std::abs(t.peds[i].yaw)<10;
            }
            std::lock_guard lock(traffic_mutex);
            static uint32_t parts_seen=0;
            if(valid && int32_t(t.sequence-peds_in.sequence)>0) {
              peds_in.sequence=t.sequence; peds_in.time=t.time; peds_in.received=MonoMs();
              peds_in.peds.assign(t.peds,t.peds+t.count); parts_seen=1u<<(t.pad&3);
            } else if(valid && t.sequence==peds_in.sequence && !(parts_seen&(1u<<(t.pad&3)))) {
              peds_in.peds.insert(peds_in.peds.end(),t.peds,t.peds+t.count); parts_seen|=1u<<(t.pad&3);
            }
          }
          last_peer=now;
        } else if(!host && connected.load() && count==sizeof(TrafficNamesPacket) && !std::memcmp(packet,"WTN1",4)) {
          TrafficNamesPacket t; std::memcpy(&t,packet,sizeof(t));
          std::lock_guard lock(traffic_mutex);
          for(unsigned i=0;i<t.count && i<16;++i) {
            t.names[i].model[45]=0;
            traffic_in_names[t.names[i].index]=t.names[i].model;
          }
          last_peer=now;
        } else if(connected.load() && count==sizeof(EventPacket) && !std::memcmp(packet,"WEV1",4)) {
          EventPacket ep; std::memcpy(&ep,packet,sizeof(ep));
          // Each event is sent three times; keep only new sequence numbers.
          if(int32_t(ep.sequence-last_event)>0) {
            last_event=ep.sequence;
            Event e{ep.kind,{},ep.f}; std::memcpy(e.a,ep.a,sizeof(e.a));
            std::lock_guard lock(event_mutex);
            if(events_in.size()<128) events_in.push_back(e);
          }
          last_peer=now;
        } else if(connected.load() && count==sizeof(MorphPacket) && !std::memcmp(packet,"WMO1",4)) {
          MorphPacket m; std::memcpy(&m,packet,sizeof(m));
          std::vector<float> values(m.values,m.values+128);
          std::lock_guard lock(pose_mutex);
          if(values!=remote_morphs) { remote_morphs=values; remote_morphs_new=true; }
          last_peer=now;
        } else if(connected.load() && count==sizeof(ExtrasPacket) && !std::memcmp(packet,"WAX1",4)) {
          ExtrasPacket x; std::memcpy(&x,packet,sizeof(x));
          if(x.length && x.length<=sizeof(x.bytes) && !(x.length&7)) {
            std::vector<uint8_t> bytes(x.bytes,x.bytes+x.length);
            std::lock_guard lock(pose_mutex);
            if(bytes!=remote_extras) { remote_extras=bytes; remote_extras_new=true; }
          }
          last_peer=now;
        } else if(connected.load() && count==sizeof(WornPacket) && !std::memcmp(packet,"WWI1",4)) {
          WornPacket w; std::memcpy(&w,packet,sizeof(w));
          if(w.length && w.length<=sizeof(w.bytes)) {
            std::vector<uint8_t> bytes(w.bytes,w.bytes+w.length);
            std::lock_guard lock(pose_mutex);
            if(bytes!=remote_worn) { remote_worn=bytes; remote_worn_new=true; }
            last_peer=now;
          }
        } else if(!host && connected.load() && count==sizeof(coop::NativePacket)) {
          coop::NativePacket p; std::memcpy(&p,packet,sizeof(p));
          if(coop::ValidateNpc(p,count) && p.sequence>last_received) {
            last_received=p.sequence; last_peer=now;
            { std::lock_guard lock(probe_mutex); native_in=p; native_pending=true; }
            ++native_received;
          }
        }
      }
    }
    if(connected.load() && now-last_peer>std::chrono::seconds(5)) {
      connected=false; has_peer=!host; last_sent=0; last_received=0; last_pose=0; last_event=0; resend.clear();
      SetStatus("Peer timed out. Waiting for connection...");
      Notify(host ? "Co-op: your friend left or lost the connection. Waiting for them to join again."
                  : "Co-op: lost the connection to the host. Trying to get back in...");
      ProbeLog("Peer timeout");
    }
    {
      std::vector<Event> out;
      { std::lock_guard lock(event_mutex); out.swap(events_out); }
      for(const Event& e:out) {
        EventPacket p{{'W','E','V','1'},++event_sequence,e.kind,{},e.f};
        std::memcpy(p.a,e.a,sizeof(p.a));
        resend.push_back({p,3});
      }
      if(connected.load() && has_peer)
        for(auto& r:resend) { NetSend(reinterpret_cast<const char*>(&r.packet),sizeof(r.packet),peer); --r.left; ++packets_out; }
      resend.erase(std::remove_if(resend.begin(),resend.end(),[](const Pending& r){return r.left<=0;}),resend.end());
    }
    if(now>=next_hello) {
      if(!host) NetSend(kHello,sizeof(kHello)-1,peer);
      else if(has_peer) NetSend(kAck,sizeof(kAck)-1,peer);
      next_hello=now+std::chrono::seconds(1);
    }
    if(connected.load() && has_peer && now>=next_snapshot) {
      PlayerPose pose;
      { std::lock_guard lock(pose_mutex); pose=local_pose; }
      if(pose.sequence) {
        PosePacket packet{{'W','P','P','1'},pose.sequence};
        std::memcpy(packet.pos,pose.pos,sizeof(packet.pos));
        std::memcpy(packet.rows,pose.rows,sizeof(packet.rows));
        std::memcpy(packet.model,pose.model,sizeof(packet.model));
        packet.look_length=pose.look_length; packet.time=pose.time;
        std::memcpy(packet.look,pose.look,sizeof(packet.look));
        packet.vehicle=pose.vehicle; packet.seat=pose.seat;
        std::memcpy(packet.vpos,pose.vpos,sizeof(packet.vpos));
        std::memcpy(packet.vrows,pose.vrows,sizeof(packet.vrows));
        std::memcpy(packet.vvel,pose.vvel,sizeof(packet.vvel));
        std::memcpy(packet.vang,pose.vang,sizeof(packet.vang));
        packet.vowner=pose.vowner; packet.vid=pose.vid; packet.flags=pose.flags;
        NetSend(reinterpret_cast<const char*>(&packet),sizeof(packet),peer);
        ++packets_out;
      }
      next_snapshot=now+std::chrono::milliseconds(33);
      // Car looks: 3 per 100 ms, cycling, so each is resent regularly.
      static auto next_car_look=now;
      if(now>=next_car_look) {
        next_car_look=now+std::chrono::milliseconds(100);
        if(host) {
          // Paused = the pause menu screen is the active screen (as in kbm.cpp);
          // read here because the game's update may not run while paused.
          const bool paused=api->read_u32(0x839E0DF8)==0x82FFB84Cu || int32_t(api->read_u32(0x8370DAE4))>0; // pause menu, or a prompt that pauses the world
          MissionStatePacket ms{{'W','M','S','1'},uint8_t(local_mission.load()),uint8_t(local_cutscene.load()),uint8_t(paused),0,{}};
          { std::lock_guard lock(time_mutex); std::memcpy(ms.time,local_time,16); }
          NetSend(reinterpret_cast<const char*>(&ms),sizeof(ms),peer); ++packets_out;
          // The running mission: the slot's table entry starts with its name hash.
          const uint32_t slot=api->read_u32(0x82B3106C);
          MissionIdPacket mi{{'W','M','N','1'},local_mission.load() && Readable(slot,4) ? api->read_u32(slot) : 0u};
          NetSend(reinterpret_cast<const char*>(&mi),sizeof(mi),peer); ++packets_out;
        }
        if(host) {
          std::vector<HudEvent> fresh_hud;
          { std::lock_guard lock(hud_mutex); fresh_hud.swap(hud_out); }
          for(const HudEvent& e:fresh_hud) {
            HudPending hp{}; std::memcpy(hp.packet.magic,"WHU1",4);
            hp.packet.sequence=++hud_out_sequence; hp.packet.kind=e.kind; std::memcpy(hp.packet.a,e.a,sizeof(e.a));
            hp.packet.length=e.length; std::memcpy(hp.packet.text,e.text,sizeof(e.text));
            hp.left=3; hud_resend.push_back(hp);
          }
          for(auto it=hud_resend.begin();it!=hud_resend.end();) {
            NetSend(reinterpret_cast<const char*>(&it->packet),sizeof(it->packet),peer); ++packets_out;
            it=--it->left<=0 ? hud_resend.erase(it) : std::next(it);
          }
        }
        std::vector<CarLookPacket> out;
        {
          std::lock_guard lock(car_look_mutex);
          static size_t cursor=0;
          const size_t n=car_looks_out.size();
          for(size_t k=0;k<3 && k<n;++k) {
            auto it=car_looks_out.begin(); std::advance(it,(cursor++)%n);
            CarLookPacket lp{{'W','V','L','1'},it->first,uint16_t(it->second.size()),0,{}};
            std::memcpy(lp.look,it->second.data(),it->second.size());
            out.push_back(lp);
          }
        }
        for(auto& lp:out) { NetSend(reinterpret_cast<const char*>(&lp),sizeof(lp),peer); ++packets_out; }
      }
      if(now>=next_worn) {
        next_worn=now+std::chrono::seconds(1);
        WornPacket w{{'W','W','I','1'},0,{}};
        { std::lock_guard lock(pose_mutex); w.length=uint16_t(local_worn.size()); std::memcpy(w.bytes,local_worn.data(),local_worn.size()); }
        if(w.length) { NetSend(reinterpret_cast<const char*>(&w),sizeof(w),peer); ++packets_out; }
        MorphPacket m{{'W','M','O','1'},{}};
        bool have=false;
        { std::lock_guard lock(pose_mutex); if(local_morphs.size()==128) { std::memcpy(m.values,local_morphs.data(),sizeof(m.values)); have=true; } }
        if(have) { NetSend(reinterpret_cast<const char*>(&m),sizeof(m),peer); ++packets_out; }
        ExtrasPacket x{{'W','A','X','1'},0,{}};
        { std::lock_guard lock(pose_mutex); x.length=uint8_t(local_extras.size()); std::memcpy(x.bytes,local_extras.data(),local_extras.size()); }
        if(x.length) { NetSend(reinterpret_cast<const char*>(&x),sizeof(x),peer); ++packets_out; }
      }
      // People's positions go out as soon as a new capture is ready (each
      // game update, up to 20 a second); traffic and appearances at 10 Hz.
      if(host && share_peds) {
        static uint32_t sent_peds=0;
        PedPacket parts[kPedParts]{};
        uint32_t sequence=0;
        {
          std::lock_guard lock(traffic_mutex);
          sequence=peds_out_sequence;
          if(sequence!=sent_peds)
            for(unsigned part=0;part<kPedParts;++part) {
              std::memcpy(parts[part].magic,"WPD1",4); parts[part].sequence=sequence; parts[part].time=peds_out_time; parts[part].pad=uint16_t(part);
              for(size_t i=part*kPedsPerPacket;i<peds_out.size() && parts[part].count<kPedsPerPacket;++i) parts[part].peds[parts[part].count++]=peds_out[i];
            }
        }
        if(sequence && sequence!=sent_peds) {
          for(unsigned part=0;part<kPedParts;++part) {
            if(part>0 && !parts[part].count) break;
            NetSend(reinterpret_cast<const char*>(&parts[part]),sizeof(parts[part]),peer); ++packets_out;
          }
          sent_peds=sequence;
        }
      }
      // The latest cutscene start, repeated for 2 s (the joiner keeps new
      // sequence numbers only).
      if(host && now>=next_traffic) {
        CutscenePacket cp{{'W','C','S','1'},0,0,0,0,0,{}};
        bool send=false;
        {
          std::lock_guard lock(cutscene_mutex);
          if(cutscene_out.sequence && MonoMs()-cutscene_out_time<2000) {
            cp.sequence=cutscene_out.sequence; cp.r4=cutscene_out.r4; cp.r5=cutscene_out.r5;
            cp.r6=cutscene_out.r6; cp.r7=cutscene_out.r7; std::memcpy(cp.name,cutscene_out.name,64); send=true;
          }
        }
        if(send) { NetSend(reinterpret_cast<const char*>(&cp),sizeof(cp),peer); ++packets_out; }
      }
      if(host && now>=next_traffic) {
        next_traffic=now+std::chrono::milliseconds(100);
        std::vector<TrafficCar> cars; uint32_t time=0, seq=0;
        { std::lock_guard lock(traffic_mutex); cars=traffic_out; time=traffic_out_time; seq=traffic_out_sequence; }
        const uint16_t parts=uint16_t(std::max<size_t>(1,(cars.size()+kTrafficPerPacket-1)/kTrafficPerPacket));
        for(uint16_t part=0;part<parts && part<kTrafficParts;++part) {
          TrafficPacket t{{'W','T','R','1'},seq,time,0,part,{}};
          for(size_t i=part*kTrafficPerPacket;i<cars.size() && t.count<kTrafficPerPacket;++i) t.cars[t.count++]=cars[i];
          NetSend(reinterpret_cast<const char*>(&t),sizeof(t),peer);
          ++packets_out;
        }
        static size_t name_cursor=0;
        TrafficNamesPacket n{{'W','T','N','1'},0,0,{}};
        {
          std::lock_guard lock(traffic_mutex);
          if(name_cursor>=traffic_names.size()) name_cursor=0;
          for(;name_cursor<traffic_names.size() && n.count<16;++name_cursor,++n.count) {
            n.names[n.count].index=uint16_t(name_cursor+1);
            std::strncpy(n.names[n.count].model,traffic_names[name_cursor].c_str(),45);
          }
        }
        if(n.count) { NetSend(reinterpret_cast<const char*>(&n),sizeof(n),peer); ++packets_out; }
        if(share_peds) {
          std::vector<PedState> all;
          { std::lock_guard lock(traffic_mutex); all=peds_out; }
          // Appearances: 4 people per packet round, so each is resent about
          // once a second (lost packets heal on their own).
          static size_t look_cursor=0;
          for(unsigned k=0;k<6 && !all.empty();++k) {
            PedLookPacket lp{{'W','P','L','1'},0,0,{}};
            {
              std::lock_guard lock(traffic_mutex);
              const PedState& ps=all[look_cursor++%all.size()];
              auto l=ped_looks_out.find(ps.id);
              if(l==ped_looks_out.end() || l->second.empty()) continue;
              lp.id=ps.id; lp.length=uint8_t(l->second.size()); std::memcpy(lp.look,l->second.data(),l->second.size());
            }
            NetSend(reinterpret_cast<const char*>(&lp),sizeof(lp),peer); ++packets_out;
          }
        }
      }
      if(host) {
        coop::NativePacket p;
        { std::lock_guard lock(probe_mutex); p=native_out; }
        if(p.sequence && p.sequence!=last_sent) {
          if(NetSend(reinterpret_cast<const char*>(&p),sizeof(p),peer)==sizeof(p)) last_sent=p.sequence;
        }
      }
      // Chat lines typed here.
      std::deque<std::string> lines;
      { std::lock_guard lock(chat_mutex); lines.swap(chat_out); }
      if(!lines.empty()) {
        const std::string me=EosDisplayName();
        for(const auto& line:lines) {
          ChatPacket c{}; std::memcpy(c.magic,"WCH1",4); c.sequence=++chat_seq_out;
          std::snprintf(c.name,sizeof(c.name),"%s",me.c_str());
          std::snprintf(c.text,sizeof(c.text),"%s",line.c_str());
          for(int k=0;k<2;++k) { NetSend(reinterpret_cast<const char*>(&c),sizeof(c),peer); ++packets_out; }
        }
      }
    }
    Sleep(5);
  }
#ifdef WHOMPAYS_EOS
  if (use_eos.load()) eos::End();
#endif
  CloseSocket();
  connected=false;
  running=false;
  WSACleanup();
}
std::string EosDisplayNameRaw();
// Reserved names ("Whompay") only with owner.key next to the exe.
std::string EosDisplayName() {
  std::string n = EosDisplayNameRaw();
  if (sr_names::IsReserved(n)) {
    FILE* k = std::fopen((std::string(self->folder)+"\\..\\..\\owner.key").c_str(),"rb");
    if (!k) return "Player";
    std::fclose(k);
  }
  return n;
}
std::string EosDisplayNameRaw() {
  // The game's player name (CO-OP tab > Player Name, player_name.txt next to the exe) first.
  if (FILE* f=std::fopen((std::string(self->folder)+"\\..\\..\\player_name.txt").c_str(),"rb")) {
    char nb[64]{}; size_t n=std::fread(nb,1,sizeof(nb)-1,f); std::fclose(f);
    std::string name(nb,n);
    while (!name.empty() && (name.back()=='\n'||name.back()=='\r'||name.back()==' ')) name.pop_back();
    if (!name.empty()) return name.substr(0,15);
  }
  char v[64]{};
  GetPrivateProfileStringA("settings","name","",v,sizeof(v),(std::string(self->folder)+"\\mod.ini").c_str());
  if (!v[0]) { const char* u=std::getenv("USERNAME"); if (u) std::snprintf(v,sizeof(v),"%s",u); }
  return v[0] ? v : "Player";
}
// A join code (letters / digits, 6 long) instead of an IP in join_ip.txt = join online.
std::string JoinCodeFrom(const std::string& text) {
  std::string c;
  for (char ch : text) if (std::isalnum(static_cast<unsigned char>(ch))) c += char(std::toupper(static_cast<unsigned char>(ch)));
  return c;
}
void StopSession() {
  cleanup_requested=true;
  const bool was_running = running.exchange(false);
  if (!was_running && !net_thread.joinable()) {
    connected.store(false);
    hosting.store(false);
    SetStatus("F6 open  |  1 host  2 join  3 stop  4 host online  5 copy of you");
    return;
  }
  stop_thread.store(true);
  if (net_thread.joinable()) net_thread.join();
  stop_thread.store(false);
  connected.store(false);
  hosting.store(false);
  SetStatus("Session stopped.");
  Notify("Co-op ended.");
}

bool StartSession(bool host, bool online = false) {
  ProbeLog(host ? (online ? "Host online pressed" : "Host pressed") : "Join pressed");
  StopSession();
  ProbeLog("Previous session stopped");
  std::lock_guard game_lock(game_mutex);
  native_encoded=0; native_received=0; native_available=0;
  replica_updates=0; tracked_host=0;
  { std::lock_guard lock(probe_mutex); native_out={}; native_in={}; native_pending=false; }
  remote_x=0; remote_y=0; remote_z=0; avatar_updates=0;
  { std::lock_guard lock(pose_mutex); remote_pose={}; remote_pose_new=false; avatar_model.clear(); remote_worn.clear(); remote_worn_new=false; remote_morphs.clear(); remote_morphs_new=false; remote_extras.clear(); remote_extras_new=false; }
  avatar_follow.Reset(); npc_follow.Reset(); npc_pending_look.clear();
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    SetStatus("Winsock startup failed.");
    return false;
  }
  // Joining: join_ip.txt holds an IP (direct) or a join code (online).
  std::string join_text = host ? std::string() : ReadJoinIp();
  in_addr probe{};
  if (!host && inet_pton(AF_INET, join_text.c_str(), &probe) != 1 && JoinCodeFrom(join_text).size() >= 4) online = true;
  use_eos.store(online);
  if (online) {
#ifdef WHOMPAYS_EOS
    eos_join_code = host ? std::string() : JoinCodeFrom(join_text);
    host_ip = host ? "online" : "code " + eos_join_code;
    hosting.store(host);
    running.store(true);
    stop_thread.store(false);
    SetStatus(host ? "Going online..." : "Joining online...");
    Notify(host ? "Co-op: going online..." : "Co-op: joining game " + eos_join_code + "...");
    const sockaddr_in peer = eos::AddressOf(size_t(0));  // the host is always the first remote player
    net_thread = std::thread([peer, host] { NetworkLoop(peer, host); });
    return true;
#else
    WSACleanup();
    SetStatus("This build has no online (EOS) support.");
    return false;
#endif
  }
  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) {
    WSACleanup();
    SetStatus("Could not create UDP socket.");
    return false;
  }
  u_long nonblocking = 1;
  ioctlsocket(sock, FIONBIO, &nonblocking);
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(kPort);
  if (host && bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    CloseSocket();
    WSACleanup();
    SetStatus("Host port 27015 is already in use.");
    return false;
  }
  ProbeLog("Socket ready");
  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_port = htons(kPort);
  std::string address = host ? LocalIp() : join_text;
  if (inet_pton(AF_INET, address.c_str(), &peer.sin_addr) != 1) {
    CloseSocket();
    WSACleanup();
    SetStatus("join_ip.txt must contain an IPv4 address.");
    return false;
  }
  host_ip = address;
  hosting.store(host);
  running.store(true);
  stop_thread.store(false);
  SetStatus(host ? "Hosting on port 27015. Waiting for a client..." : "Joining host...");
  Notify(host ? "Co-op: hosting. Your friend joins with your IP " + address + "."
              : "Co-op: joining " + address + "...");
  net_thread = std::thread([peer, host] { NetworkLoop(peer, host); });
  ProbeLog(host ? "Starting host native probe" : "Starting client native probe");
  return true;
}

// Requests from the game's own menus (the exe's Co-op rows, through the
// exported WhompaysCoop* functions below): handled here, on the frame thread,
// like the overlay keys.
std::atomic<int> menu_request{0};  // 1 host, 2 join, 3 stop

void OnFrame(void*) {
  if (const int r = menu_request.exchange(0)) {
#ifdef WHOMPAYS_EOS
    const bool online = true;
#else
    const bool online = false;
#endif
    // Progress, the join code and errors show in the game's help box
    // (Notify); the F6 overlay is only a debug view now.
    if (r == 1 && !StartSession(true, online)) Notify("Co-op: " + GetStatus());
    if (r == 2 && !StartSession(false)) Notify("Co-op: " + GetStatus());
    if (r == 3) StopSession();
  }
  // One toggle per press: the key state can bounce for a frame right after a
  // toggle, which reopened the overlay as soon as it was closed.
  static uint64_t last_toggle=0;
  if (api->key_pressed(VK_F7)) dump_request.store(true);
  if (api->key_pressed(VK_F8)) clothing_dump_request.store(true);
  if (api->key_pressed(VK_F9)) { trace_until.store(MonoMs()+15000); ProbeLog("TRACE started (15 s, F9)"); }
  // F6 read here as well as through the loader, with our own edge: the
  // overlay couldn't always be closed again with it.
  static bool f6_was_down=false;
  bool f6_down=false;
  {
    DWORD pid=0; HWND fg=GetForegroundWindow();
    if(fg) GetWindowThreadProcessId(fg,&pid);
    f6_down=pid==GetCurrentProcessId() && (GetAsyncKeyState(VK_F6)&0x8000)!=0;
  }
  const bool f6_press=(f6_down && !f6_was_down) || api->key_pressed(VK_F6);
  f6_was_down=f6_down;
  if (f6_press) {
    const uint64_t now=MonoMs();
    if (now-last_toggle>250) { visible.store(!visible.load()); last_toggle=now; }
  }
  if (visible.load()) {
    if (api->key_pressed('1')) StartSession(true);
    if (api->key_pressed('2')) StartSession(false);
    if (api->key_pressed('3')) StopSession();
    // 4: host online (Epic Online Services): shows a join code; the other
    // player puts the code in join_ip.txt and presses 2.
    if (api->key_pressed('4')) StartSession(true, true);
    // 5: a dressed copy of your own character 3 m ahead (appearance tests
    // without a second player); press 5 again to remove it.
    if (api->key_pressed('5') && !connected.load()) copy_toggle.store(true);
    // Closes by itself once connected, so number keys (weapons) don't reach
    // it during play (3 would stop the session).
    static bool was_connected=false;
    if (connected.load() && !was_connected) visible.store(false);
    was_connected=connected.load();
    std::string text = "\x01" "CENTER\nWhompays Co-op\n\n";
#ifdef WHOMPAYS_EOS
    if (use_eos.load() && hosting.load()) {
      const std::string c = eos::Code();
      text += "HOST (online)\nJoin code: " + (c.empty() ? std::string("...") : c) + "\n";
    } else
#endif
    text += hosting.load() ? "HOST\nIP: " + host_ip + ":27015\n" : "CLIENT\n";
    text += connected.load() ? "CONNECTED\n" : "DISCONNECTED\n";
    if (connected.load()) {
      char world[120];
      std::snprintf(world, sizeof(world), "NPC encoded / received / exact models: %u / %u / %u\n",
                    native_encoded.load(), native_received.load(), native_available.load());
      text += world;
      text += "NPC replica test: "+std::to_string(replica_count.load())+" created, "+std::to_string(replica_updates.load())+" pose updates\n";
      std::string model;
      { std::lock_guard lock(pose_mutex); model=avatar_model; }
      text += "Actions sent / played: "+std::to_string(actions_sent.load())+" / "+std::to_string(actions_played.load())+
              "   Hits sent / taken: "+std::to_string(hits_sent.load())+" / "+std::to_string(hits_taken.load())+"\n";
      text += "Other player: "+std::to_string(avatar_count.load())+" shown, "+std::to_string(avatar_updates.load())+" pose updates"+
              (model.empty() ? std::string() : " ("+model+")")+"\n";
    }
    if (connected.load()) {
      char position[96];
      std::snprintf(position, sizeof(position), "%s POS: %.1f, %.1f, %.1f\n",
                    hosting.load() ? "CLIENT" : "HOST",
                    remote_x.load(), remote_y.load(), remote_z.load());
      text += position;
    }
    text += "\n1  Host session\n2  Join join_ip.txt\n3  Stop session\nF6 Close\n\n";
    text += GetStatus();
    api->overlay_text(text.c_str());
  } else {
    api->overlay_text("");
  }
}


}

// For the game's menus (Options > Co-op in the pause menu, Join Co-op on the
// main menu): state, and host / join / stop requests.
// State bits: 1 hosting, 2 joined (or joining), 4 connected, 8 online (EOS) build.
extern "C" WML_EXPORT int WhompaysCoopState() {
  int s = 0;
  if (running.load()) s |= hosting.load() ? 1 : 2;
  if (connected.load()) s |= 4;
#ifdef WHOMPAYS_EOS
  s |= 8;
#endif
  return s;
}
extern "C" WML_EXPORT void WhompaysCoopHost() { menu_request.store(1); }
// Chat: a line typed in the game (sent to the other player when connected),
// and the next received one as "name\ttext" (returns its length, 0 = none).
extern "C" WML_EXPORT int WhompaysCoopChatSend(const char* text) {
  if (!text || !*text || !connected.load()) return 0;
  std::lock_guard lock(chat_mutex);
  chat_out.push_back(std::string(text).substr(0, 159));
  return 1;
}
extern "C" WML_EXPORT int WhompaysCoopChatPoll(char* out, int size) {
  if (!out || size <= 1) return 0;
  std::lock_guard lock(chat_mutex);
  if (chat_in.empty()) return 0;
  const std::string line = chat_in.front();
  chat_in.pop_front();
  const int n = int(std::min<size_t>(line.size(), size_t(size - 1)));
  std::memcpy(out, line.data(), size_t(n));
  out[n] = 0;
  return n;
}
extern "C" WML_EXPORT void WhompaysCoopStop() { menu_request.store(3); }
// code: the host's join code or IP; saved to join_ip.txt (as if typed there).
extern "C" WML_EXPORT void WhompaysCoopJoin(const char* code) {
  if (code && *code && self) {
    std::ofstream file(std::string(self->folder) + "\\join_ip.txt", std::ios::trunc);
    file << code << "\n";
  }
  menu_request.store(2);
}
// One line for the pause menu while a session runs: the join code / IP and
// whether the other player is in (upper case, like the menu rows).
extern "C" WML_EXPORT int WhompaysCoopInfo(char* out, int size) {
  if (!out || size <= 0) return 0;
  std::string s;
  if (running.load()) {
#ifdef WHOMPAYS_EOS
    if (use_eos.load() && hosting.load()) {
      const std::string c = eos::Code();
      s = "JOIN CODE: " + (c.empty() ? std::string("...") : c);
    } else
#endif
    s = hosting.load() ? "YOUR IP: " + host_ip : "HOST: " + host_ip;
    s += connected.load() ? "  (CONNECTED)" : "  (WAITING)";
  }
  for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  std::snprintf(out, size_t(size), "%s", s.c_str());
  return int(std::strlen(out));
}
// The last join code / IP (join_ip.txt), for the menu to offer it again.
extern "C" WML_EXPORT int WhompaysCoopLastJoin(char* out, int size) {
  if (!out || size <= 0 || !self) return 0;
  std::ifstream file(std::string(self->folder) + "\\join_ip.txt");
  std::string v;
  std::getline(file, v);
  while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
  std::snprintf(out, size_t(size), "%s", v.c_str());
  return int(std::strlen(out));
}

extern "C" WML_EXPORT int wml_mod_init(const WmlApi* loader, const WmlMod* mod) {
  if (!loader || loader->version != WML_API_VERSION || loader->size < offsetof(WmlApi, on_game_frame) + sizeof(loader->on_game_frame) ||  // only what co-op uses: older games keep working
     
      !loader->overlay_text) return 1;
  api = loader;
  self = mod;

  {
    char value[32]{};
    GetPrivateProfileStringA("settings","avatar","player",value,sizeof(value),(std::string(self->folder)+"\\mod.ini").c_str());
    use_player_avatar=_stricmp(value,"npc")!=0;
    forward_damage=GetPrivateProfileIntA("settings","damage_forward",0,(std::string(self->folder)+"\\mod.ini").c_str())!=0;
    sync_body=GetPrivateProfileIntA("settings","body",0,(std::string(self->folder)+"\\mod.ini").c_str())!=0;
    body_test=GetPrivateProfileIntA("settings","body_test",0,(std::string(self->folder)+"\\mod.ini").c_str())!=0;
    own_body=GetPrivateProfileIntA("settings","own_body",1,(std::string(self->folder)+"\\mod.ini").c_str())!=0;
    {
      char traffic[16]{};
      GetPrivateProfileStringA("settings","traffic","shared",traffic,sizeof(traffic),(std::string(self->folder)+"\\mod.ini").c_str());
      share_traffic=_stricmp(traffic,"own")!=0;
      char peds[16]{};
      GetPrivateProfileStringA("settings","peds","shared",peds,sizeof(peds),(std::string(self->folder)+"\\mod.ini").c_str());
      share_peds=_stricmp(peds,"own")!=0;
      char missions[16]{};
      GetPrivateProfileStringA("settings","missions","host",missions,sizeof(missions),(std::string(self->folder)+"\\mod.ini").c_str());
      share_missions=_stricmp(missions,"own")!=0;
      char cutscenes[16]{};
      GetPrivateProfileStringA("settings","cutscenes","shared",cutscenes,sizeof(cutscenes),(std::string(self->folder)+"\\mod.ini").c_str());
      share_cutscenes=_stricmp(cutscenes,"own")!=0;
    }
  }
  probe_log.open(std::string(self->folder)+"\\coop-"+std::to_string(GetCurrentProcessId())+".log",std::ios::trunc);
  StartCrashReports(self->folder);
  {
    const std::string ini=std::string(self->folder)+"\\mod.ini";
    people_model_scale=std::clamp<unsigned>(GetPrivateProfileIntA("settings","people_models",1,ini.c_str()),1,4);
    vehicle_model_scale=std::clamp<unsigned>(GetPrivateProfileIntA("settings","group1_models",1,ini.c_str()),1,4);
    pin_top=GetPrivateProfileIntA("settings","pin_top",1,ini.c_str())!=0;
    drop_own_mission=GetPrivateProfileIntA("settings","drop_own_mission",1,ini.c_str())!=0;
    pin_host_models=GetPrivateProfileIntA("settings","preload_host_models",0,ini.c_str())!=0;
    keep_worn_models=GetPrivateProfileIntA("settings","keep_worn_models",1,ini.c_str())!=0;
    free_orphan_slots=GetPrivateProfileIntA("settings","free_slots",1,ini.c_str())!=0;
    sync_idle=GetPrivateProfileIntA("settings","sync_idle",0,ini.c_str())!=0;
    keep_copies=GetPrivateProfileIntA("settings","keep_copies",1,ini.c_str())!=0;
    copy_kind=std::min<unsigned>(GetPrivateProfileIntA("settings","copy_kind",0,ini.c_str()),6);
    people_memory_scale=std::clamp<unsigned>(GetPrivateProfileIntA("settings","people_memory",1,ini.c_str()),1,4);
  }
  api->hook(0x8250EEF8,StreamSetupHook,&original_stream_setup);
  ProbeLog("Whompays Coop 1.59: chat (T in the game, WCH1 packets); 1.58: reserved names (Whompay) only with the owner key; 1.57: co-op messages (hosting, join code, joined, left, ended, errors) in the game's own help box and the join code under Pause > Options instead of the F6 box (F6 is a debug view now); 1.56: no crash on hosting / joining online (the game's System Link over Epic and co-op online no longer run two Epic sessions in one game), Host / Join / End Co-op in the game's menus; 1.49 (1.45 + one change): story characters are never sent as pedestrians (Johnny Gat, Dex, Julius and Troy came back at the church stairs on the joiner after the end prompt); 1.45: the host sends only people within 100 m of itself, pedestrians too (a finished mission\'s gang members, turned into pedestrians, T-posed on the joiner after the host was moved to the next mission), every cutscene character of this game\'s own left standing is removed (Julius stayed on the stairs), list of who stands near the joiner after a mission in its log; 1.44: hit reactions of the host\'s people are played the same on both screens, and the joiner\'s punches make them flinch on the host too (combat reactions only on one side); this game\'s own story characters left standing after a mission cutscene (Johnny Gat, Dex, Julius, gang members, T-posing) are removed at once; 1.43: people models the host\'s people wear here keep their top priority (in fights one pushed the other out: people blinking; mod.ini keep_worn_models); 1.42: copies of the host\'s people wearing their real model are no longer removed to make room (people going invisible and back again and again); 1.41: the other player\'s character is made again when it stays stuck off their path (lay flat behind them for a whole mission after being knocked down just before a cutscene), the host sends only mission characters within 100 m of itself (the finished mission\'s people at the old spot showed on the joiner for ~10 s after the host was moved to the next mission); 1.40: the host no longer sends mission characters its own game has hidden (a finished mission\'s people, and the next mission\'s made ahead, showed up only on the joiner after the mission-passed prompts); 1.39 taken back; 1.38: this game\'s own cutscene characters still standing after a cutscene are hidden (Johnny Gat and others on the stairs at the next mission), and removed when their models keep the host\'s people from loading; all generic gang members and police count as this game\'s own extras (xx_X_*); 1.37: this game\'s own generic mission characters are removed after 1 s when their model isn\'t the host\'s, and whenever they keep the host\'s models from loading (people invisible in missions), everything the joiner\'s mission left behind is removed for 8 s after it ends (mission people staying after the end); 1.36: after a cutscene the host\'s people are shown only once the host sends them again (a finished mission\'s enemies came back for a moment); 1.35: this game\'s own mission characters are removed when the host\'s mission is over here (the finished mission\'s enemies came back), the other player\'s character is put back at once when something here throws it far off (vanished after a hit); 1.34: the local player is put back when the game takes the other player\'s character for it after a cutscene (both saw only the other one and couldn\'t move), player diagnostics after cutscenes; 1.33: this game's pedestrian spawning is turned off again when a mission turns it back on (people popping in and out, their models kept the host's from loading), the host's people are kept through cutscenes and made up to 8 at a time (one by one after a cutscene), no falling over again and again when standing on a hidden character; 1.32: the arrows over the people a mission wants attacked are shown on the joiner too (the host's in-game effects on its shared people); 1.31: this game's own hidden mission characters are removed when their models keep the host's people's models from loading (people invisible in fights); 1.30: stand-ins use models the host has loaded (models only the joiner had kept the host's people from loading: invisible people), a single far-off position of the other player is ignored (vanishing for a moment after being hit), colours set before the clothes go on and clothes put on again when colours arrive later (hair colour); 1.29: the host's people are hidden during cutscenes instead of removed and made again afterwards (people flashing in, T-poses, wrong models after cutscenes); 1.28: a mission the host starts right after passing one brings the joiner over again (1.27 left them behind), the joiner is no longer moved away from the host every time they come close in a fight (only when standing inside each other for 3 s); 1.27: the host's mission no longer counts as ended during the moment between its stages (the joiner restarted it: mission people flickered, the last cutscene wasn't played); 1.26: no freeze when the host skips the last cutscene of a mission (the joiner's cutscene is ended before the mission end, not after); 1.25: body slider check (what arrives from the other player, whether the game baked it into their model, re-applied if something resets it); 1.24: body sliders applied as the game does (only the wanted value; the game bakes the change into the character's own body model), 1.23: memory for the other player's body model (the game's player-body stream group had none in single player, so it never loaded), key 5 spawns the copy again at once; 1.22: the other player's own body model is loaded the way the game loads the player's (1.21 never got it: shared body, host turned muscular); with the shared body nothing of theirs is applied; 1.21: the other player gets their own body model (multiplayer's StyleTest_PC_MP slots, mod.ini own_body) and their body sliders (mod.ini body) - no more both players turning skinny or muscular; 1.20: the other player's skin tone and hair colour (the game's colour choices) are sent and applied, body-shape diagnostic on the player copy (mod.ini body_test); 1.19: the joiner's game can no longer remove copies after 2 s of asking (they were remade: people changing model and clothes, vanishing and coming back); 1.18: new copies hidden until the host's clothes are on (a hat came and went), people standing still get no walk orders (stood up and sat down on a bench again and again); 1.17: models kept in memory that nothing uses are unloaded with the game's own unload while the host's models wait (1.15/1.16 freed their slots by hand: 5 fps), models asked for at the top level again, freeze reports say what the mod was doing; 1.16: the host's models are no longer asked for ahead on the joiner (the joiner's freezes started with that in 1.11; mod.ini preload_host_models); 1.15: people-model slots left full after a model was let go are freed (the joiner held 8 models, the host 11: people missing); 1.14: a person's walk/idle style is set once per change of the host's (no switching between two, e.g. cane and wheelchair), model memory of both games in the logs; 1.13: models loaded here that the host doesn't have make room for the host's people's models (people missing on the joiner); starting the host's people's ongoing actions is off (it started a newspaper only the joiner saw; mod.ini sync_idle), people-model lists of both games in the joiner's log; 1.12: what the host's people are doing (newspaper, tying shoes, cane walks) is started on their copies, people in cars are removed with their car again (the joiner's game froze); 1.11: the joiner loads the people models the host has loaded (same people sooner, fewer lookalikes), no more wrong position when thrown about (other player invisible after jumping out of a car); 1.10: this game no longer removes copies of the host's people, drivers and passengers by itself (people popping in and out; mod.ini keep_copies); 1.09: copies of people are made as scripted characters, which this game doesn't remove on its own (mod.ini copy_kind, 3 = as before); 1.08: getting in as a passenger walks to the door and climbs in (was put straight in the seat); copies waiting for their model (behind a stand-in) and traffic drivers and passengers are no longer removed as this game's own people (popping in and out); 1.07: people models no character uses any more are let go (removed copies kept them: models filled up, people popped in and out on the joiner); 1.06: people-model memory back to normal (twice the memory crashed in the player creator), models no longer needed let go after 4 s instead of 30 s so the host's current people get the room (NPCs in cars), loaded/waiting people models in the log; 1.05: twice the memory for people models (mod.ini people_memory); 1.04: more people models is off by default (people_models = 3 froze the game when entering the world; freeze report written when on); 1.03: room for more people models (mod.ini people_models); 1.02: the pulled-out driver carries on here (gets up) instead of a new standing copy, crash and freeze reports next to the log; 1.01: no endless driver re-making when a seat is refused (the joiner\'s game froze), a pulled-out driver is thrown out with the same exit as in the other game; 1.00: the other player\'s character gets out of its car before it is removed (crash when they time out or leave range while seated), a car stays put while the other player pulls its driver out; 0.99: getting in where the other player drives goes straight to the passenger seat (same seats on both screens), a pulled-out driver\'s copy is removed once out (no duplicate); 0.98: passers-by wait for their real model again (no switching or wrong people), the other player is never taken out of a car their game still has them in (no gliding without a car), car models of the other player\'s car asked for at top priority; 0.97: getting in as the other player\'s passenger finishes (seated directly if it stalls), a pulled-out driver plays the being-pulled-out move on both screens before stepping out, stand-ins are always the same sex; 0.96: no car removed when a driver is pulled out, drivers, passengers and passers-by whose model won't load here get a lookalike (no invisible people), getting in where the other player drives falls back to the usual way when the passenger seat is refused; 0.95: a driver the other player pulls out is pulled out here with the game's own move, whoever got in first keeps the wheel (no two drivers in one seat); 0.94: a driver the other player pulls out is found and gets out here too (no player sitting on top of it); 0.93: players never pull each other out of a seat, no jump to the passenger seat while still getting in, traffic drivers and passengers found by where people sit (NPCs in cars shown), car doors open and close on both screens; 0.92: getting in where the other player drives asks for the passenger seat (no pulling them out), drivers' and passengers' models asked for at top priority, animations played with their blend values, their car found when the host gets in one the joiner took; 0.91: the other player's getting in shows their own moves (door, pulling the driver out, climbing in) and seats them when they sit; 0.90: taking a car with a driver: the driver gets out on both screens, then the player gets in; getting in with the other player driving walks to the passenger door; up to 104 cars shared, parked ones up to 200 m; 0.89: the other player's jumps, climbs and punches start where their character is shown (held back by the follow delay); 0.88: the other player's getting in carries on at the door (door and climb-in play), a car of this game the other player drives up in is shown again; 0.87: passengers of shared cars shown, F9 trace logs getting-in progress; 0.86: the other player is seated at once if their car drives off before getting in finishes here; 0.85: the other player's getting in is no longer cancelled right after it starts; 0.84: getting in and out of cars plays the walk, door and seat on both screens (no exit on entering), the other player no longer climbs twice, wrecked cars blow up once; 0.83: F9 trace of climbing and getting in and out of cars; 0.82: car damage (smoke, fire, wrecks) from the game driving the car, no local explosions of the other game's cars, no create-remove loop for cars; 0.81: car hits no longer applied twice (cars flying off), getting in with the car state not interrupted; 0.80: getting in and out of cars (and pulling drivers out) with the player's own moves, no stuck prompts; 0.79: car hits happen on both screens, climbs keep going through their middle step and retry refused moves, pause menu no longer stuck after the host pauses, F6 always closes the overlay; 0.78: parked cars shared too (the joiner's own go), up to 52 cars, car models asked for at top priority, animated getting in; 0.77: a car a player takes stays the same car on both screens (no vanishing, no second car), climbs play out and hand back on landing; 0.76: no drivers left sitting in thin air, people fight the other player too, the other player gets in cars (and pulls drivers out) with the animation, passers-by take the joiner's punches; 0.75: own pedestrian spawner off and its model memory freed for the host's models, climbs and jumps not cut short; 0.74: people models asked for at top priority, own people removed before they show, climbing and other movement states shared, gang stand-ins for gang members; 0.73: knockdowns and hit reactions of people shared, no random models for passers-by, other player visible further away, movement recovers after jumps; 0.72: other player and copies never fight, crouch or run off on their own AI; crouching shared; 0.71: copies whose model never loads here are remade with a loaded one (no invisible people, no full pool); 0.70: people copy the host's animation sets and only do what the host's do, stand-ins while a model loads, steadier player following, joiner held while the host's game is paused by a prompt; 0.69: joiner's mission intro confirmed at once (no reset of synced characters), up to 96 people shared; 0.68: punched pedestrians stay shared, stuck copies named in the log; 0.67: the host's mission end reaches the joiner (rewards), no walking on the spot, wider takeover; 0.66: the host's mission drives the joiner's (cutscenes, character groups, teleports); 0.65: joiner's own mission characters hidden at once and harmless; 0.64: copies still loading take over the joiner's own mission characters when they appear; 0.63: the joiner's own mission characters follow the host's (no empty cutscenes, attackers shown); 0.62: the joiner's game doesn't pause the world, mission cutscenes from its own mission; 0.61: joiner runs the host's mission with the host's HUD, characters and ending; 0.60: other player's clothing in its own memory (torso/legs, garbled textures), hidden (not removed) during cutscenes; 0.58: other player dressed again when re-created (missing torso/legs), no falling over on top of other characters (breakdancing), joiner starts next to the host; 0.57: F9 rebuilds the other player's look (no crash), F8 compares with own player; players never stand inside each other (host got stuck), only the host skips cutscenes; host keeps working after the mission cutscene, no doubled people in cutscenes, overlay closes when connected; shared cutscenes (host skip skips both), joiner placed next to the host after them, other player keeps clothes on mission start; host pause freezes the joiner, no crash after mission cutscenes; smoother people (20 Hz, buffered, per-frame), no freeze after cutscenes, no false deaths; joiner hits and host enemies count across, deaths shared, mission markers hidden for the joiner, clothing kept after cutscenes; mission detection, F7 people dump, time of day from the host; missions follow the host; car colours; shared pedestrians with appearance and actions; per-frame player smoothing; shared traffic, shared cars (driver in charge, passengers), fresh character after leaving range");
  if (api->hook(0x82209E30, UpdateHook, &original_update) != 0) return 2;
  api->hook(0x82212728, MissionStartHook, &original_mission_start);
  api->hook(0x821F9098, CutsceneStartHook, &original_cutscene_start);
  api->hook(0x827166D0, InputHook, &original_input);
  api->hook(0x822EF440, ObjectiveHook, &original_objective);
  api->hook(0x822E52C8, HelpHook, &original_help);
  api->hook(0x82215360, MissionEndHook, &original_mission_end);
  api->hook(0x8220C778, PauseHook, &original_pause);
  api->hook(0x824C43B0, GroupCreateHook, &original_group_create);
  api->hook(0x824C4B58, GroupDestroyHook, &original_group_destroy);
  InstallTraces();
  InstallEventHooks();
  api->on_frame(OnFrame, nullptr);
  api->on_game_frame(GameFrame, nullptr);
  api->log(self, "Loaded Whompays Co-op prototype. F6 opens the session overlay.");
  return 0;
}

