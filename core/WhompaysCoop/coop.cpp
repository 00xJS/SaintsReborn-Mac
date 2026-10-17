#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "user32.lib")
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <unordered_set>
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
#include <functional>
#include <map>
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
#include "net.h"
#include "world.h"

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
// The local test actor: a mod-owned actor. (The other players' characters
// are in `remotes`, one per session slot.)
uint32_t test_local=0;
std::atomic<uint32_t> avatar_count{0}, avatar_updates{0};
std::atomic<bool> test_toggle{false}, copy_toggle{false};
bool diagnostics_done=false;
bool use_player_avatar=true;       // mod.ini [settings] avatar = player | npc
std::vector<uint8_t> local_worn;   // guarded by pose_mutex
std::vector<float> local_morphs;   // guarded by pose_mutex
bool sync_body=false;              // mod.ini [settings] body = 1 (experimental)
bool share_peds=true;             // mod.ini [settings] peds = shared | own
bool share_traffic=true;           // mod.ini [settings] traffic = shared | own
bool world_stream=true;            // mod.ini [settings] world = 1: the host's world stream shows its area (2.2; 0: 2.1's streams)
// Appearance extras: skin tone, hair colour and the other colour choices of
// the character (the list multiplayer customization sync packed with
// 82321F38 and applied with 82322050; up to 5 entries of 8 bytes).
std::vector<uint8_t> local_extras; // guarded by pose_mutex
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
  // Whose car it is (v2): the session slot of the game that owns it, and its
  // handle in that game (a car made from another game's - a copy - goes by
  // the original's owner and handle).
  uint32_t vowner=0, vid=0;
  uint32_t flags=0; // bit 0: crouching (more: kPose* below)
  // Their camera (v2): eye and rows (right, up, forward). Another game's
  // rockets and throws of this player aim with it (+4112 / +4124, R 4.2).
  float eye[3]{}, cam[9]{};
};
// PlayerPose::flags: bits 1-3 car damage (5: heavy smoke, 2.1), 4 sitting in
// the car, 8-15 the gun in hand (weapon index + 1), and (v2):
constexpr uint32_t kPoseRagdoll=1u<<16;    // lying ragdolled (+780 != 0 in state 8/15; 2.1: not while getting up, 8/16 and 8/17)
constexpr uint32_t kPoseAiming=1u<<17;     // gun raised (+216 bit 31)
constexpr uint32_t kPosePrecision=1u<<18;  // precision aim / zoom (+3593 bit 0x40)
constexpr uint32_t kPoseDead=1u<<19;       // no hit points left
constexpr unsigned kPoseSirenShift=17;     // bits 20-21: the car's siren lights and siren (CarSiren bits 3-4)
constexpr unsigned kPoseCarHealthShift=24; // bits 24-31: the car's hit points out of 255 (CarHealth, 2.1)
constexpr uint32_t kPoseHostWorld=1u<<22;  // a client whose own police are off in the host's world (OwnPoliceOff; 2.1): the host's police response can come for it
constexpr uint32_t kPoseWorldStream=1u<<23; // a client that shows the host's world stream (2.2: mod.ini world = 1 and the host's OwnerMap here): the host's member rule takes it, 2.1's traffic and people streams don't go to it
// A player's notoriety (2.1, kMsgNotoriety): their +3868 block's level, min,
// max and points per gang (0 Los Carnales, 1 Vice Kings, 2 Rollerz, 3 Playas,
// 4 police); its timers (+16, +20) stay each game's own.
struct Notoriety { uint8_t level[5]{}, min[5]{}, max[5]{}; float points[5]{}; };
// ---- Session (co-op v2: up to 12 players, net.h) ----
// Slot 0 is the host; this game's own slot is my_slot. The network thread
// owns the session; the game thread sees these.
constexpr int kSlots=coopnet::kMaxSlots;
std::atomic<uint8_t> my_slot{coopnet::kNoSlot};
std::atomic<uint16_t> present_mask{0};        // slots in the session (this game's own too)
std::atomic<int> player_count{0};
bool Present(int slot) { return slot>=0 && slot<kSlots && (present_mask.load()>>slot&1); }
// What arrives from each other player (network thread -> game thread).
struct RemoteIn {
  PlayerPose pose; bool pose_new=false;
  uint32_t last_pose=0;                  // network thread: newest pose sequence
  std::vector<uint8_t> worn; bool worn_new=false;
  std::vector<float> morphs; bool morphs_new=false;
  std::vector<uint8_t> extras; bool extras_new=false;
  std::string name, model;               // their name; the model shown for them (overlay)
  uint32_t generation=0;                 // +1 for each player who takes this slot
  Notoriety notoriety; bool notoriety_new=false; uint32_t notoriety_sequence=0;  // their notoriety (2.1; sequence 0: none yet)
  uint64_t pose_at=0;                    // when their newest pose came (this clock)
};
std::mutex pose_mutex; // guards local_pose, local_notoriety and remote_in
PlayerPose local_pose;
Notoriety local_notoriety; uint32_t local_notoriety_changes=0;  // this player's (2.1): +1 at each change of a level, min or max
std::atomic<uint32_t> notoriety_sent{0}, notoriety_received{0}, notoriety_old{0};  // kMsgNotoriety packets since the last Police line (network thread): sent, taken, refreshes from before the last change dropped
std::array<RemoteIn,kSlots> remote_in;
// Where another player is (false: no pose yet).
bool RemotePos(int slot,float* out) {
  std::lock_guard lock(pose_mutex);
  const PlayerPose& p=remote_in[slot].pose;
  if(!p.sequence || !Present(slot)) return false;
  std::memcpy(out,p.pos,sizeof(p.pos));
  return true;
}
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
// Horizontal distance squared from this player to another player (huge when unknown).
float DistanceTo2(int slot) {
  float p[3];
  if(!RemotePos(slot,p)) return 1e12f;
  const float dx=p[0]-local_x.load(), dz=p[2]-local_z.load();
  return dx*dx+dz*dz;
}
// Host: the clients 2.1's traffic and people streams still go to (2.2: not
// to one that shows the world stream; WorldGameTick, every update).
std::atomic<uint16_t> legacy_streams{0xFFFF};
// Host: where those clients are (the world around them is shared, 2.1).
std::vector<std::array<float,3>> ClientPositions() {
  std::vector<std::array<float,3>> out;
  const uint16_t to=legacy_streams.load();
  for(int s=1;s<kSlots;++s) { float p[3]; if((to>>s&1) && RemotePos(s,p)) out.push_back({p[0],p[1],p[2]}); }
  return out;
}
// Horizontal distance squared to the nearest of those positions.
float NearestD2(const std::vector<std::array<float,3>>& at,float x,float z) {
  float best=1e12f;
  for(const auto& a:at) { const float dx=x-a[0], dz=z-a[2]; best=std::min(best,dx*dx+dz*dz); }
  return best;
}
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
  uint32_t flags;        // bit 0: crouching (PlayerPose::flags)
  float eye[3], cam[9];  // camera (v2)
};
struct NotorietyPacket {
  char magic[4];         // WNT1 (2.1): the sender's notoriety (struct Notoriety)
  uint32_t sequence;     // the sender's change count: an older one is dropped
  uint8_t level[5], min[5], max[5], pad;
  float points[5];
};

#pragma pack(pop)

// One host page readable? QueryWorkingSetEx answers for exactly one page, but
// under Wine (the Mac) it never says "valid" and each try costs ~0.06 ms on
// top of the VirtualQuery that follows (up to 9 of every 10 s of the joiner's
// time): after 64 answers without one "valid" it isn't asked any more.
// VirtualQuery answers are kept per region for 500 ms. (Built in from Co-op
// Car Guard 0.4-0.5; mod.ini memory_speedup = 0 turns both off.)
bool memory_speedup=true;
std::atomic<uint32_t> qws_answers{0}, qws_valid{0};
std::atomic<bool> qws_off{false};
std::atomic<uint32_t> mem_os_checks{0};  // checks that went to Windows (Perf line)
bool HostPageReadable(uintptr_t page) {
  ++mem_os_checks;
  auto ok=[](DWORD protect,bool committed) { return committed && protect && !(protect&(PAGE_NOACCESS|PAGE_GUARD)); };
  if(!qws_off.load()) {
    PSAPI_WORKING_SET_EX_INFORMATION ws;
    ws.VirtualAddress=reinterpret_cast<void*>(page);
    ws.VirtualAttributes.Flags=0;
    if(K32QueryWorkingSetEx(GetCurrentProcess(),&ws,sizeof(ws))) {
      if(ws.VirtualAttributes.Valid) { ++qws_valid; return ok(DWORD(ws.VirtualAttributes.Win32Protection),true); }
      if(++qws_answers==64 && !qws_valid.load() && memory_speedup) {
        qws_off=true;
        ProbeLog("Memory checks: QueryWorkingSetEx said \"not valid\" 64 times out of 64 (Wine): not asked any more");
      }
    }
  }
  struct Region { uintptr_t begin, end; DWORD protect; bool committed; uint64_t expires; };
  thread_local Region regions[16]{};
  thread_local unsigned next_region=0;
  const uint64_t now=MonoMs();
  if(memory_speedup)
    for(const Region& r:regions) if(page>=r.begin && page<r.end && now<r.expires) return ok(r.protect,r.committed);
  MEMORY_BASIC_INFORMATION m;
  if(!VirtualQuery(reinterpret_cast<void*>(page),&m,sizeof(m))) return false;
  const uintptr_t base=reinterpret_cast<uintptr_t>(m.BaseAddress);
  regions[next_region++%16]={base,base+m.RegionSize,m.Protect,m.State==MEM_COMMIT,now+500};
  return ok(m.Protect,m.State==MEM_COMMIT);
}
bool Readable(uint32_t address, size_t length) {
  if (!address || uint64_t(address) + length > UINT32_MAX) return false;
  auto begin = reinterpret_cast<uintptr_t>(api->guest_pointer(address));
  auto end = begin + length;
  // A page check is a system call and the scans of the object table check
  // thousands of pages a second. Readable pages are remembered for a second,
  // 4096 of them looked up directly (1.65: the 32 searched one by one couldn't
  // hold one scan - a vehicle alone is 14 pages - so nearly every check was a
  // system call: the host's car scan grew to 150 ms/s in a 6 minute session).
  struct Page { uintptr_t page; uint64_t expires; };
  thread_local Page* cache=nullptr;  // kept for the thread's life
  if (!cache) cache=new Page[4096]();
  const uint64_t now=MonoMs();
  while (begin < end) {
    // One page at a time (see wml_pagequery.h: VirtualQuery walks huge regions).
    const uintptr_t page = begin & ~uintptr_t(0xFFF);
    Page& slot = cache[(page >> 12) & 4095];
    if (slot.page != page || now >= slot.expires) {
      if (!HostPageReadable(page)) return false;
      slot = {page, now+1000};
    }
    begin = page + 0x1000;
  }
  return true;
}
// The object in slot `index` of the object table when it is of `type` (1 a
// person, 5 a vehicle), else 0. The type is read first (one page); the whole
// object (a vehicle is 14 pages) is checked only for a match (1.65: every
// object in the table was checked at full size first).
uint32_t TableObject(uint32_t index,uint32_t type,size_t size) {
  const uint32_t o=api->read_u32(kObjectTable+12+index*16);
  if(!o || !Readable(o+68,8) || api->read_u32(o+72)!=type || (api->read_u32(o+68)&0xffff)!=index) return 0;
  return Readable(o,size) ? o : 0;
}
// Weapon infos (weapons.xtbl): *0x832ABCA8, 520 bytes each, count *0x832ABCAC.
// Both games load the same table, so an index names the same gun in each.
constexpr uint32_t kWeaponInfos=0x832ABCA8, kWeaponCount=0x832ABCAC, kWeaponInfoSize=520;
int32_t WeaponIndex(uint32_t info) {
  const uint32_t base=api->read_u32(kWeaponInfos), count=api->read_u32(kWeaponCount);
  if(!base || count>255 || info<base || (info-base)%kWeaponInfoSize) return -1;
  const uint32_t i=(info-base)/kWeaponInfoSize;
  return i<count ? int32_t(i) : -1;
}
// The gun in a person's hand as a weapon table index, or -1: the equipped item
// (+3548), its ammo (+16) and that ammo's weapon info (+8), the way the fire
// function 82470A50 finds it (1.67: 1.66 read the item's +8, and the player's
// pose then said "no gun" while the shots said TEC-9).
int32_t HandWeapon(uint32_t human) {
  const uint32_t item=api->read_u32(human+3548);
  if(!Readable(item,20)) return -1;
  const uint32_t ammo=api->read_u32(item+16);
  if(!Readable(ammo,12)) return -1;
  return WeaponIndex(api->read_u32(ammo+8));
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

// The other players' characters here (struct Remote, further down).
int AvatarSlotOf(uint32_t obj);        // the slot whose character this object is, or -1
uint32_t AvatarObjOf(int slot);        // that slot's character here, or 0
bool IsAvatarHandle(uint32_t handle);

bool NpcReady(uint32_t object, uint32_t index, uint32_t player, unsigned* stages) {
  if (object==player || !Readable(object,4244) || api->read_u32(object+72)!=1) return false;
  ++stages[0];
  const uint32_t handle=api->read_u32(object+68);
  if (!handle || (handle&0xffff)!=index) return false;
  if (IsAvatarHandle(handle) || handle==test_local || handle==replica_local) return false;
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

// One streaming reference per character made here (CreateActor: 8250C750,
// kind 5, as the game's own network NPC creator 823A7DB0 takes it), given
// back when the character goes. The game has no counterpart for it: no human
// destroy path releases a people-model reference (the image's three 8250CA48
// callers are the population sets' 82419C08 and the vehicles' 8247BA28 /
// 825324D0), so every copy, stand-in and traffic crew member left its
// model's count one higher (BF 1 -> 139 in 12 min of the second 2.1.2 test)
// and the request never died: a dead model's kind-5 request evicted slots
// that live copies wore. Handle -> model id and the references on its live
// request (one; a re-request after the request was dropped starts over).
// 8250CA48(group, id, kind, level, all): refs[kind] -= 1 and levels[level]
// -= 1 (all: every counter zeroed); the request is taken down only when all
// seven kinds are 0; without a request for the id it does nothing; while the
// group is locked (its own pass) it queues itself at group+64.
struct ActorModel { uint32_t id=0; uint8_t refs=0; };
std::unordered_map<uint32_t,ActorModel> actor_models;   // game thread
unsigned actor_refs_released=0, actor_refs_swept=0;
uint32_t FindStreamEntry(uint32_t id);
void NoteActorModel(uint32_t handle,uint32_t id) {
  if(!FindStreamEntry(id)) return;   // no free request record: nothing was taken (8250C750 fails silently)
  ActorModel& m=actor_models[handle];
  if(m.id!=id) m.refs=0;
  m.id=id; ++m.refs;
}
void ReleaseActorModel(GuestScratch& scratch,uint32_t handle) {
  const auto it=actor_models.find(handle);
  if(it==actor_models.end()) return;
  const ActorModel m=it->second;
  if(api->read_u8(kStreamTable+(m.id>>24)*204+200)&0x20) return;   // its pass is running (not from the update hook): the sweep comes back to it
  actor_models.erase(it);
  if(!FindStreamEntry(m.id)) return;   // let go already (ClearUnusedModelRefs)
  for(unsigned i=0;i<m.refs;++i) {
    scratch.ctx.r3.u64=kStreamTable+(m.id>>24)*204; scratch.ctx.r4.u64=m.id;
    scratch.ctx.r5.u64=5; scratch.ctx.r6.u64=0; scratch.ctx.r7.u64=0;
    scratch.Call(0x8250CA48);
    ++actor_refs_released;
  }
}
// Once a second: the characters the game removed by itself (the corpse
// clean-up, HumanDestroyHook's let-throughs) give theirs back too.
void SweepActorModels(GuestScratch& scratch,uint64_t now) {
  static uint64_t next=0;
  if(now<next || actor_models.empty()) return;
  next=now+1000;
  std::vector<uint32_t> gone;
  for(auto& [h,m]:actor_models) if(!ResolveHuman(h)) gone.push_back(h);
  for(uint32_t h:gone) { ReleaseActorModel(scratch,h); ++actor_refs_swept; }
}

void DestroyActor(GuestScratch& scratch,uint32_t& handle,const char* tag) {
  if(ResolveHuman(handle)) {
    ReleaseActorModel(scratch,handle);
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
void ClearNotoriety(GuestScratch& scratch,uint32_t obj);  // (with the notoriety, 2.1)
bool OwnPoliceOff();  // (with the client's police, 2.1)
unsigned OwnResponseUnits(std::vector<uint32_t>* units=nullptr,std::vector<uint32_t>* cars=nullptr);
void ResponseTargetGone(GuestScratch& scratch,uint32_t handle);  // (with the police response, 2.1)
void OrderExitVehicle(GuestScratch& scratch,uint32_t human);
uint32_t ResolveVehicle(uint32_t handle);
struct Remote;
void DestroyAvatar(GuestScratch& scratch,Remote& r);  // (with struct Remote)

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
// a model nobody requested: no physics body, nothing drawn. Given back when
// the character goes (ReleaseActorModel: nothing in the game does it; 2.1.4).
unsigned actors_refused=0;       // game thread
uint64_t actors_blocked_until=0;  // after a refusal, no new people for a moment
// Kind of character made (82479AB0 r3): 3 = a passer-by of this game, which
// the game removes by itself when it thinks it is out of the way (copies
// vanished 10-18 times in 10 s: people popping in and out); 0 = a scripted
// character like the missions make, left alone. mod.ini copy_kind.
unsigned copy_kind=0;
unsigned actors_kind_fallback=0;
int making_actor=0;   // inside co-op's own 82479AB0 call: the world registry (8247A6D0's hook, 2.2) leaves it out
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
    ++making_actor; const bool made=scratch.Call(0x82479AB0); --making_actor;
    return made ? scratch.ctx.r3.u32 : 0;
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
    NoteActorModel(handle,id);
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

// Cars of other games shown here (v2). A car is named by the slot of the game
// that owns it and its handle there. Copies made here: another player's own
// car, a host car (made here, or a shared traffic copy taken over when a
// player got in), or one of this game's own cars that was removed here while
// another player drove it (it keeps the original's handle). Game thread.
struct CarCopy {
  uint32_t local=0;                     // handle here
  int32_t type=-1;
  uint64_t used=0;                      // last time a player sat in it (here or in a pose)
  std::vector<uint8_t> applied_look;    // colours put on it
};
std::unordered_map<uint64_t,CarCopy> car_copies;
inline uint64_t CarKey(uint32_t owner,uint32_t id) { return (uint64_t(owner)<<32)|id; }
bool IsCarCopyHandle(uint32_t handle) {
  if(!handle) return false;
  for(auto& [key,c]:car_copies) if(c.local==handle) return true;
  return false;
}
// The copy's owner and handle there, for one of our copies.
bool CarCopyRef(uint32_t handle,uint32_t& owner,uint32_t& id) {
  for(auto& [key,c]:car_copies) if(c.local && c.local==handle) { owner=uint32_t(key>>32); id=uint32_t(key); return true; }
  return false;
}
// A copy of another game's car (not one of this game's own made again).
bool IsOtherGamesCopy(uint32_t handle) {
  uint32_t owner=0, id=0;
  return CarCopyRef(handle,owner,id) && owner!=my_slot.load();
}
// This game's handle for one of its own cars: a copy of one of them made
// again here (it was removed while another player drove it) goes by the
// original's handle.
uint32_t OwnId(uint32_t handle) {
  uint32_t owner=0, id=0;
  return CarCopyRef(handle,owner,id) && owner==my_slot.load() ? id : handle;
}
// Client: copies of the host's traffic cars this player got into (local car
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
std::unordered_map<uint64_t,std::vector<uint8_t>> car_looks_in;  // CarKey(sender, its handle) -> look
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
// Car damage the other game shows (vehicle +53976): bit 0 fire (0x800, the
// burn, effect info+860), 1 smoke (0x04000000, info+864; set with
// 8253CD70(handle, smoke, fire) as vehicle_set_smoke_and_fire_state does),
// 2 wrecked (0x80000000, set by the destroy 8252BF38 and the game's MP wreck
// replay 82542650) and 5 heavy smoke (0x02000000, info+868: no setter, sent
// for later). 2.1: 0x40000000 is the fade-out 82537D40 sets before a car is
// removed, not a wreck (intact cars fading out blew up on the other games).
uint8_t CarDamage(uint32_t vehicle) {
  const uint32_t f=api->read_u32(vehicle+53976);
  return uint8_t(((f&0x800u)?1:0) | ((f&0x04000000u)?2:0) | ((f&0x80000000u)?4:0) | ((f&0x02000000u)?0x20:0));
}
// Hit points left out of 255 (+53512 of the most +53508, the main record
// 8252D2A8 counts down; 255 when the car has no maximum).
uint8_t CarHealth(uint32_t vehicle) {
  const int32_t hp=int32_t(api->read_u32(vehicle+53512)), max=int32_t(api->read_u32(vehicle+53508));
  return max<=0 ? 255 : hp<=0 ? 0 : uint8_t(std::min<int64_t>(255,int64_t(hp)*255/max));
}
// Police, ambulance and other siren lights and the siren itself (vehicle
// +53980 0x40000000 / 0x10000000, what the game's own multiplayer record
// 8253D518 reads): bits 3 and 4 next to the damage bits. They are set once
// (the police spawn, a chase, the driver's horn button) and nothing set them
// on the copies, which drove about dark and silent.
uint8_t CarSiren(uint32_t vehicle) {
  const uint32_t f=api->read_u32(vehicle+53980);
  return uint8_t(((f&0x40000000u)?8:0) | ((f&0x10000000u)?16:0));
}
unsigned car_damage_applied=0, car_sirens_applied=0;
void ApplyCarDamage(struct GuestScratch& scratch,uint32_t vehicle,uint8_t want,uint8_t hp);
void ForgetCarDamage(uint32_t handle);   // a copy made again starts its damage record over
void ApplyCarSiren(struct GuestScratch& scratch,uint32_t vehicle,uint8_t want);
uint32_t SharedCarHostHandle(uint32_t local);   // client: host handle of a traffic copy (0 if none)
// A car here as every game names it: its owner's slot and its handle there.
uint32_t CarId(uint32_t handle,uint32_t& owner) {
  uint32_t id=0;
  if(CarCopyRef(handle,owner,id)) return id;
  if(auto t=taken_cars.find(handle); t!=taken_cars.end()) { owner=0; return t->second; }
  if(const uint32_t host=SharedCarHostHandle(handle)) { owner=0; return host; }
  owner=my_slot.load();
  return handle;
}
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
  p.vid=CarId(api->read_u32(vehicle+68),p.vowner);
}

bool WorldStreamSeen();            // (the shared area mirrored, 2.2: further down)
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
  // (the host's police response can come for us, 2.1; 2.2: the host decides who is in its area, from the poses: this client says it shows the world stream)
  const bool host_world=OwnPoliceOff(), world=WorldStreamSeen();
  std::lock_guard lock(pose_mutex);
  p.sequence=local_pose.sequence+1;
  p.time=uint32_t(MonoMs());
  if(api->read_u32(player+216)&0x20000000u) p.flags|=1; // crouching (crouch_start 82461F58 sets it)
  // bits 8-15: the gun in hand, weapon table index + 1 (1.66; older versions ignore them)
  {
    const int32_t w=HandWeapon(player);
    if(w>=0 && w<255) p.flags|=uint32_t(w+1)<<8;
    static int32_t last=-2; static unsigned logged=0;
    if(w!=last && logged<60) {
      last=w; ++logged;
      const uint32_t item=api->read_u32(player+3548);
      const int32_t by_item=Readable(item,12) ? WeaponIndex(api->read_u32(item+8)) : -1;
      char line[120];
      std::snprintf(line,sizeof(line),"Our gun in hand: weapon %d (item %08X, its +8 says %d)",w+1,item,by_item+1);
      ProbeLog(line);
    }
  }
  // v2: the camera (0x827D9778: +44 eye, +80/+92/+104 right, up, forward) and state.
  for(unsigned i=0;i<3;++i) p.eye[i]=api->read_f32(0x827D9778+44+i*4);
  for(unsigned i=0;i<9;++i) p.cam[i]=api->read_f32(0x827D9778+80+i*4);
  for(float& v:p.eye) if(!std::isfinite(v) || std::abs(v)>100000) v=0;
  for(float& v:p.cam) if(!std::isfinite(v) || std::abs(v)>1.5f) v=0;
  // Down only while lying (every knockdown sets 8/15, 82449974 / 82449AC0): the
  // get-up (82449FE0: 8/16 or 8/17, +780 stays set until 82449EB8) already
  // stands the character up in the other games.
  if(api->read_u32(player+780) && api->read_u32(player+516)==8 && api->read_u32(player+520)==15) p.flags|=kPoseRagdoll;
  if(api->read_u32(player+216)&0x80000000u) p.flags|=kPoseAiming;
  if(api->read_u8(player+3593)&0x40) p.flags|=kPosePrecision;
  if(api->read_f32(player+1912)<=0.0f) p.flags|=kPoseDead;
  if(host_world) p.flags|=kPoseHostWorld;
  if(world) p.flags|=kPoseWorldStream;
  if(const uint32_t car=ResolveVehicle(api->read_u32(player+2496))) {
    const uint8_t d=CarDamage(car);
    p.flags|=uint32_t(d&7)<<1 | (d&0x20); // bits 1-3: car damage, 5: heavy smoke
    p.flags|=uint32_t(CarSiren(car))<<kPoseSirenShift;
    p.flags|=uint32_t(CarHealth(car))<<kPoseCarHealthShift;
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
  api->write_u32(obj+3548,0);  // no equipped item from an earlier character (1.70)
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=0;
  scratch.Call(0x82435228);
  api->write_u32(obj+2320,0);
  api->write_u8(obj+3593,api->read_u8(obj+3593)&0x7F);
  api->write_u32(obj+3704,0); api->write_u32(obj+3700,0xFFFFFFFF);
  api->write_u32(obj+3728,0xFFFFFFFF); api->write_u32(obj+3732,api->read_u32(0x820144B4));
  api->write_u32(obj+3736,0xFFFFFFFF); api->write_u32(obj+3744,0);
  api->write_u32(obj+568,0);
  // 2.1: no notoriety level left on it: 8247E248 -> 8247BA28 -> 821D2D38 ->
  // 821D2950 runs 821D1A20(0, gang) on this game's player for one (H). And
  // 821D2D38 empties this game's crime queue (count 0x82B2BA6C = 0; the
  // records at 0x838F9348 stay, 821D2950's team loop sees none): this
  // player's crimes waiting for their delay, the ones other games sent too,
  // are put back after the call (H).
  for(uint32_t g=0;g<5;++g) api->write_u32(obj+3868+24*g,0);
  const uint32_t queued=std::min<uint32_t>(api->read_u32(0x82B2BA6C),30);
  uint32_t queue[30*4];
  for(uint32_t i=0;i<queued*4;++i) queue[i]=api->read_u32(0x838F9348+i*4);
  if(queued) { static unsigned logged=0; if(logged++<10) { std::snprintf(line,sizeof(line),"%s: player object made with %u crimes of this player queued (8247BA28 -> 821D2D38 empties the queue): put back",tag,queued); ProbeLog(line); } }
  scratch.ctx.r3.u64=obj;
  const bool made=scratch.Call(0x8247E248);
  if(queued) {
    for(uint32_t i=0;i<queued*4;++i) api->write_u32(0x838F9348+i*4,queue[i]);
    api->write_u32(0x82B2BA6C,queued);
  }
  if(!made) { ProbeLog("Player object: respawn call failed"); return 0; }
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
// v2: one set per player-pool slot a remote player's character is made in
// (up to 11 other players), each the first time that slot is used.
uint32_t components_slots=0;   // bit per pool slot that has a set

void EnsureComponents(GuestScratch& scratch,uint32_t slot) {
  if(slot>=12 || (components_slots>>slot&1)) return;
  components_slots|=1u<<slot;
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
  std::snprintf(line,sizeof(line),"Clothing components added for player slot %u: %u (%u failed), %.1f MB in their own block at %08X",
                slot,added,failed,bytes/1048576.0,block);
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

// The guns given to the other player's character (1.66+): holstered
// (82446280(human, 0)) and removed (82446038(human, item, 1)) before the
// player object goes back to the pool. Left in, the next character made from
// the pool met a stale equipped item and the game threw a C++ exception in
// 8247E248 while making it (1.69: the PC player died, the Mac's character
// holding a grenade was released and made again at once - crash).
void ClearInventory(GuestScratch& scratch,uint32_t obj) {
  if(api->read_u32(obj+3548)) {
    scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=0;
    scratch.Call(0x82446280);
  }
  uint32_t items[16]; unsigned n=0;
  const uint32_t head=api->read_u32(obj+2324);
  for(uint32_t node=head;node && n<16 && Readable(node,20);) {
    items[n++]=node;
    node=api->read_u32(node);
    if(node==head) break;
  }
  for(unsigned i=0;i<n;++i) {
    scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=items[i]; scratch.ctx.r5.u64=1;
    scratch.Call(0x82446038);
  }
  if(n) {
    char line[100];
    std::snprintf(line,sizeof(line),"Remote player character: %u item(s) removed before it went back to the pool",n);
    ProbeLog(line);
  }
}
void ReleasePlayerObject(GuestScratch& scratch,uint32_t obj) {
  if(!IsPlayerObject(obj)) return;
  ClearNotoriety(scratch,obj);  // no level left for the pool object's next use (2.1)
  const uint32_t custom=api->read_u32(obj+4484);
  if(Readable(custom,12) && WornCount(custom)) UnwearAll(scratch,custom); // leave the slot clean
  ClearInventory(scratch,obj);  // its guns too (1.70)
  scratch.ctx.r3.u64=obj;
  scratch.Call(0x8247DBB0);
}

// Combat and action events.
// - Actions: every animation action the local player plays goes through
//   82444F88(human, action, r5, r6, r7): punches (0xCA/0xCC/0xCE), jump
//   (0x20 start, 0x34 air, 0x2A land), stances (0xB3-0xB5, 0xD9, 0xDA), get-
//   ups (0xE0 / 0xE2: not played there since 2.1) and so on. They are sent to
//   the peer and played on the character that shows us there.
// - Damage: 824470D0(victim, attacker, r5-r10, f1 amount, stack bytes at
//   caller sp+87/+95) is the game's damage entry (npc_kill uses it). When the
//   local player damages the peer's character, the damage isn't applied to
//   that stand-in; it is sent, and the peer's game applies it to the real
//   player with our stand-in as the attacker, so it reacts and loses health.
// v2: an event names the player it came from (origin, set on arrival) and
// who it is for (target: one slot, or everyone).
struct Event { uint32_t kind, a[8]; float f; uint8_t origin=0, target=coopnet::kEveryone; };
enum : uint32_t { kEventAction=1, kEventDamage=2, kEventCall=3, kEventPedAction=4,
                  kEventPedDamage=5,      // joiner hit a copy of a host person: a[0] host handle, a[1..6] r5-r10, a[7] bytes
                  kEventPlayerHitByPed=6, // host: a shared person hit a client's stand-in (sent to that client): a[0] attacker host handle
                  kEventPedCall=7,        // host: knockdown / hit reaction on a shared person: a[0] fn, a[1] host handle, a[2..5] r4-r7, a[6] arg kinds, a[7] player slots (4 bits per arg)
                  kEventCarImpact=8,      // our car hit a car the receiver drives: a[0] its handle as the receiver knows it, a[1..3] velocity change
                  kEventEnterCar=9,       // our player's seat request (8257B2C0): a[0] car handle, a[1] its owner's slot, a[2] mode, a[3] seat, a[4..6] flag bytes
                  kEventExitCar=10,       // our player's exit request (8257B8E0): a[0..7] request words +4..+32
                  kEventCarAction=11,     // an action of our player's getting in (door, pulling out, climbing in): a[0..3] r4-r7
                  kEventCarDoor=12,       // our player's car door (825352E0): a[0] car id, a[1] its owner's slot, a[2] door, a[3..5] f1-f3
                  kEventVictimAction=13,  // the driver our player pulls out plays a move: a[0] car id, a[1] its owner's slot, a[2..5] r4-r7, a[6..7] f1-f2
                  kEventVictimExit=14,    // that driver's exit request (thrown out): a[0..7] request words +4..+32
                  kEventHostModels=15,    // host: people models loaded there: a[0] count, a[1..7] two 16-bit model ids each
                  kEventPedEffect=16,     // host: a shared person has an in-game effect (the arrow over who to attack): a[0] host handle, a[1..4] r3,r5,r6,r7 of 82403C38
                  kEventPedReact=17,      // host: hit reaction 82458EB8 on a shared person: a[0] host handle, a[1] attacker kind (2 host player, 3 a client: slot a[2], 4 shared person a[2]), a[3..5] hit position, a[6] r6, a[7] r7 | r8<<8 | 1<<16 position given
                  kEventFire=18,          // our player fired a gun (82470A50): a[0] weapon table index + 1, a[1..3] the first pellet's hit point (bullets), a[4] bit 0: hit point given
                  kEventThrow=19,         // our player threw (82475E30): a[0] weapon index + 1, f strength (+1940)
                  kEventFireRelease=20,   // our player let go of an automatic gun's trigger (82480660): a[0] weapon index + 1
                  kEventNotoriety=21,     // a crime of the receiver's player seen here (2.1, CrimeHook): a[0] activity, a[1] team, a[2] flag, a[3] bit 0: with a rocket launcher
                  kEventBusted=22,        // a cop here arrested the receiver's character (2.1, BustedHook)
                  kEventCrewDamage=23,    // joiner hit a crew member of the host's traffic (2.1.1): a[0] the car's handle in the host's game, a[1..6] r5-r10, a[7] bytes | seat<<16 (the host hits whoever sits there)
                  kEventSeen=24,          // host: whether a cop here sees the receiver's character right now (2.1.2, UpdateNotoriety; on change and once a second): a[0] 1 seen, 0 not
                  kEventCarDamage=25,     // 2.2: a member's bullet hit on a car of the owner's world, to the car's authority (Package A/9, with the members' side)
                  kEventAuthority=26,     // 2.2, owner -> its area's members: who drives a car of its world: a[0] owner slot, a[1] car id, a[2] the slot whose pose carries it (the owner's own when a[3] is 0), a[3] 1 a player sits in it or gets in, 0 back to the owner's game
                  kEventVehicleTable=27,  // 2.2 (review A2-12), to everyone when a player joins: the sender's vehicle table (cars go by its index): a[0] rows (*0x8371023C), a[1] a hash of the rows' name hashes (+120), a[2] explosion defs (*0x8370D114), a[3] weapons (*0x832ABCAC), a[4] its mod.ini world_actions (2 on, 1 off)
                  kEventExplosion=28      // 2.2 Package C, host -> its area's members within 200 m: an explosion of its world: a[0] def index | source kind << 16 (1 a prop's death, 2 a car's second explosion) | owner slot << 20 | flags << 24 (1 mover uid given, 2 the call's byte +103), a[1..3] position, a[4] forward row (3 x 10 bits x 511), a[5] the car's id (kind 2) or the attacker's (kind 1, attacker kind 3), a[6] the mover's uid (kind 1) or the call's word +92 (kind 2), a[7] mover sub | attacker kind << 8 (1 the host's player, 2 a player's character: slot << 16, 3 a person of its world)
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
bool OurCharacter(uint32_t handle);              // joiner: any character showing the host's (people, traffic crews, stand-ins)
bool CrewCharacter(uint32_t handle);             // joiner: a driver, passenger or seat stand-in of the host's traffic
bool CrewSeat(uint32_t handle,uint32_t& host_car,uint32_t& seat);  // joiner: the host's car and seat one of those shows (2.1.1)
bool IsSentCar(uint32_t id);                     // host: this car is in the traffic sent to the clients (2.1.1)
// The shared area mirrored, the members' side (2.2, with the network thread).
bool WorldMode();                                // joiner (mod.ini world = 1): the host streams its world (its OwnerMap comes): 2.1's traffic and people aren't shown
bool WorldShown();                               // joiner: in the host's area (its OwnerMap says so)
bool WorldLeversOn();                            // joiner: in it, or copies of it still here: this game's own traffic, people and police stay off
uint32_t WorldPersonId(uint32_t local,bool seated);   // joiner: the host's id of a person copy of the world stream (seated: crews too), or 0
uint32_t WorldPersonLocal(uint32_t id);          // joiner: the copy of the host's person (0 if none)
uint32_t WorldCarId(uint32_t local);             // joiner: the host's id of a car copy of the world stream, or 0
uint32_t WorldCarLocal(uint32_t id);             // joiner: the copy of the host's car (0 if none)
bool WorldCrewSeat(uint32_t local,uint32_t& car,uint32_t& seat);   // joiner: a seated person copy's car (the host's id) and seat
bool WorldFading(uint32_t local);                // joiner: a copy being faded (the game's own removal may take it)
bool WorldSends(uint32_t handle,bool seated);    // host: a person of its world stream (seated: crews too)
bool WorldCopiesLeft();
void WorldPeopleLocals(std::vector<uint32_t>& out,bool standins);   // joiner: the world stream's person copies here (with stand-ins or not)
// game thread (joiner): copies' own shots (from a seat), their local hits on our player, crews' own actions, exits and the hits on them (2.1.1)
unsigned copy_shots_blocked=0, copy_shots_seated=0, copy_hits_zeroed=0, crew_actions_blocked=0, crew_exits=0, crew_exits_refused=0, crew_hits_sent=0;
unsigned crew_hits_empty=0, crew_hits_nocar=0, crew_hits_player=0;   // host: other players' crew hits dropped here: no one in that seat, the car not here or not sent, a player in it (2.1.4)
std::atomic<uint32_t> crew_hits_applied{0};      // host: other players' hits on its crews applied here (2.1.1)
std::atomic<uint32_t> ped_hits_sent{0}, ped_hits_applied{0}, hits_from_people{0};
void ApplyPedAction(GuestScratch& scratch,const struct Event& e,uint64_t now);
void ApplyPedCall(GuestScratch& scratch,const struct Event& e,uint64_t now);
bool forward_damage=false; // mod.ini [settings] damage_forward = 1
// The shared area's actions and effects (2.2, Package C; further down).
bool WorldFireCapture(WmlContext* c,uint8_t* b,uint32_t human);   // host: a round of a person of its world stream (its records carry it)
bool WorldExplosion(WmlContext* c);                                // host: an explosion of its world to the area's members; member: false holds one
bool WorldCarHit(WmlContext* c,uint32_t v);                        // a member's bullet on a car of the host's world (sent), the host's replay of it (left to that)
uint32_t WorldProjShooter(uint32_t owner);                         // host: the id of a person of its world stream while it has members, else 0
uint32_t WorldProjSent(uint32_t proj_handle);                      // host: the shooter's id of a projectile sent to its area, else 0
void WorldProjOut(const struct ProjPacket& p);                     // host: to each member of its area
void WorldProjEnded(uint32_t proj_handle);                         // host: a projectile ended (its sent shooter forgotten; 0: all)
void WorldCarDamageEvent(GuestScratch& scratch,const struct Event& e,uint32_t avatar);   // host: a member's bullet on a car of its world (A/9)
void WorldExplosionEvent(GuestScratch& scratch,const struct Event& e,uint64_t now);      // member: an explosion of the host's world
unsigned copy_guns_given=0, copy_guns_carried=0, copy_guns_failed=0, copy_launches_refused=0, copy_car_hits_skipped=0, copy_proj_made=0, copy_proj_booms=0;   // member (game thread): guns put in copies' hands, copies' own rockets and throws refused, cars their replayed rounds hit, the host's people's projectiles copied here (and exploded)
#pragma pack(push,1)
struct EventPacket { char magic[4]; uint32_t sequence; uint32_t kind, a[8]; float f; };
#pragma pack(pop)
std::mutex event_mutex;
std::vector<Event> events_out, events_in;
std::atomic<uint32_t> ped_actions_sent{0}, ped_actions_played{0}, ped_calls_sent{0}, ped_calls_played{0};
uint32_t own_actions_blocked=0; // game thread (joiner): actions a copy's own AI tried
std::atomic<uint32_t> actions_sent{0}, actions_played{0}, hits_sent{0}, hits_taken{0};
std::atomic<uint32_t> events_dropped{0};
std::atomic<uint32_t> net_resent{0}, net_dropped{0};   // session totals for the Perf line

// Events go out reliably (v2), in order, to e.target (one slot or everyone).
void QueueEvent(const Event& e) {
  if(!connected.load()) return;
  std::lock_guard lock(event_mutex);
  if(events_out.size()<512) events_out.push_back(e); else ++events_dropped;
}
void QueueEventTo(Event e,uint8_t target) { e.target=target; QueueEvent(e); }

// Calls made inside a replicated call are replayed by that call on the other
// side; sending them too plays them twice and restarts the animation.
thread_local int replicated_depth=0;
// Our player's own getting in / out: the other game replays the request
// itself, which plays the walk to the door, the door and the seat there. The
// states and car actions it causes here are not sent on top of it.
uint64_t own_car_move_until=0;
inline bool CarMoveAction(uint32_t lr) { return (lr>=0x82570000u && lr<0x82580000u) || lr==0x8252ED04u; }
// 82451C98(person, car, ...): a car hitting a person (from the contact
// callback 82278E18); its damage calls name the driver (82534B98 seat 0).
inline bool CarHitsPerson(uint32_t lr) { return lr>=0x82451C98u && lr<0x82452730u; }
WmlGuestFunction original_play_action=nullptr;
// F9: 15 s trace of what our player and the other player's character do
// (actions, movement states, seat and exit requests, the character's state
// each frame), for working out climbing and getting in / out of cars.
std::atomic<uint64_t> trace_until{0};
bool Tracing() { return MonoMs()<trace_until.load(); }
uint32_t trace_driver=0;   // handle of the driver being pulled out (F9 trace)
const char* TraceWho(uint32_t obj) {
  if(obj && obj==api->read_u32(0x8309ABEC)) return "me";
  if(obj && AvatarSlotOf(obj)>=0) return "other";
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
void PlayActionHook(WmlContext* c,uint8_t* b) {
  {
    // The driver our player pulls out of a car: its moves go to the other
    // game, where the same driver plays them (it only stepped out there).
    const uint32_t who=uint32_t(api->get_r(c,3)), me=api->read_u32(0x8309ABEC);
    if(who && who!=me && !replicated_depth && connected.load() && CarMoveAction(api->get_lr(c)) && Readable(who,4252) && me) {
      const uint32_t mycar=api->read_u32(me+2496);
      if(mycar && AvatarSlotOf(who)<0 && api->read_u32(who+2496)==mycar) {
        uint32_t owner=0; const uint32_t id=CarId(mycar,owner);
        Event e{kEventVictimAction,{id,owner,uint32_t(api->get_r(c,4)),uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7))},0};
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
  // 2.1: the drivers, passengers and stand-ins of the host's traffic too
  // (police crews here went after a wanted player on their own), except their
  // car moves (seated by co-op, pulled out by a player: 8257xxxx).
  if(human && !hosting.load() && connected.load() && !replicated_depth && Readable(human,4252) && human!=api->read_u32(0x8309ABEC)) {
    const uint32_t h=api->read_u32(human+68), lr=api->get_lr(c);
    if(SharedPedHostHandle(h)) { ++own_actions_blocked; api->set_r(c,3,0); return; }
    if(!CarMoveAction(lr) && CrewCharacter(h)) {
      ++crew_actions_blocked;
      static std::unordered_set<uint32_t> callers;
      if(callers.size()<20 && callers.insert(lr).second) {
        char line[120]; std::snprintf(line,sizeof(line),"Traffic crew member's own action %X from %08X refused",uint32_t(api->get_r(c,4)),lr); ProbeLog(line);
      }
      api->set_r(c,3,0);
      return;
    }
  }
  // Our getting in (door, pulling a driver out, climbing in): sent as car
  // actions and played on our character there.
  if(human && human==api->read_u32(0x8309ABEC) && !replicated_depth && connected.load() && CarMoveAction(api->get_lr(c)) &&
     ResolveVehicle(api->read_u32(human+2496)) && api->read_u32(human+2556)!=0x12) {
    Event e{kEventCarAction,{uint32_t(api->get_r(c,4)),uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7))},0};
    PackActionFloats(c,e); QueueEvent(e); ++actions_sent;
  }
  // (v2: not the fire actions 126-157 and throws 243-260: the replayed shot
  // and throw play them there, R 3.0.)
  const uint32_t action_id=uint32_t(api->get_r(c,4));
  const bool weapon_action=(action_id>=126 && action_id<=157) || (action_id>=243 && action_id<=260);
  if(human && human==api->read_u32(0x8309ABEC) && !replicated_depth && !weapon_action &&
     !(CarMoveAction(api->get_lr(c)) || (MonoMs()<own_car_move_until && api->get_r(c,4)==0x160))) {
    Event e{kEventAction,{uint32_t(api->get_r(c,4)),uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7))},0};
    PackActionFloats(c,e); QueueEvent(e); ++actions_sent;
  }
  original_play_action(c,b);
}

// Calls on the local player that are repeated on our character in the other
// games. Arguments that point into the local player object are sent as an
// offset and translated to the character there (a[7] bit i); ones pointing
// into another player's character here go as that player's slot and an
// offset (bit 4+i, slot in bits 8+4i..11+4i): there it is that player's
// character, or that game's own player.
// The other player's character an address points into (its slot), or -1.
int AvatarSlotAt(uint32_t v,uint32_t size,uint32_t& base) {
  for(int s=0;s<kSlots;++s) {
    const uint32_t o=AvatarObjOf(s);
    if(o && v>=o && v<o+size) { base=o; return s; }
  }
  return -1;
}
void QueueCall(uint32_t fn,WmlContext* c) {
  const uint32_t player=api->read_u32(0x8309ABEC);
  Event e{kEventCall,{fn},0};
  uint32_t flags=0;
  for(int i=0;i<4;++i) {
    uint32_t v=uint32_t(api->get_r(c,4+i)), base=0;
    if(v>=player && v<player+22128) { v-=player; flags|=1u<<i; }
    else if(const int s=AvatarSlotAt(v,22128,base); s>=0) { v-=base; flags|=(0x10u<<i)|(uint32_t(s)<<(8+4*i)); }
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
  Event e{kEventPedCall,{fn,api->read_u32(victim+68)},0};
  uint32_t kinds=0, base=0;
  for(int i=0;i<4;++i) {
    uint32_t v=uint32_t(api->get_r(c,4+i)), k=0;
    if(v>=victim && v<victim+4252) { v-=victim; k=1; }
    else if(v>=player && v<player+4252) { v-=player; k=2; }
    else if(const int s=AvatarSlotAt(v,4252,base); s>=0) { v-=base; k=3; e.a[7]|=uint32_t(s)<<(4*i); }
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
// melee hit 82454A30 and many others). Still sent, but since 2.1 the other
// games don't replay it: the pose's ragdoll bit decides there (SyncKnockdown).
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
int knocking_avatar=0;   // inside SyncKnockdown's own 824497F0 call: the one knockdown of another player's character here
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
  // real one back if its player goes down (its pose's ragdoll bit, SyncKnockdown).
  // During a replay too (2.1.4): their own rocket's explosion replayed here
  // knocked their character down while they stood and fired (the host saw it
  // lie 3-4 s, 55 of their shots refused in one window); only SyncKnockdown
  // knocks it.
  if(connected.load() && !knocking_avatar && AvatarSlotOf(uint32_t(api->get_r(c,3)))>=0) { api->set_r(c,3,0); return; }
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
        const uint32_t attacker=uint32_t(api->get_r(c,4));
        const int attacker_slot=attacker ? AvatarSlotOf(attacker) : -1;
        Event e{kEventPedReact,{api->read_u32(victim+68)},0};
        if(attacker && attacker==player) e.a[1]=2;
        else if(attacker_slot>=0) { e.a[1]=3; e.a[2]=uint32_t(attacker_slot); }
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
  // State 8 (knocked down, getting up) on another player's character: only
  // from SyncKnockdown's knock and get-up (2.1: theirs isn't replayed), and
  // the ragdoll update's own 8/17 -> 8/16 step (82463C04); 82441308 refuses
  // anything else in 8/17 (82441320-82441344), so without it the character
  // stayed in 8/17 for good after a get-up to a free spot.
  if(state==8 && connected.load() && !replicated_depth && AvatarSlotOf(uint32_t(api->get_r(c,3)))>=0 && api->get_lr(c)!=0x82463C08u) return;
  if(state==8 && SharedPersonCall(0x82441308,c)) return;
  if(uint32_t(api->get_r(c,3))==api->read_u32(0x8309ABEC) && connected.load() && !replicated_depth) {
    // Jumps (4) and state 8 always (since 2.1 the other games drop 8: the
    // pose's ragdoll bit decides there); the other movement states
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
  if(connected.load() && AvatarSlotOf(uint32_t(api->get_r(c,3)))>=0) {
    if(forward_damage) {} // damage forwarding (below) handles it if enabled
    else { api->set_r(c,3,1); return; }
  }
  original_melee_hit(c,b);
}
// Shooting (1.66). The gun in each player's hand goes with the pose (flags
// bits 8-15) and each shot of our player is sent (kEventFire). The other game
// fires our character's gun the way the game's multiplayer replays a remote
// player's shot (823AFA88): muzzle flash, sound, recoil and tracers show. The
// replay hurts no one: what the bullets hit is the shooter's game's to decide,
// and its hits already arrive as their own events.
int fire_replay_depth=0;  // game thread: a shot of the other player is being replayed
// 2.2, Package C (the shared area's actions and effects): the member's replays.
int copy_fire_replay_depth=0;  // game thread (member): a round of a copy of the host's people is being replayed (no car takes its hit)
int burn_replay_depth=0;       // game thread (member): the host's person's burning put on its copy
int world_boom_depth=0;        // game thread (member): an explosion of the host's world played here
bool world_actions=true;       // mod.ini [settings] world_actions = 0: Package C off on this game, sent and applied alike
bool world_actions_of[16]={true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true};   // each game's world_actions (kEventVehicleTable a[4] when a player joins; on until it says)
inline bool WorldActionsWith(int slot) { return world_actions && slot>=0 && slot<16 && world_actions_of[slot]; }   // (both games have Package C on)
bool WorldMemberWithActions(int slot);   // host: that slot is in its area and has Package C on (further down)
bool share_shooting=true; // mod.ini [settings] shooting = 0: their character gets no gun, their shots aren't replayed
bool walk_as_player=true; // mod.ini [settings] walk = 0: other players' characters walk their path on move orders (82441670), as 2.1 did (DriveAsPlayer)
bool share_police=true;   // mod.ini [settings] police = 0: no notoriety per player (no sync, crime credit, arrests or response steering across games; a client's police run near the host): as 2.0
std::atomic<uint32_t> shots_sent{0}, shots_played{0}, shots_refused{0};
// 2.1.4: why a shot of theirs last waited here (Remote::Shot::held), counted
// when it is dropped after 750 ms ("refused here (lying ..." in the Shots line).
enum : uint8_t { kHeldOther=0, kHeldLying, kHeldNotSetUp, kHeldOtherGun, kHeldNoAmmo, kHeldFireRefused, kHeldRaising, kHeldInCar, kHeldReasons };
std::atomic<uint32_t> shots_held[kHeldReasons]{};
// Epic link (1.71): the largest queue sizes the network thread saw since the
// last Perf line (what waits to be sent / to be read), for the "Link:" line.
std::atomic<uint32_t> link_out_bytes{0}, link_out_packets{0}, link_in_bytes{0}, link_in_packets{0};
constexpr uint32_t kGameClock=0x827AA6E0;
// The equipped gun's ammo (+8 weapon info, +16 rounds, +24 refire timer), or 0.
uint32_t EquippedAmmo(uint32_t human) {
  const uint32_t item=api->read_u32(human+3548);
  if(!Readable(item,20)) return 0;
  const uint32_t ammo=api->read_u32(item+16);
  return Readable(ammo,28) && Readable(api->read_u32(ammo+8),kWeaponInfoSize) ? ammo : 0;
}
// The other player's gun in our character's hand (want = weapon index + 1,
// 0 = none), done the way the game's multiplayer arms a remote player
// (823AA218): the item in the weapon's inventory slot (823F79F0(human,
// 823F7958(info)), from the list at +2324) when it is that gun; otherwise that
// item is removed (82446038(human, item, 1)) and the gun given with
// 82445C80(human, item table entry, 1). The entry is the 76-byte item table
// one (0x830845A8) that the weapon info's +32 points to (825BECB0 matches
// them by it). Then 82446280(human, item) equips it; with 0 it holsters.
// 1.66/1.67 gave the weapon info itself: a broken item that never went into
// the hand, and the host froze in the world update shortly after (8241EFE0,
// 8242EFB0). Tried again at most once a second while it doesn't take.
// Per other player's character (v2: one each).
struct GunState {
  uint64_t hold_until=0;             // a shot of theirs keeps the gun in hand until then
  uint32_t last_want=~0u; uint64_t next_try=0;
  uint32_t pose_want=0; uint64_t pose_since=0;
};
constexpr uint32_t kItemTable=0x830845A8, kItemCount=0x830831A4, kItemSize=76;
bool MatchWeapon(GuestScratch& scratch,uint32_t obj,uint32_t want,GunState& g,bool copy=false) {
  const uint32_t have=uint32_t(HandWeapon(obj)+1);
  if(have==want) return true;
  const uint64_t now=MonoMs();
  if(want==g.last_want && now<g.next_try) return false;
  g.last_want=want; g.next_try=now+1000;
  uint32_t give=0;
  bool carried=false;
  if(want) {
    const uint32_t base=api->read_u32(kWeaponInfos), count=api->read_u32(kWeaponCount);
    if(!base || want>count || count>255) return false;
    const uint32_t info=base+(want-1)*kWeaponInfoSize;
    // Thrown weapons (Ammo_Type 2) are handed over again in v2: the
    // character's own projectile is refused (ProjectileCreateHook) and its
    // count is kept up before each replayed throw. (1.70 kept them back after
    // the 1.69 crash; that crash came from the pool object's stale equipped
    // item, fixed in 1.70 by ClearInventory.)
    const uint32_t entry=api->read_u32(info+32), items=api->read_u32(kItemCount);
    if(entry<kItemTable || (entry-kItemTable)%kItemSize || (entry-kItemTable)/kItemSize>=items || items>1024) {
      static unsigned bad=0;
      if(bad++<10) { char line[100]; std::snprintf(line,sizeof(line),"Other player's gun: weapon %u has no item table entry (%08X)",want,entry); ProbeLog(line); }
      return false;
    }
    scratch.ctx.r3.u64=info;
    if(!scratch.Call(0x823F7958)) return false;
    const int32_t slot=int32_t(scratch.ctx.r3.u32);
    if(slot<0) return false;
    scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=uint32_t(slot);
    if(!scratch.Call(0x823F79F0)) return false;
    const uint32_t cur=scratch.ctx.r3.u32;
    const uint32_t cur_ammo=Readable(cur,20) ? api->read_u32(cur+16) : 0;
    if(cur && Readable(cur_ammo,12) && api->read_u32(cur_ammo+8)==info) { give=cur; carried=true; }
    else {
      if(cur) {
        scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=cur; scratch.ctx.r5.u64=1;
        ++replicated_depth; scratch.Call(0x82446038); --replicated_depth;
      }
      scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=entry; scratch.ctx.r5.u64=1;
      ++replicated_depth; const bool ok=scratch.Call(0x82445C80); --replicated_depth;
      if(!ok) return false;
      give=scratch.ctx.r3.u32;
      if(!Readable(give,20)) return false;
    }
  }
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=give;
  ++replicated_depth; scratch.Call(0x82446280); --replicated_depth;
  const uint32_t now_have=uint32_t(HandWeapon(obj)+1);
  static unsigned logged=0, copies_logged=0;   // (2.2: copies of the host's people have their own lines, "World copy gun")
  if(copy && want) ++(now_have==want ? (carried ? copy_guns_carried : copy_guns_given) : copy_guns_failed);
  if(copy ? copies_logged++<20 : logged++<40) {
    char line[160]; const uint32_t info=want ? api->read_u32(kWeaponInfos)+(want-1)*kWeaponInfoSize : 0;
    char name[32]="none";
    if(info && Readable(api->read_u32(info),16)) std::snprintf(name,sizeof(name),"%.31s",static_cast<const char*>(api->guest_pointer(api->read_u32(info))));
    std::snprintf(line,sizeof(line),"%s: %s (weapon %u) %s%s",copy ? "World copy gun" : "Other player's gun",name,want,
                  now_have==want ? (want ? "in hand" : "holstered") : "not taken",want ? (carried ? ", one it carried" : ", given") : "");
    ProbeLog(line);
  }
  return now_have==want;
}
// ---- Weapons (v2, research report R) ----
// Every weapon: bullets go where the shooter's bullet went (its hit point),
// automatic guns stop with the shooter's trigger release, and rockets and
// thrown weapons (grenades, pipe bombs, Molotovs, bottles) fly as one real
// projectile - the owner's - plus a copy in every other game that explodes
// once, when and where the owner's did.
// 82470A50(human, target, aim point*, blank): fires the equipped gun - muzzle
// flash, sound, recoil and refire timer, then the bullets (8246FD18 per pellet
// -> 82476408 -> 824581C8 for a person hit) or the rocket (8241FCB0).
int capture_hit=0;                  // game thread: in our player's shot: the first pellet's hit point is kept
uint32_t capture_shooter=0;         // game thread (host, 2.2 Package C): in a shot of this person of its world stream: the same
bool hit_valid=false; float hit_point[3]{};
bool auto_trigger_down=false;       // our player is firing an automatic gun (the release is sent)
WmlGuestFunction original_fire=nullptr;
void FireHook(WmlContext* c,uint8_t* b) {
  const uint32_t human=uint32_t(api->get_r(c,3));
  // Another player's character: the game's own deferred shot (fired by the
  // human update after raising a lowered gun, outside any replay, with the
  // barrel's aim) is dropped; that player's shots come from their game (R 0.3).
  if(human && api->get_lr(c)==0x824428E4u && connected.load() && AvatarSlotOf(human)>=0) {
    api->write_u32(human+212,api->read_u32(human+212)&~0x00080000u);
    return;
  }
  // 2.1, joiner: copies of the host's people (on foot, its traffic crews,
  // stand-ins) never fire on their own, deferred shot or not, drive-bys
  // (8257F0E8) too: the host's person shoots there (police copies shot a
  // wanted player here). None of its callers use a result.
  if(human && !replicated_depth && connected.load() && !hosting.load() && OurCharacter(api->read_u32(human+68))) {
    api->write_u32(human+212,api->read_u32(human+212)&~0x00080000u);
    ++copy_shots_blocked;
    if(ResolveVehicle(api->read_u32(human+2496))) ++copy_shots_seated;
    return;
  }
  // 2.2, host: a person of its world stream fires (Package C): the rounds
  // and the first pellet's hit point go with the person's records.
  if(human && hosting.load() && !replicated_depth && WorldFireCapture(c,b,human)) return;
  if(!human || human!=api->read_u32(0x8309ABEC) || !connected.load() || replicated_depth) { original_fire(c,b); return; }
  const uint32_t ammo=EquippedAmmo(human);
  const uint32_t rounds=ammo ? api->read_u32(ammo+16) : 0, timer=ammo ? api->read_u32(ammo+24) : 0;
  ++capture_hit; hit_valid=false;
  original_fire(c,b);
  --capture_hit;
  // A shot re-arms the refire timer and uses a round; a call that changed
  // neither fired nothing.
  if(!ammo || (api->read_u32(ammo+16)==rounds && api->read_u32(ammo+24)==timer)) return;
  const uint32_t info=api->read_u32(ammo+8);
  const int32_t w=WeaponIndex(info);
  const uint32_t type=api->read_u32(info+100);
  if(w<0 || type>1) return;   // bullets (0) and rockets (1); throws go as kEventThrow
  Event e{kEventFire,{uint32_t(w+1)},0};
  if(type==0 && hit_valid) { std::memcpy(&e.a[1],hit_point,12); e.a[4]=1; }
  QueueEvent(e);
  ++shots_sent;
  if(type==0 && api->read_u32(info+96)!=0) auto_trigger_down=true;   // Trigger_Type burst
}
// 8246FD18(&hit_out, &victim_out, human, info, &origin, &end, &muzzle): one
// pellet's ray. In our player's shot (2.2: and the host's people's, by the
// shooter r5) the first pellet's hit point is kept:
// the ray's hit, or its end when it hit nothing (a point off the ray is not
// taken: the out value is read as research-grade M).
WmlGuestFunction original_bullet_ray=nullptr;
void BulletRayHook(WmlContext* c,uint8_t* b) {
  const uint32_t out=uint32_t(api->get_r(c,3)), origin=uint32_t(api->get_r(c,7)), end=uint32_t(api->get_r(c,8));
  const bool want=(capture_hit || (capture_shooter && uint32_t(api->get_r(c,5))==capture_shooter)) && !hit_valid && !replicated_depth;
  float o[3]={0,0,0}, e[3]={0,0,0};
  auto read=[](uint32_t a,float* v) {
    if(!Readable(a,12)) return false;
    for(int i=0;i<3;++i) { v[i]=api->read_f32(a+i*4); if(!std::isfinite(v[i]) || std::abs(v[i])>100000) return false; }
    return true;
  };
  const bool have_ray=want && read(origin,o) && read(end,e);
  original_bullet_ray(c,b);
  if(!have_ray) return;
  float h[3];
  if(read(out,h)) {
    // On the ray (within 1.5 m of it, between its ends)?
    float d[3], v[3], dd=0, t=0;
    for(int i=0;i<3;++i) { d[i]=e[i]-o[i]; v[i]=h[i]-o[i]; dd+=d[i]*d[i]; t+=v[i]*d[i]; }
    if(dd>0.01f) {
      t/=dd;
      float off=0; for(int i=0;i<3;++i) { const float q=v[i]-d[i]*t; off+=q*q; }
      if(t>=0.0f && t<=1.01f && off<1.5f*1.5f) { std::memcpy(hit_point,h,12); hit_valid=true; return; }
    }
  }
  std::memcpy(hit_point,e,12); hit_valid=true;
}
// 82480660(): our player let go of the trigger. For an automatic gun the
// release goes out too: their character stops firing there (loop and release
// sounds, the trigger latch), as multiplayer does (R 3.2).
WmlGuestFunction original_trigger_release=nullptr;
void TriggerReleaseHook(WmlContext* c,uint8_t* b) {
  original_trigger_release(c,b);
  if(!auto_trigger_down) return;
  auto_trigger_down=false;
  if(!connected.load() || replicated_depth) return;
  const uint32_t player=api->read_u32(0x8309ABEC);
  const int32_t w=Readable(player,4252) ? HandWeapon(player) : -1;
  QueueEvent(Event{kEventFireRelease,{uint32_t(w+1)},0});
}
// 82475E30(human, r4): a throw starts (grenade, pipe bomb, Molotov): pitch,
// raise, throw action, sound; the projectile follows at the action's release
// (824726F0). The controls (824806D0) set the strength at +1940 first. Our
// player's throws are sent with it; the other games replay the throw on our
// character (as multiplayer's 823B24D0 does), whose own projectile is refused
// there: the copy comes from our projectile's message.
WmlGuestFunction original_throw=nullptr;
void ThrowHook(WmlContext* c,uint8_t* b) {
  const uint32_t human=uint32_t(api->get_r(c,3)), player=api->read_u32(0x8309ABEC);
  const bool mine=human && human==player && connected.load() && !replicated_depth && Readable(player,4252);
  const int32_t w=mine ? HandWeapon(player) : -1;
  original_throw(c,b);
  if(!mine || w<0) return;
  const float strength=api->read_f32(player+1940);
  QueueEvent(Event{kEventThrow,{uint32_t(w+1)},std::isfinite(strength) ? strength : 1.0f});
  ++shots_sent;
}
// 824581C8(victim, attacker, hit pos, ?, shooter pos, info): a bullet hits a
// person (damage, ragdoll shove). Its callers don't use a result. Skipped for
// a replayed shot.
WmlGuestFunction original_bullet_hit=nullptr;
void BulletHitHook(WmlContext* c,uint8_t* b) {
  if(fire_replay_depth) return;
  original_bullet_hit(c,b);
}

// ---- Projectiles: one real one (the owner's), copies elsewhere ----
// 8241FCB0(owner handle, weapon info, &pos, &rows, &velocity) makes a rocket
// or thrown object (its pointer, 0 when the pool is full); 8241EFE0 flies it;
// 825BF418(proj) explodes it (fuse, contact, or chained by an explosion);
// 8241EE80(proj, 0, 0) ends it (explodes first in single player, then
// deletes it). The fuse and range ends of 8241EFE0 explode / delete it
// without 8241EE80. proj +20 position, +68 handle (each one made gets a new
// one: the 36-slot pool reuses its pointers), +76 last position (the
// explosion's ray starts there), +208 weapon info, +228 owner handle
// (R 2.4/2.5, I A.2-A.4).
#pragma pack(push,1)
struct ProjPacket { char magic[4]; uint32_t kind, id; int32_t weapon; float pos[3], rows[9], vel[3]; uint32_t shooter; };  // WPJ1: kind 1 made, 2 exploded; id = the owner's handle for it; shooter (2.2) 0 the sender's player, else the id of a person of the host's world stream
#pragma pack(pop)
struct ProjOut { uint8_t target; ProjPacket p; };
std::vector<ProjOut> proj_out;                     // game thread -> network (event_mutex): to everyone, or (2.2: a person of the host's world's) to each member of its area
struct ProjIn { uint8_t origin; ProjPacket p; };
std::vector<ProjIn> proj_in;                       // network -> game thread (event_mutex)
struct ProjCopy { uint32_t proj=0, handle=0, owner=0; int32_t weapon=-1; uint64_t made=0; };
std::unordered_map<uint64_t,ProjCopy> proj_copies; // (sender slot, their handle) -> copy here
int making_copy=0, explosion_depth=0, fire_update_depth=0;
uint32_t replaying_proj=0;   // the copy whose owner's explosion is being played here
std::atomic<uint32_t> projs_sent{0}, projs_copied{0}, projs_refused{0}, booms_sent{0}, booms_played{0}, booms_held{0};
// A live projectile (in the active list *0x8309A494, next at +204).
bool LiveProjectile(uint32_t proj) {
  const uint32_t head=api->read_u32(0x8309A494);
  unsigned guard=0;
  for(uint32_t p=head;p && Readable(p,232) && guard++<256;) {
    if(p==proj) return true;
    p=api->read_u32(p+204);
    if(p==head) break;
  }
  return false;
}
uint32_t ProjHandle(uint32_t proj) { return proj && Readable(proj,72) ? api->read_u32(proj+68) : 0; }
// One of our copies (by pointer and handle: a pointer of a copy the game
// ended without telling us can come back as another projectile).
bool IsProjCopy(uint32_t proj) {
  if(!proj) return false;
  const uint32_t h=ProjHandle(proj);
  bool copy=false;
  for(auto it=proj_copies.begin();it!=proj_copies.end();) {
    if(it->second.proj!=proj) { ++it; continue; }
    if(h && it->second.handle==h) { copy=true; ++it; }
    else it=proj_copies.erase(it);   // that slot is something else now
  }
  return copy;
}
WmlGuestFunction original_proj_create=nullptr;
void ProjectileCreateHook(WmlContext* c,uint8_t* b) {
  const uint32_t owner=uint32_t(api->get_r(c,3)), info=uint32_t(api->get_r(c,4));
  const uint32_t pos=uint32_t(api->get_r(c,5)), rows=uint32_t(api->get_r(c,6)), vel=uint32_t(api->get_r(c,7));
  if(making_copy || !connected.load()) { original_proj_create(c,b); return; }
  // Another player's character: their game's projectile is the real one
  // (the fire function and the throw take 0 here, R 3.3).
  if(owner && IsAvatarHandle(owner)) { ++projs_refused; api->set_r(c,3,0); return; }
  // 2.2 (Package C): nor a copy of the host's people's: the replayed rocket
  // or throw of a copy makes none here; the host's person's comes as a copy.
  if(owner && !hosting.load() && WorldActionsWith(0) && WorldShown() && WorldPersonId(owner,true)) { ++copy_launches_refused; api->set_r(c,3,0); return; }
  const uint32_t player=api->read_u32(0x8309ABEC);
  const bool ours=owner && Readable(player,72) && owner==api->read_u32(player+68);
  const uint32_t shooter=!ours ? WorldProjShooter(owner) : 0;   // (2.2: the host's person's, to its area's members)
  ProjPacket p{{'W','P','J','1'},1,0,WeaponIndex(info),{},{},{},shooter};
  const bool readable=(ours || shooter) && Readable(pos,12) && Readable(rows,36) && Readable(vel,12);
  if(readable) {
    for(int i=0;i<3;++i) { p.pos[i]=api->read_f32(pos+i*4); p.vel[i]=api->read_f32(vel+i*4); }
    for(int i=0;i<9;++i) p.rows[i]=api->read_f32(rows+i*4);
  }
  original_proj_create(c,b);
  const uint32_t proj=uint32_t(api->get_r(c,3));
  // A new projectile in a reused slot: whatever we kept for that pointer is stale.
  if(proj) for(auto it=proj_copies.begin();it!=proj_copies.end();) it=it->second.proj==proj ? proj_copies.erase(it) : std::next(it);
  // (Ended already inside the call - a point-blank rocket - : nothing to send;
  // its explosion was sent before its creation would be.)
  if(!readable || !proj || p.weapon<0 || !LiveProjectile(proj)) return;
  p.id=ProjHandle(proj);
  if(!p.id) return;
  if(shooter) { WorldProjOut(p); return; }
  { std::lock_guard lock(event_mutex); if(proj_out.size()<256) proj_out.push_back({coopnet::kEveryone,p}); }
  ++projs_sent;
}
WmlGuestFunction original_proj_detonate=nullptr;
void ProjectileDetonateHook(WmlContext* c,uint8_t* b) {
  const uint32_t proj=uint32_t(api->get_r(c,3));
  // Copies never explode by themselves (also not one made right now, and not
  // when an explosion played here sets them off): the owner's comes. Only the
  // copy whose owner's explosion is being played explodes.
  if(proj && proj!=replaying_proj && (making_copy || IsProjCopy(proj))) { ++booms_held; return; }
  const uint32_t player=api->read_u32(0x8309ABEC);
  // Ours (also when another player's explosion set it off): sent. 2.2: a
  // person's of the host's world stream too, to its area's members.
  const bool live=connected.load() && proj && Readable(proj,240);
  const bool ours=live && Readable(player,72) && api->read_u32(proj+228)==api->read_u32(player+68);
  const bool replay=proj==replaying_proj || IsProjCopy(proj);   // (a member's projectile played here: theirs)
  uint32_t shooter=live && !ours && !replay && hosting.load() ? WorldProjSent(ProjHandle(proj)) : 0;
  if(live && !ours && !replay && !shooter) shooter=WorldProjShooter(api->read_u32(proj+228));
  if(ours || shooter) {
    // (its weapon from its own info: set at creation, before anything can end it)
    ProjPacket p{{'W','P','J','1'},2,ProjHandle(proj),WeaponIndex(api->read_u32(proj+208)),{},{},{},shooter};
    for(int i=0;i<3;++i) p.pos[i]=api->read_f32(proj+20+i*4);
    for(int i=0;i<9;++i) p.rows[i]=api->read_f32(proj+32+i*4);
    if(shooter) WorldProjOut(p);
    else { { std::lock_guard lock(event_mutex); if(proj_out.size()<256) proj_out.push_back({coopnet::kEveryone,p}); } ++booms_sent; }
  }
  original_proj_detonate(c,b);
}
WmlGuestFunction original_proj_end=nullptr;
void ProjectileEndHook(WmlContext* c,uint8_t* b) {
  const uint32_t proj=uint32_t(api->get_r(c,3));
  const bool copy=IsProjCopy(proj);
  const uint32_t handle=ProjHandle(proj);
  original_proj_end(c,b);   // (a copy's own explosion inside is held by ProjectileDetonateHook)
  if(copy) for(auto it=proj_copies.begin();it!=proj_copies.end();) it=it->second.proj==proj ? proj_copies.erase(it) : std::next(it);
  if(handle && hosting.load()) WorldProjEnded(handle);   // (2.2: one sent for the host's people is forgotten)
}
// 82138AE8(def, source, attacker, rows, pos, ...): an explosion. Its damage to
// copies of the host's people isn't sent from a client (the host plays the
// same explosion and decides for its people); each game applies it to its
// own player (R 3.0).
// 2.1: a car copy wrecked here that wasn't shown intact just before (made of
// a car wrecked before, or replayed late) is wrecked without its explosion:
// 82542650 only keeps the result (+50292).
WmlGuestFunction original_explosion=nullptr;
uint32_t quiet_wreck=0;
void ExplosionHook(WmlContext* c,uint8_t* b) {
  if(quiet_wreck && uint32_t(api->get_r(c,4))==quiet_wreck) { api->set_r(c,3,0); return; }
  // 2.2 (Package C): the host sends its world's to the area; a member holds
  // its own copy of one the host sends (a copy car's, a prop broken already).
  if(connected.load() && world_actions && !WorldExplosion(c)) { api->set_r(c,3,0); return; }
  ++explosion_depth;
  original_explosion(c,b);
  --explosion_depth;
}
// 8245ED40 (world update): burning people. Each tick hurts the burning one
// (returning to 8245F11C); in single player it also sets people within 1.1 m
// alight through 8245E3A0, which hurts them itself (8245EC38), then adds a
// tick (8245F374), with the burning character as the attacker. Each game
// plays fire on its own (R 3.0), so none of it is sent.
WmlGuestFunction original_fire_update=nullptr;
void FireUpdateHook(WmlContext* c,uint8_t* b) {
  ++fire_update_depth;
  original_fire_update(c,b);
  --fire_update_depth;
}
WmlGuestFunction original_damage=nullptr;
void DamageHook(WmlContext* c,uint8_t* b) {
  // A replayed shot of another player hurts no one here (TryRemoteShots).
  if(fire_replay_depth) { api->set_f(c,1,0.0); return; }
  const uint32_t victim=uint32_t(api->get_r(c,3)), attacker=uint32_t(api->get_r(c,4));
  const int victim_slot=victim ? AvatarSlotOf(victim) : -1;   // another player's character
  const uint32_t sp=uint32_t(api->get_r(c,1));
  // Joiner: copies of the host's people take no damage here. Our player's
  // hits on them go to the host, whose game applies them to the real person
  // (a kill there comes back as a death).
  // Joiner: this game's own mission characters are hidden while the host's
  // are shown; hidden ones don't hurt anyone.
  if(connected.load() && !hosting.load() && attacker && Readable(attacker,4252) && api->read_u32(attacker+72)==1 &&
     (api->read_u32(attacker+120)&0x08000000u)) { api->set_f(c,1,0.0); return; }
  // 2.1, joiner: a copy of the host's people (on foot, in its traffic, a
  // stand-in) hurts our player only through the host, whose person's hits on
  // our character there come here replayed (kEventPlayerHitByPed); a local hit
  // would count twice. Not explosions or fire (each game plays them on its own
  // player, R 3.0), nor a car hitting our player (82451C98, from the contact
  // callback 82278E18, with its driver as the attacker): the host's car hits
  // our character there with no damage.
  if(connected.load() && !hosting.load() && !replicated_depth && !explosion_depth && !fire_update_depth && victim &&
     victim==api->read_u32(0x8309ABEC) && attacker && Readable(attacker,4252) && api->read_u32(attacker+72)==1 &&
     !CarHitsPerson(api->get_lr(c)) && OurCharacter(api->read_u32(attacker+68))) {
    ++copy_hits_zeroed; api->set_f(c,1,0.0); return;
  }
  if(connected.load() && !hosting.load() && !replicated_depth && victim && Readable(victim,4252)) {
    if(const uint32_t host_handle=SharedPedHostHandle(api->read_u32(victim+68))) {
      // (Not from an explosion, nor from the burning people update 8245ED40 -
      // burn ticks and the fire spreading to people nearby: each game plays
      // the same explosion and its fire, R 3.0.)
      if(attacker && attacker==api->read_u32(0x8309ABEC) && !explosion_depth && !fire_update_depth) {
        Event e{kEventPedDamage,{host_handle,uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7)),
                                 uint32_t(api->get_r(c,8)),uint32_t(api->get_r(c,9)),uint32_t(api->get_r(c,10)),
                                 uint32_t(api->read_u8(sp+87))|uint32_t(api->read_u8(sp+95))<<8},float(api->get_f(c,1))};
        QueueEventTo(e,0); ++ped_hits_sent;   // the host's person: the host's game applies it
      }
      api->set_f(c,1,0.0);
      return;
    }
    // 2.1.1: the crews of its traffic too (drivers, passengers, seat
    // stand-ins): they took our bullets here and died on this screen alone
    // (the host's cop drove on; the body here was refused removal every
    // frame). The host knows them by their seat in its car (the record names
    // their models, not handles): it hits whoever sits there, so the hit can
    // kill and the kill is credited to us (KillCrimeHook).
    // 2.2: a crew member of the world stream is a person of the host's (an
    // id of its own): hit as one (kEventPedDamage; spec A/11).
    if(const uint32_t id=WorldPersonId(api->read_u32(victim+68),true)) {
      if(attacker && attacker==api->read_u32(0x8309ABEC) && !explosion_depth && !fire_update_depth) {
        Event e{kEventPedDamage,{id,uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7)),
                                 uint32_t(api->get_r(c,8)),uint32_t(api->get_r(c,9)),uint32_t(api->get_r(c,10)),
                                 uint32_t(api->read_u8(sp+87))|uint32_t(api->read_u8(sp+95))<<8},float(api->get_f(c,1))};
        QueueEventTo(e,0); ++ped_hits_sent;
      }
      api->set_f(c,1,0.0);
      return;
    }
    uint32_t host_car=0, seat=0;
    if(CrewSeat(api->read_u32(victim+68),host_car,seat)) {
      if(attacker && attacker==api->read_u32(0x8309ABEC) && !explosion_depth && !fire_update_depth) {
        Event e{kEventCrewDamage,{host_car,uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7)),
                                  uint32_t(api->get_r(c,8)),uint32_t(api->get_r(c,9)),uint32_t(api->get_r(c,10)),
                                  uint32_t(api->read_u8(sp+87))|uint32_t(api->read_u8(sp+95))<<8|seat<<16},float(api->get_f(c,1))};
        QueueEventTo(e,0); ++crew_hits_sent;
      }
      api->set_f(c,1,0.0);
      return;
    }
  }
  if(victim_slot>=0 && connected.load()) {
    // Host: one of its people (a mission enemy, a cop...) hit a client's
    // stand-in. That client's game applies it to the real player (with its
    // copy of that person as the attacker, or none).
    // 2.1: from any person here, not only the shared ones on foot: the police
    // response's units (82479AB0(4, ...), never on the ambient list) and
    // traffic crews hurt the stand-in to no effect. From people it doesn't
    // share only their own hits: not a car hitting the stand-in (82451C98:
    // the client's copy of the car hits there), nor explosions or fire (each
    // game plays those on its own player, R 3.0).
    if(hosting.load() && attacker && attacker!=api->read_u32(0x8309ABEC) && Readable(attacker,4252) && api->read_u32(attacker+72)==1 &&
       AvatarSlotOf(attacker)<0 && !CarHitsPerson(api->get_lr(c)) &&
       (IsSentPed(api->read_u32(attacker+68)) || (!explosion_depth && !fire_update_depth)) &&
       !((explosion_depth || fire_update_depth) && WorldMemberWithActions(victim_slot))) {   // (2.2 Package C: a member plays the host's people's explosions, projectiles and fire on its own player, R 3.0)
      Event e{kEventPlayerHitByPed,{api->read_u32(attacker+68),uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7)),
                                    uint32_t(api->get_r(c,8)),uint32_t(api->get_r(c,9)),uint32_t(api->get_r(c,10)),
                                    uint32_t(api->read_u8(sp+87))|uint32_t(api->read_u8(sp+95))<<8},float(api->get_f(c,1))};
      QueueEventTo(e,uint8_t(victim_slot)); ++hits_from_people;
      api->set_f(c,1,0.0);
      return;
    }
    if(forward_damage && attacker==api->read_u32(0x8309ABEC) && !explosion_depth && !fire_update_depth) {   // (their game plays explosions and fire on its own player)
      Event e{kEventDamage,{uint32_t(api->get_r(c,5)),uint32_t(api->get_r(c,6)),uint32_t(api->get_r(c,7)),
                            uint32_t(api->get_r(c,8)),uint32_t(api->get_r(c,9)),uint32_t(api->get_r(c,10)),
                            api->read_u8(sp+87),api->read_u8(sp+95)},float(api->get_f(c,1))};
      QueueEventTo(e,uint8_t(victim_slot)); ++hits_sent;
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
  if(running.load() && components_slots) {
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
uint64_t local_enter_time=0;  // our player's last seat request
uint32_t passenger_watch_car=0; uint64_t passenger_watch_since=0;  // our passenger getting-in, forced if it stalls
uint32_t AdoptSharedCar(GuestScratch& scratch,uint32_t host_id,uint64_t now);
uint32_t SharedCarHostHandle(uint32_t local);   // client: host handle of a traffic copy (0 if none)
uint32_t SeatedIn(uint32_t car_handle,uint32_t seat);
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
  // (v2: the nearest car another player drives, within 6 m; one try.)
  uint32_t acar=0;
  if(mine && api->read_u32(r+12)==0xFFFFFFFFu && api->read_u32(r+8)==0) {
    const uint32_t asked=api->read_u32(r+4);
    float best=6.0f*6.0f;
    for(int slot=0;slot<kSlots;++slot) if(const uint32_t av=AvatarObjOf(slot)) {
      const uint32_t car=ResolveVehicle(api->read_u32(av+2496));
      if(!car || api->read_u32(av+2500)!=0 || !(asked==0 || asked==car)) continue;
      const float dx=api->read_f32(car+20)-api->read_f32(player+20), dz=api->read_f32(car+28)-api->read_f32(player+28);
      if(dx*dx+dz*dz<best) { best=dx*dx+dz*dz; acar=car; }
    }
  }
  if(acar) {
    {
      // A free passenger seat (other players may sit in the others).
      uint32_t seat=0;
      for(uint32_t s2=1;s2<=3 && !seat;++s2) if(!SeatedIn(api->read_u32(acar+68),s2)) seat=s2;
      if(seat) {
        uint32_t saved[7]; for(unsigned i=0;i<7;++i) saved[i]=api->read_u32(r+i*4);
        // Straight into the passenger seat (the game's own getting in took
        // the player to the driver's seat whatever seat was asked, and each
        // game then had a different driver).
        api->write_u32(r+4,acar); api->write_u32(r+8,3); api->write_u32(r+12,seat);
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
  uint32_t owner=0; const uint32_t id=CarId(h,owner);
  Event e{kEventEnterCar,{id,owner,req[2],req[3],req[4],req[5],req[6]},0};
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
    if(who && who!=me && AvatarSlotOf(who)<0 && Readable(who,4252) && me && api->read_u32(me+2496) && api->read_u32(who+2496)==api->read_u32(me+2496)) {
      Event e{kEventVictimExit,{},0};
      for(unsigned i=0;i<8;++i) e.a[i]=api->read_u32(r+4+i*4);
      QueueEvent(e);
      static unsigned logged=0;
      if(logged++<20) { char line[140]; std::snprintf(line,sizeof(line),"Pulled-out driver's exit sent (%08X %08X %08X %08X)",e.a[0],e.a[2],e.a[6],e.a[7]); ProbeLog(line); }
    }
  }
  // 2.1, joiner: a crew member of the host's traffic asking to get out here
  // (not a co-op replay). 2.1.1: refused unless it is the driver this player
  // pulls out (the game's own throw-out, 8257D77C): 13 crews left their cars
  // under fire here while the host's sat on. Most asked through 8244D5F8, a
  // generic exit (LR 8244D66C; 128-byte frame: the request at r1+80, its
  // caller's LR at r1+120) whose nine callers (teleports, AI behaviours,
  // scripts) read no result; 8257B8E0 returns no value of its own (r3 as the
  // last call left it: 0 from 82448010 when the character isn't in a car),
  // so 0 stands for "did nothing".
  if(connected.load() && !hosting.load() && !replicated_depth && Readable(r,36)) {
    const uint32_t who=api->read_u32(r), me=api->read_u32(0x8309ABEC);
    if(who && who!=me && Readable(who,4252) && CrewCharacter(api->read_u32(who+68))) {
      const bool pullout=me && api->read_u32(me+2496) && api->read_u32(who+2496)==api->read_u32(me+2496);
      const uint32_t lr=api->get_lr(c), sp=uint32_t(api->get_r(c,1));
      const uint32_t asker=lr==0x8244D66C && sp+80==r ? api->read_u32(sp+120) : 0;
      if(pullout) ++crew_exits; else ++crew_exits_refused;
      static unsigned logged=0;
      if(logged++<20) {
        char from[40]; std::snprintf(from,sizeof(from),"%08X",lr);
        if(asker) std::snprintf(from+8,sizeof(from)-8," for %08X",asker);
        char line[160];
        std::snprintf(line,sizeof(line),"Traffic crew member %08X asks to get out (from %s): %s",api->read_u32(who+68),from,pullout?"this player's pull-out":"let through (2.1.3)");
        ProbeLog(line);
      }
      // 2.1.3: let through again. 2.1.2 refused these, and the first asker
      // seen was the game's own character removal (824789A8, return 82478AC0:
      // the human is taken out of its car before it goes); a refusal there
      // leaves a deleted character in the seat (the 1.64 freeze class). Which
      // asker is the crews' own AI is read from these lines first.
    }
  }
  original_exit_request(c,b);
}
void DetonateHook(WmlContext* c,uint8_t* b);
extern WmlGuestFunction original_detonate;
void CarHpHook(WmlContext* c,uint8_t* b);
extern WmlGuestFunction original_car_hp;
void CarBlastHook(WmlContext* c,uint8_t* b);
extern WmlGuestFunction original_car_blast;
void CarDestroyedHook(WmlContext* c,uint8_t* b);
extern WmlGuestFunction original_car_destroyed;
void VehicleRemoveHook(WmlContext* c,uint8_t* b);
extern WmlGuestFunction original_vehicle_remove;
// 82444270(human, ...): starts a climb or vault when a ledge is in front.
// The other player's character here never starts one itself: it was pushed
// into walls by its move orders and began its own climb 0.3 s before theirs
// arrived, then climbed again (two climbs fighting). Their climb is replayed.
WmlGuestFunction original_climb_start=nullptr;
unsigned own_climbs_blocked=0;
void ClimbStartHook(WmlContext* c,uint8_t* b) {
  if(connected.load() && !replicated_depth && AvatarSlotOf(uint32_t(api->get_r(c,3)))>=0) {
    ++own_climbs_blocked; api->set_r(c,3,0); return;
  }
  original_climb_start(c,b);
}
// The world registry (2.2, with the network thread): what this game makes and removes.
void WorldCarMade(uint32_t handle,uint32_t source);
void WorldObjectGone(uint32_t obj,bool car);
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
unsigned copies_kept=0, copies_let_go=0; uint32_t copies_kept_from=0;
std::unordered_map<uint32_t,uint64_t> destroy_asks;
bool keep_copies=true;
extern bool debug_cars;
std::string RiderText(uint32_t h);
bool ListedBy(uint32_t vehicle,uint32_t human);
void CarCheckLog(const std::string& what,uint32_t lr);
void HumanDestroyHook(WmlContext* c,uint8_t* b) {
  const uint32_t obj=uint32_t(api->get_r(c,3));
  const uint32_t lr=uint32_t(api->get_lr(c));
  // Never kept: people in a car (the car's own removal, 825324D0, takes
  // its occupants with it; one kept in a removed car froze the game), and
  // one the game has asked for 200 times.
  // 2.2: a world copy being faded lets the game's own ask through (its corpse
  // clean-up 824793C0 asks once the fade bits are set; spec 3.10/46).
  const bool fading=Readable(obj,4252) && WorldFading(api->read_u32(obj+68));
  if(fading && !mod_destroying) ++copies_let_go;
  if(keep_copies && !mod_destroying && !fading && connected.load() && Readable(obj,4252) && OurCharacter(api->read_u32(obj+68))) {
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
  // mod.ini debug_cars: someone removed while getting in or out of a car
  // must come off that car's list (VehicleRemoveHook's entries).
  std::string before; uint32_t listed_own=0;
  if(debug_cars && Readable(obj,4252) && (api->read_u8(obj+2569)&0x10)) {
    before=RiderText(obj);
    const uint32_t own=ResolveVehicle(api->read_u32(obj+2496));
    if(own && ListedBy(own,obj)) listed_own=own;
    else CarCheckLog("removing "+before+", getting in or out but not listed by its own car",lr);
  }
  WorldObjectGone(obj,false);   // (out of the world registry, 2.2)
  original_human_destroy(c,b);
  if(listed_own && ListedBy(listed_own,obj)) CarCheckLog("removed "+before+", but its car still lists it",lr);
}
// 82575438(car, entering human, occupant): gets the occupant out of the
// seat the human is getting into. Players never pull each other out: the one
// getting in ended up driving and the other was thrown to the passenger seat.
WmlGuestFunction original_eject=nullptr;
void EjectHook(WmlContext* c,uint8_t* b) {
  const uint32_t who=uint32_t(api->get_r(c,4)), occ=uint32_t(api->get_r(c,5));
  const uint32_t player=api->read_u32(0x8309ABEC);
  auto is_player=[&](uint32_t h) { return h && (h==player || AvatarSlotOf(h)>=0); };
  if(connected.load() && occ && is_player(who) && is_player(occ)) {
    static unsigned logged=0;
    if(logged++<10) ProbeLog("Kept the other player in their seat (not pulled out)");
    api->set_r(c,3,0); return;
  }
  original_eject(c,b);
}
// 825352E0(car, door, f1-f3): a door opening or closing. Our player's car
// door is sent (the other game showed the climb-in with a closed door).
WmlGuestFunction original_door=nullptr;
void DoorHook(WmlContext* c,uint8_t* b) {
  const uint32_t v=uint32_t(api->get_r(c,3));
  const uint32_t player=api->read_u32(0x8309ABEC);
  if(connected.load() && !replicated_depth && player && Readable(v,kVehicleSize) && api->read_u32(v+72)==5 && api->read_u32(v+68)==api->read_u32(player+2496)) {
    uint32_t owner=0; const uint32_t id=CarId(api->read_u32(v+68),owner);
    Event e{kEventCarDoor,{id,owner,uint32_t(api->get_r(c,4))},0};
    for(int i=0;i<3;++i) { const float f=float(api->get_f(c,1+i)); std::memcpy(&e.a[3+i],&f,4); }
    QueueEvent(e);
  }
  original_door(c,b);
}
void ParkedSpawnHook(WmlContext* c,uint8_t* b);  // (with the shared traffic)
extern WmlGuestFunction original_parked_spawn;
void VehicleCreateHook(WmlContext* c,uint8_t* b);  // (a car this player asked for: kept)
extern WmlGuestFunction original_vehicle_create;
void HumanActivateHook(WmlContext* c,uint8_t* b);  // (the world registry, 2.2)
extern WmlGuestFunction original_human_activate;
void HoodSpawnsHook(WmlContext* c,uint8_t* b);     // (a member's levers, 2.2 step 4)
extern WmlGuestFunction original_hood_spawns;
void InstallWorldSpawnHooks();                     // (content around every member, 2.2 Package B)
void InstallWorldActionHooks();                    // (the area's actions and effects, 2.2 Package C)
void GangManagerHook(WmlContext* c,uint8_t* b);  // (with the shared people)
extern WmlGuestFunction original_gang_manager;
void PoliceResponseHook(WmlContext* c,uint8_t* b);
void RoadblocksHook(WmlContext* c,uint8_t* b);
extern WmlGuestFunction original_police_response, original_roadblocks;
void GetUpHook(WmlContext* c,uint8_t* b);       // (with struct Remote)
extern WmlGuestFunction original_get_up;
void EjectCheckHook(WmlContext* c,uint8_t* b);
extern WmlGuestFunction original_eject_check;
void CrimeHook(WmlContext* c,uint8_t* b);        // (with the notoriety, 2.1)
void KillCrimeHook(WmlContext* c,uint8_t* b);
void HitCrimeHook(WmlContext* c,uint8_t* b);
void CarCrimeHook(WmlContext* c,uint8_t* b);
void CarjackCrimeHook(WmlContext* c,uint8_t* b);
void GunSeenHook(WmlContext* c,uint8_t* b);
void PoliceCarCrimeHook(WmlContext* c,uint8_t* b);   // (2.1.2)
void AlertCrimeHook(WmlContext* c,uint8_t* b);
void DetectionHook(WmlContext* c,uint8_t* b);
void DecayHook(WmlContext* c,uint8_t* b);
void BustedHook(WmlContext* c,uint8_t* b);
void GroupLevelHook(WmlContext* c,uint8_t* b);     // (with the police response, 2.1)
void GroupTimersHook(WmlContext* c,uint8_t* b);
void PoliceBudgetHook(WmlContext* c,uint8_t* b);
void GangBudgetHook(WmlContext* c,uint8_t* b);
void GangDominantHook(WmlContext* c,uint8_t* b);
void SpawnRingHook(WmlContext* c,uint8_t* b);
void PursuitOrderHook(WmlContext* c,uint8_t* b);
void PursuitHook(WmlContext* c,uint8_t* b);
void HeliPursuitHook(WmlContext* c,uint8_t* b);
void PursuitLevelHook(WmlContext* c,uint8_t* b);
void PursuitManoeuvreHook(WmlContext* c,uint8_t* b);
extern WmlGuestFunction original_group_level, original_group_timers, original_police_budget, original_gang_budget, original_gang_dominant, original_spawn_ring, original_pursuit_order, original_pursuit, original_heli_pursuit,
                        original_pursuit_level, original_pursuit_manoeuvre;
extern WmlGuestFunction original_crime, original_kill_crime, original_hit_crime, original_car_crime, original_carjack_crime, original_gun_seen, original_police_car_crime, original_alert_crime, original_detection, original_decay, original_busted;
void InstallEventHooks() {
  api->hook(0x82575438,EjectHook,&original_eject);
  api->hook(0x825352E0,DoorHook,&original_door);
  api->hook(0x82444270,ClimbStartHook,&original_climb_start);
  api->hook(0x823ACF50,DestroyHook,&original_destroy);
  api->hook(0x825324D0,VehicleRemoveHook,&original_vehicle_remove);
  api->hook(0x824789A8,HumanDestroyHook,&original_human_destroy);
  api->hook(0x8257B2C0,SeatRequestHook,&original_seat_request);
  api->hook(0x825397E0,DetonateHook,&original_detonate);
  // 2.1: hit points, explosion pushes and wrecks of cars other games drive.
  api->hook(0x8252D2A8,CarHpHook,&original_car_hp);
  api->hook(0x82536B00,CarBlastHook,&original_car_blast);
  api->hook(0x8252BF38,CarDestroyedHook,&original_car_destroyed);
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
  api->hook(0x82470A50,FireHook,&original_fire);
  api->hook(0x824581C8,BulletHitHook,&original_bullet_hit);
  api->hook(0x82403C38,EffectAddHook,&original_effect_add);
  api->hook(0x82403F58,EffectRemoveHook,&original_effect_remove);
  // v2: weapons (hit points, trigger release, throws), projectiles and
  // explosions, knockdowns and crash ejections.
  api->hook(0x8246FD18,BulletRayHook,&original_bullet_ray);
  api->hook(0x82480660,TriggerReleaseHook,&original_trigger_release);
  api->hook(0x82475E30,ThrowHook,&original_throw);
  api->hook(0x8241FCB0,ProjectileCreateHook,&original_proj_create);
  api->hook(0x825BF418,ProjectileDetonateHook,&original_proj_detonate);
  api->hook(0x8241EE80,ProjectileEndHook,&original_proj_end);
  api->hook(0x82138AE8,ExplosionHook,&original_explosion);
  api->hook(0x8245ED40,FireUpdateHook,&original_fire_update);
  api->hook(0x82449FE0,GetUpHook,&original_get_up);
  api->hook(0x82540780,EjectCheckHook,&original_eject_check);
  // v2: a client's own parked cars and gang members while the host's world is shown.
  api->hook(0x8241E378,ParkedSpawnHook,&original_parked_spawn);
  api->hook(0x82569700,VehicleCreateHook,&original_vehicle_create);
  api->hook(0x824189E0,GangManagerHook,&original_gang_manager);
  // 2.2: every person this game activates goes into the world registry (cars through VehicleCreateHook).
  api->hook(0x8247A6D0,HumanActivateHook,&original_human_activate);
  // 2.2: a member of the host's area runs no hood or special spawns of its own (824290D0, the MP client's skip).
  api->hook(0x824290D0,HoodSpawnsHook,&original_hood_spawns);
  // 2.1: and its own police response and roadblocks.
  api->hook(0x8242ABA0,PoliceResponseHook,&original_police_response);
  api->hook(0x82424708,RoadblocksHook,&original_roadblocks);
  // 2.1: notoriety per player: crimes go to the culprit's own game, other
  // players' characters' levels aren't decayed here, arrests per player.
  api->hook(0x821D3148,CrimeHook,&original_crime);
  api->hook(0x82482098,KillCrimeHook,&original_kill_crime);
  api->hook(0x82481CD8,HitCrimeHook,&original_hit_crime);
  api->hook(0x82482568,CarCrimeHook,&original_car_crime);
  api->hook(0x824828C0,CarjackCrimeHook,&original_carjack_crime);
  api->hook(0x82482290,GunSeenHook,&original_gun_seen);
  // 2.1.2: a police car taken and a cop's alert by another player's character
  // go to their game too; the detection test is logged.
  api->hook(0x82482400,PoliceCarCrimeHook,&original_police_car_crime);
  api->hook(0x820DB9D0,AlertCrimeHook,&original_alert_crime);
  api->hook(0x821D1F98,DetectionHook,&original_detection);
  api->hook(0x821D24F0,DecayHook,&original_decay);
  api->hook(0x82483440,BustedHook,&original_busted);
  // 2.1: the host's response sent after a wanted client in its world: the
  // group level, spawn timers, budgets and spawn point read the target
  // (ResponseAvatar), the pursuit orders name it (8242A228 says which group
  // orders one), and the cars after it work at its level.
  api->hook(0x823FCE40,GroupLevelHook,&original_group_level);
  api->hook(0x823FBFE0,GroupTimersHook,&original_group_timers);
  api->hook(0x823FCC80,PoliceBudgetHook,&original_police_budget);
  api->hook(0x823FD008,GangBudgetHook,&original_gang_budget);
  api->hook(0x823FCEB0,GangDominantHook,&original_gang_dominant);
  api->hook(0x8242A540,SpawnRingHook,&original_spawn_ring);
  api->hook(0x8242A228,PursuitOrderHook,&original_pursuit_order);
  api->hook(0x825A91A0,PursuitHook,&original_pursuit);
  api->hook(0x8259DCC8,HeliPursuitHook,&original_heli_pursuit);
  api->hook(0x825A8828,PursuitLevelHook,&original_pursuit_level);
  api->hook(0x825A5C00,PursuitManoeuvreHook,&original_pursuit_manoeuvre);
  InstallWorldSpawnHooks();
  InstallWorldActionHooks();
}

uint32_t SharedCarLocal(uint32_t host_id);
// Who sits in a seat, by every person's car (+2496) and seat (+2500): the
// car's seat slots are empty for most drivers, so a pulled-out driver was
// never found (it stayed in and the player sat on top of it).
uint32_t OccupantHandle(uint32_t vehicle,uint32_t seat);
uint32_t SeatedIn(uint32_t car_handle,uint32_t seat) {
  if(!car_handle) return 0;
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t h=TableObject(index,1,4252);
    if(!h) continue;
    if(api->read_u32(h+2496)==car_handle && api->read_u32(h+2500)==seat) return api->read_u32(h+68);
  }
  const uint32_t v=ResolveVehicle(car_handle);
  return v ? OccupantHandle(v,seat) : 0;
}
// A seat's occupant: the slot when it holds one, else the scan above (the
// slots are empty for most AI crews: other players' bullets never reached
// the host's own traffic crews through them, 2.1.4).
uint32_t SeatOccupant(uint32_t v,uint32_t seat) { if(const uint32_t h=OccupantHandle(v,seat)) return h; return SeatedIn(api->read_u32(v+68),seat); }
// The same, remembered per car and seat for 250 ms: a burst of hits on one
// crew member scanned the 4096 slots once per hit (the hit lands on whoever
// still sits there; nobody found stays nobody for the 250 ms).
uint32_t SeatOccupantMemo(uint32_t v,uint32_t seat,uint64_t now) {
  static std::unordered_map<uint64_t,uint32_t> memo; static uint64_t cleared=0;   // game thread
  if(now-cleared>=250) { memo.clear(); cleared=now; }
  const uint32_t car=api->read_u32(v+68);
  const uint64_t key=uint64_t(car)<<2|seat;
  if(const auto it=memo.find(key);it!=memo.end()) {
    const uint32_t h=it->second, o=h ? ResolveHuman(h) : 0;
    if(!h || (o && api->read_u32(o+2496)==car && api->read_u32(o+2500)==seat)) return h;
  }
  return memo[key]=SeatOccupant(v,seat);
}
std::vector<uint32_t> host_models; uint64_t host_models_time=0;
bool pin_host_models=false;  // mod.ini preload_host_models: asking for models no copy here needs yet (new stream entries) came in with the joiner's freezes (1.11 on)  // joiner: people models the host's game has loaded (game thread)
void OrderExitVehicle(GuestScratch& scratch,uint32_t human);
void DestroyObject(GuestScratch& scratch,uint32_t handle);
uint32_t OccupantHandle(uint32_t vehicle,uint32_t seat);
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
bool Normalize2(float* v) { const float n=std::sqrt(v[0]*v[0]+v[1]*v[1]); if(n<=1e-4f) return false; v[0]/=n; v[1]/=n; return true; }

// Moves another player's character the way the game moves its own player
// (82168D60) and multiplayer's remote players (8237B7B8): the point to walk
// to 5 m ahead in +464, state 1, and the player locomotion 8245FC08(human,
// f1 side, f2 forward, r6 sprint, r7 standing) with the stick as the player
// holds it, every update. The game picks walk, run, sprint, strafe, aim-walk
// and the run turn-around from that and turns the body as the strafe needs
// (state 1, 82460468). A move order (82441670; state 2 is "walking a path")
// left the character sliding with its legs still while its player walked
// with the gun up (the 2.1 tests' "floats" lines). The stick is the direction
// of travel in the frame the player's own game uses: its camera's right and
// forward rows, flat (+4124 / +4148 as WriteRemoteCamera sets them; the
// character's own rows when no camera came), +3028 / +4424 forward and
// +3032 / +4420 sideways as 82168D60 stores them. 8245FC08 takes walk up to
// 0.81 (squared), run above, sprint by flag, and goes by the magnitude and
// its reversals only: the direction is +464. After MoveHuman: its teleport
// (8243E6F8) writes +464 too. gait 0 standing, 1 walk, 2 run, 3 sprint.
// False when the state isn't its to drive (jump, climb, landing, car walk,
// knocked down: 4-8 keep their own handling). last_dir: the way it last
// moved (kept on the Follower), used while the path speed dips inside the
// 150 ms stop debounce (the camera forward there stepped it forward at each
// stop). stick: side, forward (log).
bool DriveAsPlayer(GuestScratch& scratch,uint32_t obj,const float* path_pos,const float* rows,const float* vel,int gait,float* last_dir,float* stick) {
  const uint32_t state=api->read_u32(obj+516);
  if(state>2) return false;
  ++replicated_depth;   // (none of it is the player's own: StateHook, PlayActionHook)
  if(state==2) { scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=0; scratch.ctx.r5.u64=0; scratch.Call(0x82441308); }   // the game's own "cancel the move order" (frees its path)
  float f[2]={api->read_f32(obj+4148),api->read_f32(obj+4156)}, r[2]={api->read_f32(obj+4124),api->read_f32(obj+4132)};
  if(!Normalize2(f) || !Normalize2(r)) { f[0]=rows[6]; f[1]=rows[8]; r[0]=rows[0]; r[1]=rows[2]; Normalize2(f); Normalize2(r); }
  float dir[2]={0,0}, fwd=0, side=0;
  if(gait) {
    dir[0]=vel[0]; dir[1]=vel[2];
    if(Normalize2(dir)) { last_dir[0]=dir[0]; last_dir[1]=dir[1]; }
    else if(last_dir[0]!=0.0f || last_dir[1]!=0.0f) { dir[0]=last_dir[0]; dir[1]=last_dir[1]; }
    else { dir[0]=f[0]; dir[1]=f[1]; }
    const float push=gait==1 ? 0.6f : 1.0f;
    fwd=(dir[0]*f[0]+dir[1]*f[1])*push; side=(dir[0]*r[0]+dir[1]*r[1])*push;
  }
  api->write_f32(obj+464,path_pos[0]+dir[0]*5.0f); api->write_f32(obj+468,path_pos[1]); api->write_f32(obj+472,path_pos[2]+dir[1]*5.0f);
  api->write_f32(obj+3028,fwd); api->write_f32(obj+4424,fwd); api->write_f32(obj+3032,side); api->write_f32(obj+4420,side);
  if(api->read_u32(obj+516)==0) { scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=1; scratch.ctx.r5.u64=0; scratch.Call(0x82441308); }
  scratch.ctx.r3.u64=obj; scratch.ctx.f1.f64=side; scratch.ctx.f2.f64=fwd;
  scratch.ctx.r6.u64=gait==3 ? 1 : 0; scratch.ctx.r7.u64=gait ? 0 : 1;
  scratch.Call(0x8245FC08);
  --replicated_depth;
  if(stick) { stick[0]=side; stick[1]=fwd; }
  return true;
}

// How another player's character is moving right now (what its own
// animations carry); null for the host's people.
struct AvatarMotion {
  uint64_t action_until=0;      // no move orders while an action plays
  uint64_t car_anim_until=0;    // getting in / out plays: the on-foot follower leaves the character alone
  bool jumping=false;           // between jump start (state 4/1) and landing (4/0xE)
  bool reorder=false;           // give the character a fresh move order (after landing)
  bool climbing=false;          // a climb plays: its animation moves the character over the wall
};
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
  uint64_t slow_since=0;       // when the path speed dropped below walking (stop debounce)

  int base_delay=-1; // the configured delay; the working one grows with network jitter
  // One position far off the path (the other player's hit reaction sent
  // one 742 m away: they vanished for a moment) is held until the next one
  // confirms it; a real teleport is only one packet late.
  bool suspect=false; float suspect_pos[3]{};
  // 2.1.1 diagnostic: where the last update left it (MoveHuman) and since
  // when its own locomotion has not moved it from there (the first 2.1 test:
  // a character floated along its path for 3 s after its replayed exit).
  float left_pos[3]{}; uint64_t left_at=0, still_since=0, last_order=0, next_still_log=0;
  // 2.1.4 diagnostic (walk = 1): updates in a row moved as a player, lines logged (10 per character made).
  unsigned walk_updates=0, walk_logged=0;
  float walk_dir[2]{};   // the way it last moved as a player (DriveAsPlayer's stop debounce)
  void Reset() { count=0; offset=INT64_MAX; recent_count=0; recent_next=0; next_order=0; moving=false; left_at=0; still_since=0; last_order=0; walk_updates=0; walk_dir[0]=walk_dir[1]=0; if(base_delay>=0) delay=base_delay; }

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
    while(j>0 && int32_t(last.time-snaps[j].time)<100) --j;
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

  // Moves obj along the path. Uses scratch data+1300..1640. av: another
  // player's character (its own jumps, climbs and car moves).
  bool Drive(GuestScratch& scratch,uint32_t obj,uint64_t now,const char* tag,AvatarMotion* av=nullptr) {
    if(!count) return false;
    const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316, velocity=data+1360;
    float path_pos[3], rows[9], vel[3];
    const float path_speed=Sample(uint32_t(int64_t(now)-offset-delay),path_pos,rows,vel);
    // How far the character is from where it should be (before this update's
    // move). Teleports usually close this; locomotion lag shows here.
    const float gx=path_pos[0]-api->read_f32(obj+20), gz=path_pos[2]-api->read_f32(obj+28);
    const float gap=std::sqrt(gx*gx+gz*gz);
    gap_sum+=gap; gap_max=std::max(gap_max,gap); ++gap_count; last_gap=gap;
    if(left_at && now-left_at<200) {
      const float mx=api->read_f32(obj+20)-left_pos[0], my=api->read_f32(obj+24)-left_pos[1], mz=api->read_f32(obj+28)-left_pos[2];
      if(mx*mx+my*my+mz*mz<0.01f*0.01f) { if(!still_since) still_since=now; } else still_since=0;
    } else still_since=0;
    // Thrown far off by something played here (2.0: the other player's
    // replayed get-up sent their character ~740 m off to the get-up spots it
    // never had, see SetGetUpSpots; they vanished): back on the path at once.
    if(gap>25.0f && av && count>=2) {
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
    if(av && now<av->car_anim_until && gap<6.0f) return true;
    const float speed=path_speed<0.3f ? 0.0f : path_speed+(gap>1.0f ? gap*1.5f : 0.0f);
    for(unsigned i=0;i<3;++i) { api->write_f32(pos+i*4,path_pos[i]); api->write_f32(velocity+i*4,vel[i]); }
    for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,rows[i]);
    const bool is_avatar=av!=nullptr;
    if(av && now<av->action_until && !av->jumping && path_speed>2.0f) av->action_until=0;
    // In the air (jumping, falling, thrown): the game refuses move orders and
    // its own jump physics drifted up to ~2 m from where the other player
    // really is. Put the character on the path every update until it lands.
    if(av && gap<6.0f && std::abs(vel[1])>2.0f && !av->climbing) {  // stairs stay below 2 m/s vertical
      for(unsigned i=0;i<3;++i) api->write_f32(obj+20+i*4,path_pos[i]);
      for(unsigned i=0;i<9;++i) api->write_f32(obj+32+i*4,rows[i]);
      ++airborne_placed;
      if(moving) { moving=false; next_order=0; order_mode=0; }
      return true;
    }
    // The gait from the path speed, with hysteresis: keep the current one
    // near its band edges so the walk cycle doesn't switch back and forth.
    int gait=0;
    if(speed>0.6f) {
      gait=speed<2.2f ? 1 : speed<5.5f ? 2 : 3;
      if(order_mode) {
        const float lo=order_mode==1?0.0f:order_mode==2?2.2f:5.5f, hi=order_mode==1?2.2f:order_mode==2?5.5f:1e9f;
        if(speed>lo-0.4f && speed<hi+0.4f) gait=order_mode;
      }
    }
    // 2.1.4: another player's character is moved as a player (DriveAsPlayer,
    // after MoveHuman below), not on move orders; the host's people copies
    // keep theirs. Only a character from the player pool (IsPlayerObject,
    // 22128 bytes): the stick block at +4420..+4448 that 8245FC08 and
    // 82460468 read lies past the end of an NPC human (4252 bytes), so an
    // NPC-class character shown for a player keeps the move orders. The stop
    // waits 150 ms as the stop order did (1.62: a quick turn dips the path
    // speed for a moment).
    const bool as_player=is_avatar && walk_as_player && IsPlayerObject(obj);
    if(as_player) {
      if(gait) slow_since=0;
      else if(moving) { if(!slow_since) slow_since=now; if(now-slow_since<150) gait=order_mode; else slow_since=0; }
      order_mode=gait; moving=gait>0;
      if(gait) last_order=now;   // (the "floats" line below keeps watching)
    } else if(speed>0.6f && !(av && now<av->action_until)) {
      // Keep one steady order: re-aim only every 400 ms, or sooner when the
      // direction or gait changes, so the walk cycle doesn't stop and start.
      const int mode=gait;
      const float heading=std::atan2(vel[0],vel[2]);
      float turn=std::abs(heading-order_heading); if(turn>3.14159f) turn=6.28318f-turn;
      // (Over 25 m off: no route from there, MoveHuman below places it first.)
      if(gap<=25.0f && (now>=next_order || mode!=order_mode || turn>0.25f)) {
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
        next_order=now+250; moving=true; last_order=now;
      }
    } else if(moving && !(av && now<av->action_until)) {
      // (not during a replicated action: the stop order cut climbs and jumps
      // short and the character went idle while still moving over the wall)
      // 1.62: only after 150 ms of standing still - a quick turn dips the
      // speed for a moment and the stop made the character glide in its idle pose.
      if(!slow_since) slow_since=now;
      if(now-slow_since>=150) {
        if(gap<=25.0f) OrderMove(scratch,obj,pos,1); // stop on the spot (over 25 m: MoveHuman places it)
        moving=false; next_order=0; order_mode=0; slow_since=0;
      }
    }
    if(speed>0.6f) slow_since=0;
    // 2.1.1 diagnostic (no change of behaviour): told to move, placed on the
    // path every update, and not moving by itself between the placements.
    if(tag && speed>0.6f && still_since && now-still_since>500 && now-last_order<=250 && now>=next_still_log) {
      next_still_log=now+2000;
      static unsigned logged=0;
      if(logged++<10) {
        char line[220];
        std::snprintf(line,sizeof(line),"%s floats: not moving by itself for %llu ms (order %llu ms ago; +508 %u, state %u/%u, car %08X, action %d, gap %.2f m)",tag,
                      (unsigned long long)(now-still_since),(unsigned long long)(now-last_order),api->read_u32(obj+508),api->read_u32(obj+516),api->read_u32(obj+520),api->read_u32(obj+2496),int32_t(api->read_u32(obj+756)),gap);
        ProbeLog(line);
      }
    }
    // While a replicated action (punch, jump, reaction) plays, let the
    // animation own the character unless the original has moved away.
    if(av && now<av->action_until && gap<3.0f) {
      // Climbing: the animation carries the character up and over; placing
      // it every update broke the climb (it went idle while floating over).
      if(av->climbing && gap<2.5f) return true;
      for(unsigned i=0;i<3;++i) api->write_f32(obj+20+i*4,path_pos[i]);
      for(unsigned i=0;i<9;++i) api->write_f32(obj+32+i*4,rows[i]);
      return true;
    }
    const bool find_node=now>=next_node;
    if(find_node) next_node=now+200;
    const bool moved=MoveHuman(scratch,obj,pos,matrix,velocity,find_node);
    if(as_player) {
      float stick[2]={0,0};
      const bool driven=DriveAsPlayer(scratch,obj,path_pos,rows,vel,gait,walk_dir,stick);
      // 2.1.4 diagnostic: the first updates after it starts moving this way -
      // whether the walk / aim-walk layer (layer 0, +576 at +580 weight; the
      // run turn-around 335/336 shows on layer 1, +764) gets going.
      if(!gait) walk_updates=0;
      else if(tag && (++walk_updates==1 || walk_updates==3 || walk_updates==10) && walk_logged<10) {
        ++walk_logged;
        char line[260];
        std::snprintf(line,sizeof(line),"%s walks here (update %u): state %u/%u, mode %u, gait %u (%d asked), layer 0 action %d at %.2f, layer 1 %d, action %d, gun %s, stick %.2f/%.2f (band %+.0f deg), r3=%X%s",tag,walk_updates,
                      api->read_u32(obj+516),api->read_u32(obj+520),api->read_u32(obj+508),api->read_u32(obj+512),gait,int32_t(api->read_u32(obj+576)),api->read_f32(obj+580),
                      int32_t(api->read_u32(obj+764)),int32_t(api->read_u32(obj+756)),(api->read_u32(obj+216)&0x80000000u)?"up":"down",stick[0],stick[1],
                      std::atan2(stick[0],stick[1])*57.2958f,scratch.ctx.r3.u32,driven?"":" (not driven: its state)");
        ProbeLog(line);
      }
    }
    for(unsigned i=0;i<3;++i) left_pos[i]=api->read_f32(obj+20+i*4);
    left_at=now;
    return moved;
  }
};
Follower npc_follow; // game thread (the NPC replica test)
std::vector<uint8_t> npc_pending_look; // applied once setup completes
void SetPendingLook(const uint8_t* look,unsigned length) { npc_pending_look.assign(look,look+length); }
void ResetReplicaFollow() { npc_follow.Reset(); }
void FeedReplica(uint32_t time,const float* pos,const float* rows,uint64_t now) { npc_follow.Add(time,pos,rows,now); }

// Cars are shared: each game has its real cars, and copies of the cars other
// players use (car_copies). Whoever sits in the driver's seat is in charge
// of the car: the other games move their instance along the driver's path.
// Another player's character is seated in the matching seat of the matching
// instance, so getting in behind a driver makes you a passenger, like with
// homies. RemoteCar is one other player's side of it: getting in and out
// here, and the path of the car while they drive.
struct RemoteCar {
  Follower follow;
  uint32_t last_sequence=0, followed=0;
  uint64_t next_spawn=0, next_enter=0, next_exit=0, next_log=0, next_swap=0;
  unsigned enter_tries=0, snaps=0, snap_nophys=0, snap_turn=0, snap_far=0;
  ImpactWatch impact;
  uint64_t want_since=0;     // their car was first reported (enter events arrive with it)
  uint64_t enter_ordered=0;  // the character was told to get in (walk to the door, open it, pull a driver out)
  uint64_t moving_since=0;   // their car started moving while our copy of them was still getting in
  uint32_t enter_target=0;
  uint64_t stuck_since=0, next_push=0, exit_first=0, next_damage=0;
};

// ---- Another player (v2: one record per session slot; game thread) ----
// Everything that shows one other player here: their character (a player-
// pool object dressed like them, or a stand-in), its clothes, movement, gun
// and the car they are in.
struct Remote {
  uint32_t generation=0;      // remote_in's generation this record belongs to
  char tag[24]="Remote player";
  uint32_t avatar=0;          // their character's handle here
  uint64_t seen=0;            // last fresh pose
  bool player_obj=false;      // a player-pool object (dressed)
  bool own_body=false;        // with its own body model
  uint32_t last_sequence=0;
  uint64_t next_attempt=0, own_body_wait=0, next_item_check=0;
  uint64_t cutscene_seen=0; bool cut_hidden=false;
  std::vector<uint8_t> applied_look; uint32_t look_handle=0;
  // Clothes: dressing again happens over several updates.
  std::vector<uint8_t> applied_worn, pending_worn;
  int dress_step=0;            // 0 idle, 1 taking off, 2 waiting to wear
  uint32_t dressed_record=0;   // customization record last dressed
  uint64_t next_clothes_check=0, dress_started=0, last_cutscene=0;
  unsigned missing_for=0;
  // Hair colour is taken when the hair is put on (multiplayer sets the
  // colours right after the clothes, before they load): colours that arrive
  // or change after dressing need the clothes put on again.
  bool redress_for_colours=false;
  std::vector<uint8_t> extras_at_dress, applied_extras; uint32_t extras_record=0;
  std::vector<float> applied_morphs;
  uint64_t morph_check_at=0, next_morph_recheck=0; int morph_checks=0, morph_resets=0;
  // Movement
  Follower follow;
  AvatarMotion motion;
  bool on_foot=false;
  Event pending_action{};      // an action the character refused, retried after the next state change
  uint64_t pending_action_until=0;
  uint64_t next_crouch=0, off_since=0, last_remake=0;
  uint64_t exit_ordered=0;     // told to get out of its car before removal
  bool force_remake=false;     // stuck in a car they left: remove, make again
  GunState gun;
  // Their shots, automatic-gun releases and throws, played in order; one the
  // character refuses (turning, falling, gun lowered...) is tried again for
  // up to 750 ms (R 5).
  struct Shot { uint32_t kind=0, weapon=0; float hit[3]{}; bool has_hit=false; float strength=0; uint64_t at=0; uint8_t held=kHeldOther; };   // held: why it last waited
  std::deque<Shot> shots;
  uint64_t next_raise=0;
  // Knocked down / ragdolled: their game decides (research S 2d); applied here.
  // 2.1: knocked on the rising edge of their ragdoll bit (tried up to 4 times;
  // again at most once a second while they lie and it doesn't), up with its
  // own get-up; until getup_until none of their moves is replayed.
  bool owner_down=false, knock_wanted=false, stranded_logged=false; unsigned knock_tries=0;
  uint64_t next_knock=0, up_since=0, next_getup=0, getup_until=0, knock_check_at=0, stranded_since=0;
  // Cars
  RemoteCar car;
  uint32_t enter_hold_car=0; uint64_t enter_hold_until=0, exit_hold_until=0;
  uint32_t enter_car=0; uint64_t enter_until=0, enter_time=0;   // their getting in is playing (car actions)
  uint32_t pending_victim=0; uint64_t victim_deadline=0, victim_exit_at=0;  // a driver they pull out here
  // Their notoriety on the character here (2.1, UpdateNotoriety): the handle
  // it was last written on; an arrest of it is sent to their game at most
  // every 3 s.
  uint32_t notoriety_on=0; uint64_t next_notoriety=0, next_busted=0;
  // Host, 2.1.2: whether a cop here sees their character, as last told to
  // their game (kEventSeen), and when it is tested / told next.
  bool police_see=false; uint64_t next_seen_check=0, next_seen_send=0;
};
std::array<Remote,kSlots> remotes;

uint32_t AvatarObjOf(int slot) {
  if(slot<0 || slot>=kSlots || !remotes[slot].avatar) return 0;
  return ResolveHuman(remotes[slot].avatar);
}
int AvatarSlotOf(uint32_t obj) {
  if(!obj || !Readable(obj,72)) return -1;
  const uint32_t h=api->read_u32(obj+68);
  if(!h) return -1;
  for(int s=0;s<kSlots;++s) if(remotes[s].avatar==h) return ResolveHuman(h)==obj ? s : -1;
  return -1;
}
bool IsAvatarHandle(uint32_t handle) {
  if(!handle) return false;
  for(const Remote& r:remotes) if(r.avatar==handle) return true;
  return false;
}
// The car another player's character sits in here (handle), or 0.
uint32_t AvatarCar(int slot) {
  const uint32_t o=AvatarObjOf(slot);
  return o && ResolveVehicle(api->read_u32(o+2496)) ? api->read_u32(o+2496) : 0;
}
// Another player's character sits at the wheel of that car here.
bool AvatarDrives(uint32_t car_handle) {
  for(int s=0;s<kSlots;++s) if(const uint32_t av=AvatarObjOf(s)) if(api->read_u32(av+2496)==car_handle && api->read_u32(av+2500)==0) return true;
  return false;
}
// Someone (this player or another player's character) sits in that car here.
bool PlayerSitsIn(uint32_t car_handle) {
  if(!car_handle) return false;
  const uint32_t player=api->read_u32(0x8309ABEC);
  if(Readable(player,4252) && api->read_u32(player+2496)==car_handle) return true;
  for(int s=0;s<kSlots;++s) if(remotes[s].avatar && AvatarCar(s)==car_handle) return true;
  return false;
}
// This game's player drives that car (seated at the wheel, not still getting in).
bool LocalDrives(uint32_t car_handle) {
  const uint32_t pl=api->read_u32(0x8309ABEC);
  if(!car_handle || !Readable(pl,4252) || api->read_u32(pl+2496)!=car_handle || api->read_u32(pl+2500)!=0) return false;
  const uint32_t task=api->read_u32(pl+2556);
  return !(task>=3 && task<=0xE) && task!=0x13;
}
// Another player is getting into that car here right now.
bool PlayerGettingIn(uint32_t car_handle,uint64_t now) {
  if(!car_handle) return false;
  for(const Remote& r:remotes) if((r.enter_car==car_handle && now<r.enter_until) || now<r.enter_hold_until) return true;
  return false;
}

// Returns with the character kept while it still sits in a car: it gets out
// first (released while seated, the car kept a stale pointer and the game
// crashed a moment later - both crashes in the 0.99 test were at a removal).
void DestroyAvatar(GuestScratch& scratch,Remote& r) {
  if(const uint32_t obj=ResolveHuman(r.avatar)) {
    if(ResolveVehicle(api->read_u32(obj+2496))) {
      const uint64_t t=MonoMs();
      if(!r.exit_ordered) {
        OrderExitVehicle(scratch,obj); r.exit_ordered=t;
        ProbeLog((std::string(r.tag)+": character leaves its car before removal").c_str());
      }
      if(t-r.exit_ordered<6000) return;
      ProbeLog((std::string(r.tag)+": character still in its car after 6 s, removed anyway").c_str());
    }
  }
  r.exit_ordered=0;
  r.force_remake=false;  // removed now; the next update makes a fresh one
  {
    const uint32_t player=api->read_u32(0x8309ABEC);
    if(r.avatar && api->read_u32(player+4128)==r.avatar) api->write_u32(player+4128,0);
  }
  ResponseTargetGone(scratch,r.avatar);   // (the host's response after it is called off first, 2.1)
  if(r.player_obj) {
    if(const uint32_t obj=ResolveHuman(r.avatar)) {
      ReleasePlayerObject(scratch,obj);
      ProbeLog((std::string(r.tag)+": character returned to the player pool").c_str());
    }
    r.avatar=0; r.player_obj=false;
  } else DestroyActor(scratch,r.avatar,"remote player");
  r.applied_worn.clear(); r.applied_morphs.clear(); r.applied_extras.clear();
  r.on_foot=false;
}

// Car copies nobody uses any more go: the owner left, this player is far from
// it, or no player sat in it for a minute and it is out of sight. Never one
// someone sits in. all: the session is over.
void DestroyObject(GuestScratch& scratch,uint32_t handle);
void CleanCarCopies(GuestScratch& scratch,uint64_t now,bool all) {
  static uint64_t next=0;
  if(!all && now<next) return;
  next=now+500;
  const uint32_t player=api->read_u32(0x8309ABEC);
  const uint32_t mine=Readable(player,4252) ? api->read_u32(player+2496) : 0;
  for(auto it=car_copies.begin();it!=car_copies.end();) {
    CarCopy& c=it->second;
    const uint32_t v=ResolveVehicle(c.local);
    if(!v) { it=car_copies.erase(it); continue; }   // the game removed it
    if(c.local==mine) {                              // this player drives it now
      if(all) { it=car_copies.erase(it); continue; } // kept as a car of this game's own
      c.used=now; ++it; continue;
    }
    if(!all && PlayerSitsIn(c.local)) { c.used=now; ++it; continue; }
    const uint32_t owner=uint32_t(it->first>>32);
    const float dx=api->read_f32(v+20)-local_x.load(), dz=api->read_f32(v+28)-local_z.load(), d2=dx*dx+dz*dz;
    const bool owner_gone=owner!=my_slot.load() && !Present(int(owner));
    if(all || owner_gone || d2>150.0f*150.0f || (now-c.used>60000 && d2>60.0f*60.0f)) {
      if(PlayerSitsIn(c.local)) { ++it; continue; }   // a character still in it: next time
      DestroyObject(scratch,c.local);
      char line[160];
      std::snprintf(line,sizeof(line),"Car copy %08X (slot %u's car %08X) removed%s",c.local,owner,uint32_t(it->first),
                    all ? " (session over)" : owner_gone ? " (that player left)" : " (no longer used here)");
      ProbeLog(line);
      it=car_copies.erase(it); continue;
    }
    ++it;
  }
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

// Client: cars this player asked for while the host's world is shown (a
// cheat or mod menu spawn, a script's vehicle_create: 82569700 with source 7
// or 3, not the game's own spawners 2 traffic, 4 response, 5 parked, 6
// special, 9 the El Train) are this player's own and stay: the removal of this
// game's own cars took them away the moment they appeared. Handle -> made at.
std::unordered_map<uint32_t,uint64_t> personal_cars;
int making_car=0;   // inside co-op's own CreateVehicle (a copy, not this player's car)
unsigned car_model_refs_taken=0, car_model_refs_total=0; uint32_t car_model_group=0;   // the copies' model references (log)
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
  ++making_car; const bool made=scratch.Call(0x82569700); --making_car;
  if(!made) return 0;
  const uint32_t handle=scratch.ctx.r3.u32;
  const uint32_t vehicle=ResolveVehicle(handle);
  if(!vehicle) return 0;
  // Like the network vehicle creator 823BA888: take a streaming reference on
  // the model (id in the info entry's first word), or the car stays unloaded
  // and invisible, then place it. 2.2: through the game's own 82532360(v,
  // kind) - 8250C750(group, *(*(v+50324)+0), kind, 0, 1), then v+50304 =
  // kind - as its own cars ask, so the vehicle destructor 825324D0 gives it
  // back when the copy goes (it releases the kind noted at +50304 when that
  // isn't -1). 823BA888's direct 8250C750, which 2.1 copied, left the -1 the
  // constructor 825317A8 writes: every car copy kept its model's count up.
  const uint32_t id=api->read_u32(kVehicleInfo+uint32_t(type)*kVehicleInfoSize);
  if(int32_t(api->read_u32(vehicle+50304))==-1) { scratch.ctx.r3.u64=vehicle; scratch.ctx.r4.u64=5; scratch.Call(0x82532360); }
  else {   // (a kind noted already: its reference is there, the destructor gives it back)
    static unsigned noted=0;
    if(noted++<5) { char line[120]; std::snprintf(line,sizeof(line),"Car copy %08X made with a model reference noted already (+50304 = %d)",handle,int32_t(api->read_u32(vehicle+50304))); ProbeLog(line); }
  }
  ++car_model_refs_taken; ++car_model_refs_total; car_model_group=id>>24;
  { static unsigned logged=0;
    if(logged++<3) { char line[140]; std::snprintf(line,sizeof(line),"Car copy %08X made: model %X, reference kind noted at +50304 = %d (its destructor releases it; -1: none)",handle,id,int32_t(api->read_u32(vehicle+50304))); ProbeLog(line); } }
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

// What 8257B2C0 reads when it refuses a seat (2.1.1; the first test's host
// refused a character every seat of a car for 21 s with the seat empty): the
// human's +508 (9 in a car, 12 falling: 82448590), state +516/+520 (4 jump,
// 6 landing), ragdoll +780, current action +756 (a reload-flagged one with a
// gun in hand refuses, 82440FE8) and melee move +1956, the gun, its car
// +2496 / seat +2500 and +2569 bit 0x10 (on a car's enter list); the car's
// enter list count +52132 (8 refuse, 82556E50), flags +120 and +53976
// (82576D20), ai mode +50376, and the asked seat's occupant +52316 (82534B98),
// +52320 (82534C98, what OccupantHandle reads) and reservation +52360
// (82534CF0; 108 bytes a seat, 82575350).
void LogSeatRefusal(const char* who,uint32_t human,uint32_t vehicle,uint32_t seat) {
  const uint32_t s=vehicle+seat*108;
  char line[400];
  std::snprintf(line,sizeof(line),"%s: seat %u refused: +508 %u, state %u/%u, +780 %u, action %d, melee %d, weapon %d, car %08X seat %u, +2569 %02X; car: list %u, +120 %08X, +53976 %08X, ai %u, seat +52316 %08X, +52320 %08X, reserved %08X",
                who,seat,api->read_u32(human+508),api->read_u32(human+516),api->read_u32(human+520),api->read_u32(human+780),int32_t(api->read_u32(human+756)),int32_t(api->read_u32(human+1956)),HandWeapon(human),
                api->read_u32(human+2496),api->read_u32(human+2500),api->read_u8(human+2569),
                api->read_u32(vehicle+52132),api->read_u32(vehicle+120),api->read_u32(vehicle+53976),api->read_u32(vehicle+50376),
                api->read_u32(s+52316),api->read_u32(s+52320),api->read_u32(s+52360));
  ProbeLog(line);
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
void FollowVehicle(GuestScratch& scratch,RemoteCar& car,uint32_t vehicle,const PlayerPose& p,uint64_t now) {
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

/// The car here that another game names (its owner's slot, its handle in the
// owner's game), or 0. Nothing is made or taken over.
uint32_t AdoptSharedCar(GuestScratch& scratch,uint32_t host_id,uint64_t now);
void PinModel(GuestScratch& scratch,uint32_t id,uint64_t now);
uint32_t OccupantHandle(uint32_t vehicle,uint32_t seat);
uint32_t FindCar(uint32_t owner,uint32_t id) {
  if(owner==my_slot.load()) if(const uint32_t v=ResolveVehicle(id)) return v;
  if(auto it=car_copies.find(CarKey(owner,id)); it!=car_copies.end()) if(const uint32_t v=ResolveVehicle(it->second.local)) return v;
  if(owner==0 && !hosting.load()) {
    for(auto& [local,host]:taken_cars) if(host==id) if(const uint32_t v=ResolveVehicle(local)) return v;
    if(const uint32_t v=SharedCarLocal(id)) return v;
  }
  return 0;
}
// As FindCar; a copy of the host's traffic shown here is taken over first
// (it stops following the host's traffic: a player sits in it now).
uint32_t FindCarToUse(GuestScratch& scratch,uint32_t owner,uint32_t id,uint64_t now) {
  if(owner==0 && !hosting.load() && SharedCarLocal(id)) AdoptSharedCar(scratch,id,now);
  return FindCar(owner,id);
}

// Our getting in as a passenger (another player's car) stalled before (the
// player stood there and never sat): after 3.5 s the seat is taken directly.
void WatchPassengerSeat(GuestScratch& scratch,uint64_t now) {
  if(!passenger_watch_car) return;
  const uint32_t me=api->read_u32(0x8309ABEC);
  const uint32_t pc=ResolveVehicle(passenger_watch_car);
  if(!pc) passenger_watch_car=0;
  else if(api->read_u32(me+2496)==passenger_watch_car && api->read_u32(me+2500)>=1 && api->read_u32(me+2556)==0x12) passenger_watch_car=0;
  else if(now-passenger_watch_since>3500) {
    uint32_t seat=0;
    for(uint32_t s2=1;s2<=3 && !seat;++s2) if(!SeatedIn(passenger_watch_car,s2)) seat=s2;
    const bool ok=seat && SeatInVehicle(scratch,me,pc,seat,false);
    ProbeLog(ok?"Passenger getting-in stalled: seated directly":"Passenger getting-in stalled: seat refused");
    passenger_watch_car=0;
  }
}

// Returns true while a vehicle owns the character (the on-foot follower must
// not move it then).
bool UpdateRemoteCar(GuestScratch& scratch,Remote& r,uint32_t avatar,const PlayerPose& p,uint64_t now) {
  RemoteCar& car=r.car;
  // Getting in here stops at the door: the enter task (+2556 = 3) waits in
  // state 3/13 for 8244D128 to say the walk to the door is done, which it
  // never does for this character (the player's own is done in ~0.1 s). The
  // game's next step (82465578, as 8257F0E8 does) is called for it then,
  // and the door, climb-in and seat play.
  if(now<r.enter_hold_until && api->read_u32(avatar+2556)==3 && api->read_u32(avatar+516)==3 && api->read_u32(avatar+520)==0x13) {
    if(!car.stuck_since) car.stuck_since=now;
    if(now-car.stuck_since>250 && now>=car.next_push) {
      car.next_push=now+700;
      scratch.ctx.r3.u64=avatar; scratch.ctx.r4.u64=0;
      ++replicated_depth; scratch.Call(0x82465578); --replicated_depth;
      r.enter_hold_until=std::max(r.enter_hold_until,now+4000); r.motion.car_anim_until=std::max(r.motion.car_anim_until,now+4000);
      static unsigned logged=0;
      if(logged++<20) ProbeLog("Remote player: at the door, getting in carried on");
    }
  } else car.stuck_since=0;
  // Their own enter/exit is playing here: no placing meanwhile.
  if(now<r.enter_hold_until) { car.enter_target=r.enter_hold_car; car.next_enter=std::max(car.next_enter,r.enter_hold_until); }
  if(now<r.exit_hold_until) car.next_exit=std::max(car.next_exit,r.exit_hold_until);
  const uint32_t avatar_car=api->read_u32(avatar+2496);
  const bool avatar_in=ResolveVehicle(avatar_car)!=0;
  const bool sitting=p.vehicle>=0 && (p.flags&0x10);
  if(!sitting) {
    // Still getting in there: the character follows them on foot (their
    // car actions play on it) and is put in the seat once they sit.
    if(p.vehicle>=0 && !avatar_in) return false;
    if(p.vehicle>=0) return avatar_in;   // seated here already: never taken out while their car is set
    if(now<r.enter_hold_until) return avatar_in;
    if(avatar_in) {
      SetRideAlong(avatar,false);
      if(!car.exit_first) car.exit_first=now;
      // Exits refused (their car was wrecked: 30+ orders in a row, the
      // character stayed in the wreck while they walked away): after 4.5 s
      // the character is removed and made again where they are.
      if(now-car.exit_first>4500) {
        static unsigned logged=0;
        if(!r.force_remake && logged++<10) ProbeLog("Remote player still in the car 4.5 s after leaving it there: made again");
        r.force_remake=true;
      } else if(now>=car.next_exit) {
        OrderExitVehicle(scratch,avatar); car.next_exit=now+1500;
        ProbeLog("Remote player left the vehicle; exit ordered");
      }
    } else car.exit_first=0;
    car.enter_tries=0; car.want_since=0;
    return avatar_in;
  }
  if(!car.want_since) car.want_since=now;
  // Which car here is the one they sit in? Named by its owner's slot and its
  // handle there: this game's own car, a copy of another game's, or a copy of
  // the host's traffic shown here (taken over).
  const uint32_t owner=p.vowner, id=p.vid;
  uint32_t target=FindCarToUse(scratch,owner,id,now);
  if(!target) {
    // Not here (another game's car, or one of ours this game removed far
    // away: they drove up in an invisible car): a copy is made.
    const float dx=p.vpos[0]-local_x.load(), dz=p.vpos[2]-local_z.load();
    if(dx*dx+dz*dz>100.0f*100.0f || now<car.next_spawn) return avatar_in;
    car.next_spawn=now+2000;
    const uint32_t made=CreateVehicle(scratch,p.vehicle,p.vpos,p.vrows);
    if(made && p.vehicle>=0) PinModel(scratch,api->read_u32(kVehicleInfo+uint32_t(p.vehicle)*kVehicleInfoSize),now);
    char line[180];
    std::snprintf(line,sizeof(line),"%s: vehicle type %d %s (local %08X; slot %u's car %08X)",r.tag,p.vehicle,made?"created":"not created",made,owner,id);
    ProbeLog(line);
    if(!made) return avatar_in;
    CarCopy& c=car_copies[CarKey(owner,id)];
    c=CarCopy{}; c.local=made; c.type=p.vehicle; c.used=now;
    ForgetCarDamage(made);
    car.enter_tries=0; car.next_enter=0;
    target=ResolveVehicle(made);
  }
  if(!target) return avatar_in;
  const uint32_t target_handle=api->read_u32(target+68);
  // A copy: kept while used, with its owner's colours.
  if(auto it=car_copies.find(CarKey(owner,id)); it!=car_copies.end() && it->second.local==target_handle) {
    CarCopy& c=it->second;
    c.used=now;
    std::vector<uint8_t> look;
    { std::lock_guard lock(car_look_mutex); auto l=car_looks_in.find(CarKey(owner,id)); if(l!=car_looks_in.end()) look=l->second; }
    if(!look.empty() && look!=c.applied_look && ApplyCarLook(scratch,target,look)) {
      c.applied_look=look;
      char line[100]; std::snprintf(line,sizeof(line),"Remote vehicle colours applied (%zu bytes)",look.size()); ProbeLog(line);
    }
  }
  // Both want the driver's seat: the game that owns the car keeps its
  // driver. When that is the other game, this player moves over to the
  // passenger seat; when it is this game, the other player's character sits
  // in the passenger seat here (their game moves them over too).
  uint32_t want_seat=p.seat;
  const uint32_t player=api->read_u32(0x8309ABEC);
  const bool local_driving=api->read_u32(player+2496)==target_handle && api->read_u32(player+2500)==0 && api->read_u32(player+2556)==0x12;
  const int their_slot=int(&r-remotes.data()), mine=int(my_slot.load());
  if(p.seat==0 && local_driving) {
    // Both in the driver's seat (the game put the one getting in at the
    // wheel even when asked for the passenger seat): the car's own game keeps
    // its driver, otherwise the lower slot - a rule every game works out the
    // same way (v1 compared arrival times, which each game saw differently).
    const bool they_first=int(p.vowner)==their_slot ? true : int(p.vowner)==mine ? false : their_slot<mine;
    if(they_first) {
      if(now>=car.next_swap) {
        car.next_swap=now+1000;
        uint32_t seat=0;
        for(uint32_t s2=1;s2<=3 && !seat;++s2) if(!SeatedIn(target_handle,s2)) seat=s2;
        // 2.1.1: 8257B2C0 refuses the local player a direct seat while the
        // driver's seat holds anyone, itself included (82576D20 at 825771E8:
        // the in-world game mode, human == local player, 82534B98(v, 0) set),
        // so this never worked before: off the wheel first with 8257C400 mode
        // 19 (what 8257B2C0 itself does for a human already in the car), only
        // while the car is slow; moving fast this player keeps the wheel (the
        // driver's game keeps it anyway).
        const uint32_t vel=VehicleVelocityAddress(target);
        const float sp=vel ? std::sqrt(api->read_f32(vel)*api->read_f32(vel)+api->read_f32(vel+4)*api->read_f32(vel+4)+api->read_f32(vel+8)*api->read_f32(vel+8)) : 0.0f;
        if(seat && sp>=1.5f) {
          static unsigned logged=0;
          if(logged++<10) { char line[120]; std::snprintf(line,sizeof(line),"Local player stays at the wheel: the car moves (%.1f m/s), the other player's game drives it",sp); ProbeLog(line); }
          return true;
        }
        if(seat) { ++replicated_depth; scratch.ctx.r3.u64=player; scratch.ctx.r4.u64=19; scratch.Call(0x8257C400); --replicated_depth; }
        const bool ok=seat && SeatInVehicle(scratch,player,target,seat,false);
        if(!ok) {
          car.next_swap=now+5000;   // (tried every second, and each try was sent to the other game)
          static unsigned logged=0;
          if(seat && logged++<10) LogSeatRefusal("Local player",player,target,seat);
        }
        ProbeLog(ok?"Local player moved to a passenger seat (the other player was there first)":"Local player: passenger seat refused");
      }
      return true;
    }
    want_seat=1;
  }
  // The driver's game is in charge of the car.
  // Their car drives off while their getting in still plays here (it was
  // left behind and put in the seat 6-10 s later): hold the car a moment,
  // then seat the character at once.
  if(want_seat==0 && now<r.enter_hold_until) {
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
      const bool ok=SeatInVehicle(scratch,avatar,target,0,false);
      r.enter_hold_until=0; r.motion.car_anim_until=0; car.moving_since=0;
      ProbeLog(ok?"Remote player: seated at once (their car drove off before getting in finished here)":"Remote player: seating at once refused");
    }
  } else car.moving_since=0;
  if(want_seat==0) {
    // Steered along their path - unless another player's character holds
    // the wheel here (two games both seeing their own player drive).
    const uint32_t d=OccupantHandle(target,0);
    if(!d || d==api->read_u32(avatar+68) || !IsAvatarHandle(d)) {
      if(now>=car.next_damage) {
        car.next_damage=now+250;
        ApplyCarDamage(scratch,target,uint8_t((p.flags>>1)&7 | (p.flags&0x20)),uint8_t(p.flags>>kPoseCarHealthShift));
        ApplyCarSiren(scratch,target,uint8_t((p.flags>>kPoseSirenShift)&0x18));
      }
      if(!WatchImpact(target,car.impact,id,now)) { FollowVehicle(scratch,car,target,p,now); AfterSteer(target,car.impact); }
    }
  }
  SetRideAlong(avatar,want_seat==0 && avatar_car==target_handle);
  // Their own enter is replayed here (kEventEnterCar), and a replayed exit
  // must not be undone while their game still shows them seated: the pose
  // only moves the character when neither did it.
  const bool car_hold=now<r.exit_hold_until || now<r.enter_hold_until || now<car.want_since+100;
  if(car_hold && !avatar_in) {}
  else if(avatar_car!=target_handle || api->read_u32(avatar+2500)!=want_seat) {
    if(avatar_in && avatar_car!=target_handle && now>=car.next_exit) { OrderExitVehicle(scratch,avatar); car.next_exit=now+1000; }
    else if(now>=car.next_enter && [&]{
      // Far from this player (1.63): this game doesn't keep a character
      // seated in a car it isn't simulating near the player, so seating was
      // undone and retried every second, each time remaking the copies.
      // Seated once they're close again; the car itself keeps following.
      const float fx=api->read_f32(target+20)-api->read_f32(player+20), fz=api->read_f32(target+28)-api->read_f32(player+28);
      if(fx*fx+fz*fz<150.0f*150.0f) return true;
      car.next_enter=now+1000;
      return false; }()) {
      // Someone else sits in that seat here. Only a pulled-out traffic
      // driver (a copy, the driver's seat) is removed; anyone else - a
      // passenger, a mission character, another player - keeps the seat and
      // this player's character takes a free one (1.63: getting in as the
      // host's passenger removed Troy, and his drop-off mission couldn't be
      // finished).
      uint32_t seat=want_seat;
      const uint32_t me_h=api->read_u32(player+68), av_h=api->read_u32(avatar+68);
      if(const uint32_t occ=SeatedIn(target_handle,seat)) if(occ!=av_h && (occ==me_h || ResolveHuman(occ))) {
        if(occ==me_h && seat!=0 && mine>their_slot) {
          // Both games seated their own player in this passenger seat: the
          // higher slot moves over (here, this player); the lower keeps it.
          if(now>=car.next_swap) {
            car.next_swap=now+1000;
            uint32_t s3=0;
            for(uint32_t s2=1;s2<=3 && !s3;++s2) if(s2!=seat && !SeatedIn(target_handle,s2)) s3=s2;
            if(!(s3 && SeatInVehicle(scratch,player,target,s3,false))) car.next_swap=now+5000;
          }
          car.next_enter=now+200;
          return true;
        }
        if(seat==0 && occ!=me_h && OurCharacter(occ)) {
          DestroyObject(scratch,occ);   // the driver they pulled out is still sitting here
          ProbeLog("Driver still in the seat the other player took: removed");
        } else {
          seat=~0u;
          for(uint32_t s2=1;s2<6 && seat==~0u;++s2) {
            const uint32_t o2=SeatedIn(target_handle,s2);
            if(!o2 || o2==av_h) seat=s2;
          }
          static unsigned logged=0;
          if(logged++<20) { char line[140]; std::snprintf(line,sizeof(line),"Seat %u here holds %08X (kept): the other player %s",want_seat,occ,seat==~0u?"waits outside":"takes a free seat"); ProbeLog(line); }
        }
      }
      if(seat==~0u) { car.next_enter=now+2000; return true; }
      if(seat!=want_seat && api->read_u32(avatar+2496)==target_handle) { car.next_enter=now+2000; return true; }  // already sits in the car
      const bool ok=SeatInVehicle(scratch,avatar,target,seat,false);
      ++car.enter_tries; car.next_enter=now+(ok?1000:500);   // 2.1.1: every 500 ms (was 1-6 s)
      char line[160]; std::snprintf(line,sizeof(line),"%s: seat %u in %08X %s (try %u)",r.tag,want_seat,target_handle,ok?"ok":"refused",car.enter_tries); ProbeLog(line);
      if(!ok) {
        { static unsigned logged=0; if(logged++<10) LogSeatRefusal(r.tag,avatar,target,seat); }
        // 2.1.1: refused twice in a row (the first test: 7 tries over 21 s
        // with the seat empty; the character had sat on the moving car with
        // nothing resetting it): before the next try the character is reset
        // the way the knock does - out of any car (8257C400 mode 19, what
        // 8257B2C0 itself does for a human already in the car), its current
        // action ended (82445778 with r4 = 1, as the seat path's own 82556E50
        // does) and placed at the car - and after 3 more refusals made again
        // (1.41). Request byte +17 (skip every check) is never set: 82556E50
        // can still leave the human half-entered, and no game caller sets it.
        if(car.enter_tries>=5) {
          static unsigned logged=0;
          if(!r.force_remake && logged++<10) { std::snprintf(line,sizeof(line),"%s: seat refused %u times in a row: made again",r.tag,car.enter_tries); ProbeLog(line); }
          r.force_remake=true; car.enter_tries=0; car.next_enter=0;
        } else if(car.enter_tries>=2) {
          const uint32_t in=api->read_u32(avatar+2496);
          const int32_t action=int32_t(api->read_u32(avatar+756));
          ++replicated_depth;
          if(ResolveVehicle(in)) { scratch.ctx.r3.u64=avatar; scratch.ctx.r4.u64=19; scratch.Call(0x8257C400); }
          if(action!=-1) { scratch.ctx.r3.u64=avatar; scratch.ctx.r4.u64=1; scratch.ctx.f1.f64=0.1; scratch.Call(0x82445778); }
          --replicated_depth;
          const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316;
          for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,api->read_f32(target+20+i*4));
          for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,api->read_f32(target+32+i*4));
          const bool placed=MoveHuman(scratch,avatar,pos,matrix,0,true);
          static unsigned logged=0;
          if(logged++<10) { std::snprintf(line,sizeof(line),"%s: reset before the next seat try (out of %08X, action %d ended, %s at the car)",r.tag,in,action,placed?"placed":"not placed"); ProbeLog(line); }
        }
      }
    }
  } else {
    car.enter_tries=0;
    if(now>=car.next_log) {
      car.next_log=now+5000;
      char line[240];
      std::snprintf(line,sizeof(line),"%s vehicle: seated %u, slot %u's car, pos %.1f %.1f %.1f, physics %s, teleports %u (far %u, turn %u, no physics %u)",r.tag,p.seat,owner,
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

// ---- Other players' weapons and knockdowns here (v2) ----
// The game clock as the weapon timers count it (wraps at 0x6B49D200).
uint32_t WeaponClock() {
  uint32_t t=api->read_u32(kGameClock);
  if(int32_t(t)>0x6B49D200) t-=0x6B49D200;
  return t;
}
// Their camera on their character here: rockets and throws of a player-kind
// character that isn't this game's player aim with +4112 (eye) and +4124
// (rows), as multiplayer writes them from each snapshot (R 4.2).
void WriteRemoteCamera(uint32_t obj,const PlayerPose& p) {
  bool any=false;
  for(float v:p.cam) any=any || v!=0.0f;
  if(!any) return;
  for(unsigned i=0;i<3;++i) api->write_f32(obj+4112+i*4,p.eye[i]);
  for(unsigned i=0;i<9;++i) api->write_f32(obj+4124+i*4,p.cam[i]);
}
std::atomic<uint32_t> throws_played{0}, releases_played{0}, knockdowns_synced{0}, getups_synced{0};
std::atomic<uint32_t> knockdowns_refused{0}, stranded_found{0}, stranded_recovered{0}, replayed_calls_dropped{0}, replayed_actions_dropped{0};
// Their shots, automatic-gun releases and throws on their character, in
// order (R 3.1-3.4). A shot the character refuses stays queued and is tried
// again next update, for up to 750 ms (R 5). 2.1.4: each wait is noted on the
// shot and the last reason counted when it is dropped (the second test refused
// 9% of a player's shots, most while the character lay here).
void TryRemoteShots(GuestScratch& scratch,Remote& r,uint32_t obj,uint64_t now) {
  for(unsigned n=0;n<4 && !r.shots.empty();++n) {
    Remote::Shot& s=r.shots.front();
    if(now-s.at>750) { r.shots.pop_front(); ++shots_refused; ++shots_held[s.held]; continue; }
    // (Seated, whatever refused the gun or the fire is put down to the car.)
    auto held=[&](uint8_t why) { s.held=why>kHeldNotSetUp && ResolveVehicle(api->read_u32(obj+2496)) ? kHeldInCar : why; };
    if(!(api->read_u32(obj+216)&0x4000)) { held(kHeldNotSetUp); return; }   // not set up: later
    if(api->read_u32(obj+780) || api->read_u32(obj+516)==8) { held(kHeldLying); return; }   // lying, or getting up (state 8): later
    if(s.kind==kEventFireRelease) {
      // Trigger released: latch, loop sound stop and release sound (82474738,
      // 82476B70), trigger-held bit off.
      if(EquippedAmmo(obj)) {
        ++replicated_depth;
        scratch.ctx.r3.u64=obj; scratch.Call(0x82474738);
        scratch.ctx.r3.u64=obj; scratch.Call(0x82476B70);
        --replicated_depth;
      }
      api->write_u8(obj+3592,uint8_t(api->read_u8(obj+3592)&~0x40));
      ++releases_played;
      r.shots.pop_front(); continue;
    }
    if(!s.weapon || !MatchWeapon(scratch,obj,s.weapon,r.gun)) { held(kHeldOtherGun); return; }   // the gun first (tried again next update)
    r.gun.hold_until=now+1500;   // the pose's gun lags behind the shot: it waits
    const uint32_t ammo=EquippedAmmo(obj);
    if(!ammo) { held(kHeldNoAmmo); return; }
    // Never empty here: this game would reload or click (or, for a thrown
    // weapon, take the item away) on its own (R 5).
    if(int32_t(api->read_u32(ammo+16))<2) api->write_u32(ammo+16,2);
    if(int32_t(api->read_u32(ammo+20))<2) api->write_u32(ammo+20,2);
    // Turning on the spot ("run 180", actions 335/336) refuses the fire: stopped.
    auto is_turn=[](uint32_t a) { return a==335 || a==336; };
    if(is_turn(api->read_u32(obj+756))) { scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=1; scratch.ctx.f1.f64=0.1; ++replicated_depth; scratch.Call(0x82445778); --replicated_depth; }
    if(is_turn(api->read_u32(obj+764))) { scratch.ctx.r3.u64=obj; scratch.ctx.f1.f64=0.1; ++replicated_depth; scratch.Call(0x82445908); --replicated_depth; }
    if(is_turn(api->read_u32(obj+772))) { scratch.ctx.r3.u64=obj; scratch.ctx.f1.f64=0.1; ++replicated_depth; scratch.Call(0x824459A0); --replicated_depth; }
    // The gun up first: from a lowered gun the fire only raises it and the
    // game fires later on its own, with the barrel's aim (R 0.3).
    if(!(api->read_u32(obj+216)&0x80000000u)) {
      scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=1; scratch.ctx.r5.u64=0;
      ++replicated_depth; scratch.Call(0x82449708); --replicated_depth;
    }
    if(s.kind==kEventThrow) {
      // Their throw, as multiplayer replays one (823B24D0): strength, then the
      // throw (animation, release); the projectile it makes here is refused,
      // the copy of theirs comes with its message.
      api->write_f32(obj+1940,s.strength>0.0f ? s.strength : 1.0f);
      api->write_u8(ammo+12,uint8_t(api->read_u8(ammo+12)&~0xA0));
      scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=0;
      ++replicated_depth; scratch.Call(0x82475E30); --replicated_depth;
      ++throws_played;
      r.shots.pop_front(); continue;
    }
    // A shot: trigger latch and "blocked" cleared, the refire timer now
    // (exactly one round), no random spread (the bullet follows the shooter's
    // line to their hit point; rockets aim with the camera written above).
    api->write_u8(ammo+12,uint8_t(api->read_u8(ammo+12)&~0xA0));
    const uint32_t t=WeaponClock();
    api->write_u32(ammo+24,t);
    api->write_u32(ammo+40,0);
    const uint32_t rounds=api->read_u32(ammo+16);
    const uint32_t aim=scratch.data()+2420;
    if(s.has_hit) for(int i=0;i<3;++i) api->write_f32(aim+i*4,s.hit[i]);
    scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=0; scratch.ctx.r5.u64=s.has_hit ? aim : 0; scratch.ctx.r6.u64=0;
    ++fire_replay_depth; ++replicated_depth;
    scratch.Call(0x82470A50);
    --replicated_depth; --fire_replay_depth;
    if(api->read_u32(obj+212)&0x00080000u) {   // deferred (it was raising the gun): next update
      api->write_u32(obj+212,api->read_u32(obj+212)&~0x00080000u);
      held(kHeldRaising); return;
    }
    // Fired when the refire timer was re-armed or a round used (as FireHook).
    if(api->read_u32(ammo+24)!=t || api->read_u32(ammo+16)!=rounds) { ++shots_played; r.shots.pop_front(); continue; }
    held(kHeldFireRefused); return;   // refused: next update (until 750 ms)
  }
}
// The get-up's two spots (+792 start, +808 end; +804 / +820 the object they
// are relative to, 0 none). Only 82449FE0 writes them. In sub-state 16 the
// per-frame pass 82443808 teleports the character between them (8243C780),
// in 17 onto +808 (82463A28). A character that never ran its own get-up still
// had the constructor's zero vectors (82476DD8): the owner's replayed 8/16
// put it at (0,0,0), ~700 m off, for a moment ("follow gap ... max 725 m").
// 2.1.1: a character lying over 3 m from where its owner stands (a car here
// ploughed the ragdoll 42 m along) gets their position in both spots: the
// sub-state-16 pass lerps it to lerp(+792, +808, +788), so it gets up where
// they are, with no teleport of co-op's own.
std::atomic<uint32_t> getup_spots_moved{0};
void SetGetUpSpots(uint32_t obj,const float* owner,const char* tag,const char* when) {
  const float dx=api->read_f32(obj+20)-owner[0], dy=api->read_f32(obj+24)-owner[1], dz=api->read_f32(obj+28)-owner[2];
  const float gap=std::sqrt(dx*dx+dy*dy+dz*dz);
  const bool theirs=gap>3.0f;
  for(unsigned k=0;k<2;++k) { for(unsigned i=0;i<3;++i) api->write_f32(obj+792+k*16+i*4,theirs?owner[i]:api->read_f32(obj+20+i*4)); api->write_u32(obj+804+k*16,0); }
  if(!theirs) return;
  ++getup_spots_moved;
  static unsigned logged=0;
  if(logged++<20) { char line[160]; std::snprintf(line,sizeof(line),"%s's get-up spots set where they stand, %.1f m from the character here (%s)",tag,gap,when); ProbeLog(line); }
}
// Knocked down in their game: down here too, and up when they get up. Their
// game decides; here the character's own get-up waits for theirs (GetUpHook)
// and nothing seats it or moves it meanwhile (research S 2d: one screen
// showed a player knocked out in the car, the other running into a wall).
// 2.1: their own knockdown, get-up states and animations aren't replayed
// (ApplyEvents); the character falls when their ragdoll bit comes on and gets
// up with its own get-up when it goes off.
// Returns true while the character lies here.
bool SyncKnockdown(GuestScratch& scratch,Remote& r,uint32_t obj,const PlayerPose& p,uint64_t now) {
  const bool they=(p.flags&kPoseRagdoll)!=0;
  if(they && !r.owner_down) { r.knock_wanted=true; r.knock_tries=0; r.next_knock=0; }
  r.owner_down=they;
  if(!they) r.knock_wanted=false;
  const uint32_t state=api->read_u32(obj+516), sub=api->read_u32(obj+520);
  const float dx=api->read_f32(obj+20)-p.pos[0], dz=api->read_f32(obj+28)-p.pos[2], off=std::sqrt(dx*dx+dz*dz);
  // ~100 ms after a knock / 300 ms into a get-up (where 2.0's ~700 m jump
  // happened): where it is here against where they are.
  if(r.knock_check_at && now>=r.knock_check_at) {
    r.knock_check_at=0;
    static unsigned logged=0;
    if(logged++<40) {
      char line[200];
      std::snprintf(line,sizeof(line),"%s %s here at %.1f %.1f %.1f, %.1f m from their position (state %u/%u, +780 %u, +120 %08X)",r.tag,they?"down":"getting up",
                    api->read_f32(obj+20),api->read_f32(obj+24),api->read_f32(obj+28),off,state,sub,api->read_u32(obj+780),api->read_u32(obj+120));
      ProbeLog(line);
    }
  }
  if(they) {
    r.up_since=0; r.stranded_since=0; r.stranded_logged=false;
    // Still down in their game but up here (the game ended the ragdoll: its
    // slot taken for someone else, 82267200 -> 82449EB8; or refused 4 times):
    // knocked again, at most once a second (2.0 did every second).
    if(!r.knock_wanted && !api->read_u32(obj+780) && state!=8 && now>=r.next_knock) {
      static unsigned logged=0;
      if(logged++<20) {
        char line[160];
        std::snprintf(line,sizeof(line),"%s is still down in their game, up here (state %u/%u, %s): knocked down again",r.tag,state,sub,
                      r.knock_tries>3?"refused":"the knockdown did not hold");
        ProbeLog(line);
      }
      r.knock_wanted=true; r.knock_tries=0;
    }
    // (Not in 8/17: no ragdoll is made there, 824498D8. In 8/16 the game
    // builds it again from ragdoll slot +784, unchecked: 824498E0-8244991C.)
    if(r.knock_wanted && now>=r.getup_until && now>=r.next_knock && (api->read_u32(obj+216)&0x4000) && !(state==8 && sub==17) &&
       (!api->read_u32(obj+780) || int32_t(api->read_u32(obj+784))>=0)) {
      if(api->read_u32(obj+780) && state==8 && sub==15) r.knock_wanted=false;   // lying here already
      else {
        // (It refuses with +216 bit 0x00800000 set or +120 bit 0x04000000
        // clear, 82449824-8244984C: then nothing is touched here either.)
        const bool takes=!(api->read_u32(obj+216)&0x00800000) && (api->read_u32(obj+120)&0x04000000);
        if(takes) {
          SetGetUpSpots(obj,p.pos,r.tag,"knockdown");
          ++replicated_depth;
          // Out of its seat first (the game does this only for +508 == 9, and a
          // character seated by co-op may not have it: "knocked out in the car").
          if(ResolveVehicle(api->read_u32(obj+2496))) { scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=19; scratch.Call(0x8257C400); }
          scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=60000; scratch.ctx.r5.u64=0;
          ++knocking_avatar; scratch.Call(0x824497F0); --knocking_avatar;
          --replicated_depth;
        }
        // Lying if it took (every knockdown ends in 8/15, 82449974 / 82449AC0).
        // (Its r3 says nothing: 1 even when no ragdoll slot was free, 82449A28;
        // nor +780 alone, already set in 8/16.)
        if(takes && api->read_u32(obj+780) && api->read_u32(obj+516)==8 && api->read_u32(obj+520)==15) {
          r.knock_wanted=false; r.knock_tries=0; r.knock_check_at=now+100; r.next_knock=now+1000;
          ++knockdowns_synced;
          static unsigned logged=0;
          if(logged++<30) ProbeLog((std::string(r.tag)+" is down in their game: knocked down here too").c_str());
        } else {
          ++knockdowns_refused;
          static unsigned logged=0;
          if(!r.knock_tries && logged++<20) {
            char line[200];
            std::snprintf(line,sizeof(line),"%s is down in their game: knockdown refused here (+120 %08X, +216 %08X, state %u/%u); tried again up to 3 times",r.tag,
                          api->read_u32(obj+120),api->read_u32(obj+216),api->read_u32(obj+516),api->read_u32(obj+520));
            ProbeLog(line);
          }
          r.next_knock=now+250;
          if(++r.knock_tries>3) {
            r.knock_wanted=false; r.next_knock=now+1000;
            static unsigned gave_up=0;
            if(gave_up++<20) ProbeLog((std::string(r.tag)+": knockdown refused here 4 times, given up (again in 1 s while they lie and it stands)").c_str());
          }
        }
      }
    }
    return api->read_u32(obj+780)!=0;
  }
  if(api->read_u32(obj+780)) {
    if(!r.up_since) r.up_since=now;
    // They got up: so does the character, with its own get-up and its
    // preconditions (8244A000-8244A010), once its body is at rest as the
    // game's own waits for (8226BDC0, sub-state 15) or after 1 s.
    if(sub==15 && int32_t(api->read_u32(obj+784))>=0 && now>=r.next_getup) {
      const bool waited=now-r.up_since>=1000;
      bool rest=waited;
      if(!rest) { scratch.ctx.r3.u64=api->read_u32(obj+784); rest=scratch.Call(0x8226BDC0) && uint8_t(scratch.ctx.r3.u32)==1; }
      if(rest) {
        r.next_getup=now+1500; r.getup_until=now+2500; r.knock_check_at=now+300;
        SetGetUpSpots(obj,p.pos,r.tag,"get-up");   // (where they stand when the ragdoll ended far off, 2.1.1)
        scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=1;
        ++replicated_depth; scratch.Call(0x82449FE0); --replicated_depth;
        ++getups_synced;
        static unsigned logged=0;
        if(logged++<30) {
          char line[200];
          std::snprintf(line,sizeof(line),"%s got up in their game %llu ms ago: getting up here (%s), state %u/%u, %.1f m from their position",r.tag,
                        (unsigned long long)(now-r.up_since),waited?"after 1 s":"at rest",api->read_u32(obj+516),api->read_u32(obj+520),off);
          ProbeLog(line);
        }
      }
    }
    // Stranded: still ragdolled outside state 8 (another state set while it
    // lay or got up), and only state 8 runs the ragdoll (82463A28), so nothing
    // ends it. The game's own "knocked while down" (824497F0 on a ragdolled
    // character: 8/15 again, 824498E0-82449A04) lets the next get-up work.
    // (Never 82449EB8 for this: it indexes the ragdoll slots with +784 unchecked.)
    if(api->read_u32(obj+516)!=8) {
      if(!r.stranded_since) r.stranded_since=now;
      if(now-r.stranded_since>500) {
        const int32_t slot=int32_t(api->read_u32(obj+784));
        const bool can=slot>=0 && sub!=17;
        bool ok=false;
        if(can) {
          r.next_getup=0;
          SetGetUpSpots(obj,p.pos,r.tag,"knocked again");
          scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=500; scratch.ctx.r5.u64=0;
          ++replicated_depth; ++knocking_avatar; scratch.Call(0x824497F0); --knocking_avatar; --replicated_depth;
          ok=api->read_u32(obj+516)==8;
          if(ok) ++stranded_recovered;
        }
        r.stranded_since=ok ? 0 : now;   // not: again in 0.5 s
        if(!r.stranded_logged) {   // once per time it gets stranded
          r.stranded_logged=true; ++stranded_found;
          static unsigned logged=0;
          if(logged++<20) {
            char line[200];
            std::snprintf(line,sizeof(line),"%s's character ragdolled in state %u/%u for 0.5 s (+780 %u, slot %d): %s",r.tag,state,sub,api->read_u32(obj+780),slot,
                          ok?"knocked again here so it can get up":can?"knock refused, tried again every 0.5 s":"can't be knocked again (no ragdoll slot or sub-state 17)");
            ProbeLog(line);
          }
        }
      }
    } else { r.stranded_since=0; r.stranded_logged=false; }
    return api->read_u32(obj+780)!=0;
  }
  r.up_since=0; r.stranded_since=0; r.stranded_logged=false;
  return false;
}
// 82449FE0(human, r4): getting up from a ragdoll. r4 = 1: the ragdoll update
// once its timer ran out and the body is at rest (82463DC4); another player's
// character gets up only when they do. r4 = 0: teleports and scripts that end
// the ragdoll with 82449EB8 right after (8243F448, 8243EB60 ...); let through,
// so its controller reset 8227B6A8(+476,0,1) at 8244A548 runs first.
WmlGuestFunction original_get_up=nullptr;
void GetUpHook(WmlContext* c,uint8_t* b) {
  if(connected.load() && !replicated_depth) {
    const int s=AvatarSlotOf(uint32_t(api->get_r(c,3)));
    if(s>=0 && remotes[s].owner_down && uint8_t(api->get_r(c,4))==1) return;
    // (Its states 8/16-17 pass StateHook, which takes state 8 on another
    // player's character only from co-op's own calls.)
    if(s>=0) { ++replicated_depth; original_get_up(c,b); --replicated_depth; return; }
  }
  original_get_up(c,b);
}
// 82540780(car): a crash may throw people out of a car (each game rolls its
// own dice: 5% for players, 50% for others). Not for a car another game
// drives or with another player's character or a copy of the host's people
// in it: their game decides, and its exit comes as an event.
WmlGuestFunction original_eject_check=nullptr;
// (Counted once per car: the check runs every frame while a car crashes.)
std::unordered_set<uint32_t> ejection_cars_skipped;  // game thread, cleared every 10 s
void EjectCheckHook(WmlContext* c,uint8_t* b) {
  const uint32_t v=uint32_t(api->get_r(c,3));
  if(connected.load() && Readable(v,kVehicleSize) && api->read_u32(v+72)==5) {
    const uint32_t h=api->read_u32(v+68);
    // (A copy this player drives: this game decides for it now.)
    bool other=!LocalDrives(h) && (IsOtherGamesCopy(h) || (!hosting.load() && SharedCarHostHandle(h)));
    for(uint32_t seat=0;seat<4 && !other;++seat) {
      const uint32_t occ=OccupantHandle(v,seat);
      other=occ && (IsAvatarHandle(occ) || SharedPedHostHandle(occ));
    }
    for(int s=0;s<kSlots && !other;++s) other=remotes[s].avatar && AvatarCar(s)==h;
    if(other) { if(ejection_cars_skipped.size()<256) ejection_cars_skipped.insert(h); return; }
  }
  original_eject_check(c,b);
}
// Other players' projectiles: a copy made here (owner: their character) that
// flies like theirs and never explodes by itself; when theirs explodes, the
// copy is put at that spot and explodes once (R 3.3, 3.4).
void ApplyProjectiles(GuestScratch& scratch,uint64_t now) {
  std::vector<ProjIn> in;
  { std::lock_guard lock(event_mutex); in.swap(proj_in); }
  const uint32_t base=api->read_u32(kWeaponInfos), count=api->read_u32(kWeaponCount);
  for(const ProjIn& m:in) {
    const ProjPacket& p=m.p;
    if(m.origin>=kSlots || m.origin==my_slot.load()) continue;
    Remote& r=remotes[m.origin];
    const uint64_t key=CarKey(m.origin,p.id);
    // Owner: their character here (kills credited to it: 825BF418 passes the
    // object of +228 to 82138AE8 as the attacker, so 82482098 names it and
    // KillCrimeHook sends their game the credit; and the copy doesn't hit it),
    // or nobody when it isn't here: the copy and its explosion still happen
    // (a rocket from 300 m away, people the host shares near it).
    // 2.2: a person of the host's world stream's (shooter): its copy here, if any.
    if(p.shooter && !WorldActionsWith(m.origin)) continue;   // (Package C off here)
    const uint32_t shooter_copy=p.shooter && m.origin==0 && !hosting.load() ? WorldPersonLocal(p.shooter) : 0;
    const uint32_t credit=p.shooter ? (ResolveHuman(shooter_copy) ? shooter_copy : 0) : AvatarObjOf(m.origin) ? r.avatar : 0;
    auto make=[&](int32_t weapon,const float* vel,uint32_t owner)->uint32_t {
      if(weapon<0 || !base || uint32_t(weapon)>=count || count>255) return 0;
      const uint32_t info=base+uint32_t(weapon)*kWeaponInfoSize;
      const uint32_t type=api->read_u32(info+100);
      if((type!=1 && type!=2) || !api->read_u32(info+144)) return 0;   // projectiles with an explosion only (I A.4)
      const uint32_t d=scratch.data()+2420, rp=d+16, vp=d+56;
      for(int i=0;i<3;++i) { api->write_f32(d+i*4,p.pos[i]); api->write_f32(vp+i*4,vel[i]); }
      for(int i=0;i<9;++i) api->write_f32(rp+i*4,p.rows[i]);
      scratch.ctx.r3.u64=owner; scratch.ctx.r4.u64=info;
      scratch.ctx.r5.u64=d; scratch.ctx.r6.u64=rp; scratch.ctx.r7.u64=vp;
      ++making_copy; const bool ok=scratch.Call(0x8241FCB0); --making_copy;
      const uint32_t proj=ok ? scratch.ctx.r3.u32 : 0;
      // A rocket can end inside the call (its owner-to-muzzle ray hit
      // something): it was held, deleted, and isn't a copy.
      if(!proj || !LiveProjectile(proj)) return 0;
      if(!owner && credit) api->write_u32(proj+228,credit);
      return proj;
    };
    bool finite=true;
    for(float v:p.pos) finite=finite && std::isfinite(v) && std::abs(v)<100000;
    for(float v:p.rows) finite=finite && std::isfinite(v) && std::abs(v)<=1.5f;
    for(float v:p.vel) finite=finite && std::isfinite(v) && std::abs(v)<1000;
    if(!finite) continue;
    if(p.kind==1) {
      // Not when their character isn't here and it is far away: far players
      // mustn't fill the shared pool (36) and leave our own rockets none.
      const float dx=p.pos[0]-local_x.load(), dz=p.pos[2]-local_z.load();
      if(!credit && dx*dx+dz*dz>250.0f*250.0f) continue;
      const uint32_t proj=make(p.weapon,p.vel,credit);
      if(proj) { proj_copies[key]=ProjCopy{proj,ProjHandle(proj),credit,p.weapon,now}; ++projs_copied; if(p.shooter) ++copy_proj_made; }
      static unsigned logged=0;
      if(logged++<30) { char line[140]; std::snprintf(line,sizeof(line),"%s's projectile (weapon %d): %s",r.tag,p.weapon,proj?"copy made here":"no copy (pool full or not a projectile)"); ProbeLog(line); }
    } else if(p.kind==2) {
      uint32_t proj=0; int32_t weapon=p.weapon;
      if(auto it=proj_copies.find(key); it!=proj_copies.end()) {
        if(weapon<0) weapon=it->second.weapon;
        if(LiveProjectile(it->second.proj) && ProjHandle(it->second.proj)==it->second.handle) proj=it->second.proj;
        else proj_copies.erase(it);
      }
      if(!proj) {
        // Ended here already (its own fuse, contact or range, or set off by an
        // explosion here), or never made: a still copy at the spot, exploded
        // at once (owner 0 while making it: no ray from the character).
        // (A tiny velocity along its heading, not zero: a rocket's orientation
        // is built from its velocity.)
        float still[3]={p.rows[6]*0.01f,p.rows[7]*0.01f,p.rows[8]*0.01f};
        if(still[0]*still[0]+still[1]*still[1]+still[2]*still[2]<1e-8f) { still[0]=0; still[1]=0; still[2]=0.01f; }
        proj=make(weapon,still,0);
        if(proj) proj_copies[key]=ProjCopy{proj,ProjHandle(proj),credit,weapon,now};
      }
      if(!proj) continue;
      // At their spot: the explosion's ray (from +76 to +20) starts there too.
      for(int i=0;i<3;++i) { api->write_f32(proj+20+i*4,p.pos[i]); api->write_f32(proj+76+i*4,p.pos[i]); }
      replaying_proj=proj;
      scratch.ctx.r3.u64=proj; scratch.ctx.r4.u64=0; scratch.ctx.r5.u64=0;
      scratch.Call(0x8241EE80);
      replaying_proj=0;
      proj_copies.erase(key);
      ++booms_played; if(p.shooter) ++copy_proj_booms;
    }
  }
  // Copies the game ended without our hook (fuse, range): forgotten.
  for(auto it=proj_copies.begin();it!=proj_copies.end();)
    it=(now-it->second.made>60000 || !LiveProjectile(it->second.proj) || ProjHandle(it->second.proj)!=it->second.handle) ? proj_copies.erase(it) : std::next(it);
}

// ---- Other players on the maps (2.1.4) ----
// An arrow per other player on the minimap and the pause map, drawn like the
// player's own (map_blip, HUD item 0x83023660[9]) the way the minimap draws
// homies (822E8FE8: heading from the forward row, a tint, 8227E720): after
// the game's followers pass on the minimap, after the pause map's own drawing
// (822B3F70, the MAP tab's view 0x8206169C). Where their character is while
// it exists here, else their newest pose (far off, not made yet, hidden in a
// cutscene, dead); a colour per slot; half alpha while they lie or are dead;
// on the map's edge when off it. Their character's purple people dot
// (822E8C78, the minimap's team-0 dot, icon [4] map_other_blip_revive) is
// skipped. Nothing goes in the icon lists: no mission clears it, no icon slot
// is used. The people, car and follower passes run from the HUD minimap
// 822ECDB0 only, so the pause map never showed them (the host couldn't be
// found there after a death).
struct MapMarker { float pos[3]{}; float yaw=0; bool have=false, down=false, from_character=false; };
std::array<MapMarker,kSlots> markers;    // UpdateAvatar writes, the HUD hooks read
std::atomic<uint32_t> markers_minimap{0}, markers_minimap_edge{0}, markers_pausemap{0}, markers_pausemap_edge{0}, dots_skipped{0}, marker_calls_failed{0};   // (Markers line; failed: since the start)
// Not the player's yellow (255,255,0), the homies' purple (160,0,255) or the enemies' red.
constexpr uint8_t kSlotColours[kSlots][3]={{0,200,255},{0,230,80},{255,140,0},{255,100,180},{255,255,255},{0,160,160},{130,200,0},{255,200,120},{120,120,255},{200,160,100},{0,100,255},{180,255,220}};
constexpr float kMapPi=3.14159265f;      // what the game adds to a map arrow's heading (0x820380B0)
// Every update, with the pose just taken: where this slot's player is.
void NoteMapMarker(const Remote& r,int slot,const PlayerPose& p) {
  MapMarker& m=markers[slot];
  const uint32_t obj=ResolveHuman(r.avatar);
  if(obj) { for(unsigned i=0;i<3;++i) m.pos[i]=api->read_f32(obj+20+4*i); m.yaw=std::atan2(api->read_f32(obj+56),api->read_f32(obj+64)); }   // (8262EB58(obj+32): the forward row's x and z)
  else { std::memcpy(m.pos,p.pos,sizeof(m.pos)); m.yaw=std::atan2(p.rows[6],p.rows[8]); }
  m.down=(p.flags&(kPoseRagdoll|kPoseDead))!=0;
  if(!m.have || m.from_character!=(obj!=0)) { static unsigned logged=0; if(logged++<40) { char line[160]; std::snprintf(line,sizeof(line),"Map marker: %s from %s at %.0f %.0f %.0f, yaw %.2f",r.tag,obj?"their character here":"their last pose",m.pos[0],m.pos[1],m.pos[2],m.yaw); ProbeLog(line); } }
  m.from_character=obj!=0; m.have=true;
}
// Another player in the session with a position (not this game's own slot).
bool MarkerShown(int slot) { return slot!=int(my_slot.load()) && Present(slot) && markers[slot].have; }
bool AnyMarker() { for(int s=0;s<kSlots;++s) if(MarkerShown(s)) return true; return false; }
inline float HudScale() { return std::max(api->read_f32(0x82FFB4EC),api->read_f32(0x82FFB4E8)); }   // the HUD scale the arrows are drawn with (both maps take the larger of the two)
// The draw colour: 8263E7D8(r, g, b, a) - a slot's, or white again (what the
// minimap's passes leave; 822ECDB0 sets its own before the player's arrow).
void MarkerColour(GuestScratch& scratch,int slot,float alpha) {
  const uint8_t* col=slot>=0 ? kSlotColours[slot] : nullptr;
  const float a=std::clamp(alpha,0.0f,1.0f)*(col && markers[slot].down ? 128.0f : 255.0f);
  scratch.ctx.r3.u64=col?col[0]:255; scratch.ctx.r4.u64=col?col[1]:255; scratch.ctx.r5.u64=col?col[2]:255; scratch.ctx.r6.u64=uint32_t(a);
  scratch.Call(0x8263E7D8);
}
// One arrow: 8227E720(bitmap, f1 x, f2 y, f3 angle, f4 scale, r8 params), a
// quad of the bitmap's size times scale, turned by angle around x, y.
bool DrawMarker(GuestScratch& scratch,uint32_t bitmap,float x,float y,float angle,float scale,uint32_t params) {
  scratch.ctx.r3.u64=bitmap; scratch.ctx.r8.u64=params;
  scratch.ctx.f1.f64=x; scratch.ctx.f2.f64=y; scratch.ctx.f3.f64=angle; scratch.ctx.f4.f64=scale;
  if(scratch.Call(0x8227E720)) return true;
  if(marker_calls_failed++<3) ProbeLog("Map marker: the game's bitmap draw 8227E720 failed here");
  return false;
}
WmlGuestFunction original_team0_dot=nullptr, original_followers=nullptr, original_pause_map=nullptr;
// 822E8C78(pos = person+20, params): the minimap's purple dot of a team-0
// person (from the people pass 822E8D00, which skips homies and people in
// cars). Not for another player's character: its arrow stands there.
void Team0DotHook(WmlContext* c,uint8_t* b) {
  const uint32_t pos=uint32_t(api->get_r(c,3));
  if(connected.load() && pos>=20 && AvatarSlotOf(pos-20)>=0) { ++dots_skipped; return; }
  original_team0_dot(c,b);
}
// 822E8FE8(f1 alpha, r4 &params): the minimap's followers pass (story mode,
// from 822ECDB0 with the MP byte clear; it returns inside when the player
// has no homies). After it, the other players' arrows: 822E8A78(&xy, &aux,
// pos, 0) projects a world position onto the minimap (a point past its radius
// is put on the edge, and it says so), the arrow turned by their heading + pi
// plus the map's turn *0x8370E438 while the map doesn't turn with the player
// (byte 0x8370E407), at the player's own size (homies: 0.8 of it).
void FollowersHook(WmlContext* c,uint8_t* b) {
  // (read before the original: it runs on this context and leaves r4 / f1
  // as it last wrote them - with a homie, a stack slot and 160.0)
  const float alpha=float(api->get_f(c,1)); const uint32_t params=uint32_t(api->get_r(c,4));
  original_followers(c,b);
  if(!connected.load() || !AnyMarker()) return;
  const uint32_t blip=api->read_u32(0x83023660+36);   // map_blip (24x24, hud_sheet_01)
  const float ui=HudScale(), turn=api->read_u8(0x8370E407) ? 0.0f : api->read_f32(0x8370E438);
  { static bool logged=false; if(!logged) { logged=true; char line[200]; std::snprintf(line,sizeof(line),"Map markers: map_blip bitmap %u (HUD item 9), HUD scale %.2f, the map turns with the player: %u, MP blip 108's bitmap %u (story HUD: none expected), params %08X",blip,ui,api->read_u8(0x8370E407),api->read_u32(0x83027258+4*108),params); ProbeLog(line); } }
  GuestScratch scratch(c);
  const uint32_t d=scratch.data(), xy=d, aux=d+16, pos=d+32;
  for(int s=0;s<kSlots;++s) {
    if(!MarkerShown(s)) continue;
    const MapMarker& m=markers[s];
    for(unsigned i=0;i<3;++i) api->write_f32(pos+4*i,m.pos[i]);
    scratch.ctx.r3.u64=xy; scratch.ctx.r4.u64=aux; scratch.ctx.r5.u64=pos; scratch.ctx.r6.u64=0;
    if(!scratch.Call(0x822E8A78)) { if(marker_calls_failed++<3) ProbeLog("Map marker: the minimap projection 822E8A78 failed here"); continue; }
    if(scratch.ctx.r3.u32&0xFF) ++markers_minimap_edge;
    MarkerColour(scratch,s,alpha);
    if(DrawMarker(scratch,blip,api->read_f32(xy),api->read_f32(xy+4),m.yaw+kMapPi+turn,ui,params)) ++markers_minimap;
  }
  MarkerColour(scratch,-1,alpha);
}
// 822B3F70(view, -, &top-left, &bottom-right, f1 alpha): the pause map's
// drawing - the map, the icon lists (822EDC50), the route, the player's
// arrow. After it, the other players' arrows with its own transform (the icon
// pass's and the arrow's, block 0x8370DD60: px = +1072 + +1080*zoom +
// (+1104 - x)*zoom, py = +1076 + +1084*zoom + (z - +1108)*zoom, zoom
// *0x827AF7B4; the page's x runs against the world's), its arrow bitmap
// 0x8300FC78[0] at HUD scale * (clamped zoom *0x827AF7A8 * 0.333 + 0.667),
// turned by their heading + pi (the player's own turns with the camera), kept
// on the page by hand: the game's scissor is back to the whole screen when it
// returns.
void PauseMapHook(WmlContext* c,uint8_t* b) {
  // (read before the original: its last call is the scissor reset
  // 8263E210(0,0,w,h), which leaves r5 / r6 as the screen size)
  const float alpha=float(api->get_f(c,1));
  const uint32_t tl=uint32_t(api->get_r(c,5)), br=uint32_t(api->get_r(c,6));
  original_pause_map(c,b);
  if(!connected.load() || api->read_u8(0x8370E9F6) || !AnyMarker()) return;
  if(!Readable(tl,8) || !Readable(br,8)) return;
  constexpr uint32_t t=0x8370DD60;
  const float zoom=api->read_f32(0x827AF7B4), ox=api->read_f32(t+1072)+api->read_f32(t+1080)*zoom, oy=api->read_f32(t+1076)+api->read_f32(t+1084)*zoom, cx=api->read_f32(t+1104), cz=api->read_f32(t+1108);
  const float scale=HudScale()*(api->read_f32(0x827AF7A8)*api->read_f32(0x82089440)+api->read_f32(0x8208943C)), half=16.0f*scale;
  const float x0=api->read_f32(tl)+half, y0=api->read_f32(tl+4)+half, x1=api->read_f32(br)-half, y1=api->read_f32(br+4)-half;
  if(!(x0<x1) || !(y0<y1)) return;   // (no page)
  const uint32_t arrow=api->read_u32(0x8300FC78);
  { static bool logged=false; if(!logged) { logged=true; char line[240]; std::snprintf(line,sizeof(line),"Map markers: pause-map arrow bitmap %u (0x8300FC78[0]), zoom %.2f (clamped %.2f, arrow scale %.2f), origin %.0f %.0f, centre %.0f %.0f, page %.0f %.0f - %.0f %.0f",arrow,zoom,api->read_f32(0x827AF7A8),scale,ox,oy,cx,cz,api->read_f32(tl),api->read_f32(tl+4),api->read_f32(br),api->read_f32(br+4)); ProbeLog(line); } }
  GuestScratch scratch(c);
  for(int s=0;s<kSlots;++s) {
    if(!MarkerShown(s)) continue;
    const MapMarker& m=markers[s];
    const float px=ox+(cx-m.pos[0])*zoom, py=oy+(m.pos[2]-cz)*zoom, x=std::clamp(px,x0,x1), y=std::clamp(py,y0,y1);
    if(x!=px || y!=py) ++markers_pausemap_edge;
    MarkerColour(scratch,s,alpha);
    if(DrawMarker(scratch,arrow,x,y,m.yaw+kMapPi,scale,0x82839AA0)) ++markers_pausemap;
  }
  MarkerColour(scratch,-1,alpha);
}

// Making a character (a blocking body-model load the first time, the player
// object) and its first dressing (waits for the clothes to load) are slow:
// one of them per 300 ms for all other players together, so joining a full
// session doesn't stall one update for seconds (game thread).
uint64_t heavy_avatar_until=0;
// One other player's character here (v2: called for each slot).
float AvatarKeepRadius(int slot);   // (130 m; a member of the host's area member_leave_m + 30 m, 2.2: further down)
void UpdateAvatar(GuestScratch& scratch,Remote& r,int slot,uint64_t now,bool clothing_dump) {
  bool made_now=false;
  PlayerPose p; bool fresh;
  {
    std::lock_guard lock(pose_mutex);
    RemoteIn& in=remote_in[slot];
    p=in.pose; fresh=in.pose_new; in.pose_new=false;
  }
  if(!p.sequence) return;
  if(fresh) r.seen=now;
  NoteMapMarker(r,slot,p);   // (their arrow on the maps, from here on whatever happens below)
  if(now-r.seen>3000 && !HostPauseHold(now)) { if(r.avatar) DestroyAvatar(scratch,r); return; }
  // No other player's character during a cutscene here, and for 2 s after:
  // both games play it with their own player on the same spot, so the
  // character stood inside this player (who then got stuck and kept falling
  // over). The joiner steps aside when it ends; the character comes back
  // dressed.
  {
    if(CutscenePlaying() || (!hosting.load() && host_cutscene.load())) r.cutscene_seen=now;
    // Hidden like character_hide / character_show (824C92E0 / 824C9488) do:
    // not removed (removing takes the clothes off and waits for the game,
    // which never finished while a cutscene loads: the game froze).
    const uint32_t shown_obj=ResolveHuman(r.avatar);
    if(r.cutscene_seen && now-r.cutscene_seen<2000) {
      if(shown_obj && !r.cut_hidden) { SetCharacterHidden(scratch,shown_obj,true); r.cut_hidden=true; ProbeLog((std::string("Cutscene: ")+r.tag+"'s character hidden until it ends").c_str()); }
      r.follow.Reset();
      return;
    }
    if(r.cut_hidden) {
      r.cut_hidden=false;
      if(shown_obj) { SetCharacterHidden(scratch,shown_obj,false); ProbeLog((std::string(r.tag)+"'s character shown again").c_str()); }
    }
  }
  if(p.sequence!=r.last_sequence) { r.last_sequence=p.sequence; r.follow.Add(p.time,p.pos,p.rows,now); }
  {
    // Out of this game's streaming range the character's clothing is unloaded
    // for good. Remove the character and make a fresh one when the player
    // comes back; a new character is dressed like the first. (Their car copy
    // goes with CleanCarCopies once nobody sits in it.) 2.2: a member of the
    // host's area keeps its character to member_leave_m + 30 m (the host's
    // police come for it, PvP), made within 60 m as before.
    const float dx=p.pos[0]-local_x.load(), dz=p.pos[2]-local_z.load(), keep=AvatarKeepRadius(slot);
    if(dx*dx+dz*dz>keep*keep && r.avatar) {
      DestroyAvatar(scratch,r);
      if(!r.avatar) ProbeLog((std::string(r.tag)+" out of range; character removed until they come back").c_str());
      r.follow.Reset();
      return;
    }
    if(dx*dx+dz*dz>keep*keep) { r.follow.Reset(); return; }
  }
  const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316, name=data+1400;
  uint32_t obj=ResolveHuman(r.avatar);
  if(!obj) {
    r.avatar=0;
    WritePose(p,pos,matrix);
    const float dx=p.pos[0]-local_x.load(),dy=p.pos[1]-local_y.load(),dz=p.pos[2]-local_z.load();
    // Only create close by: a character made far from the camera is built
    // without its body and clothing meshes and never gets them back.
    if(dx*dx+dy*dy+dz*dz>60*60) return;
    if(MonoMs()<heavy_avatar_until) return;   // (another player's character first)
    if(now<r.next_attempt) return;
    r.next_attempt=now+1000;
    heavy_avatar_until=MonoMs()+300;
    const char* used=p.model;
    char local_model[64];
    { std::lock_guard lock(pose_mutex); std::memcpy(local_model,local_pose.model,sizeof(local_model)); }
    const uint32_t player=api->read_u32(0x8309ABEC);
    // Own body model for them: ask for it and wait for it (up to 8 s, then
    // the shared one as before).
    if(use_player_avatar && own_body && !OwnBodyDefinition(scratch,NextPlayerSlot(),true)) {
      if(!r.own_body_wait) r.own_body_wait=now;
      if(now-r.own_body_wait<8000) return;
    }
    r.own_body_wait=0;
    if(use_player_avatar && p.model[0] && !std::strcmp(p.model,local_model)) {
      if(const uint32_t made=SpawnPlayerObject(scratch,player,pos,matrix,r.tag)) {
        r.avatar=api->read_u32(made+68); r.player_obj=true; r.own_body=last_spawn_own_body;
        if(ResolveHuman(r.avatar)!=made) {
          ProbeLog((std::string(r.tag)+": player object not in the object table, returning it").c_str());
          ReleasePlayerObject(scratch,made); r.avatar=0; r.player_obj=false;
        }
      }
    }
    if(!r.avatar) {
      uint32_t definition=LookupModel(scratch,p.model,name);
      if(!definition) { used=kFallbackModel; definition=LookupModel(scratch,kFallbackModel,name); }
      if(!definition) { ProbeLog((std::string(r.tag)+": no usable model in this game").c_str()); return; }
      r.avatar=CreateActor(scratch,definition,pos,matrix,r.tag);
    }
    obj=ResolveHuman(r.avatar);
    heavy_avatar_until=MonoMs()+300;
    if(!obj) { r.avatar=0; r.player_obj=false; return; }
    made_now=true;
    r.follow.walk_logged=0;   // (its "walks here" lines: 10 per character made)
    r.next_item_check=0; r.applied_worn.clear(); r.dress_step=0;
    // Get-up spots where it stands (SetGetUpSpots); down at once if they lie.
    SetGetUpSpots(obj,p.pos,r.tag,"made");
    r.owner_down=false; r.getup_until=0; r.knock_check_at=0; r.stranded_since=0; r.stranded_logged=false;
    // A character made again (back in range, after a gap in updates) starts
    // with nothing on: dress it from the last clothing received, the same way
    // as the first one. It was left undressed until the other player changed
    // clothes (the missing torso and legs: F8 showed "0 items listed").
    { std::lock_guard lock(pose_mutex); if(!remote_in[slot].worn.empty()) remote_in[slot].worn_new=true; remote_in[slot].model=used; }
    r.dressed_record=0;
    r.follow.next_order=0; r.follow.moving=false;
    char line[200];
    std::snprintf(line,sizeof(line),"%s avatar uses model %s (their model '%s')",r.tag,used,p.model);
    ProbeLog(line);
  }
  // Pedestrians pick up props (bottles, phones) by themselves; the player's
  // held item isn't synchronized yet, so the character carries nothing.
  if(now>=r.next_item_check) {
    scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=0;
    scratch.Call(0x82446E50);
    r.next_item_check=now+500;
  }
  if(r.player_obj && p.look_length) {
    std::vector<uint8_t> look(p.look,p.look+p.look_length);
    if(look!=r.applied_look || r.look_handle!=r.avatar) {
      const bool ok=ApplyLook(scratch,r.avatar,p.look,p.look_length);
      char line[140];
      std::snprintf(line,sizeof(line),"%s look block %u bytes (%02X %02X) %s",r.tag,p.look_length,p.look[0],p.look_length>1?p.look[1]:0,ok?"applied":"not applied");
      ProbeLog(line);
      if(ok) { r.applied_look=look; r.look_handle=r.avatar; }
    }
  }
  if(r.player_obj) {
    std::vector<uint8_t> worn;
    { std::lock_guard lock(pose_mutex); RemoteIn& in=remote_in[slot]; if(in.worn_new) { worn=in.worn; in.worn_new=false; } }
    if(clothing_dump) {
      // F8: what this player's clothing looks like right now (press it while
      // the torso/legs are missing): each worn item's node and piece, then the
      // same for this game's own player, which looks right.
      for(int who=0;who<2;++who) {
      const uint32_t subject=who==0 ? obj : api->read_u32(0x8309ABEC);
      if(!Readable(subject,4488)) continue;
      const uint32_t custom=api->read_u32(subject+4484);
      char line[260];
      std::snprintf(line,sizeof(line),"F8 clothing (%s): object %08X record %08X slot %u, %u items listed, flags216 %08X, cutscene %d/%d, queue %08X",
                    who==0?r.tag:"own player",subject,custom,Readable(custom,12)?api->read_u32(custom+8):99,Readable(custom,12)?WornCount(custom):0,
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
    // Some game events (e.g. taking a hit) strip or replace the character's
    // clothing. Once a second, check the slot still lists what was put on.
    if(now>=r.next_clothes_check && !r.applied_worn.empty() && !r.dress_step) {
      r.next_clothes_check=now+1000;
      const uint32_t custom=api->read_u32(obj+4484);
      // Clothing pieces can also be unloaded while the list stays (e.g. after
      // driving far away and back): count listed nodes without a component.
      // Single player hands a free clothing piece (+132 without 0x10/0x40)
      // to whoever needs one, without checking whose it was (8248D5F0), so
      // a released piece of this character can be reloaded with someone
      // else's clothing (cutscene characters...) while still listed here:
      // count those as missing too.
      unsigned missing=0, released=0;
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
      r.missing_for=missing ? r.missing_for+1 : 0;
      // No dressing again just because a cutscene ended: doing that right
      // after one froze (0.51) or crashed (0.52) the game.
      if(CutscenePlaying() || host_cutscene.load()) r.missing_for=0;
      // Far from the camera the game unloads clothing and may hand the pieces
      // to something else. Dress again when the character comes back close.
      if(r.redress_for_colours && !CutscenePlaying() && !host_cutscene.load()) {
        r.redress_for_colours=false;
        ProbeLog((std::string(r.tag)+" dressed again for their colours (hair colour is set when the hair goes on)").c_str());
        std::lock_guard lock(pose_mutex); worn=remote_in[slot].worn;
        r.applied_worn.clear();
      } else
      if(custom!=r.dressed_record || WornCount(custom)!=r.applied_worn[0] || r.missing_for>=3) {
        r.missing_for=0;
        char line[200];
        std::snprintf(line,sizeof(line),"%s clothing lost (record %08X, %u items listed, %u expected, %u pieces unloaded, %u taken over); dressing again",
                      r.tag,custom,Readable(custom,12)?WornCount(custom):0,r.applied_worn[0],missing-released,released);
        ProbeLog(line);
        std::lock_guard lock(pose_mutex); worn=remote_in[slot].worn;
        r.applied_worn.clear();
      }
    }
    // The first dressing of a new character waits for the clothing to load,
    // as before. Dressing again happens over several frames without waiting:
    // take off (queued), let the game's frame update finish that, then wear.
    // Waiting inside the update right after a cutscene never returned (the
    // game froze), so nothing starts during a cutscene or while the game
    // still has clothing requests queued.
    if(!worn.empty()) r.pending_worn=worn;
    const bool first=r.applied_worn.empty() && r.dressed_record!=api->read_u32(obj+4484);
    if(CutscenePlaying() || host_cutscene.load()) r.last_cutscene=now;
    const bool quiet=now-r.last_cutscene>10000 && ClothingQueueIdle();
    if(r.dress_step==0 && !r.pending_worn.empty() && r.pending_worn!=r.applied_worn && quiet) {
      if(first && !made_now && MonoMs()<heavy_avatar_until) {}   // another character's turn: next update
      else if(first) {
        // Colours first, as multiplayer does, so the hair goes on in its colour.
        { std::vector<uint8_t> ex; { std::lock_guard lock(pose_mutex); ex=remote_in[slot].extras; }
          r.extras_at_dress.clear();
          if(!ex.empty() && ApplyExtras(scratch,obj,ex.data(),unsigned(ex.size()),false)) r.extras_at_dress=ex; }
        const bool ok=DressPlayerObject(scratch,obj,r.pending_worn.data(),unsigned(r.pending_worn.size()));
        char line[140];
        std::snprintf(line,sizeof(line),"%s clothing: %zu bytes %s",r.tag,r.pending_worn.size(),ok?"worn":"not fully applied");
        ProbeLog(line);
        r.applied_worn=r.pending_worn; r.pending_worn.clear();
        r.dressed_record=api->read_u32(obj+4484);
        heavy_avatar_until=MonoMs()+300;
      } else {
        const uint32_t custom=api->read_u32(obj+4484);
        const uint32_t cslot=Readable(custom,12) ? api->read_u32(custom+8) : 99;
        { std::vector<uint8_t> ex; { std::lock_guard lock(pose_mutex); ex=remote_in[slot].extras; }
          if(cslot<12 && !ex.empty() && ApplyExtras(scratch,obj,ex.data(),unsigned(ex.size()),false)) r.extras_at_dress=ex; }
        if(cslot<12) {
          for(uint32_t i=0;i<54 && i<api->read_u32(kSlotTables+13056+cslot*4);++i) {
            scratch.ctx.r3.u64=custom; scratch.ctx.r4.u64=api->read_u32(kSlotTables+13120+cslot*1080+i*20);
            scratch.Call(0x8248D198);
          }
          r.dress_step=1; r.dress_started=now;
        }
      }
    } else if(r.dress_step==1 && ClothingQueueIdle()) {
      const uint32_t custom=api->read_u32(obj+4484);
      scratch.ctx.r3.u64=custom; scratch.Call(0x8248D340);
      r.dress_step=2;
    } else if(r.dress_step==2 && ClothingQueueIdle() && !CutscenePlaying()) {
      const uint32_t custom=api->read_u32(obj+4484);
      const bool ok=WearItems(scratch,custom,r.pending_worn.data(),unsigned(r.pending_worn.size()));
      char line[160];
      std::snprintf(line,sizeof(line),"%s dressed again: %zu bytes %s, %u items listed (%.1f s)",r.tag,r.pending_worn.size(),
                    ok?"worn":"not fully applied",WornCount(custom),(now-r.dress_started)/1000.0);
      ProbeLog(line);
      r.applied_worn=r.pending_worn; r.pending_worn.clear();
      r.dressed_record=custom; r.dress_step=0;
    }
    if(r.dress_step && now-r.dress_started>20000) {
      ProbeLog((std::string(r.tag)+" dressing gave up: the game's clothing requests didn't finish in 20 s; trying again later").c_str());
      r.dress_step=0;
    }
    std::vector<float> morphs;
    { std::lock_guard lock(pose_mutex); RemoteIn& in=remote_in[slot]; if(in.morphs_new) { morphs=in.morphs; in.morphs_new=false; } }
    // Only with its own body model: with the shared one the other player's
    // sliders would reshape this game's own player too.
    if(morphs.size()==128 && morphs!=r.applied_morphs) {
      ProbeLog((std::string(r.tag)+" body sliders received: "+MorphSummary(morphs.data(),128)+
                (sync_body?"":" (mod.ini body = 0: not applied)")+(r.own_body?"":" (shared body model: not applied)")).c_str());
    }
    if(sync_body && r.own_body && morphs.size()==128 && morphs!=r.applied_morphs) {
      ProbeLog((std::string(r.tag)+(ApplyMorphs(scratch,obj,morphs.data()) ? " body shape applied" : " body shape not applied")).c_str());
      r.applied_morphs=morphs;
      r.morph_check_at=now+3000; r.morph_checks=0;
    }
    // Check that the game baked them into the model, and put them back if
    // something (dressing, a look rebuild) reset them.
    if(r.morph_check_at && now>=r.morph_check_at && r.applied_morphs.size()==128) {
      ProbeLog((std::string(r.tag)+" body after "+std::to_string(r.morph_checks==0?3:15)+" s: "+MorphState(obj,r.applied_morphs)).c_str());
      r.morph_check_at=++r.morph_checks<2 ? now+12000 : 0;
    }
    if(sync_body && r.own_body && r.applied_morphs.size()==128 && now>=r.next_morph_recheck) {
      r.next_morph_recheck=now+2000;
      const uint32_t custom=api->read_u32(obj+4484);
      const uint32_t sliders=Readable(custom,12)?api->read_u32(custom+4):0;
      if(Readable(sliders,kMorphs*12)) {
        unsigned wrong=0;
        for(unsigned i=0;i<kMorphs;++i) wrong+=std::fabs(api->read_f32(sliders+i*12+4)-r.applied_morphs[i])>=0.001f;
        if(wrong) {
          if(r.morph_resets++<10) ProbeLog((std::string(r.tag)+" body sliders were reset ("+std::to_string(wrong)+" changed): applied again").c_str());
          ApplyMorphs(scratch,obj,r.applied_morphs.data());
        }
      }
    }
    // Colours (skin tone, hair colour): once dressed, and again when they
    // change or the character was made again.
    {
      const uint32_t custom=api->read_u32(obj+4484);
      std::vector<uint8_t> extras;
      { std::lock_guard lock(pose_mutex); extras=remote_in[slot].extras; remote_in[slot].extras_new=false; }
      if(r.own_body && !extras.empty() && r.dress_step==0 && r.dressed_record==custom && ClothingQueueIdle() &&
         (extras!=r.applied_extras || r.extras_record!=custom)) {
        const bool ok=ApplyExtras(scratch,obj,extras.data(),unsigned(extras.size()));
        ProbeLog((std::string(r.tag)+" colours "+std::string(ok?"applied":"not applied")+" ("+
                  std::to_string(extras.size())+" bytes): "+HexBytes(extras.data(),unsigned(extras.size()))).c_str());
        if(ok) {
          // Put on before these colours were set: dress again once so the hair takes them.
          if(extras!=r.extras_at_dress && !r.applied_worn.empty()) { r.redress_for_colours=true; r.extras_at_dress=extras; }
          r.applied_extras=extras; r.extras_record=custom;
        }
      }
    }
  }
  // The gun in their hand (pose flags bits 8-15), on foot and in cars: changed
  // once the poses have said the same for 400 ms (1.5 s for holstering), and
  // not while a shot of theirs holds it (1.67).
  if(share_shooting && (api->read_u32(obj+216)&0x4000)) {
    const uint32_t want=(p.flags>>8)&0xFF;
    if(want!=r.gun.pose_want) { r.gun.pose_want=want; r.gun.pose_since=now; }
    if(now>=r.gun.hold_until && now-r.gun.pose_since>=(want ? 400u : 1500u) && r.shots.empty()) MatchWeapon(scratch,obj,want,r.gun);
  }
  // Their camera (rockets and throws aim with it) and their aiming stance:
  // the gun up while they aim (each raise holds 2.5 s), so shots fire at once.
  WriteRemoteCamera(obj,p);
  if((p.flags&kPoseAiming) && HandWeapon(obj)>=0 && !api->read_u32(obj+780) && (api->read_u32(obj+216)&0x4000) && now>=r.next_raise) {
    r.next_raise=now+2000;
    scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=1; scratch.ctx.r5.u64=0;
    ++replicated_depth; scratch.Call(0x82449708); --replicated_depth;
  }
  // Knocked down there: down here (nothing moves or seats it meanwhile).
  if(SyncKnockdown(scratch,r,obj,p,now)) { for(auto& s:r.shots) s.held=kHeldLying; r.on_foot=false; r.follow.Reset(); return; }   // (its shots wait: lying)
  TryRemoteShots(scratch,r,obj,now);
  // Stuck in a car they left (see UpdateRemoteCar): made again.
  if(r.force_remake) {
    DestroyAvatar(scratch,r);
    if(!ResolveHuman(r.avatar)) { r.force_remake=false; r.follow.Reset(); }
    return;
  }
  if(UpdateRemoteCar(scratch,r,obj,p,now)) { ++avatar_updates; r.follow.Reset(); r.on_foot=false; return; }
  r.on_foot=true;
  r.car.enter_target=0;
  AvatarMotion& m=r.motion;
  // A landing that never arrived kept the character in "jumping" for good
  // (no hold after later actions, move orders fighting the animation).
  if(m.jumping && now>m.action_until) { m.jumping=false; m.reorder=true; }
  if(m.climbing && now>m.action_until) m.climbing=false;
  if(m.reorder) { m.reorder=false; r.follow.next_order=0; r.follow.order_mode=0; r.follow.moving=true; }
  MakePassive(obj,true);
  if(now>=r.next_crouch) { r.next_crouch=now+250; MatchCrouch(scratch,obj,(p.flags&1)!=0); }
  r.follow.last_gap=0;
  if(r.follow.Drive(scratch,obj,now,r.tag,&m)) ++avatar_updates;
  // Stuck off the path (knocked down just before a cutscene hid it: after the
  // cutscene it lay flat 11-18 m behind for a whole mission, every move order
  // refused): remove it; a fresh one is made where the player is.
  {
    const bool busy=now<m.car_anim_until || m.climbing || m.jumping;
    if(r.follow.last_gap>5.0f && !busy) { if(!r.off_since) r.off_since=now; } else r.off_since=0;
    if(r.off_since && now-r.off_since>3000 && now-r.last_remake>10000) {
      char line[140]; std::snprintf(line,sizeof(line),"%s's character stuck %.0f m off its path for 3 s: made again",r.tag,r.follow.last_gap);
      ProbeLog(line);
      r.off_since=0; r.last_remake=now;
      DestroyAvatar(scratch,r); r.follow.Reset();
    }
  }
}

// Every other player in the session (game thread, each update). The record
// of this game's own slot is emptied too (it was another player's before a
// rejoin gave this game that slot).
bool avatars_cleaning=false;   // game thread: the session ended or this game's slot changed; UpdateBody empties everything first
void UpdateAvatars(GuestScratch& scratch,uint64_t now) {
  const bool clothing_dump=clothing_dump_request.exchange(false);
  const uint8_t me=my_slot.load();
  unsigned shown=0;
  for(int s=0;s<kSlots;++s) {
    Remote& r=remotes[s];
    const bool present=s!=me && Present(s);
    uint32_t generation;
    { std::lock_guard lock(pose_mutex); generation=remote_in[s].generation; }
    if(!present || r.generation!=generation) {
      markers[s].have=false;   // (no arrow on the maps)
      // They left (or someone new has the slot): their character goes first.
      if(r.avatar) {
        DestroyAvatar(scratch,r);
        if(r.avatar) { ++shown; continue; }  // still getting out of a car
      }
      if(r.generation!=generation) {
        r=Remote{};
        r.generation=generation;
        std::snprintf(r.tag,sizeof(r.tag),"Player %d",s+1);
      }
      if(!present) continue;
    }
    UpdateAvatar(scratch,r,s,now,clothing_dump);
    if(r.avatar) ++shown;
  }
  avatar_count=shown;
  WatchPassengerSeat(scratch,now);
  CleanCarCopies(scratch,now,false);
}

// Game thread: play received events. Each event comes from one player
// (e.origin) and is played on that player's character here.
// Their moves (jumps, climbs, punches) arrive at once, but their position is
// shown ~50-130 ms behind: a jump started before the character reached the
// take-off spot. These are held back by that much (their follower's delay).
void ApplyCrime(GuestScratch& scratch,const Event& e,const char* who);  // (with the notoriety, 2.1)
void ApplyBusted(GuestScratch& scratch,const char* who);
void ApplySeen(const Event& e,uint64_t now);   // (2.1.2)
void WorldAuthorityEvent(const Event& e,uint64_t now);   // (2.2, the members' side: with the network thread)
void WorldVehicleTableEvent(const Event& e);
void ApplyEvents(GuestScratch& scratch,uint64_t now) {
  std::vector<Event> in;
  { std::lock_guard lock(event_mutex); in.swap(events_in); }
  {
    static std::vector<std::pair<uint64_t,Event>> held;
    std::vector<Event> ready;
    for(auto& [due,e]:held) if(due<=now) ready.push_back(e);
    held.erase(std::remove_if(held.begin(),held.end(),[now](auto& h){ return h.first<=now; }),held.end());
    for(const Event& e:in) {
      const int delay=e.origin<kSlots ? remotes[e.origin].follow.delay : 0;
      const bool waiting=std::any_of(held.begin(),held.end(),[&](auto& h){ return h.second.origin==e.origin; });
      if((e.kind==kEventAction || e.kind==kEventCall || e.kind==kEventCarAction || e.kind==kEventFire || e.kind==kEventThrow || e.kind==kEventFireRelease) && (waiting || delay>0))
        held.push_back({now+uint64_t(std::clamp(delay,0,200)),e});
      else ready.push_back(e);
    }
    in.swap(ready);
  }
  for(Remote& r:remotes) if(r.pending_victim) {
    const uint32_t v=ResolveHuman(r.pending_victim);
    // The car stays where it is while the other player walks up and pulls
    // the driver out (it drove off meanwhile and the two games differed).
    if(v) if(const uint32_t vc=ResolveVehicle(api->read_u32(v+2496))) if(const uint32_t vel=VehicleVelocityAddress(vc))
      for(unsigned i=0;i<3;++i) { api->write_f32(vel+i*4,0); api->write_f32(vel+16+i*4,0); }
    if(!v) r.pending_victim=0;
    else if((r.victim_exit_at && now>=r.victim_exit_at) || now>=r.victim_deadline) {
      ++replicated_depth; OrderExitVehicle(scratch,v); --replicated_depth;   // (a crew copy's moves in it pass PlayActionHook)
      ProbeLog(r.victim_exit_at?"Pulled-out driver gets out after its move":"Pulled-out driver ordered out (no move came)");
      r.pending_victim=0;
    }
  }

  const uint32_t player=api->read_u32(0x8309ABEC);
  const uint8_t me=my_slot.load();
  for(const Event& e:in) {
    if(e.origin>=kSlots || e.origin==me) continue;
    Remote& r=remotes[e.origin];
    const uint32_t avatar=AvatarObjOf(e.origin);
    const bool from_host=e.origin==0;
    // The host's world: its people, its people's models and effects.
    if(e.kind==kEventPedAction) { if(from_host) ApplyPedAction(scratch,e,now); continue; }
    if(e.kind==kEventPedCall) { if(from_host) ApplyPedCall(scratch,e,now); continue; }
    if(e.kind==kEventPedReact) { if(from_host) ApplyPedReact(scratch,e,now); continue; }
    // Our player's crimes and arrests seen in their game (2.1).
    if(e.kind==kEventNotoriety) { if(share_police) ApplyCrime(scratch,e,r.tag); continue; }
    if(e.kind==kEventBusted) { if(share_police) ApplyBusted(scratch,r.tag); continue; }
    if(e.kind==kEventSeen) { if(share_police && from_host) ApplySeen(e,now); continue; }
    if(e.kind==kEventAuthority) { if(from_host) WorldAuthorityEvent(e,now); continue; }   // (2.2: who drives a car of the host's world)
    if(e.kind==kEventCarDamage) { if(hosting.load()) WorldCarDamageEvent(scratch,e,avatar); continue; }   // (2.2 A/9: a member's bullet on a car of this game's world)
    if(e.kind==kEventExplosion) { if(from_host && !hosting.load()) WorldExplosionEvent(scratch,e,now); continue; }   // (2.2 Package C: an explosion of the host's world)
    if(e.kind==kEventVehicleTable) { WorldVehicleTableEvent(e); continue; }   // (2.2: the same vehicle table?)
    if(e.kind==kEventFire || e.kind==kEventThrow || e.kind==kEventFireRelease) {
      // Played in order by TryRemoteShots (each update), retried while refused.
      if(!share_shooting || !avatar) continue;
      Remote::Shot shot; shot.kind=e.kind; shot.weapon=e.a[0]; shot.at=now;
      if(e.kind==kEventFire && (e.a[4]&1)) {
        std::memcpy(shot.hit,&e.a[1],12);
        shot.has_hit=std::isfinite(shot.hit[0]) && std::isfinite(shot.hit[1]) && std::isfinite(shot.hit[2]);
      }
      if(e.kind==kEventThrow) shot.strength=std::isfinite(e.f) && e.f>0.0f && e.f<1000.0f ? e.f : 1.0f;
      if(r.shots.size()<64) r.shots.push_back(shot); else { ++shots_refused; ++shots_held[kHeldOther]; }
      continue;
    }
    if(e.kind==kEventEnterCar || e.kind==kEventExitCar) {
      if(!avatar || !(api->read_u32(avatar+216)&0x4000)) continue;
      if(e.kind==kEventExitCar) {
        const uint32_t order=scratch.data()+1760;
        api->write_u32(order,avatar);
        for(unsigned i=0;i<8;++i) api->write_u32(order+4+i*4,e.a[i]);
        scratch.ctx.r3.u64=order;
        ++replicated_depth; scratch.Call(0x8257B8E0); --replicated_depth;
        r.exit_hold_until=now+4000;   // the pose-driven exit waits for this one
        r.motion.car_anim_until=now+2200;
        ProbeLog((std::string(r.tag)+" gets out (their exit played here)").c_str());
        continue;
      }
      // The car by its owner's slot (a[1]) and handle there (a[0]).
      const uint32_t car=FindCarToUse(scratch,e.a[1],e.a[0],now);
      if(!car) { ProbeLog((std::string(r.tag)+" gets in a car not shown here yet (placed when it is)").c_str()); continue; }
      // Their getting in is shown with their own moves (door, pulling the
      // driver out, climbing in: sent as car actions) and the character is
      // put in the seat once they sit. Replaying the request here stopped at
      // the door or gave up for this character.
      const uint32_t car_handle=api->read_u32(car+68);
      r.enter_car=car_handle; r.enter_until=now+8000; r.enter_time=now;
      if(int32_t(e.a[3])<=0 && !(api->read_u32(player+2496)==car_handle && api->read_u32(player+2500)==0))
        if(const uint32_t occ=SeatedIn(car_handle,0)) if(occ!=api->read_u32(avatar+68) && occ!=api->read_u32(player+68) && !IsAvatarHandle(occ)) if(ResolveHuman(occ)) {
          // The driver waits for the other player's pull-out: its own "being
          // pulled out" move is sent and played on it, then it gets out.
          // (Calling the game's pull-out 82575438 here removed the whole car;
          // an exit order was ignored: the driver stayed in and was removed.)
          r.pending_victim=occ; r.victim_deadline=now+4000; r.victim_exit_at=0;
          ProbeLog("Other player takes a car with a driver: driver waits for the pull-out");
        }
      char line[160]; std::snprintf(line,sizeof(line),"%s gets in %08X (mode %u, seat %d): their moves shown, seated when they sit",r.tag,car_handle,e.a[2],int32_t(e.a[3]));
      ProbeLog(line);
      continue;
    }
    if(e.kind==kEventPedDamage || e.kind==kEventPlayerHitByPed || e.kind==kEventCrewDamage) {
      if(!std::isfinite(e.f) || e.f<-2 || e.f>100000) continue;
      uint32_t victim=0, attacker=0;
      if(e.kind==kEventPedDamage) {   // host: a client hit one of our people
        if(!hosting.load() || !(IsSentPed(e.a[0]) || WorldSends(e.a[0],true))) continue;   // (2.2: a crew member of the world stream by its id too)
        victim=ResolveHuman(e.a[0]); attacker=avatar;
      } else if(e.kind==kEventCrewDamage) {   // host: a client hit a crew member of our traffic, named by its seat in our car (2.1.1)
        if(!hosting.load()) continue;
        const uint32_t crew_car=IsSentCar(e.a[0]) ? FindCar(0,e.a[0]) : 0, seat=(e.a[7]>>16)&3;
        if(!crew_car) { ++crew_hits_nocar; continue; }
        // 2.1.4: who sits there by their own +2496/+2500 (SeatOccupant), as
        // CaptureTraffic names them: the car's seat slots are empty for most
        // AI crews, so every one of these hits was dropped here (352 in a test).
        victim=ResolveHuman(SeatOccupantMemo(crew_car,seat,now)); attacker=avatar;
        if(!victim) {
          ++crew_hits_empty;
          static unsigned logged=0;
          if(logged++<20) { char line[160]; std::snprintf(line,sizeof(line),"%s hit the crew in seat %u of car %08X: no one in that seat here (slot %08X)",r.tag,seat,e.a[0],api->read_u32(crew_car+52320+seat*108)); ProbeLog(line); }
          continue;
        }
        if(victim==player || AvatarSlotOf(victim)>=0) { ++crew_hits_player; continue; }   // (a player sits there: their own hit path)
      } else {                        // client: one of the host's people hit us
        if(hosting.load() || !from_host || !Readable(player,4488)) continue;
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
      if(ok) { ++ped_hits_applied; if(e.kind==kEventCrewDamage) ++crew_hits_applied; }
      // The client's punch lands here as damage only: the person didn't
      // flinch on the host (nor, as the host decides reactions, on the
      // client). Play the game's own hit reaction from the client's side when
      // close enough for a melee hit; it goes to the clients like any other.
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
        char line[160];
        if(e.kind==kEventCrewDamage) std::snprintf(line,sizeof(line),"%s hit the crew in seat %u of car %08X: %.1f damage%s",r.tag,(e.a[7]>>16)&3,e.a[0],e.f,attacker?"":" (attacker not here)");
        else std::snprintf(line,sizeof(line),e.kind==kEventPedDamage?"%s hit person %08X: %.1f damage%s":"%s's person %08X hit us: %.1f damage%s",
                           r.tag,e.a[0],e.f,attacker?"":" (attacker not here)");
        ProbeLog(line);
      }
      continue;
    }
    if(e.kind==kEventPedEffect) {
      if(from_host) joiner_effects[e.a[0]]=PedEffect{e.a[1],e.a[2],e.a[3],e.a[4],now};
      continue;
    }
    if(e.kind==kEventHostModels) {
      if(!from_host) continue;
      std::vector<uint32_t> ids;
      const unsigned count=std::min<unsigned>(e.a[0],14);
      for(unsigned i=0;i<count;++i) ids.push_back((e.a[1+i/2]>>((i&1)?0:16))&0xFFFF);
      host_models.swap(ids); host_models_time=now;
      continue;
    }
    if(e.kind==kEventVictimExit) {
      uint32_t victim=r.pending_victim ? ResolveHuman(r.pending_victim) : 0;
      if(!victim && r.enter_car) {
        const uint32_t occ=SeatedIn(r.enter_car,0);
        if(occ && !IsAvatarHandle(occ) && occ!=api->read_u32(player+68)) victim=ResolveHuman(occ);
      }
      if(victim) {
        const uint32_t order=scratch.data()+1760;
        api->write_u32(order,victim);
        for(unsigned i=0;i<8;++i) api->write_u32(order+4+i*4,e.a[i]);
        scratch.ctx.r3.u64=order;
        ++replicated_depth; scratch.Call(0x8257B8E0); --replicated_depth;
        r.pending_victim=0;
      }
      ProbeLog(victim?"Pulled-out driver thrown out here too (their exit)":"Pulled-out driver's exit: no driver here");
      continue;
    }
    if(e.kind==kEventVictimAction) {
      const uint32_t car=FindCar(e.a[1],e.a[0]);
      uint32_t victim=r.pending_victim ? ResolveHuman(r.pending_victim) : 0;
      if(!victim && car) {
        const uint32_t occ=SeatedIn(api->read_u32(car+68),0);
        if(occ && !IsAvatarHandle(occ) && occ!=api->read_u32(player+68)) victim=ResolveHuman(occ);
      }
      bool played=false;
      if(victim) {
        scratch.ctx.r3.u64=victim; scratch.ctx.r4.u64=e.a[2]; scratch.ctx.r5.u64=e.a[3]; scratch.ctx.r6.u64=e.a[4]; scratch.ctx.r7.u64=e.a[5];
        float f[2]; for(int i=0;i<2;++i) std::memcpy(&f[i],&e.a[6+i],4);
        scratch.ctx.f1.f64=std::isfinite(f[0])?f[0]:0; scratch.ctx.f2.f64=std::isfinite(f[1])?f[1]:0; scratch.ctx.f3.f64=0;
        ++replicated_depth; played=scratch.Call(0x82444F88) && scratch.ctx.r3.u32; --replicated_depth;
        if(r.pending_victim && ResolveHuman(r.pending_victim)==victim && !r.victim_exit_at) r.victim_exit_at=now+2500;  // (their exit normally comes first)
      }
      static unsigned logged=0;
      if(logged++<20) { char line[120]; std::snprintf(line,sizeof(line),"Pulled-out driver's move %02X: %s",e.a[2],victim?(played?"playing":"refused"):"no driver here"); ProbeLog(line); }
      continue;
    }
    if(e.kind==kEventCarDoor) {
      const uint32_t car=FindCar(e.a[1],e.a[0]);
      if(car && e.a[2]<16) {
        scratch.ctx.r3.u64=car; scratch.ctx.r4.u64=e.a[2];
        float f[3]; for(int i=0;i<3;++i) std::memcpy(&f[i],&e.a[3+i],4);
        scratch.ctx.f1.f64=std::isfinite(f[0])?f[0]:0; scratch.ctx.f2.f64=std::isfinite(f[1])?f[1]:0; scratch.ctx.f3.f64=std::isfinite(f[2])?f[2]:0;
        ++replicated_depth; scratch.Call(0x825352E0); --replicated_depth;
      }
      static unsigned logged=0;
      if(logged++<20) { char line[120]; std::snprintf(line,sizeof(line),"Car door %u of %s's car: %s",e.a[2],r.tag,!car?"car not here":e.a[2]<16?"moved":"not applied (door 16 or over)"); ProbeLog(line); }
      continue;
    }
    AvatarMotion& m=r.motion;
    // 2.1: while their character lies or gets up here (its own get-up, see
    // SyncKnockdown) nothing of theirs is replayed on it: a state set mid-
    // ragdoll left it ragdolled in state 0 for good, and their get-up's states
    // (8/16-17) on a character that never got up itself sent it to (0,0,0).
    const bool down=avatar && (api->read_u32(avatar+780) || api->read_u32(avatar+516)==8 || now<r.getup_until);
    if(e.kind==kEventCarAction && avatar && down) { ++replayed_actions_dropped; continue; }   // (UpdateRemoteCar seats it anyway)
    if(e.kind==kEventCarAction && avatar && (api->read_u32(avatar+216)&0x4000)) {
      scratch.ctx.r3.u64=avatar; scratch.ctx.r4.u64=e.a[0]; scratch.ctx.r5.u64=e.a[1];
      scratch.ctx.r6.u64=e.a[2]; scratch.ctx.r7.u64=e.a[3];
      UnpackActionFloats(scratch,e);
      ++replicated_depth;
      const bool played=scratch.Call(0x82444F88) && scratch.ctx.r3.u32;
      --replicated_depth;
      m.action_until=std::max(m.action_until,now+2500);   // no move orders over it
      static unsigned logged=0;
      if(logged++<40) { char line[120]; std::snprintf(line,sizeof(line),"Car action %02X on %s: %s",e.a[0],r.tag,played?"playing":"refused"); ProbeLog(line); }
      continue;
    }
    if(e.kind==kEventAction && avatar && (api->read_u32(avatar+216)&0x4000)) {
      // Their get-up animations (0xE2 / 0xE0 back / front, 0x232; only
      // 824517F8 plays them, from the get-up) never: the character plays its
      // own. Nothing while it lies or gets up (not kept for later either).
      if(e.a[0]==0xE0 || e.a[0]==0xE2 || e.a[0]==0x232 || down) { ++replayed_actions_dropped; continue; }
      scratch.ctx.r3.u64=avatar; scratch.ctx.r4.u64=e.a[0]; scratch.ctx.r5.u64=e.a[1];
      scratch.ctx.r6.u64=e.a[2]; scratch.ctx.r7.u64=e.a[3];
      UnpackActionFloats(scratch,e);
      ++replicated_depth;
      const bool played=scratch.Call(0x82444F88) && scratch.ctx.r3.u32;
      --replicated_depth;
      if(played) ++actions_played;
      else { r.pending_action=e; r.pending_action_until=now+400; }
      if(!m.jumping) m.action_until=std::max(m.action_until,now+600);
      static unsigned logged=0;
      if(logged++<40) {
        char line[120];
        std::snprintf(line,sizeof(line),"Action %02X on %s: %s",e.a[0],r.tag,played?"playing":"refused");
        ProbeLog(line);
      }
    } else if(e.kind==kEventCall && avatar && (api->read_u32(avatar+216)&0x4000) &&
              (e.a[0]==0x82459430u || e.a[0]==0x824497F0u || e.a[0]==0x82458EB8u || e.a[0]==0x82441308u)) {
      // (Only the calls QueueCall sends: punch, knockdown, melee attack,
      // movement state. Another game can't make this one call anything else.)
      // 2.1: their knockdown and state 8 (only ever 8/15-17: knocked down,
      // getting up) never; the pose's ragdoll bit drives both (SyncKnockdown).
      // Nothing while the character lies or gets up here.
      if(e.a[0]==0x824497F0u || (e.a[0]==0x82441308u && e.a[1]==8) || down) { ++replayed_calls_dropped; continue; }
      // Arguments: an offset into the sender's own player (bit i: their
      // character here), into a player character in the sender's game (bit
      // 4+i, slot in bits 8+4i: this game's player when it is us, else that
      // player's character here), or a plain value.
      uint64_t a[4]; bool missing=false;
      for(int i=0;i<4;++i) {
        const uint32_t v=e.a[1+i];
        if((e.a[7]>>i)&1) a[i]=uint64_t(avatar+v);
        else if((e.a[7]>>(4+i))&1) {
          const int s=int((e.a[7]>>(8+4*i))&15);
          const uint32_t base=s==me ? player : AvatarObjOf(s);
          if(!base) missing=true;
          a[i]=uint64_t(base+v);
        } else a[i]=uint64_t(v);
      }
      if(missing) continue;   // the other character it names isn't here
      // A punch is refused while the character's current action (+756, an
      // index into the action table) is marked busy, e.g. a walk/run cycle
      // from a move order. The original was allowed to punch, so clear it.
      const uint32_t busy_action=api->read_u32(avatar+756);
      if(e.a[0]==0x82459430) api->write_u32(avatar+756,0xFFFFFFFF);
      scratch.ctx.r3.u64=avatar; scratch.ctx.r4.u64=a[0]; scratch.ctx.r5.u64=a[1];
      scratch.ctx.r6.u64=a[2]; scratch.ctx.r7.u64=a[3];
      ++replicated_depth;
      const bool ok=scratch.Call(e.a[0]);
      --replicated_depth;
      if(e.a[0]==0x82441308 && e.a[1]==4) {
        if(e.a[2]==1) { m.jumping=true; m.action_until=now+3000; }   // take-off: until landing
        else if(e.a[2]==0xE) { // landed: hand back quickly, with a fresh move order (the jump left
          m.jumping=false; m.action_until=now+120;  // the old one stale: sliding, walking on the spot)
          m.reorder=true;
        }
      } else if(e.a[0]==0x82441308 && e.a[1]==3) m.car_anim_until=now+4000; // getting in / out of a car (state 3)
      else if(e.a[0]==0x82441308 && !m.jumping) {
        // Climbing and other movement states: let the animation carry the
        // character (positions still follow the path) until the next state.
        // 0/1: back to normal movement; 5 climbing, 6 dropping/landing,
        // others: hold until the next state arrives (at most 3 s).
        // 6/E is the landing after a climb or drop (like 4/E after a jump):
        // hand back at once, it held the character for 3 s after the climb.
        // (state 1 also comes up in the middle of a climb, between the climb
        // and the drop: not an end then.)
        const bool normal=(e.a[1]==0) || (e.a[1]==1 && !m.climbing) || (e.a[1]==6 && e.a[2]==0xE);
        if(!(e.a[1]==1 && m.climbing)) {
          // (2.1.4: no 120 ms hold for the owner's own 82441308(0/1, 0) on a
          // character moved as a player: it is in state 1 by itself and the
          // hold only skipped DriveAsPlayer for 120 ms at each of them)
          if(!(normal && walk_as_player && IsPlayerObject(avatar))) m.action_until=now+(normal ? 120 : 3000);
          if(normal) m.reorder=true;
          m.climbing=e.a[1]==5;
        }
        // An action the character refused just before (the sender played it
        // a moment before this state change): play it now that the state fits.
        if(r.pending_action.kind && now<r.pending_action_until) {
          const Event pa=r.pending_action; r.pending_action.kind=0;
          scratch.ctx.r3.u64=avatar; scratch.ctx.r4.u64=pa.a[0]; scratch.ctx.r5.u64=pa.a[1];
          scratch.ctx.r6.u64=pa.a[2]; scratch.ctx.r7.u64=pa.a[3];
          UnpackActionFloats(scratch,pa);
          ++replicated_depth;
          const bool played=scratch.Call(0x82444F88) && scratch.ctx.r3.u32;
          --replicated_depth;
          static unsigned logged=0;
          if(logged++<30) { char line[120]; std::snprintf(line,sizeof(line),"Action %02X on %s, after its state change: %s",pa.a[0],r.tag,played?"playing":"refused"); ProbeLog(line); }
        }
      }
      else if(!m.jumping) m.action_until=std::max(m.action_until,now+600);
      static unsigned logged=0;
      if(logged++<60) {
        char line[160];
        std::snprintf(line,sizeof(line),"Call %08X(%08X %08X %08X %08X) on %s: %s, r3=%X, action was %X",e.a[0],e.a[1],e.a[2],e.a[3],e.a[4],r.tag,ok?"done":"failed",scratch.ctx.r3.u32,busy_action);
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
      std::snprintf(line,sizeof(line),"Hit by %s: %.1f damage",r.tag,e.f);
      ProbeLog(line);
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
  // (the first other player's character)
  uint32_t other=0;
  for(int s=0;s<kSlots && !other;++s) other=AvatarObjOf(s);
  const uint32_t objs[3]={api->read_u32(0x8309ABEC), other, trace_driver ? ResolveHuman(trace_driver) : 0};
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
  const uint64_t now=MonoMs();
  for(Remote& r:remotes) {
    if(!r.avatar || !r.on_foot) continue;
    const uint32_t obj=ResolveHuman(r.avatar);
    if(!obj || !r.follow.count || r.follow.offset==INT64_MAX) continue;
    if(now<r.motion.action_until) continue;  // jumps, punches: the animation owns it
    float pos[3], rows[9], vel[3];
    r.follow.Sample(uint32_t(int64_t(now)-r.follow.offset-r.follow.delay),pos,rows,vel);
    // Only small corrections: a big difference means a teleport or a
    // transition the update hook handles.
    const float dx=pos[0]-api->read_f32(obj+20), dy=pos[1]-api->read_f32(obj+24), dz=pos[2]-api->read_f32(obj+28);
    if(dx*dx+dy*dy+dz*dz>4.0f) continue;
    for(unsigned i=0;i<3;++i) api->write_f32(obj+20+i*4,pos[i]);
    for(unsigned i=0;i<9;++i) api->write_f32(obj+32+i*4,rows[i]);
  }
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
  uint8_t damage;       // CarDamage (bit 0 on fire, 1 smoking, 2 wrecked, 5 heavy smoke) | CarSiren (3 siren lights, 4 siren)
  uint8_t hp;           // CarHealth: hit points out of 255 (2.1)
  uint8_t pad[2];
  uint16_t pass[3];     // passengers in seats 1-3: 1-based model name index, 0 none
  uint16_t teams;       // the teams (+232) of whoever sits in seats 0-3, 4 bits each from bit 0; 0xF unknown (2.1)
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
unsigned response_units_out=0, response_cars_out=0;   // the host's police response sent to its clients (2.1): units on foot, cars (last capture)
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
// A person's team (+232: 0-6, the index set_team 824DB708 stores from
// 8244A6A8's names; 0 Playas ... 5 Police, 6 Civilian), 0xFF for anything else.
uint8_t TeamOf(uint32_t human) { const uint32_t t=api->read_u32(human+232); return t<=6 ? uint8_t(t) : 0xFF; }
// One of this player's followers (homies): player +2096 count, +2100 entries
// of 20 bytes with the handle first (8244C2E8). The minimap draws them purple.
bool PlayerFollower(uint32_t player,uint32_t handle) {
  if(!handle || !Readable(player,4252)) return false;
  const uint32_t n=std::min<uint32_t>(api->read_u32(player+2096),16);
  for(uint32_t i=0;i<n;++i) if(api->read_u32(player+2100+i*20)==handle) return true;
  return false;
}
// 2.1, joiner: whatever shows one of the host's people here (a copy, a
// lookalike or stand-in, one of this game's own taken over, a traffic crew
// member or seat stand-in) gets that person's team there. The minimap draws a
// person's dot from +232 after its own skips (822E8D00: +120 0x20000000 or
// 0x08000000 set or 0x04000000 clear, dead, players, followers): 0 the
// purple "revive" dot, 6 none, 4 red only for the definition at 0x8370D020
// (82114610), any other red; a car by its driver's (822E8E80, +52316).
// Copies took theirs from their model (8243DAE8: definition +108), stand-ins
// from whatever model was loaded (the likely cause of the purple police
// here). Written as set_team does (a plain store). The host's police (LW_* or
// team 5) are logged once each with what shows them here.
unsigned teams_written=0;
std::unordered_set<uint32_t> crews_out;   // joiner: crew members seen out of their seat (10 s window)
void ShowHostTeam(uint32_t obj,uint8_t team,const char* kind,const char* host_model,uint32_t player) {
  if(!obj || !Readable(obj,4252)) return;
  const uint32_t have=api->read_u32(obj+232);
  if(team==5 || !std::strncmp(host_model,"LW_",3)) {
    static std::unordered_set<uint32_t> seen; static unsigned logged=0;
    if(logged<40 && seen.insert(api->read_u32(obj+68)).second) {
      ++logged;
      char mn[46]="?"; ModelName(obj,mn,sizeof(mn));
      char line[260];
      std::snprintf(line,sizeof(line),"Host's police shown here: %s %08X, host's %s team %d, here %s team %d (+224 %d)%s",kind,api->read_u32(obj+68),
                    host_model[0]?host_model:"?",team>6?-1:int(team),mn,int32_t(have),int32_t(api->read_u32(obj+224)),
                    PlayerFollower(player,api->read_u32(obj+68))?", one of this player's followers":"");
      ProbeLog(line);
    }
  }
  if(team<=6 && have!=team) { api->write_u32(obj+232,team); ++teams_written; }
}

// Host: the traffic cars nearest the other players.
// Who sits where: every person's car (+2496) and seat (+2500). The car's
// own seat slots (+52320) were empty for most traffic drivers, so none were
// sent and the other game showed empty cars.
std::unordered_map<uint32_t,std::array<uint32_t,4>> CarOccupants() {
  std::unordered_map<uint32_t,std::array<uint32_t,4>> m;
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t h=TableObject(index,1,4252);
    if(!h) continue;
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
  static uint64_t next=0;
  if(now<next) return;
  next=now+100;
  // v2: around every client (the nearest client counts for each car); 2.2:
  // every client 2.1's streams still go to (none: nothing sent or named as sent).
  const auto clients=ClientPositions();
  if(clients.empty()) { std::lock_guard lock(traffic_mutex); traffic_out.clear(); return; }
  // Cars other players sit in or are getting into: their own game drives them.
  std::vector<uint32_t> players_cars;
  for(int s=0;s<kSlots;++s) if(const uint32_t c=AvatarCar(s)) players_cars.push_back(c);
  for(const Remote& r:remotes) if(r.enter_car && now<r.enter_until) players_cars.push_back(r.enter_car);
  auto players_car=[&](uint32_t h) { return IsOtherGamesCopy(h) || std::find(players_cars.begin(),players_cars.end(),h)!=players_cars.end(); };
  // 1.61: the seat scan (all 4096 objects) ran every frame before the 10 Hz
  // check above - most of the host's mod time, growing with fps (desync).
  const auto occupants=CarOccupants();
  auto seated=[&](uint32_t v,unsigned seat)->uint32_t {
    auto it=occupants.find(api->read_u32(v+68));
    if(it!=occupants.end() && it->second[seat]) return it->second[seat];
    return ResolveHuman(OccupantHandle(v,seat));
  };
  struct Found { float d2; TrafficCar car; };
  std::vector<Found> found;
  std::unordered_set<uint32_t> found_ids;  // 1.62: was a scan of 'found' for every object
  const uint32_t own=api->read_u32(player+2496);
  auto describe=[&](uint32_t v,uint32_t handle,int32_t type,TrafficCar& c) {   // the car itself
    c.id=OwnId(handle); c.type=int16_t(type); c.damage=CarDamage(v)|CarSiren(v); c.hp=CarHealth(v); c.teams=0xFFFF;
    for(unsigned i=0;i<3;++i) c.pos[i]=api->read_f32(v+20+i*4);
    for(unsigned i=0;i<9;++i) c.rows[i]=api->read_f32(v+32+i*4);
    if(const uint32_t vel=VehicleVelocityAddress(v))
      for(unsigned i=0;i<3;++i) { c.vel[i]=api->read_f32(vel+i*4); c.ang[i]=api->read_f32(vel+16+i*4); }
  };
  auto crew=[&](uint32_t v,TrafficCar& c) {   // who sits in it: the seats' teams, the driver's and passengers' models
    for(unsigned seat=0;seat<4;++seat) if(const uint32_t who=seated(v,seat)) c.teams=uint16_t((c.teams&~(0xFu<<(seat*4)))|((TeamOf(who)&0xFu)<<(seat*4)));
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
  };
  const uint32_t head=api->read_u32(kTrafficList);
  uint32_t node=head;
  // The list is circular: it ends when it comes back to the first node.
  for(unsigned guard=0;node && guard<64 && Readable(node,12);++guard) {
    const uint32_t handle=api->read_u32(node+8);
    node=api->read_u32(node);
    if(node==head) node=0;
    if(handle==own || players_car(handle)) continue;   // a player's car (or one they get into)
    const uint32_t v=ResolveVehicle(handle);
    if(!v) continue;
    const int32_t type=VehicleType(v);
    if(type<0) continue;
    TrafficCar c{};
    describe(v,handle,type,c);
    const float d2=NearestD2(clients,c.pos[0],c.pos[2]);
    if(d2>250.0f*250.0f) continue;
    crew(v,c);
    found.push_back({d2,c}); found_ids.insert(handle);
  }
  // 2.1: the police response's cars with their crews (the 12 groups' squad
  // cars, OwnResponseUnits, and any car a response unit drives): 82411118
  // makes them off the traffic list, and the table scan below is off during
  // a mission, so a client the response is after saw none of them come.
  response_cars_out=0;
  if(hosting.load() && share_police) {
    std::vector<uint32_t> units, cars;
    OwnResponseUnits(&units,&cars);
    for(uint32_t h:units) if(const uint32_t o=ResolveHuman(h)) if(const uint32_t v=ResolveVehicle(api->read_u32(o+2496))) if(api->read_u32(o+2500)==0 || OccupantHandle(v,0)==h) cars.push_back(api->read_u32(v+68));   // (its own seat +2500: the slots are empty for most AI crews)
    for(uint32_t handle:cars) {
      if(handle==own || players_car(handle) || found_ids.count(handle)) continue;
      const uint32_t v=ResolveVehicle(handle);
      if(!v) continue;
      const int32_t type=VehicleType(v);
      if(type<0) continue;
      TrafficCar c{};
      describe(v,handle,type,c);
      const float d2=NearestD2(clients,c.pos[0],c.pos[2]);
      if(d2>250.0f*250.0f) continue;
      crew(v,c);
      found.push_back({d2,c}); found_ids.insert(handle); ++response_cars_out;
    }
  }
  // Every other car near the joiner as well (parked ones aren't in the
  // traffic list: they were missing or a different car on the joiner's
  // side). Not during a mission: its cars come from each game's own copy of
  // the mission.
  if(!MissionActive()) {
    for(uint32_t index=0;index<4096;++index) {
      const uint32_t v=TableObject(index,5,kVehicleSize);
      if(!v) continue;
      const uint32_t handle=api->read_u32(v+68);
      if(handle==own || players_car(handle)) continue;
      if(found_ids.count(handle)) continue;
      const int32_t type=VehicleType(v);
      if(type<0) continue;
      TrafficCar c{};
      describe(v,handle,type,c);
      const float d2=NearestD2(clients,c.pos[0],c.pos[2]);
      if(d2>200.0f*200.0f) continue;
      found.push_back({d2*1.5f,c});  // traffic first when there are too many
    }
  }
  std::sort(found.begin(),found.end(),[](const Found& a,const Found& b){return a.d2<b.d2;});
  if(found.size()>kMaxShared) found.resize(kMaxShared);
  for(auto& f:found) if(const uint32_t v=ResolveVehicle(f.car.id)) ShareCarLook(scratch,v,now);
  // This game's cars another player sits in (left out of the traffic sent)
  // keep their paint going out: nobody else sends it.
  std::vector<uint32_t> ridden;
  for(uint32_t h:players_cars) if(h && !IsOtherGamesCopy(h)) if(const uint32_t v=ResolveVehicle(h)) { ShareCarLook(scratch,v,now); ridden.push_back(h); }
  {
    // Only the cars still sent keep their looks in the outgoing set.
    std::lock_guard lock(car_look_mutex);
    const uint32_t own_car=api->read_u32(player+2496);
    for(auto it=car_looks_out.begin();it!=car_looks_out.end();)
      it=(it->first==own_car || std::find(ridden.begin(),ridden.end(),it->first)!=ridden.end() ||
          std::any_of(found.begin(),found.end(),[&](const Found& f){return f.car.id==it->first;})) ? std::next(it) : car_looks_out.erase(it);
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
  uint8_t damage=0, hp=255; uint64_t next_damage=0, made_at=0;
};
std::unordered_map<uint32_t,SharedCar> shared_cars; // client, game thread
bool traffic_shared=false;
// v2 (research S 1c): while the host's traffic is shown, this game's traffic
// spawner is switched off the way spawning_vehicles_do(false) does it (byte
// 0x8309A35D: no spawn and no despawn pass), and its parked-car spawns
// (8241E378) make nothing. Density 0 and the removals stay as a backstop.
constexpr uint32_t kTrafficSpawning=0x8309A35D;
bool own_traffic_off=false;            // this game's spawner byte was on and we turned it off
std::atomic<bool> block_parked{false};  // parked-car spawns refused (game thread sets it)
std::atomic<uint32_t> parked_refused{0}, gang_skipped{0}, police_skipped{0}, roadblocks_skipped{0};
// *0x8309ABEC is obj for one game call (0: left alone), back on every path.
// Under a swap the character is what the game calls the player: nothing run
// there may tell the two apart by *0x8309ABEC (ResolveHuman, AvatarObjOf,
// AvatarSlotOf, IsSentPed, OurCharacter...).
struct PlayerSwap {
  const bool on; uint32_t was=0;
  explicit PlayerSwap(uint32_t obj) : on(obj!=0) { if(on) { was=api->read_u32(0x8309ABEC); api->write_u32(0x8309ABEC,obj); } }
  ~PlayerSwap() { if(on) api->write_u32(0x8309ABEC,was); }
  PlayerSwap(const PlayerSwap&)=delete; PlayerSwap& operator=(const PlayerSwap&)=delete;
};
// 2.2 Package B: a parked pass run for a member's viewpoint (ParkedPassHook)
// swaps *0x8309ABEC to a stand-in holding the member's position; the spawn
// (8241E378: the vehicle constructor, seats, model references) runs with the
// real player.
thread_local int parked_member_depth=0;
thread_local uint32_t parked_real_player=0;
WmlGuestFunction original_parked_spawn=nullptr;
void ParkedSpawnHook(WmlContext* c,uint8_t* b) {
  if(block_parked.load() && connected.load()) { ++parked_refused; api->set_r(c,3,0); return; }
  PlayerSwap unswap(parked_member_depth ? parked_real_player : 0);
  original_parked_spawn(c,b);
}
// 82569700(source, 2|1, info, pos, rows, ...) -> handle: every vehicle. A car a
// script or cheat makes for this player on a client is remembered (at most 8,
// the oldest forgotten) so the own-car removals leave it be.
WmlGuestFunction original_vehicle_create=nullptr;
void WorldMadeForCar(uint32_t handle,uint32_t source);   // (Package B: made for a member's viewpoint, further down)
void VehicleCreateHook(WmlContext* c,uint8_t* b) {
  const uint32_t source=uint32_t(api->get_r(c,3)), info=uint32_t(api->get_r(c,5));
  original_vehicle_create(c,b);
  WorldCarMade(uint32_t(api->get_r(c,3)),source);   // (into the world registry, 2.2)
  WorldMadeForCar(uint32_t(api->get_r(c,3)),source);
  if(!connected.load() || hosting.load() || making_car || (source!=7 && source!=3)) return;
  const uint32_t handle=uint32_t(api->get_r(c,3));
  if(!ResolveVehicle(handle)) return;
  if(personal_cars.size()>=8) {
    auto oldest=personal_cars.begin();
    for(auto it=personal_cars.begin();it!=personal_cars.end();++it) if(it->second<oldest->second) oldest=it;
    personal_cars.erase(oldest);
  }
  personal_cars[handle]=MonoMs();
  static unsigned logged=0;
  if(logged++<10) { char line[140]; std::snprintf(line,sizeof(line),"Car %08X (type %u) made here by a script or cheat (source %u): this player's own, kept",handle,info,source); ProbeLog(line); }
}
float saved_traffic_density=-1;
unsigned car_looks_applied=0, drivers_removed=0, passengers_made=0, drivers_made=0, drivers_seat_refused=0, drivers_loading=0, cars_with_driver_in=0;
std::unordered_map<uint32_t,uint64_t> quick_gone; // joiner: host car id -> not made again until
std::vector<std::pair<uint32_t,uint64_t>> orphan_drivers; // joiner: pulled-out drivers to remove
unsigned shared_snaps=0, shared_made=0, shared_removed=0, own_removed=0, gone_count=0, stale_count=0, far_count=0;

extern int mod_destroying;
void DestroyObject(GuestScratch& scratch,uint32_t handle) {
  ReleaseActorModel(scratch,handle);   // (a character's model reference; nothing for a car)
  scratch.ctx.r3.u64=handle;
  ++mod_destroying; scratch.Call(0x823ACF50); --mod_destroying;
}
bool IsSharedCarHandle(uint32_t handle) {
  if(!handle) return false;
  if(IsCarCopyHandle(handle) || WorldCarId(handle)) return true;
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

// Client: a player got into one of the host's traffic cars that is shown
// here as a copy: that copy becomes their car here (its driver is pulled out
// by our copy of that player, as in their game). It is kept as a car copy
// (owner: the host) and stops following the host's traffic.
uint32_t SharedCarLocal(uint32_t host_id) {
  auto it=shared_cars.find(host_id);
  return it==shared_cars.end() ? ResolveVehicle(WorldCarLocal(host_id)) : ResolveVehicle(it->second.car);
}
uint32_t AdoptSharedCar(GuestScratch& scratch,uint32_t host_id,uint64_t now) {
  (void)scratch;
  auto it=shared_cars.find(host_id);
  if(it==shared_cars.end() || !ResolveVehicle(it->second.car)) return 0;
  CarCopy& c=car_copies[CarKey(0,host_id)];
  c=CarCopy{}; c.local=it->second.car; c.type=it->second.type; c.used=now; c.applied_look=it->second.applied_look;
  if(it->second.driver) orphan_drivers.push_back({it->second.driver,now+4000});
  for(uint32_t h:it->second.pass) if(h) orphan_drivers.push_back({h,now+4000});
  shared_cars.erase(it);
  ProbeLog("Remote vehicle: a player took a car of the host's shown here; using that one");
  return ResolveVehicle(c.local);
}
uint32_t SharedCarHostHandle(uint32_t local) {
  if(!local) return 0;
  for(auto& [id,sc]:shared_cars) if(sc.car==local) return id;
  return WorldCarId(local);
}
// 825324D0(car): the car's own removal (vtable, from 823ACF50). It takes
// everyone off the car's list of people getting in or out (+52132 count, at
// most 8; 20-byte entries from +52136, the person first; +2569 bit 0x10 set
// while listed) with 8257C400(person, 19), which takes them off the list of
// the car their +2496 names. Someone on this list whose +2496 names another
// car or none (or who is no longer a person) never came off and the removal
// looped for good: the joiner froze inside 823ACF50 while shared traffic
// removed a car copy (1.63). Such entries are taken off first, the way
// 82556FE8 does, and logged with who they were.
WmlGuestFunction original_vehicle_remove=nullptr;
std::string RiderText(uint32_t h) {
  char line[160];
  if(!h || !Readable(h,4252) || api->read_u32(h+72)!=1) { std::snprintf(line,sizeof(line),"not a person (%08X)",h); return line; }
  const uint32_t handle=api->read_u32(h+68), named=api->read_u32(h+2496);
  const char* who="one of this game's own people";
  if(h==api->read_u32(0x8309ABEC)) who="this player";
  else if(IsAvatarHandle(handle)) who="another player's character";
  else if(h==player_copy || handle==test_local || handle==replica_local) who="a test character";
  else if(std::any_of(remotes.begin(),remotes.end(),[&](const Remote& r){return r.pending_victim==handle;})) who="the driver another player pulls out";
  else if(std::any_of(orphan_drivers.begin(),orphan_drivers.end(),[&](const auto& o){return o.first==handle;})) who="a pulled-out driver";
  else if(SharedPedHostHandle(handle)) who="a copy of the host's people";
  else for(auto& [id,sc]:shared_cars) {
    if(sc.driver==handle) who="a shared car's driver";
    for(uint32_t p:sc.pass) if(p==handle) who="a shared car's passenger";
  }
  std::snprintf(line,sizeof(line),"%s %08X, its car %08X%s",who,handle,named,ResolveVehicle(named)?"":" (none)");
  return line;
}
const char* CarKind(uint32_t handle) {
  return IsCarCopyHandle(handle) ? "another game's car here" : SharedCarHostHandle(handle) ? "a shared car" :
         taken_cars.count(handle) ? "a car a player took" : "this game's own car";
}
bool ListedBy(uint32_t vehicle,uint32_t human) {
  const int32_t n=int32_t(api->read_u32(vehicle+52132));
  for(int32_t i=0;i<n && i<8;++i) if(api->read_u32(vehicle+52136+uint32_t(i)*20)==human) return true;
  return false;
}
// mod.ini debug_cars = 1: finds what leaves the entries VehicleRemoveHook
// takes off. People removed while getting in or out are checked (in
// HumanDestroyHook), and every 250 ms so are the lists of the cars co-op
// handles; each wrong entry is logged once, when it first shows.
bool debug_cars=false;
void CarCheckLog(const std::string& what,uint32_t lr) {
  static unsigned logged=0;
  if(logged++>=60) return;
  char line[480];
  std::snprintf(line,sizeof(line),"Car list check: %s (from %08X%s, stage %s)",what.c_str(),lr,mod_destroying?", co-op":"",mod_stage.load());
  ProbeLog(line);
}
void AuditCarLists(uint32_t player,uint64_t now) {
  static uint64_t next=0;
  if(now<next) return;
  next=now+250;
  std::vector<uint32_t> cars{api->read_u32(player+2496)};
  for(auto& [key,c]:car_copies) cars.push_back(c.local);
  for(int s=0;s<kSlots;++s) if(const uint32_t c=AvatarCar(s)) cars.push_back(c);
  for(auto& [id,sc]:shared_cars) cars.push_back(sc.car);
  for(auto& [local,host]:taken_cars) cars.push_back(local);
  static std::unordered_set<uint64_t> reported;
  if(reported.size()>256) reported.clear();
  for(uint32_t handle:cars) {
    const uint32_t v=ResolveVehicle(handle);
    if(!v) continue;
    const int32_t n=int32_t(api->read_u32(v+52132));
    for(int32_t i=0;i<n && i<8;++i) {
      const uint32_t h=api->read_u32(v+52136+uint32_t(i)*20);
      if(h && Readable(h,4252) && api->read_u32(h+72)==1 && ResolveVehicle(api->read_u32(h+2496))==v) continue;
      if(!reported.insert((uint64_t(handle)<<32)|h).second) continue;
      char what[400];
      std::snprintf(what,sizeof(what),"%08X (%s) lists %s as getting in or out (entry %d of %d)",handle,CarKind(handle),RiderText(h).c_str(),i+1,n);
      CarCheckLog(what,0);
    }
  }
}
void VehicleRemoveHook(WmlContext* c,uint8_t* b) {
  const uint32_t v=uint32_t(api->get_r(c,3)), list=v+52132;
  int32_t n=Readable(v,kVehicleSize) && api->read_u32(v+72)==5 ? int32_t(api->read_u32(list)) : 0;
  static unsigned logged=0;
  if(n>8) {
    if(logged++<30) { char line[120]; std::snprintf(line,sizeof(line),"Car removal: %08X lists %d people (at most 8), left as it is",api->read_u32(v+68),n); ProbeLog(line); }
    n=0;
  }
  for(int32_t i=0;i<n;) {
    const uint32_t h=api->read_u32(list+4+uint32_t(i)*20);
    const bool person=h && Readable(h,4252) && api->read_u32(h+72)==1;
    const uint32_t named=person ? ResolveVehicle(api->read_u32(h+2496)) : 0;
    if(person && named==v) { ++i; continue; }
    if(logged++<30) {
      const uint32_t handle=api->read_u32(v+68);
      char line[400];
      std::snprintf(line,sizeof(line),"Car removal: %08X (%s, from %08X%s) still listed %s; taken off (the removal froze on this)",
                    handle,CarKind(handle),uint32_t(api->get_lr(c)),mod_destroying?", removed by co-op":"",RiderText(h).c_str());
      ProbeLog(line);
    }
    for(int32_t k=i;k+1<n;++k)
      for(uint32_t w=0;w<20;w+=4) api->write_u32(list+4+uint32_t(k)*20+w,api->read_u32(list+4+uint32_t(k+1)*20+w));
    api->write_u32(list,uint32_t(--n));
    // On no car's list now: the flag 82556FE8 clears. A person another car
    // lists keeps it.
    if(person && !named) api->write_u8(h+2569,api->read_u8(h+2569)&~0x10);
  }
  if(Readable(v,kVehicleSize) && api->read_u32(v+72)==5) WorldObjectGone(v,true);   // (out of the world registry, 2.2)
  original_vehicle_remove(c,b);
}
// 2.1: co-op's own damage call on one car (the wreck of one of this game's
// cars another player drives): the hooks below let only that car through. A
// global exemption let the whole explosion's area pass hurt every copy here
// (chain wrecks the owners never saw).
uint32_t applying_vehicle=0;
unsigned car_hp_applied=0, car_smoke_failed=0, car_wrecks_applied=0, car_wrecks_quiet=0, car_wrecks_waited=0;
unsigned car_wreck_nophys=0, car_wreck_nodef=0, car_wrecks_refused=0;   // wreck replays put off (tries without physics)
// Per car another game drives here (by handle): when that game's damage was
// last applied (fed: its hit points are that game's, CarHpHook), when it was
// last shown intact (its wreck explodes here only right after), when smoke
// and fire may be tried again, the tries of its wreck, and its tries without
// physics here (4 counted, then it waits quietly for the physics).
struct CarSeen { uint64_t fed=0, intact=0, next_smoke=0, next_wreck=0; unsigned wreck_tries=0, nophys=0; };
std::unordered_map<uint32_t,CarSeen> car_seen;   // game thread
void ForgetCarDamage(uint32_t handle) { car_seen.erase(handle); }   // a copy made again starts over
// Copies wrecked in their owner's game that still wait for their physics here (the Car damage line).
unsigned CarsWaitingForPhysics() { unsigned n=0; for(auto& [h,s]:car_seen) if(s.nophys>=4) if(const uint32_t v=ResolveVehicle(h)) if(!(api->read_u32(v+53976)&0x80000000u)) ++n; return n; }
bool DrivenFromOtherGame(uint32_t vehicle);
// A copy here of another game's car (its owner's game has the real one): a
// copy of another player's car, or on a client a copy of the host's traffic
// (shown or taken over).
bool CarCopyHere(uint32_t handle) {
  return IsOtherGamesCopy(handle) || (!hosting.load() && (SharedCarHostHandle(handle) || taken_cars.count(handle)));
}
// The damage the driving game sends (CarDamage bits, CarHealth), applied to a
// car another game drives here: hit points, smoke and fire, and its wreck.
void ApplyCarDamage(GuestScratch& scratch,uint32_t vehicle,uint8_t want,uint8_t hp) {
  if(!vehicle || !DrivenFromOtherGame(vehicle)) return;
  const uint32_t handle=api->read_u32(vehicle+68);
  const uint64_t now=MonoMs();
  if(car_seen.size()>512) for(auto it=car_seen.begin();it!=car_seen.end();) it=ResolveVehicle(it->first) ? std::next(it) : car_seen.erase(it);
  CarSeen& s=car_seen[handle];
  s.fed=now;
  if(!(want&4)) {
    s.intact=now; s.wreck_tries=0; s.nophys=0; s.next_wreck=0;
    // Hit points: written to the main record (+53504: +4 most, +8 left),
    // never down to 0 (the burn tick or pending damage would then run the
    // destroy 8252BF38 here: explosion, cash, script callbacks).
    const int32_t max=int32_t(api->read_u32(vehicle+53508));
    if(max>0 && CarHealth(vehicle)!=hp) {
      api->write_u32(vehicle+53512,uint32_t(std::max<int64_t>(1,(int64_t(hp)*max+254)/255)));
      ++car_hp_applied;
    }
    // Smoke and fire (not on a car being removed: +124 0x80, which 8253CD70
    // refuses). Heavy smoke (0x20) has no setter: shown as smoke.
    if((CarDamage(vehicle)&3)!=(want&3) && now>=s.next_smoke && !(api->read_u8(vehicle+124)&0x80)) {
      // 8253CD70's r4 sets 0x04000000 (bit 1 here) and r5 0x800 (bit 0): v1
      // passed them the other way round, so a car with only one of the two
      // got the other here and never matched (research report S, 1d).
      const bool burning=CarDamage(vehicle)&1;
      scratch.ctx.r3.u64=handle; scratch.ctx.r4.u64=(want&2)?1:0; scratch.ctx.r5.u64=(want&1)?1:0;
      scratch.Call(0x8253CD70);
      // Refused (its 4 effect slots used up, or the effect isn't there): again
      // in 2 s, not every 250 ms (23 "changes" per 10 s that never showed).
      if((CarDamage(vehicle)&3)!=(want&3)) { ++car_smoke_failed; s.next_smoke=now+2000; }
      else {
        ++car_damage_applied;
        // 8253CD70 sets the fire bit without the burn, and 8252D2A8 gives the
        // burn only with the bit (skipped when it is set): the burn as
        // 8252D2A8 gives it (+54408 = 0.03 of the most a second, +54412 the
        // count), or the car burned for good once this game decides again.
        if((want&1) && !burning && max>0 && api->read_f32(vehicle+54408)==0.0f) {
          api->write_f32(vehicle+54412,api->read_f32(0x820875EC));
          api->write_f32(vehicle+54408,float(max)*api->read_f32(0x8208A044));
        }
      }
    }
    return;
  }
  if(api->read_u32(vehicle+53976)&0x80000000u) return;   // wrecked here already
  if(now<s.next_wreck) return;
  if(CarCopyHere(handle)) {
    // A copy: the game's own MP wreck replay 82542650 (its client 8238BDE0):
    // the wreck bit and the explosion at the car's explosion point (82538318
    // as 8252BF38 finds it), no cash and no script callbacks. Its smoke and
    // fire are stopped first (82542650 clears the bits, not the effects).
    // A copy not shown intact just before (made of a car wrecked before, or
    // without physics until long after the wreck) is wrecked without the
    // explosion (ExplosionHook): it blew up each time it was made, or late
    // next to the player.
    // Without its physics set up (+50300: far off or parked) 82542650 returns
    // at once (its first test), as the game's own MP client 8238BDE0 lets it.
    // 2.1.4: 4 tries (1 s) are counted, then the copy waits quietly (no call,
    // no count: 48-154 counted tries per 10 s all through the second test)
    // and is wrecked without the explosion as soon as its physics is set up.
    if(api->read_u8(vehicle+50300)!=1) {
      if(s.nophys<4) { ++car_wreck_nophys; ++s.nophys; }
      return;
    }
    const uint32_t info=api->read_u32(vehicle+50324);
    const uint32_t def=Readable(info,876) ? api->read_u32(info+872) : 0;
    if(!def) {
      ++car_wreck_nodef;
      static unsigned logged=0;
      if(logged++<10) { char line[120]; std::snprintf(line,sizeof(line),"Car wrecked in the other game: type %d has no explosion here, not wrecked",VehicleType(vehicle)); ProbeLog(line); }
      return;
    }
    const uint32_t rows=scratch.data()+1700, pos=scratch.data()+1736;
    scratch.ctx.r3.u64=rows; scratch.ctx.r4.u64=pos; scratch.ctx.r5.u64=vehicle; scratch.ctx.r6.u64=api->read_u32(vehicle+46512);
    if(!scratch.Call(0x82538318)) {
      for(unsigned i=0;i<9;++i) api->write_f32(rows+i*4,api->read_f32(vehicle+32+i*4));
      for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,api->read_f32(vehicle+20+i*4));
    }
    scratch.ctx.r3.u64=handle; scratch.ctx.r4.u64=0; scratch.ctx.r5.u64=0;
    scratch.Call(0x8253CD70);
    const bool waited=s.nophys>=4, quiet=waited || !s.intact || now-s.intact>1500;
    // 82542650 also throws the car up after the explosion (a random lift on
    // its body, vtable +100), whatever the explosion did: a quiet wreck keeps
    // the motion it had.
    const uint32_t vel=quiet ? VehicleVelocityAddress(vehicle) : 0;
    float motion[6]{};
    if(vel) for(unsigned i=0;i<3;++i) { motion[i]=api->read_f32(vel+i*4); motion[3+i]=api->read_f32(vel+16+i*4); }
    if(quiet) quiet_wreck=vehicle;
    scratch.ctx.r3.u64=def; scratch.ctx.r4.u64=vehicle; scratch.ctx.r5.u64=0; scratch.ctx.r6.u64=rows; scratch.ctx.r7.u64=pos; scratch.ctx.r8.u64=0;
    scratch.Call(0x82542650);
    quiet_wreck=0;
    if(vel && VehicleVelocityAddress(vehicle)==vel) for(unsigned i=0;i<3;++i) { api->write_f32(vel+i*4,motion[i]); api->write_f32(vel+16+i*4,motion[3+i]); }
    if(!(api->read_u32(vehicle+53976)&0x80000000u)) { ++car_wrecks_refused; s.next_wreck=now+(++s.wreck_tries<3 ? 1000 : 30000); return; }
    ++car_wrecks_applied; if(quiet) ++car_wrecks_quiet; if(waited) ++car_wrecks_waited;
    static unsigned logged=0;
    if(logged++<20) ProbeLog(waited?"Car wrecked in the other game while its copy here had no physics: wrecked here without an explosion now that it has":
                             quiet?"Car wrecked in the other game before it was shown here (or replayed late): wrecked here without an explosion":"Car wrecked in the other game: wrecked here too");
    return;
  }
  // One of this game's own cars another player drives: the full destroy
  // through the game's detonation 825397E0 as vehicle_detonate (8252BF38,
  // with the script callbacks a mission car needs). Only this car passes the
  // hooks; the explosion's area pass still spares every other game's car.
  if(CutscenePlaying()) return;   // (825397E0 refuses in a cutscene: tried after it)
  const uint32_t sp=scratch.ctx.r1.u32, pos=scratch.data()+1700, zero=scratch.data()+1720;
  for(unsigned i=0;i<3;++i) { api->write_f32(pos+i*4,api->read_f32(vehicle+20+i*4)); api->write_f32(zero+i*4,0); }
  api->write_u32(sp+84,1); api->write_u32(sp+92,zero); api->write_u8(sp+103,0); api->write_u8(sp+119,0);
  scratch.ctx.r3.u64=vehicle; scratch.ctx.r4.u64=0; scratch.ctx.r5.u64=0; scratch.ctx.r6.u64=0;
  scratch.ctx.r7.u64=pos; scratch.ctx.r8.u64=0; scratch.ctx.r9.u64=0; scratch.ctx.r10.u64=0x7FFFFFFF;
  scratch.ctx.f1.f64=api->read_f32(0x820875F0); scratch.ctx.f2.f64=api->read_f32(0x820875EC);
  const uint32_t was=applying_vehicle; applying_vehicle=vehicle;
  scratch.Call(0x825397E0);
  applying_vehicle=was;
  if(!(api->read_u32(vehicle+53976)&0x80000000u)) {
    // Refused (an invulnerable mission car: 82543240, the +54684 / +120 gate
    // of 8252D2A8): each try still damages every part (825447D0) and ran 4
    // times a second; 3 tries 1 s apart, then every 30 s.
    ++car_wrecks_refused;
    s.next_wreck=now+(++s.wreck_tries<3 ? 1000 : 30000);
    static unsigned logged=0;
    if(s.wreck_tries==3 && logged++<10) {
      char line[160];
      std::snprintf(line,sizeof(line),"Our car wrecked in the game that drives it: refused here 3 times (+53976 %08X, +54684 %08X, +120 %08X), tried every 30 s",
                    api->read_u32(vehicle+53976),api->read_u32(vehicle+54684),api->read_u32(vehicle+120));
      ProbeLog(line);
    }
    return;
  }
  ++car_wrecks_applied;
  static unsigned logged=0;
  if(logged++<20) ProbeLog("Our car wrecked in the game that drives it: blown up here too");
}
// The siren lights and siren the driving game shows (CarSiren), set the way
// the game's multiplayer client frame 823BB068 does: 825419F0(handle, on)
// for the lights, 825255E0(handle, on) for the siren. The siren only sounds
// with someone at the wheel (its per-frame update 82523638 switches it off
// without one), so it waits for the driver copy; not on a broken siren
// (+53976 0x10, which 825255E0 refuses). Only on a car another game drives
// (as ApplyCarDamage): this game's own keep the game's lights and siren.
void ApplyCarSiren(GuestScratch& scratch,uint32_t vehicle,uint8_t want) {
  if(!vehicle || !DrivenFromOtherGame(vehicle)) return;
  const uint8_t have=CarSiren(vehicle);
  const uint32_t handle=api->read_u32(vehicle+68);
  if((have^want)&8) {
    scratch.ctx.r3.u64=handle; scratch.ctx.r4.u64=(want&8)?1:0;
    scratch.Call(0x825419F0);
    ++car_sirens_applied;
  }
  if(((have^want)&16) && !(api->read_u32(vehicle+53976)&0x10) && (!(want&16) || OccupantHandle(vehicle,0))) {
    scratch.ctx.r3.u64=handle; scratch.ctx.r4.u64=(want&16)?1:0;
    scratch.Call(0x825255E0);
    ++car_sirens_applied;
  }
}
// A car the other game drives (our copy of its traffic, the other player's
// car here, or one of ours the other player drives) is only wrecked when the
// other game says so: it blew up here from local knocks while it was fine
// there, and killed the player riding in it.
bool DrivenFromOtherGame(uint32_t vehicle) {
  const uint32_t handle=api->read_u32(vehicle+68);
  if(LocalDrives(handle)) return false;   // this player drives it: this game decides (a copy too)
  if(IsOtherGamesCopy(handle)) return true;
  if(!hosting.load() && SharedCarHostHandle(handle)) return true;
  return AvatarDrives(handle);
}
// Calls counted by caller (and type) for the 10 s lines; the 3 most frequent.
void CountBy(std::unordered_map<uint64_t,unsigned>& m,uint64_t key) {
  if(auto it=m.find(key); it!=m.end()) ++it->second; else if(m.size()<64) m[key]=1;
}
std::string TopCallers(std::unordered_map<uint64_t,unsigned>& m,bool typed) {
  std::vector<std::pair<unsigned,uint64_t>> v;
  for(auto& [key,n]:m) v.push_back({n,key});
  std::sort(v.begin(),v.end(),[](const auto& a,const auto& b){return a.first>b.first;});
  std::string text; char part[48];
  for(size_t i=0;i<v.size() && i<3;++i) {
    if(typed) std::snprintf(part,sizeof(part),"%s%08X type %d x%u",i?", ":"",uint32_t(v[i].second>>32),int32_t(uint32_t(v[i].second)),v[i].first);
    else std::snprintf(part,sizeof(part),"%s%08X x%u",i?", ":"",uint32_t(v[i].second),v[i].first);
    text+=part;
  }
  m.clear();
  return text.empty() ? "-" : text;
}
bool OtherGamesCar(uint32_t v) {
  return v!=applying_vehicle && Readable(v,kVehicleSize) && api->read_u32(v+72)==5 && DrivenFromOtherGame(v);
}
WmlGuestFunction original_detonate=nullptr;
unsigned detonations_blocked=0, detonations_window=0;   // (the first gates the 20 one-shot lines)
std::unordered_map<uint64_t,unsigned> detonate_callers;  // caller << 32 | type (caller's r1+84) -> skipped this window
void DetonateHook(WmlContext* c,uint8_t* b) {
  const uint32_t v=uint32_t(api->get_r(c,3));
  // 2.2 (Package C): a replayed round of a copy of the host's people hits no
  // car here (the host's round decided it; a car of its world shows the
  // result with its damage stream, this player's own car is left alone as in
  // 2.1); this player's bullet on a car of the host's world goes to the host
  // (kEventCarDamage, A/9) and the host's replay of it leaves the car to that.
  if(copy_fire_replay_depth) { ++copy_car_hits_skipped; return; }
  if(connected.load() && WorldCarHit(c,v)) return;
  if(connected.load() && OtherGamesCar(v)) {
    if(detonations_blocked++<20) ProbeLog("Car explosion here skipped: the game that drives it decides");
    ++detonations_window;
    CountBy(detonate_callers,uint64_t(uint32_t(api->get_lr(c)))<<32 | api->read_u32(uint32_t(api->get_r(c,1))+84));
    return;
  }
  original_detonate(c,b);
}
// 8252D2A8(vehicle, attacker, .., .., record, damage): a car loses hit points
// (smoke 0.5, heavy smoke 0.25 and fire 0.15 of the most, the destroy 8252BF38
// at 0). Besides 825397E0 the burn tick 8252DB00 (while 0x800 is set),
// pending damage (+53544, 8253DE30) and 82540370 / 82540458 call it; all of
// them ignore r3. 2.1: a car another game drives loses none here while that
// game sends its damage (ApplyCarDamage in the last second; its burn tick
// wrecked copies the owner still had): its hit points come from that game.
// One nobody sends for (another player's car after they got out) burns down
// here as in 2.0; the copy burned for good. Only real losses are counted (the
// burn tick of a car without a burn passes 0 every frame).
WmlGuestFunction original_car_hp=nullptr;
unsigned car_hp_blocked=0;
std::unordered_map<uint64_t,unsigned> car_hp_callers;
bool CarFed(uint32_t v,uint64_t now) {
  const auto it=car_seen.find(api->read_u32(v+68));
  return it!=car_seen.end() && now<it->second.fed+1000;
}
void CarHpHook(WmlContext* c,uint8_t* b) {
  const uint32_t v=uint32_t(api->get_r(c,3));
  if(connected.load() && OtherGamesCar(v) && CarFed(v,MonoMs())) {
    if(int32_t(api->get_r(c,8))!=0) { ++car_hp_blocked; CountBy(car_hp_callers,uint32_t(api->get_lr(c))); }
    api->set_r(c,3,1); return;
  }
  original_car_hp(c,b);
}
// 82536B00(vehicle, ...): an explosion reaching a car pushes it (826CD960,
// 8224A738 / 8224A788 on its body) before damaging it through 825397E0. 2.1: a
// car another game drives isn't pushed here (the copy flipped from the local
// push alone); the owner's explosion pushes the real one and the copy follows.
// The game's own vehicle_prevent_explosion_fling bit (+53976 0x00010000,
// 824D5208) is set for this call only and put back as it was.
WmlGuestFunction original_car_blast=nullptr;
unsigned car_pushes_blocked=0;
void CarBlastHook(WmlContext* c,uint8_t* b) {
  const uint32_t v=uint32_t(api->get_r(c,3));
  if(!connected.load() || !OtherGamesCar(v)) { original_car_blast(c,b); return; }
  const uint32_t saved=api->read_u32(v+53976);
  if(!(saved&0x00010000u)) ++car_pushes_blocked;
  api->write_u32(v+53976,saved|0x00010000u);
  original_car_blast(c,b);
  api->write_u32(v+53976,(api->read_u32(v+53976)&~0x00010000u)|(saved&0x00010000u));
}
// 8252BF38(vehicle, ...): a car destroyed (the wreck bit, its explosion, cash
// and script callbacks; from 8252D2A8 at 0 hit points and from 823FEFE0).
// Counted for this game's own cars; another game's car wrecked here by this
// game is logged with the caller.
WmlGuestFunction original_car_destroyed=nullptr;
unsigned own_cars_wrecked=0, their_cars_wrecked=0;
void CarDestroyedHook(WmlContext* c,uint8_t* b) {
  const uint32_t v=uint32_t(api->get_r(c,3));
  const bool watch=connected.load() && v!=applying_vehicle && Readable(v,kVehicleSize) && api->read_u32(v+72)==5 && !(api->read_u32(v+53976)&0x80000000u);
  const bool theirs=watch && DrivenFromOtherGame(v);
  original_car_destroyed(c,b);
  if(!watch || !(api->read_u32(v+53976)&0x80000000u)) return;
  if(!theirs) { ++own_cars_wrecked; return; }
  ++their_cars_wrecked;
  static unsigned logged=0;
  if(logged++<10) { char line[120]; std::snprintf(line,sizeof(line),"Car another game drives wrecked by this game (from %08X)",uint32_t(api->get_lr(c))); ProbeLog(line); }
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
  // 1.62: a car without physics (parked / asleep) is only placed when it is
  // really off its path - it was placed every update (~60 per second per
  // parked car: "placed/teleports" in the thousands, jittering cars).
  if(!v && dist2<0.15f*0.15f && align>2.995f) return;
  if(!v || dist2>snap*snap || align<2.0f) {
    (void)next_place;
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
  // (82410F38 skips the game's own occupied-car checks: never a car another
  // player's character sits in or gets into here, nor this player's last car.)
  const uint32_t own=api->read_u32(player+2496), last=api->read_u32(player+22120);
  const uint64_t now=MonoMs();
  for(unsigned removed=0;removed<32;++removed) {
    const uint32_t head=api->read_u32(kTrafficList);
    uint32_t node=head, target=0;
    for(unsigned guard=0;node && guard<64 && Readable(node,12);++guard) {
      const uint32_t handle=api->read_u32(node+8);
      bool ours=false;
      for(auto& [id,sc]:shared_cars) if(sc.car==handle) ours=true;
      if(WorldCarId(handle)) ours=true;   // (the world stream's, 2.2)
      if(handle && !ours && handle!=own && handle!=last && !IsCarCopyHandle(handle) && !personal_cars.count(handle) && !PlayerSitsIn(handle) && !PlayerGettingIn(handle,now)) { target=node; break; }
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
  for(auto it=personal_cars.begin();it!=personal_cars.end();) it=ResolveVehicle(it->first) ? std::next(it) : personal_cars.erase(it);   // (gone from the game)
  const float px=local_x.load(), pz=local_z.load();
  std::vector<uint32_t> remove;
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t v=TableObject(index,5,kVehicleSize);
    if(!v) continue;
    const uint32_t handle=api->read_u32(v+68);
    if(handle==own || IsCarCopyHandle(handle) || taken_cars.count(handle) || personal_cars.count(handle)) continue;
    if(PlayerSitsIn(handle) || PlayerGettingIn(handle,MonoMs()) || handle==api->read_u32(player+22120)) continue;   // another player's character in it / this player's last car
    bool shared=false;
    for(auto& [id,sc]:shared_cars) if(sc.car==handle) { shared=true; break; }
    if(WorldCarId(handle)) shared=true;   // (the world stream's, 2.2)
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
  const float d2=DistanceTo2(0);   // to the host
  // (2.2, world mode: this game's own traffic stays off while it shows the host's area and until its last copy is gone)
  const bool want=connected.load() && share_traffic && (WorldMode() ? WorldLeversOn() : traffic_shared ? d2<200.0f*200.0f : d2<120.0f*120.0f);
  if(!want) {
    if(traffic_shared) {
      // A few per update (1.63): all of them in one go right after the other
      // player dropped hung the joiner's game inside 823ACF50 for 8 s.
      // Copies someone else now sits in (the avatar, this player) are let go,
      // not destroyed.
      static uint64_t next_remove=0;
      if(now<next_remove) return;
      next_remove=now+50;
      unsigned removed=0;
      for(auto it=shared_cars.begin();it!=shared_cars.end() && removed<2;) {
        SharedCar& sc=it->second;
        if(sc.car && PlayerSitsIn(sc.car)) { sc.car=0; sc.driver=0; for(uint32_t& h:sc.pass) h=0; }
        else RemoveSharedCar(scratch,sc,player);
        it=shared_cars.erase(it); ++removed;
      }
      if(!shared_cars.empty()) return;
      if(saved_traffic_density>=0) api->write_f32(kTrafficDensity,saved_traffic_density);
      if(own_traffic_off) { api->write_u8(kTrafficSpawning,1); own_traffic_off=false; }
      block_parked=false;
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
    // (a mission may switch it on again: off again)
    if(api->read_u8(kTrafficSpawning)) { api->write_u8(kTrafficSpawning,0); if(!own_traffic_off) ProbeLog("This game's traffic spawner switched off while the host's traffic is shown"); own_traffic_off=true; }
    RemoveOwnTraffic(scratch,player);
    const bool parked_ok=!MissionActive() && !mirror_mission;   // (a mission's script may own them)
    block_parked=parked_ok;
    static uint64_t next_parked=0;
    if(now>=next_parked && parked_ok) { next_parked=now+500; RemoveOwnParked(scratch,player); }
  }
  if(WorldMode()) {
    // 2.2 (mod.ini world = 1): the world stream shows the host's cars here;
    // 2.1's copies go (two per 50 ms, 1.63) and none are made.
    static uint64_t next_drop=0;
    if(now>=next_drop && !shared_cars.empty()) {
      next_drop=now+50;
      unsigned removed=0;
      for(auto it=shared_cars.begin();it!=shared_cars.end() && removed<2;++removed) {
        SharedCar& sc=it->second;
        if(sc.car && PlayerSitsIn(sc.car)) { sc.car=0; sc.driver=0; for(uint32_t& h:sc.pass) h=0; }
        else RemoveSharedCar(scratch,sc,player);
        it=shared_cars.erase(it);
      }
    }
    return;
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
      // A car of the host's a player took here (a copy): back to following
      // the host's traffic once nobody sits in it or gets in.
      if(auto cp=car_copies.find(CarKey(0,c.id)); !reuse && cp!=car_copies.end() && ResolveVehicle(cp->second.local)) {
        const uint32_t local=cp->second.local;
        if(PlayerSitsIn(local) || PlayerGettingIn(local,now)) continue;
        reuse=local; car_copies.erase(cp);
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
      ForgetCarDamage(s.car);
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
    s.seen=now; s.damage=c.damage; s.hp=c.hp;
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
            // (Seated as a replay: the moves it plays pass PlayActionHook.)
            ++replicated_depth; const bool seated=SeatInVehicle(scratch,d,v,0); --replicated_depth;
            if(!seated) { ++drivers_seat_refused; DestroyObject(scratch,s.driver); s.driver=0; s.next_pass=now+3000; }
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
          ++replicated_depth; const bool seated=SeatInVehicle(scratch,d,v,i+1); --replicated_depth;
          if(!seated) { DestroyObject(scratch,s.pass[i]); s.pass[i]=0; s.next_pass=now+3000; }
          else ++passengers_made;
        } else s.pass[i]=0;
      }
    }
    // 2.1: its crew (copies and seat stand-ins), seated or not: the host's
    // teams (ShowHostTeam), and passive like the host's people here (their
    // own AI sent police crews after a wanted player here). One out of its
    // seat while the host's still sits is counted.
    if(own!=s.car) for(unsigned seat=0;seat<4;++seat) {
      const uint32_t o=ResolveHuman(seat ? s.pass[seat-1] : s.driver);
      if(!o) continue;
      if(api->read_u32(o+2496)!=s.car && (seat ? c.pass[seat-1] : c.driver) && crews_out.size()<512) crews_out.insert(api->read_u32(o+68));
      auto n=names.find(seat ? c.pass[seat-1] : c.driver);
      ShowHostTeam(o,uint8_t((c.teams>>(seat*4))&0xF),s.seat_standin[seat]?"seat stand-in":seat?"traffic passenger":"traffic driver",n!=names.end()?n->second.c_str():"",player);
      MakePassive(o);
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
      { std::lock_guard lock(car_look_mutex); auto l=car_looks_in.find(CarKey(0,it->first)); if(l!=car_looks_in.end()) look=l->second; }
      if(!look.empty() && look!=s.applied_look && ApplyCarLook(scratch,v,look)) { s.applied_look=look; ++car_looks_applied; }
    }
    if(!WatchImpact(v,s.impact,it->first,now)) { SteerAlongPath(scratch,v,s.follow,s.vel,s.ang,now,s.next_place,shared_snaps); AfterSteer(v,s.impact); }
    // 2.1: not while another player's character drives it here: that
    // driver's pose decides (two sources took turns, each change using up one
    // of the car's 4 effect slots).
    if(now>=s.next_damage && !AvatarDrives(s.car) && !IsAvatarHandle(OccupantHandle(v,0))) { s.next_damage=now+250; ApplyCarDamage(scratch,v,s.damage,s.hp); ApplyCarSiren(scratch,v,s.damage); }
    ++it;
  }
  if(now>=next_log) {
    next_log=now+10000;
    drivers_loading=0;
    for(auto& [id,sc]:shared_cars) if(const uint32_t d=ResolveHuman(sc.driver)) if(!(api->read_u32(d+216)&0x4000)) ++drivers_loading;
    cars_with_driver_in=0;
    for(const TrafficCar& c:cars) if(c.driver) ++cars_with_driver_in;
    char line[260];
    std::snprintf(line,sizeof(line),"Shared traffic: %zu cars (host sent %zu), made %u, removed %u (gone %u, not sent %u, far %u), own removed %u (parked %u; this player's own cars kept %zu), placed/teleports %u, colours applied %u, passengers seated %u",
                  shared_cars.size(),cars.size(),shared_made,shared_removed,gone_count,stale_count,far_count,own_removed,own_parked_removed,personal_cars.size(),shared_snaps,car_looks_applied,passengers_made);
    car_looks_applied=0; own_parked_removed=0; passengers_made=0;
    ProbeLog(line);
    std::snprintf(line,sizeof(line),"Traffic people: drivers made %u, seat refused %u, still loading %u, stand-ins %u; host cars with a driver %u",drivers_made,drivers_seat_refused,drivers_loading,seat_standins,cars_with_driver_in);
    seat_standins=0;
    ProbeLog(line);
    drivers_made=drivers_seat_refused=0;
    if(car_type_mismatch) { std::snprintf(line,sizeof(line),"Cars made with a different model than asked: %u",car_type_mismatch); ProbeLog(line); car_type_mismatch=0; }
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
  uint8_t team;         // its team (+232, TeamOf: 0-6), 0xFF unknown (2.1)
  uint8_t spare;
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
  // v2: the people near any client (the nearest client counts); 2.2: of the
  // clients 2.1's streams still go to.
  const auto clients=ClientPositions();
  if(clients.empty()) { std::lock_guard lock(traffic_mutex); peds_out.clear(); return; }
  const uint32_t replica=ResolveHuman(replica_local);
  const uint32_t head=api->read_u32(kAmbientPeds);
  uint32_t obj=head;
  std::vector<uint32_t> listed;  // everyone in the ambient list
  static unsigned ambient_far=0, ambient_named=0; static uint64_t next_far_log=0;
  if(now>=next_far_log) {
    next_far_log=now+10000;
    if(ambient_far || ambient_named) { char line[140]; std::snprintf(line,sizeof(line),"Pedestrians not sent: %u over 100 m from this player, %u story characters (in 10 s)",ambient_far,ambient_named); ProbeLog(line); }
    ambient_far=0; ambient_named=0;
  }
  auto describe=[&](uint32_t cur,PedState& p) {   // where the person is and what they do
    p.id=api->read_u32(cur+68);
    for(unsigned i=0;i<3;++i) p.pos[i]=api->read_f32(cur+20+i*4);
    const float fx=api->read_f32(cur+56), fz=api->read_f32(cur+64);
    p.yaw=std::atan2(fx,fz);
    if(api->read_f32(cur+32)*fz-api->read_f32(cur+40)*fx>=0) p.flags|=1;
    if(PersonDead(cur)) p.flags|=4;
    if(api->read_u32(cur+216)&0x20000000u) p.flags|=8;  // crouching
    p.anim=AnimSet(cur);
    p.action=CurrentPedAction(cur);
    p.team=TeamOf(cur);
  };
  auto near_player=[&](const PedState& p) { const float hx=p.pos[0]-api->read_f32(player+20), hz=p.pos[2]-api->read_f32(player+28); return hx*hx+hz*hz<=100.0f*100.0f; };
  for(unsigned guard=0;obj && guard<128 && Readable(obj,4252);++guard) {
    const uint32_t cur=obj;
    obj=api->read_u32(cur+3600);
    if(obj==head) obj=0;
    if(cur==player || cur==replica || api->read_u32(cur+72)!=1 || AvatarSlotOf(cur)>=0) continue;
    listed.push_back(cur);
    // Everyone in the ambient list, whatever the +120 flags say: a pedestrian
    // that turns on a player (punched) gets flags there, stopped being sent
    // and vanished from the other screen.
    if(api->read_u32(cur+2496)) continue; // people in cars
    PedState p{};
    describe(cur,p);
    const float d2=NearestD2(clients,p.pos[0],p.pos[2]);
    if(d2>90.0f*90.0f) continue;
    // Far from this player: people there aren't animated here (T-pose on the
    // joiner) and this player can't see them. After a mission the game turns
    // the leftover gang members into pedestrians; once the host was moved to
    // the next mission they were sent to the joiner still at the old spot.
    if(!near_player(p)) { ++ambient_far; continue; }
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
  // 2.1: the police response's units on foot (82479AB0(4, ...) from 8242A0B8,
  // never on the ambient list; a squad's 8 at +12, OwnResponseUnits): a client
  // the response is after sees them get out and come. Seated ones go with
  // their car (CaptureTraffic). Same distances as the ambient people.
  response_units_out=0;
  if(hosting.load() && share_police) {
    std::vector<uint32_t> units;
    OwnResponseUnits(&units,nullptr);
    for(uint32_t h:units) {
      const uint32_t cur=ResolveHuman(h);
      if(!cur || cur==player || cur==replica || AvatarSlotOf(cur)>=0 || api->read_u32(cur+2496)) continue;
      if(std::find(listed.begin(),listed.end(),cur)!=listed.end()) continue;
      PedState p{};
      describe(cur,p);
      const float d2=NearestD2(clients,p.pos[0],p.pos[2]);
      if(d2>90.0f*90.0f || !near_player(p)) continue;
      p.model=ModelIndex(cur);
      if(!p.model) continue;
      found.push_back({d2,p}); ++response_units_out;
      uint8_t look[127];
      if(const unsigned n=ReadLook(cur,look,sizeof(look))) {
        std::lock_guard lock(traffic_mutex);
        ped_looks_out[p.id].assign(look,look+n);
      }
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
      if(cur==player || cur==replica || AvatarSlotOf(cur)>=0) continue;
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
      describe(cur,p);
      p.flags|=2;  // mission character
      const float d2=NearestD2(clients,p.pos[0],p.pos[2]);
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
  uint8_t team=0xFF;         // the host's team for this person (ShowHostTeam; 2.1)
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
  void SmoothWorldPeople(uint64_t now);
  SmoothWorldPeople(now);   // (the world stream's, 2.2)
}
uint32_t StandingOnSharedCharacter(uint32_t player) {
  if(!Readable(player,4252)) return 0;
  const float px=api->read_f32(player+20), py=api->read_f32(player+24), pz=api->read_f32(player+28);
  auto on=[&](uint32_t obj) {
    if(!obj || obj==player) return false;
    const float dx=api->read_f32(obj+20)-px, dz=api->read_f32(obj+28)-pz, dy=py-api->read_f32(obj+24);
    return dx*dx+dz*dz<1.3f*1.3f && dy>-0.5f && dy<2.6f;
  };
  for(int s=0;s<kSlots;++s) if(const uint32_t avatar=AvatarObjOf(s); on(avatar)) return avatar;
  for(auto& [id,sp]:shared_peds) { const uint32_t obj=ResolveHuman(sp.handle); if(on(obj)) return obj; }
  std::vector<uint32_t> world; WorldPeopleLocals(world,true);
  for(uint32_t h:world) { const uint32_t obj=ResolveHuman(h); if(on(obj) && !ResolveVehicle(api->read_u32(obj+2496))) return obj; }
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
bool CrewSeat(uint32_t handle,uint32_t& host_car,uint32_t& seat) {
  if(!handle) return false;
  if(WorldCrewSeat(handle,host_car,seat)) return true;
  for(auto& [id,sc]:shared_cars) {
    if(sc.driver==handle) { host_car=id; seat=0; return true; }
    for(unsigned i=0;i<3;++i) if(sc.pass[i]==handle) { host_car=id; seat=i+1; return true; }
  }
  return false;
}
bool CrewCharacter(uint32_t handle) { uint32_t car, seat; return CrewSeat(handle,car,seat); }
bool OurCharacter(uint32_t handle) { return SharedPedHostHandle(handle) || CrewCharacter(handle); }
uint32_t SharedPedHostHandle(uint32_t local) {
  if(!local) return 0;
  for(auto& [id,sp]:shared_peds) if(sp.handle==local || sp.pending==local) return id;
  return WorldPersonId(local,false);   // (the world stream's on foot; its crews are CrewSeat's)
}
uint32_t SharedPedLocalHandle(uint32_t host) {
  auto it=shared_peds.find(host);
  return it==shared_peds.end() ? WorldPersonLocal(host) : it->second.handle;
}
SharedPed* WorldPerson(uint32_t id);
void ForWorldPeople(const std::function<void(uint32_t,SharedPed&)>& fn);
// A copy of the host's person: 2.1's, or the world stream's (2.2).
SharedPed* HostPerson(uint32_t host) { const auto it=shared_peds.find(host); return it!=shared_peds.end() ? &it->second : WorldPerson(host); }
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
  if(WorldSends(handle,false)) return true;   // (on foot in the world stream, 2.2)
  std::lock_guard lock(traffic_mutex);
  return std::any_of(peds_out.begin(),peds_out.end(),[&](const PedState& p){return p.id==handle;});
}
bool IsSentCar(uint32_t id) {
  std::lock_guard lock(traffic_mutex);
  return std::any_of(traffic_out.begin(),traffic_out.end(),[&](const TrafficCar& c){return c.id==id;});
}

void ApplyPedAction(GuestScratch& scratch,const Event& e,uint64_t now) {
  SharedPed* const sp=HostPerson(e.a[4]);   // (2.1's copies, or the world stream's: 2.2)
  if(!sp) return;
  const uint32_t obj=ResolveHuman(sp->handle);
  if(!obj || !(api->read_u32(obj+216)&0x4000)) return;
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=e.a[0]; scratch.ctx.r5.u64=e.a[1];
  scratch.ctx.r6.u64=e.a[2]; scratch.ctx.r7.u64=e.a[3];
  ++replicated_depth;
  const bool played=scratch.Call(0x82444F88) && scratch.ctx.r3.u32;
  --replicated_depth;
  sp->action_args_for=uint16_t(e.a[0]); sp->action_args[0]=e.a[1]; sp->action_args[1]=e.a[2]; sp->action_args[2]=e.a[3];
  if(played) { ++ped_actions_played; sp->action_until=now+8000; sp->next_action_sync=now+2000; }
  static unsigned logged=0;
  if(logged++<60) {
    char line[120];
    std::snprintf(line,sizeof(line),"Person action %02X (%X %X %X): %s",e.a[0],e.a[1],e.a[2],e.a[3],played?"playing":"refused");
    ProbeLog(line);
  }
}
void ApplyPedCall(GuestScratch& scratch,const Event& e,uint64_t now) {
  if(hosting.load() || (e.a[0]!=0x824497F0u && e.a[0]!=0x82441308u)) return;
  SharedPed* const sp=HostPerson(e.a[1]);   // (2.1's copies, or the world stream's: 2.2)
  if(!sp || sp->dead) return;
  const uint32_t obj=ResolveHuman(sp->handle);
  if(!obj || !(api->read_u32(obj+216)&0x4000)) return;
  // Arguments: an offset into the person itself (1), the host's player (2:
  // the host's character here), a client's player (3: slot in a[7], this
  // game's own player when it is us), another shared person (4).
  const uint32_t player=api->read_u32(0x8309ABEC), host_avatar=AvatarObjOf(0);
  uint64_t r[4];
  for(int i=0;i<4;++i) {
    const uint32_t k=(e.a[6]>>(i*3))&7, v=e.a[2+i];
    const int s=int((e.a[7]>>(4*i))&15);
    const uint32_t client=k==3 ? (s==my_slot.load() ? player : AvatarObjOf(s)) : 0;
    r[i]=k==1 ? uint64_t(obj+v) : k==2 ? (host_avatar ? uint64_t(host_avatar+v) : 0) : k==3 ? (client ? uint64_t(client+v) : 0)
        : k==4 ? uint64_t(ResolveHuman(SharedPedLocalHandle(v))) : uint64_t(v);
  }
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=r[0]; scratch.ctx.r5.u64=r[1]; scratch.ctx.r6.u64=r[2]; scratch.ctx.r7.u64=r[3];
  ++applying_ped_call; ++replicated_depth;
  const bool ok=scratch.Call(e.a[0]);
  --replicated_depth; --applying_ped_call;
  if(ok) ++ped_calls_played;
  // Let the fall / reaction play instead of walking the copy along its path.
  sp->action_until=std::max(sp->action_until,now+(e.a[0]==0x824497F0u ? 3000 : 700));
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
  SharedPed* const sp=HostPerson(e.a[0]);   // (2.1's copies, or the world stream's: 2.2)
  if(!sp || sp->dead) return;
  const uint32_t obj=ResolveHuman(sp->handle);
  if(!obj || !(api->read_u32(obj+216)&0x4000)) return;
  // The attacker: the host's player (2: its character here), a client (3:
  // slot a[2], this game's player when it is us), a shared person (4).
  const uint32_t player=api->read_u32(0x8309ABEC);
  const uint32_t attacker=e.a[1]==2 ? AvatarObjOf(0) : e.a[1]==3 ? (e.a[2]==my_slot.load() ? player : AvatarObjOf(int(e.a[2]&15)))
                         : e.a[1]==4 ? ResolveHuman(SharedPedLocalHandle(e.a[2])) : 0;
  uint32_t hit=0;
  if(e.a[7]&(1u<<16)) { hit=scratch.data()+2200; for(int i=0;i<3;++i) api->write_u32(hit+i*4,e.a[3+i]); }
  else if(attacker) { hit=scratch.data()+2200; for(int i=0;i<3;++i) api->write_f32(hit+i*4,api->read_f32(attacker+20+i*4)); }
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=attacker; scratch.ctx.r5.u64=hit;
  scratch.ctx.r6.u64=e.a[6]; scratch.ctx.r7.u64=e.a[7]&0xFF; scratch.ctx.r8.u64=(e.a[7]>>8)&0xFF;
  ++applying_ped_call; ++replicated_depth;
  const bool ok=scratch.Call(0x82458EB8);
  --replicated_depth; --applying_ped_call;
  if(ok) ++ped_reacts_played;
  sp->action_until=std::max(sp->action_until,now+1200);
  static unsigned logged=0;
  if(logged++<40) {
    char line[140];
    std::snprintf(line,sizeof(line),"Person hit reaction (full) from the host, attacker kind %u%s: %s",e.a[1],attacker?"":" (not here)",ok?"played":"failed");
    ProbeLog(line);
  }
}
bool peds_shared=false;
// 824189E0: the gang manager (ambient gang members) runs whatever the people
// spawner bytes say (research S 2c): skipped on a client while the host's
// people are shown (its gang members came only to the client).
WmlGuestFunction original_gang_manager=nullptr;
void GangManagerHook(WmlContext* c,uint8_t* b) {
  if(peds_shared && connected.load() && !hosting.load()) { ++gang_skipped; return; }
  original_gang_manager(c,b);
}
// 2.1: a client's own police while it shows the host's world (its people or
// its traffic), both from the world update 824131F0, which ignores their r3:
// the response 8242ABA0 (cars, helicopter and units sent after this player
// for its own notoriety, spawned around it: police only this client saw) ends
// the way its own multiplayer exit does, with the response group the
// spawners serve next cleared (0x8309A540, one of the 12 group pointers at
// 0x8309A510; 824129C8 / 82411310 skip it when 0); the roadblocks 82424708
// are skipped. The host's police are shown instead. (allow_cop_spawning
// 0x827AD06C and the roadblock counter 82425150 are left alone: mission
// scripts own both.) Only while this player's character stands in the host's
// game, which is what its police come after (UpdateResponseTarget): made
// within 60 m of the host and removed past 130 m (UpdateAvatar, the same both
// ways), so the host's character here (remotes[0]: slot 0 is the host) stands
// for it, kept 2 s after it goes so the edge doesn't flap; beyond, this game's
// own police come back (its levels were its own all along; 2.0 left no police
// at all from 130 m to the shared world's 200 m). Not with mod.ini police = 0:
// a client's police then run as in 2.0. 2.2 (world mode): while a member of
// the host's area by its OwnerMap (the host's rule, member_join_m / _leave_m) and
// the host's character is shown here (kept to member_leave_m + 30 m for a
// member, UpdateAvatar): one membership rule, not 2.1's 120 / 200 m too.
WmlGuestFunction original_police_response=nullptr, original_roadblocks=nullptr;
bool OwnPoliceOff() {   // (game thread)
  static uint64_t host_shown_until=0;
  if(!share_police || !connected.load() || hosting.load()) return false;
  if(!WorldMode() && !(peds_shared || traffic_shared)) return false;
  if(WorldMode() && !WorldShown()) return false;
  const uint64_t now=MonoMs();
  if(remotes[0].avatar) host_shown_until=now+2000;
  return now<host_shown_until;
}
void PoliceResponseHook(WmlContext* c,uint8_t* b) {
  if(OwnPoliceOff()) { api->write_u32(0x8309A540,0); ++police_skipped; return; }
  original_police_response(c,b);
}
void RoadblocksHook(WmlContext* c,uint8_t* b) {
  if(OwnPoliceOff()) { ++roadblocks_skipped; return; }
  original_roadblocks(c,b);
}
// What the response had out when the host's world came (this player wanted
// here first): with 8242ABA0 skipped nothing updates or calls off its groups,
// so they kept after this player, seen only here. Each of the 12 groups
// (82429E38 updates one) has 8 squads of 44 bytes from +92: an active byte,
// the squad's car (+4) and 8 units (+12), all handles. Removed once, as the
// host's world comes: units (never a player, a copy, a follower or a mission
// character) and their cars (never one a player sits in, gets into or took);
// a unit seated in a car that stays, stays too (removed with its car, 1.12).
// The host sends the same squads to its clients (CapturePeds, CaptureTraffic).
constexpr uint32_t kResponseGroups=0x8309A510;
unsigned OwnResponseUnits(std::vector<uint32_t>* units,std::vector<uint32_t>* cars) {
  unsigned n=0;
  for(uint32_t g=0;g<12;++g) {
    const uint32_t group=api->read_u32(kResponseGroups+g*4);
    if(!group || !Readable(group,92+8*44)) continue;
    for(uint32_t k=0;k<8;++k) {
      const uint32_t squad=group+92+k*44;
      if(!api->read_u8(squad)) continue;
      if(const uint32_t car=api->read_u32(squad+4); cars && ResolveVehicle(car)) cars->push_back(car);
      for(uint32_t i=0;i<8;++i) if(const uint32_t h=api->read_u32(squad+12+i*4); ResolveHuman(h)) { ++n; if(units) units->push_back(h); }
    }
  }
  return n;
}
bool own_police_off=false;
void RemoveOwnResponse(GuestScratch& scratch,uint32_t player) {
  const bool off=OwnPoliceOff();
  const bool edge=off && !own_police_off;
  own_police_off=off;
  if(!edge) return;
  std::vector<uint32_t> units, cars;
  OwnResponseUnits(&units,&cars);
  const uint64_t now=MonoMs();
  auto unit_ok=[&](uint32_t h) {
    const uint32_t o=ResolveHuman(h);
    return o && o!=player && AvatarSlotOf(o)<0 && h!=replica_local && !OurCharacter(h) && !PlayerFollower(player,h) && !(api->read_u32(o+120)&0xF0000000u);
  };
  for(uint32_t h:units) if(const uint32_t o=ResolveHuman(h)) if(const uint32_t v=ResolveVehicle(api->read_u32(o+2496))) cars.push_back(api->read_u32(v+68));
  std::sort(cars.begin(),cars.end()); cars.erase(std::unique(cars.begin(),cars.end()),cars.end());
  unsigned people=0, removed_cars=0;
  std::vector<uint32_t> kept_cars;
  for(uint32_t car:cars) {
    const uint32_t v=ResolveVehicle(car);
    bool ok=v && !IsSharedCarHandle(car) && !taken_cars.count(car) && !PlayerSitsIn(car) && !PlayerGettingIn(car,now) &&
            car!=api->read_u32(player+2496) && car!=api->read_u32(player+22120);
    for(uint32_t seat=0;seat<4 && ok;++seat) if(const uint32_t o=OccupantHandle(v,seat)) ok=std::find(units.begin(),units.end(),o)!=units.end() && unit_ok(o);
    if(!ok) { kept_cars.push_back(car); continue; }
    for(uint32_t seat=0;seat<4;++seat) if(const uint32_t o=OccupantHandle(v,seat)) if(ResolveHuman(o)) { DestroyObject(scratch,o); ++people; }
    if(ResolveVehicle(car)) { DestroyObject(scratch,car); ++removed_cars; }
  }
  unsigned left=0;
  for(uint32_t h:units) {
    const uint32_t o=ResolveHuman(h);
    if(!o) continue;
    const uint32_t v=ResolveVehicle(api->read_u32(o+2496));
    if(!unit_ok(h) || (v && std::find(kept_cars.begin(),kept_cars.end(),api->read_u32(v+68))!=kept_cars.end())) { ++left; continue; }
    DestroyObject(scratch,h); ++people;
  }
  static unsigned logged=0;
  if((people || removed_cars || left || !kept_cars.empty()) && logged++<10) {
    char line[200];
    std::snprintf(line,sizeof(line),"This game's own police response removed as the host's world is shown: %u people, %u cars; left: %u people, %u cars (a player's, or someone else in them)",
                  people,removed_cars,left,unsigned(kept_cars.size()));
    ProbeLog(line);
  }
}
// ---- Notoriety (2.1): per player ----
// Each game keeps its own player's notoriety: +3868, 5 entries of 24 bytes,
// one per gang (0 Los Carnales, 1 Vice Kings, 2 Rollerz, 3 Playas, 4 police;
// team to entry 821D2EE8, entry to team 0x8203A29C = 1, 2, 3, 0, 5): +0 level,
// +4 min, +8 max, +12 points (float), +16 decay timer (-1: off), +20 timer.
// Its decay (8247E850 -> 821D24F0), HUD (822F5300) and stars stay the game's.
// Level, min, max and points go to every game (kMsgNotoriety), which writes
// them on that player's character: the police there go after a player-type
// target by its own entry 4 (820D28E0: +3964 >= 1 when 823FEBE8 says +72 == 1
// and +224 == 1), the gangs by 82481AB8(target, team) (H). Never with the
// game's setters (821D2F50, 821D3070, 821D2D38, 821D2950): they end in
// 821D1A20, which works on this game's player (H). mod.ini police = 0: none
// of this (as 2.0: the host's stars rose for everyone's crimes).
void CaptureNotoriety(uint32_t player) {
  Notoriety n;
  for(uint32_t g=0;g<5;++g) {
    const uint32_t e=player+3868+24*g;
    n.level[g]=uint8_t(std::clamp<int32_t>(int32_t(api->read_u32(e)),0,5));
    n.min[g]=uint8_t(std::clamp<int32_t>(int32_t(api->read_u32(e+4)),0,5));
    n.max[g]=uint8_t(std::clamp<int32_t>(int32_t(api->read_u32(e+8)),0,5));
    const float p=api->read_f32(e+12);
    n.points[g]=std::isfinite(p) && p>0.0f ? p : 0.0f;
  }
  std::lock_guard lock(pose_mutex);
  const Notoriety& o=local_notoriety;
  if(!local_notoriety_changes || std::memcmp(n.level,o.level,5) || std::memcmp(n.min,o.min,5) || std::memcmp(n.max,o.max,5)) ++local_notoriety_changes;
  local_notoriety=n;
}
// The five levels as text (Carnales/Vice Kings/Rollerz/Playas/police), with
// the police points (entry 4's +12; 2.1.1: credit short of a star shows).
std::string NotorietyText(uint32_t obj) {
  char t[128];
  const float points=api->read_f32(obj+3964+12);
  std::snprintf(t,sizeof(t),"%d/%d/%d/%d/%d (police points %.0f)",int32_t(api->read_u32(obj+3868)),int32_t(api->read_u32(obj+3892)),int32_t(api->read_u32(obj+3916)),
                int32_t(api->read_u32(obj+3940)),int32_t(api->read_u32(obj+3964)),std::isfinite(points)?points:0.0f);
  return t;
}
// A gang's level on another player's character dropping to 0 (or the character
// going): forgiven the way 821D1A20 forgives this game's player at level 0 -
// every person of that gang's team on the list *0x8309ABB8 (next +3592, round
// to the head) takes the character's handle off its enemy lists (820D8398 /
// 820D8538: the lists at +3976 / +4044 with their counts at +3972 / +4040, H),
// under 821D1A20's own skips (+120 bits 0x80000000 / 0x40000000 / 0x10000000
// / 0x08000000 / 0x04000000, 82448D18: dead or held), and one whose target
// (820D8168, by its +3716 state) is the character is told to stop (event 61,
// 820B04E8(61, h+68, h+68, 0, -1, 0, 0), as 821D1A20 sends it to the player's
// attackers; not one with +3695 & 8) - the list removals alone left the
// response's units on foot shooting at a forgiven character.
unsigned forgivings=0, stops_told=0;
void CallOffPursuit(GuestScratch& scratch,uint32_t handle,int32_t team,const char* why,const char* who=nullptr);  // (with the police response, 2.1)
void Forgive(GuestScratch& scratch,uint32_t handle,uint32_t g) {
  const uint32_t team=api->read_u32(0x8203A29C+g*4), head=api->read_u32(0x8309ABB8);
  uint32_t h=head;
  for(unsigned guard=0;h && Readable(h,4252) && guard<512;++guard) {
    if(api->read_u32(h+232)==team && !(api->read_u32(h+120)&0xDC000000u)) {
      scratch.ctx.r3.u64=h; scratch.Call(0x82448D18);
      if(!scratch.ctx.r3.u32) {
        scratch.ctx.r3.u64=h; scratch.ctx.r4.u64=handle; scratch.Call(0x820D8398);
        scratch.ctx.r3.u64=h; scratch.ctx.r4.u64=handle; scratch.Call(0x820D8538);
        if(!(api->read_u8(h+3695)&8)) {
          scratch.ctx.r3.u64=h; scratch.Call(0x820D8168);
          if(const uint32_t target=scratch.ctx.r3.u32; target && Readable(target,72) && api->read_u32(target+68)==handle) {
            scratch.ctx.r3.u64=61; scratch.ctx.r4.u64=scratch.ctx.r5.u64=api->read_u32(h+68); scratch.ctx.r6.u64=0; scratch.ctx.r7.u64=~0ull; scratch.ctx.r8.u64=0; scratch.ctx.r9.u64=0;
            scratch.Call(0x820B04E8); ++stops_told;
          }
        }
      }
    }
    h=api->read_u32(h+3592);
    if(h==head) break;
  }
  ++forgivings;
  if(hosting.load() && connected.load() && share_police) CallOffPursuit(scratch,handle,int32_t(team),"level 0");   // (821D1A20's car pass: the cars the response sent after it)
}
// Writes a player's notoriety on their character here (n), or none (null:
// before the pool object is let go - making a character from it, 8247BA28 ->
// 821D2D38 -> 821D2950, runs 821D1A20(0, gang) on this game's player for each
// gang it still holds a level in, H). +16 is always -1: 8247E850 decays every
// player of the active list 0x8309AC54 (821D24F0 runs once +16 >= 0 and due;
// the game's own hit credits, 82481D50 -> 821D2468, arm it), and each step
// calls 821D1A20 on this game's player. True when a level changed.
bool WriteNotoriety(GuestScratch& scratch,uint32_t obj,const Notoriety* n) {
  if(!Readable(obj,4252)) return false;
  const uint32_t handle=api->read_u32(obj+68);
  bool changed=false;
  for(uint32_t g=0;g<5;++g) {
    const uint32_t e=obj+3868+24*g, level=n ? n->level[g] : 0;
    const int32_t was=int32_t(api->read_u32(e));
    api->write_u32(e,level);
    if(n) { api->write_u32(e+4,n->min[g]); api->write_u32(e+8,n->max[g]); }
    api->write_f32(e+12,n ? n->points[g] : 0.0f);
    api->write_u32(e+16,0xFFFFFFFFu);
    changed=changed || was!=int32_t(level);
    if(was>=1 && !level) Forgive(scratch,handle,g);
  }
  return changed;
}
void ClearNotoriety(GuestScratch& scratch,uint32_t obj) {
  if(obj && obj!=api->read_u32(0x8309ABEC)) WriteNotoriety(scratch,obj,nullptr);
}
// Detection (2.1.2). A crime record's flag byte (+12) means witnessed. When it
// is 0 and the activity's entry has Check_Detection (byte +16 of the 20-byte
// sub-record at 0x82B2BAC8 + activity*100 + entry*20: +0 Points, +4 Delay,
// +8 Min_Level, +12 Max_Level; 821D1488 fills 0-2 from the Gang block, 4 from
// the Police block, 3 stays zero), 821D2840 runs 821D1F98(*0x8309ABEC, entry)
// as the record is applied (500 ms after the crime, 821D2AD0): a living member
// of the entry's team (0x8203A29C; entry 3: none) on the list *0x8309ABB8 -
// not dead, going or held (+120, 82448D18) - sees the human: in a car
// (82448010) within 40 m all round (820EF3F8; a helicopter: in front only,
// twice that), on foot (820D79E8 -> 820D7828) within 8 m unless behind it, or
// within 40 m in its front 120-degree cone (ai_rank_attribs.xtbl, the same for
// all four ranks), through walls; one sent after the player (+3692 bit 0x80)
// sees a player-type target anyway. Unseen, the crime gives points/4
// (821D2178; a client's crimes gave no stars in the first 2.1 test), seen at
// least what its Min_Level takes. A record whose flag is set - the game's own
// for a cop or gang member as the victim, a cop hearing the gun, an alert -
// skips the test. The test reads only its arguments and the list (H), so the
// host, where the real cops are, runs it for the culprit's character when it
// forwards a crime and sends the flag set when it holds; the client's 821D2840
// then takes it as seen.
bool Seen(GuestScratch& scratch,uint32_t human,uint32_t entry) {
  scratch.ctx.r3.u64=human; scratch.ctx.r4.u64=entry;
  return scratch.Call(0x821D1F98) && (scratch.ctx.r3.u32&0xFF);
}
// The entry 821D2BE0 credits a team's crime to: the gangs their own (821D2EE8:
// team 1-3 -> 0-2), everything else (police, civilians, no team, Playas) the
// police (its jump table 0x821D2C88 sends -1, 3 and 4 to 4).
inline uint32_t CreditEntry(int32_t team) { return team>=1 && team<=3 ? uint32_t(team-1) : 4u; }
bool ChecksDetection(uint32_t activity,uint32_t entry) { return activity<24 && api->read_u8(0x82B2BAC8+activity*100+entry*20+16)!=0; }
unsigned crimes_witnessed=0, crimes_marked=0, seen_sent=0, seen_taken=0;   // (2.1.2) other players' crimes seen by the cops here; this player's own queued witnessed; kEventSeen sent / taken
int detection_poll=0;   // host: inside UpdateNotoriety's test (DetectionHook doesn't log it)
// The flag sent with another player's crime seen here: the game's, or 1 when
// their character is seen by the entry's team right now (from the hook's own
// context: GuestScratch scratch(c), as the streaming hook calls the game).
uint32_t WitnessedFlag(WmlContext* c,int slot,uint32_t activity,int32_t team,uint32_t flag) {
  flag&=0xFF;
  if(flag) return flag;
  const uint32_t entry=CreditEntry(team), avatar=AvatarObjOf(slot);
  if(!avatar || !ChecksDetection(activity,entry)) return 0;
  GuestScratch scratch(c);
  if(!Seen(scratch,avatar,entry)) return 0;
  ++crimes_witnessed;
  return 1;
}
// Client: the host's cops see our character there (kEventSeen, 1 or 0, on
// change and once a second): for 1.5 s after a 1, the crimes this game credits
// itself (its hits 82481CD8 and carjacks 824828C0 with a civilian victim: flag
// 0) go in witnessed, as if a cop here had seen them - whether the copies of
// the host's cops pass the test's own skips is open (DetectionHook tells). Only
// in the host's world (OwnPoliceOff): far from the host this player's own cops
// judge.
uint64_t police_sees_until=0;
bool HostCopsSeeUs() { return OwnPoliceOff() && MonoMs()<police_sees_until; }
void ApplySeen(const Event& e,uint64_t now) {
  const bool was=now<police_sees_until;
  police_sees_until=e.a[0] ? now+1500 : 0; ++seen_taken;
  static unsigned logged=0;
  if(was!=(e.a[0]!=0) && logged++<20) ProbeLog(e.a[0] ? "The host's cops see our character there: our own crimes go in witnessed" : "The host's cops don't see our character there any more");
}
// Every update: this player's notoriety for the network thread, and each other
// player's on their character here - when theirs changes and once a second
// (+16 back to -1; a character made again), only on a player-pool character
// (+224 == 1, as the police check 823FEBE8 wants; the NPC stand-in has 2). A
// player who left, or whose slot someone else took, has none here.
void UpdateNotoriety(GuestScratch& scratch,uint32_t player,uint64_t now) {
  CaptureNotoriety(player);
  const uint8_t me=my_slot.load();
  for(int s=0;s<kSlots;++s) {
    Remote& r=remotes[s];
    const uint32_t obj=s!=me && r.player_obj ? AvatarObjOf(s) : 0;
    if(!obj || api->read_u32(obj+224)!=1) { r.notoriety_on=0; continue; }
    // 2.1.2, host: whether a cop here sees their character (tested every
    // 250 ms; told on change and once a second).
    if(hosting.load() && now>=r.next_seen_check) {
      r.next_seen_check=now+250;
      ++detection_poll; const bool seen=Seen(scratch,obj,4); --detection_poll;
      if(seen!=r.police_see || now>=r.next_seen_send) {
        QueueEventTo(Event{kEventSeen,{seen?1u:0u},0},uint8_t(s)); ++seen_sent;
        static unsigned logged=0;
        if(seen!=r.police_see && logged++<20) ProbeLog((std::string("A cop here ")+(seen?"sees ":"no longer sees ")+r.tag+"'s character: their game is told").c_str());
        r.police_see=seen; r.next_seen_send=now+1000;
      }
    }
    Notoriety n; bool fresh=false, theirs=false;
    {
      std::lock_guard lock(pose_mutex);
      RemoteIn& in=remote_in[s];
      theirs=Present(s) && r.generation==in.generation && in.notoriety_sequence;
      n=in.notoriety; fresh=in.notoriety_new; in.notoriety_new=false;
    }
    if(!theirs) {
      if(r.notoriety_on==r.avatar && WriteNotoriety(scratch,obj,nullptr)) { static unsigned logged=0; if(logged++<20) ProbeLog((std::string(r.tag)+": their character's notoriety here cleared (nothing from their game)").c_str()); }
      r.notoriety_on=0;
      continue;
    }
    if(!fresh && r.notoriety_on==r.avatar && now<r.next_notoriety) continue;
    const bool changed=WriteNotoriety(scratch,obj,&n);
    r.notoriety_on=r.avatar; r.next_notoriety=now+1000;
    static unsigned logged=0;
    if(changed && logged++<60) ProbeLog((std::string(r.tag)+"'s notoriety on their character here (Carnales/Vice Kings/Rollerz/Playas/police): "+NotorietyText(obj)).c_str());
  }
}

// Crimes. The game that sees one credits the culprit's own game: around the
// game's crime functions crime_culprit says whose it is - another player's
// character (their slot: sent to their game, kEventNotoriety), kCrimeDropped
// (another game counts it) or -1 (this player's: queued as usual).
// 821D3148(activity, team, flag, weapon info or 0) queues a crime of this
// game's player (16-byte records at 0x838F9348, count 0x82B2BA6C, at most 30:
// +0 activity, +4 team, +8 due = now + the activity's delay (100-byte records
// from 0x82B2BACC), +12 flag, +13 1 for a rocket launcher, 825BF760; 821D2AD0
// -> 82481B28 applies a due one, a full queue applies at once), unless
// notoriety is locked (*0x8370D1A0) - from 82482098 (kills), 82481CD8 (hits,
// 82481D50; shots near people, 82481EE8; cars hit, 82482760) and 82482568
// (government cars, helicopters, trains destroyed). 824828C0 (a carjack,
// 8257F0E8) and 82482290 (a cop sees a gun fired, 820DA358) put their record
// in the queue themselves (H): RunQueuingCrime. 2.1.2: 82482400 (a police
// car taken, 825786A0) and 820DB9D0 (a cop's alert) too.
constexpr int kCrimeDropped=-2;
thread_local int crime_culprit=-1;
int crime_replay=0;   // inside ApplyCrime's 821D3148 (the sender's flag stands)
unsigned crimes_sent=0, crimes_received=0, crimes_dropped=0, crimes_refused=0, crimes_lost=0;   // (lost: another player's, with this game's queue full)
// The activity's name (0x827D65C8: 24 of them, "burglary" to "tagging").
const char* CrimeName(uint32_t activity) {
  const uint32_t s=activity<24 ? api->read_u32(0x827D65C8+activity*4) : 0;
  return Readable(s,24) && std::memchr(api->guest_pointer(s),0,24) ? static_cast<const char*>(api->guest_pointer(s)) : "?";
}
// 825BF760's rocket test: weapon info +20 == 4, named rpg_launcher or rpg_laser.
bool RocketLauncher(uint32_t info) {
  if(!info || !Readable(info,24) || api->read_u32(info+20)!=4) return false;
  const uint32_t name=api->read_u32(info);
  if(!Readable(name,16) || !std::memchr(api->guest_pointer(name),0,16)) return false;
  const char* s=static_cast<const char*>(api->guest_pointer(name));
  return !std::strcmp(s,"rpg_launcher") || !std::strcmp(s,"rpg_laser");
}
void SendCrime(int slot,uint32_t activity,uint32_t team,uint32_t flag,bool rocket) {
  QueueEventTo(Event{kEventNotoriety,{activity,team,flag&0xFF,rocket?1u:0u},0},uint8_t(slot));
  ++crimes_sent;
  static unsigned logged=0;
  if(logged++<30) {
    char line[160];
    std::snprintf(line,sizeof(line),"Crime of %s's character here: %s (team %d, flag %u%s): sent to their game",remotes[slot].tag,CrimeName(activity),int32_t(team),flag&0xFF,rocket?", rocket launcher":"");
    ProbeLog(line);
  }
}
WmlGuestFunction original_crime=nullptr;
void CrimeHook(WmlContext* c,uint8_t* b) {
  const uint32_t activity=uint32_t(api->get_r(c,3)), team=uint32_t(api->get_r(c,4)), flag=uint32_t(api->get_r(c,5));
  if(connected.load() && share_police) {
    static std::unordered_set<uint32_t> callers;   // (each caller once)
    if(callers.size()<16 && callers.insert(api->get_lr(c)).second) {
      char line[180];
      std::snprintf(line,sizeof(line),"Crime %s (team %d, flag %u) from %08X: %s",CrimeName(activity),int32_t(team),flag&0xFF,api->get_lr(c),
                    crime_culprit>=0 ? "another player's, sent to their game" : crime_culprit==kCrimeDropped ? "dropped (another game counts it)" : "this player's");
      ProbeLog(line);
    }
  }
  if(crime_culprit==kCrimeDropped) { ++crimes_dropped; return; }
  if(crime_culprit>=0) {
    // (behind the game's own gates: notoriety locked, 0x8370D1A0, or multiplayer)
    if(!api->read_u32(0x8370D1A0) && !api->read_u8(0x8370E9F6)) SendCrime(crime_culprit,activity,team,WitnessedFlag(c,crime_culprit,activity,int32_t(team),flag),RocketLauncher(uint32_t(api->get_r(c,6))));
    return;
  }
  // (2.1.2, a client in the host's world: this player's own, by the game's hit
  // credit, goes in witnessed while the host's cops see our character; not one
  // the host saw and sent, whose flag is the host's)
  if(!(flag&0xFF) && !crime_replay && CreditEntry(int32_t(team))==4 && HostCopsSeeUs()) { api->set_r(c,5,1); ++crimes_marked; }
  original_crime(c,b);
}
void RunCrime(WmlGuestFunction original,WmlContext* c,uint8_t* b,int culprit) {
  const int outer=crime_culprit;
  crime_culprit=culprit; original(c,b); crime_culprit=outer;
}
// 824828C0 and 82482290 queue their record themselves: when it isn't this
// player's, the record they add is taken out again and sent to the culprit's
// game, or dropped. With the queue full they apply it to this game's player at
// once instead (82481B28): not run then - a dropped one is dropped all the
// same, another player's is lost (counted) - and both come back: 824828C0
// marks its car (+53976 bit 0x200) only after the record, so 8257F0E8 asks
// again each frame; 82482290 puts the cop on the shooter's seen list
// (8247D2D0) after its own gates, so that cop asks again at the next shot.
void RunQueuingCrime(WmlGuestFunction original,WmlContext* c,uint8_t* b,int culprit,const char* what) {
  if(culprit==-1) {
    if(!connected.load() || !share_police) { original(c,b); return; }
    const uint32_t before=api->read_u32(0x82B2BA6C);
    original(c,b);
    // (this player's own, as queued: neither caller reaches 821D3148, so this
    // is the only sign of its carjacks of copies counting here)
    if(before<30 && api->read_u32(0x82B2BA6C)==before+1) {
      const uint32_t rec=0x838F9348+before*16;
      // (2.1.2: witnessed while the host's cops see our character, as CrimeHook marks the hit credits)
      if(!api->read_u8(rec+12) && CreditEntry(int32_t(api->read_u32(rec+4)))==4 && HostCopsSeeUs()) { api->write_u8(rec+12,1); ++crimes_marked; }
      static unsigned logged=0;
      if(logged++<10) {
        char line[160];
        std::snprintf(line,sizeof(line),"Crime queued here by this player: %s (team %d, flag %u) from %s",CrimeName(api->read_u32(rec)),int32_t(api->read_u32(rec+4)),api->read_u8(rec+12),what);
        ProbeLog(line);
      }
    }
    return;
  }
  const uint32_t before=api->read_u32(0x82B2BA6C);
  if(before>=30) {
    if(culprit>=0) {
      ++crimes_lost;
      static unsigned logged=0;
      if(logged++<10) { char line[160]; std::snprintf(line,sizeof(line),"Crime of %s's character here (%s) lost: this game's crime queue is full (30)",remotes[culprit].tag,what); ProbeLog(line); }
    }
    return;
  }
  original(c,b);
  if(api->read_u32(0x82B2BA6C)!=before+1) return;
  const uint32_t rec=0x838F9348+before*16;
  if(culprit>=0) SendCrime(culprit,api->read_u32(rec),api->read_u32(rec+4),WitnessedFlag(c,culprit,api->read_u32(rec),int32_t(api->read_u32(rec+4)),api->read_u8(rec+12)),api->read_u8(rec+13)!=0);
  else ++crimes_dropped;
  api->write_u32(0x82B2BA6C,before);
  for(uint32_t i=0;i<16;i+=4) api->write_u32(rec+i,0);   // (as 821D2950 leaves a removed one)
}
// 82482098(killer, victim, weapon): a kill. Another player's character's: sent
// to their game. On a client, of a copy of the host's (a person, crew member
// or stand-in): dropped, the host's game counts its person's death.
WmlGuestFunction original_kill_crime=nullptr;
void KillCrimeHook(WmlContext* c,uint8_t* b) {
  int culprit=-1;
  if(connected.load() && share_police) {
    const uint32_t victim=uint32_t(api->get_r(c,4));
    if(!hosting.load() && Readable(victim,72) && OurCharacter(api->read_u32(victim+68))) culprit=kCrimeDropped;
    else culprit=AvatarSlotOf(uint32_t(api->get_r(c,3)));
  }
  RunCrime(original_kill_crime,c,b,culprit);
}
// 82481CD8(attacker, position, activity, flag, team, weapon): hits (82481D50,
// from melee 82457900, a car hitting someone 82451C98 and 8256A138; its call
// returns to 82481EDC), shots near people (82481EE8, from the bullet's
// 824581C8 before its damage call, so a copy's refused damage doesn't skip
// it; returns to 82482084) and cars hit (82482760; returns to 824828B4). By
// another player's character: dropped - their own game credits its player's
// (its bullets and punches land there; here they replay).
WmlGuestFunction original_hit_crime=nullptr;
const char* HitCrimeCaller(uint32_t lr) { return lr==0x82481EDCu ? "a hit, 82481D50" : lr==0x82482084u ? "a shot near someone, 82481EE8" : lr==0x824828B4u ? "a car hit, 82482760" : "another caller"; }
void HitCrimeHook(WmlContext* c,uint8_t* b) {
  const bool theirs=connected.load() && share_police && AvatarSlotOf(uint32_t(api->get_r(c,3)))>=0;
  if(connected.load() && share_police) {
    // (each caller once, for this player's and for theirs: whether a client's
    // bullets at copies reach 82481EE8 there at all, or only its punches 82481D50)
    static std::unordered_set<uint32_t> callers;
    if(callers.size()<8 && callers.insert(api->get_lr(c)|(theirs?1u:0u)).second) {
      char line[180];
      std::snprintf(line,sizeof(line),"Hit credit %s (activity %u) from %08X (%s): %s",CrimeName(uint32_t(api->get_r(c,5))),uint32_t(api->get_r(c,5)),api->get_lr(c),HitCrimeCaller(api->get_lr(c)),
                    theirs?"another player's character's, dropped (their game counts it)":"this player's");
      ProbeLog(line);
    }
  }
  if(theirs) { ++crimes_dropped; return; }
  original_hit_crime(c,b);
}
// 82482568(human, car, weapon): a government car, helicopter or train destroyed
// (from 8252BF38). Another player's character's: sent to their game (a copy
// wrecked there replays the wreck, 82542650, which skips 8252BF38).
WmlGuestFunction original_car_crime=nullptr;
void CarCrimeHook(WmlContext* c,uint8_t* b) {
  RunCrime(original_car_crime,c,b,connected.load() && share_police ? AvatarSlotOf(uint32_t(api->get_r(c,3))) : -1);
}
// 824828C0(human, car): a carjack, checked each frame for a player-type human
// driving a car (8257F0E8) whose pulled-out driver (human+2592) had another
// team, once per car (+53976 bit 0x200). The jacker's own game credits it (a
// copy's crew wears the host's team): by another player's character here,
// dropped like their hits (the car is marked all the same).
WmlGuestFunction original_carjack_crime=nullptr;
void CarjackCrimeHook(WmlContext* c,uint8_t* b) {
  RunQueuingCrime(original_carjack_crime,c,b,connected.load() && share_police && AvatarSlotOf(uint32_t(api->get_r(c,3)))>=0 ? kCrimeDropped : -1,"a carjack, 824828C0");
}
// 82482290(shooter, cop): a cop (team 5) sees a gun fired (820DA358): firearm
// discharge, police. Another player's character's: sent to their game. On a
// client, by a copy of the host's cops: dropped, the host's cop sees the
// shooter's character there. (A cop that saw this shooter within 15 s only
// restarts the decay of the shooter's own entry 4, 821D2468: see DecayHook.)
WmlGuestFunction original_gun_seen=nullptr;
void GunSeenHook(WmlContext* c,uint8_t* b) {
  int culprit=-1;
  if(connected.load() && share_police) {
    const uint32_t cop=uint32_t(api->get_r(c,4));
    if(!hosting.load() && Readable(cop,72) && OurCharacter(api->read_u32(cop+68))) culprit=kCrimeDropped;
    else culprit=AvatarSlotOf(uint32_t(api->get_r(c,3)));
  }
  RunQueuingCrime(original_gun_seen,c,b,culprit,"a cop seeing a gun fired, 82482290");
}
// 82482400(human, car): a police car (+53992 == 5; +55580 == 0, +53976 bit
// 0x200 and +53980 bit 0x2000 clear) taken: once a player-type human sits at
// its wheel (825786A0 after 823FEBE8, human +2568 bit 0x80 clear), government
// car theft, police, flag 0, into the queue itself for this game's player
// whoever sat down (the human is only asked 82448D18). Another player's
// character's: dropped, like a carjack - their own game counts the act: a
// pull-out there is a carjack (824828C0; the crew copy wears the host's team)
// and the car's 0x200 bit then blocks its own 82482400, and a copy whose
// driver copy sat at the wheel carries team 5 in +53992 (825786A0 writes the
// driver's team there), so an empty police car taken counts there too; a
// never-crewed copy counts nowhere. Forwarded, one act was credited twice
// (review of 2.1.2).
WmlGuestFunction original_police_car_crime=nullptr;
void PoliceCarCrimeHook(WmlContext* c,uint8_t* b) {
  RunQueuingCrime(original_police_car_crime,c,b,connected.load() && share_police && AvatarSlotOf(uint32_t(api->get_r(c,3)))>=0 ? kCrimeDropped : -1,"a police car taken, 82482400");
}
// 820DB9D0(cop, event): a cop's or gang member's alert at the event's subject
// (the handle at event+36, 820D5FF0), from its AI (820CD398, 820B4BE8,
// 820DF280, 820CF520) while +3694 bit 0x40 and +3692 bit 0x08 (combat off, set
// on the copies here by MakePassive) are clear and its state +3716 is one of
// the table's. A player-type subject: a cop (team 5) puts police alert, a gang
// member of another gang than this player's gang alert, flag 1, into the queue
// itself for this game's player whoever the subject was (H); then its reaction
// to the subject, 820D2FB8. Another player's character's: sent to their game
// (the flag is set: no detection to run); the reaction runs here all the same.
WmlGuestFunction original_alert_crime=nullptr;
void AlertCrimeHook(WmlContext* c,uint8_t* b) {
  int culprit=-1;
  if(connected.load() && share_police) {
    const uint32_t cop=uint32_t(api->get_r(c,3)), event=uint32_t(api->get_r(c,4));
    // (a copy's own alert on a client - passers-by copies keep combat on - is
    // dropped, as their sightings are: the host's real member alerts there)
    if(!hosting.load() && Readable(cop,72) && OurCharacter(api->read_u32(cop+68))) culprit=kCrimeDropped;
    else if(Readable(event,40)) culprit=AvatarSlotOf(ResolveHuman(api->read_u32(event+36)));
  }
  RunQueuingCrime(original_alert_crime,c,b,culprit,"a cop's alert, 820DB9D0");
}
// 821D1F98(human, entry): the detection test (Seen), from its one caller
// 821D2840 on this game's player or from WitnessedFlag on another player's
// character. Logged with the nearest of the entry's team on the list (handle,
// distance, +120, +2996, in a car or not): whether the copies of the host's
// cops can pass the test's own skips at all, and what the forwarded flags do.
WmlGuestFunction original_detection=nullptr;
void DetectionHook(WmlContext* c,uint8_t* b) {
  const uint32_t human=uint32_t(api->get_r(c,3)), entry=uint32_t(api->get_r(c,4));
  original_detection(c,b);
  if(!connected.load() || !share_police || detection_poll) return;
  static unsigned logged=0;
  if(logged++>=20) return;
  const bool seen=api->get_r(c,3)&0xFF;
  const int slot=AvatarSlotOf(human);
  const int32_t team=entry<5 && entry!=3 ? int32_t(api->read_u32(0x8203A29C+entry*4)) : -1;
  char nearest[120]="";
  if(team>=0 && Readable(human,32)) {
    uint32_t best=0; float best_d=0;
    const uint32_t head=api->read_u32(0x8309ABB8);
    uint32_t h=head;
    for(unsigned guard=0;h && Readable(h,4252) && guard<512;++guard) {
      if(api->read_u32(h+232)==uint32_t(team)) {
        float d=0;
        for(unsigned i=0;i<3;++i) { const float x=api->read_f32(h+20+i*4)-api->read_f32(human+20+i*4); d+=x*x; }
        if(!best || d<best_d) { best=h; best_d=d; }
      }
      h=api->read_u32(h+3592);
      if(h==head) break;
    }
    if(best) std::snprintf(nearest,sizeof(nearest),"; nearest of team %d here: %08X at %.1f m, %s (+120 %08X, +2996 %d)",team,api->read_u32(best+68),std::sqrt(best_d),api->read_u32(best+2496)?"in a car":"on foot",api->read_u32(best+120),int32_t(api->read_u32(best+2996)));
    else std::snprintf(nearest,sizeof(nearest),"; none of team %d on the list here",team);
  }
  char line[220];
  std::snprintf(line,sizeof(line),"%s%s: crime detection: %s (entry %u)%s",slot>=0?remotes[slot].tag:"This player",slot>=0?"'s character":"",seen?"seen":"not seen",entry,nearest);
  ProbeLog(line);
}
// A crime of this player seen in the sender's game (kEventNotoriety): queued
// with 821D3148 as the game queues its own (its lock, its delay per activity,
// a full queue applied at once), with a rocket launcher's weapon info as far as
// 825BF760 reads it (+0 the name, "rpg_launcher" at 0x8205FA94; +20 = 4) when
// it was one. The activity indexes tables of 24 (0x827D65C8 names, 0x82B2BACC
// delays) and the team one from -1 (0x820387D8; 0-6 are teams, 821D2EE8 maps
// the rest to no entry): anything else is refused.
void ApplyCrime(GuestScratch& scratch,const Event& e,const char* who) {
  const int32_t team=int32_t(e.a[1]);
  if(e.a[0]>=24 || team<-1 || team>6) {
    ++crimes_refused;
    static unsigned logged=0;
    if(logged++<10) { char line[120]; std::snprintf(line,sizeof(line),"%s's game sent a crime of ours that isn't one (activity %u, team %d): refused",who,e.a[0],team); ProbeLog(line); }
    return;
  }
  uint32_t weapon=0;
  if(e.a[3]&1) {
    weapon=scratch.data()+2600;
    for(uint32_t i=0;i<24;i+=4) api->write_u32(weapon+i,0);
    api->write_u32(weapon,0x8205FA94); api->write_u32(weapon+20,4);
  }
  scratch.ctx.r3.u64=e.a[0]; scratch.ctx.r4.u64=uint64_t(int64_t(team)); scratch.ctx.r5.u64=e.a[2]&0xFF; scratch.ctx.r6.u64=weapon;
  ++crime_replay; scratch.Call(0x821D3148); --crime_replay;
  ++crimes_received;
  static unsigned logged=0;
  if(logged++<30) {
    char line[160];
    std::snprintf(line,sizeof(line),"%s's game saw a crime of ours: %s (team %d, flag %u%s): queued here",who,CrimeName(e.a[0]),team,e.a[2]&0xFF,weapon?", rocket launcher":"");
    ProbeLog(line);
  }
}
// 821D24F0(entry, gang): a notoriety entry's decay step (8247E850, for every
// player of the active list). Never on another player's character: its level
// is their game's, and a step calls 821D1A20 on this game's player (H).
WmlGuestFunction original_decay=nullptr;
unsigned decays_skipped=0;
bool AnyAvatar() { for(const Remote& r:remotes) if(r.avatar) return true; return false; }
void DecayHook(WmlContext* c,uint8_t* b) {
  const uint32_t entry=uint32_t(api->get_r(c,3)), g=uint32_t(api->get_r(c,4)), obj=entry-3868-24*g;
  // (no memory check in single player: another player's character first)
  if(share_police && g<5 && obj!=api->read_u32(0x8309ABEC) && AnyAvatar() && AvatarSlotOf(obj)>=0) {
    // Its +16 was armed here (the game's own hit credit 82481D50 -> 821D2CE0
    // -> 821D2468; a cop seeing its gun again, 82482290): back to -1, as
    // 821D24F0 leaves a finished entry (821D27F8), so this is once per arming.
    if(int32_t(api->read_u32(entry+16))>=0) { api->write_u32(entry+16,0xFFFFFFFFu); ++decays_skipped; }
    return;
  }
  original_decay(c,b);
}
// Busted. 82483440(target) arrests: from the cops' arrest 820B1A28, once a cop
// stands by a player-type target lying down - the bust check 820B1F90 wants
// busting on (byte 0x827D95C5), no multiplayer, a cop of team 5 within reach,
// the target down (+780) and not getting up (8/16), and no other cop already
// arresting it (820B1EB8 walks *0x8309ABB8 for +3716 == 2 with +3728 = the
// target's handle); neither asks whether the target is this game's player, so
// other players' characters qualify (H). It sets the target's busted bit (byte
// +3592, 0x20), then works on this game's player and screen whoever the target
// is: its +3594 flags, the mission's busted callbacks, the busted screen
// 82215360 (H). So:
// - another player's character: their game is told (kEventBusted, at most
//   every 3 s; not by a client's copies of the host's cops) and arrests its
//   player if it lies there too;
// - this game's player on a client showing the host's world: only through
//   the host's kEventBusted (its own cops there are copies).
int busted_replay=0;
unsigned busted_sent=0, busted_applied=0, busted_refused=0;
WmlGuestFunction original_busted=nullptr;
void BustedHook(WmlContext* c,uint8_t* b) {
  const uint32_t target=uint32_t(api->get_r(c,3));
  if(connected.load() && share_police && !busted_replay) {
    const int s=AvatarSlotOf(target);
    if(s>=0) {
      // (never the original on it: it would bust this player and screen)
      Remote& r=remotes[s];
      const uint64_t now=MonoMs();
      if(OwnPoliceOff()) {
        ++busted_refused;   // (a copy of the host's cops: the host's own arrests it there)
        static unsigned logged=0;
        if(logged++<10) ProbeLog((std::string("Arrest of ")+r.tag+"'s character by a copy of the host's cops refused").c_str());
      } else if(now>=r.next_busted) {
        r.next_busted=now+3000;
        QueueEventTo(Event{kEventBusted,{},0},uint8_t(s)); ++busted_sent;
        static unsigned logged=0;
        if(logged++<20) ProbeLog((std::string("A cop here arrests ")+r.tag+"'s character: sent to their game").c_str());
      }
      return;
    }
    if(target && target==api->read_u32(0x8309ABEC) && OwnPoliceOff()) {
      ++busted_refused;
      static unsigned logged=0;
      if(logged++<10) ProbeLog("Arrest of our player by a copy of the host's cops refused (the host's cops arrest)");
      return;
    }
  }
  original_busted(c,b);
}
// A cop in the sender's game arrested our character there (kEventBusted): this
// player is arrested too behind the bust check's own gates here (820B1F90:
// busting on, byte 0x827D95C5; no multiplayer; lying down, +780, and not
// getting up, 8/16), with a police level left (entry 4 >= 1: what the cops
// still chase) and not busted already (+3592 bit 0x20). The game's busted
// screen, respawn and notoriety reset follow; the new levels go out as usual.
void ApplyBusted(GuestScratch& scratch,const char* who) {
  const uint32_t player=api->read_u32(0x8309ABEC);
  const char* refused=nullptr;
  if(!Readable(player,4252)) refused="no player here";
  else if(!api->read_u8(0x827D95C5)) refused="busting is off here";
  else if(api->read_u8(0x8370E9F6)) refused="multiplayer here";
  else if(!api->read_u32(player+780)) refused="not down here";
  else if(api->read_u32(player+516)==8 && api->read_u32(player+520)==16) refused="getting up here";
  else if(int32_t(api->read_u32(player+3964))<1) refused="no police level left here";
  else if(api->read_u8(player+3592)&0x20) refused="busted here already";
  if(!refused) {
    scratch.ctx.r3.u64=player;
    ++busted_replay; ++replicated_depth; scratch.Call(0x82483440); --replicated_depth; --busted_replay;
    ++busted_applied;
  } else ++busted_refused;
  static unsigned logged=0;
  if(logged++<20) {
    char line[140];
    std::snprintf(line,sizeof(line),"%s's police arrested our character there: %s%s",who,refused?refused:"busted here too",refused?", refused":"");
    ProbeLog(line);
  }
}
// ---- Police response for a wanted client (2.1) ----
// The host's response manager 8242ABA0 (every frame from 824131F0; a client
// showing the host's world skips its own, PoliceResponseHook) sends the police
// cars, the helicopter and the gangs' cars after *0x8309ABEC alone (H, read):
// - the level a group works at, 823FCE40(group) = *0x8309ABEC+3868 + 24 *
//   821D2EE8(group+452) (lifted to +480 while +652 > 0), goes into the group
//   virtuals +4 / +8 (823FC930 / 823FC9F0: the car's per-level values, from
//   8242A228), +32 (823FD260: the level's models asked for), +40 (823FCC38 /
//   823FCAB8 / 823FCF88: at the group's lowest spawning level +480 yet?) and
//   821D0020 (an activity's helicopter); the spawn timers and squad pick
//   823FBFE0 (+36, from 82429E38 for each group) read that entry themselves
//   (823FC248); the police vehicle budget 823FCC80 (+44; the helicopter
//   group's +44, 823FCAF8, reads no player) and the gangs' 823FD008 (+44) and
//   823FCEB0 (+104, the dominant gang; the police and helicopter groups'
//   +104, 821B8578, says 1) read the player for 82481990 / 82481AB8 and
//   entry 4;
// - the spawn point 8242A540 rings 200 / 150 / 100 / 50 m around +20..+28;
// - the pursuit: 8242A228(group, car) (the group's +56, 823FC660, from
//   8242AFD0 once 82411118 made the car or 82411310 turned a traffic spawn
//   into it) gives *0x8309ABEC+68 to 825A91A0(car+50376, &order) (cars: order
//   +0 the handle, kept at +50468; +8 the kind, 2 = the player's level, to
//   car+50724 +8, +4 a fixed level to +12) or 8259DCC8(helicopter, handle)
//   (+54840: mode +684 = 2, target +160), after the crew is made (+0 =
//   8242A0B8 -> 82479AB0(4, ...)); 8242AFD0 also hands the handle to
//   8242A8A0, a speed from the player's distance to the new car, which its
//   caller drops;
// - while a car pursues (modes 4 / 10 / 11), the level it works at is
//   825A8828(car) = 82481AB8(*0x8309ABEC, car+53992 team) with kind 2 (its
//   timers in 825A91A0 and the mode-4 update's 825A97F8 / 825A8998 /
//   825A88C8), and 825A5C00(car, target, &out) (825A9080) gates a manoeuvre
//   on the same >= 2; the helicopter's 8259EFD8 level-5 case holds for the
//   player only.
// So while a client in this world (kPoseHostWorld) is the target, each of the
// six level and position readers above gets a hook that swaps *0x8309ABEC to
// its character around the original alone (PlayerSwap: back on every path,
// never across a frame); the two pursuit orders get the character's handle
// for the player's at their call sites in 8242A228 (LR 8242A378 / 8242A2F4)
// while 8242A228 runs for the police, helicopter or a gang group (slots 0-4
// of 0x8309A510, PursuitOrderHook; the activity spawners, 5-11, reach
// 8242A228 and 8242A540 for their own cars, which stay the player's); the
// spawn point at 8242ABA0's call (LR 8242AC94) while the group the spawners
// serve next (*0x8309A540) is one of those; and 825A8828 / 825A5C00 read the
// character's levels for a car after its handle (their only use of the
// player is 82481AB8). Nothing under the swapped calls reaches 821D1A20,
// 822F5300, 821D3148 / 82481B28, the camera, HUD, save or input: their
// callees are 821D2EE8, 82481990 / 82481AB8, the tables 823FBBC8 / 823FBD10 /
// 823FBE78, the timers 826291E0 / 82629278, the model lookups 82114A10 /
// 82418B60 (streaming requests), 82448010, the geometry 8259FAA0 / 8259FB50 /
// 8262F690 / 82534368 and the group's own +40 / +52 / +100 / +104 (read;
// 821D1A20 and 822F5300 are called from 821D2178, 821D2950, 821D2F50,
// 821D24F0 and 824C9D60 only). Not one swap around 8242ABA0 itself: that
// would run the car and crew creation (82569700, 82479AB0, 8257B2C0), the
// activity spawners' own virtuals and 8242ABA0's read of player+21252 on the
// character. That read stays the player's: 8242ABA0 picks no new group while
// player+21252 (the player's nearest road node, every 200 ms from 8256A000 ->
// 825910E0 -> 824A5B18; -1 indoors, swimming or off the roads) is -1, so a
// response for a client comes only while this player stands near a road
// (counted; never written here: it is the game's own). Roadblocks (82424708
// -> 824245B0 -> 82423F18, on the road the player drives) stay the player's.
// The gang groups work at the character's gang levels (the swap, not a max
// with the player's): a gang this player stands higher with still sends its
// cars after the target.
// What the game does at a level change stays the player's too: 821D1A20(0,
// gang) takes the player's handle off the team's people's enemy lists, tells
// the ones attacking it to stop (event 61) and calls off the team's cars
// whose pursuit target is the player (820C6A58: +50468 in modes 4 / 10 / 11
// of +50376, +51324 in mode 3) with 825B9B10(car+50376, 0), 825419F0(handle,
// 0) and 825255E0(handle, 0) (siren lights off); nothing dismisses a car
// aimed at another handle (the helicopter's pursue update 8259DD80 leaves,
// 825A3000, only once its target is dead or gone). So Forgive does the people
// and CallOffPursuit the car pass for a character's handle: when a level
// mirrored on it drops to 0 (that team's cars) and when the target changes or
// goes (every team, and the helicopter the way its own update ends a pursuit).
// The target: the highest police level among this player and the clients
// whose game shows this world (kPoseHostWorld: a client's own police are off
// until it is 200 m from this player, the shared world's radius; 2.2: while it
// is a member of this game's area, past member_leave_m it leaves), on their
// character here within 200 m (the response spawns 50-200 m around it, in
// the world streamed around this player); gang levels decide among equal
// police levels (the gangs' groups follow the same target). It is kept 10 s
// at least, until it is over 230 m away, no longer wanted or gone, or a
// strictly higher level takes over. This player as the target means no swap
// at all: the game's own response. The response's units on foot and cars go
// to the clients with the people and traffic (CapturePeds, CaptureTraffic).
// 821D2EE8: a team's notoriety entry (0 Playas -> 3, 1 -> 0, 2 -> 1, 3 -> 2, 5 police -> 4; 4 and over 5: none).
inline int NotorietyEntry(uint32_t team) { constexpr int entry[6]={3,0,1,2,-1,4}; return team<6 ? entry[team] : -1; }
int response_slot=-1;        // whose character the response is sent after (-1: this player)
uint32_t response_handle=0;  // that character's handle here
uint64_t response_since=0;   // when it became the target
unsigned response_rings=0, response_steered=0, response_called_off=0, response_helis_off=0, response_changes=0, response_offroad=0;
// The character the response is steered to, or 0 (this player's own turn, or
// no session): the handle's object in the table, a player-pool person (+224 ==
// 1) - whatever *0x8309ABEC says (ResolveHuman refuses the object there, which
// is this character inside a swap).
uint32_t ResponseAvatar() {
  if(!response_handle || !hosting.load() || !connected.load() || !share_police || (response_handle&0xffff)>=4096) return 0;
  const uint32_t obj=api->read_u32(kObjectTable+12+(response_handle&0xffff)*16);
  return Readable(obj,4252) && api->read_u32(obj+68)==response_handle && api->read_u32(obj+72)==1 && api->read_u32(obj+224)==1 ? obj : 0;
}
// A response group's slot in 0x8309A510 (0 police, 1 helicopter, 2-4 gangs), or -1 (an activity spawner's, 5-11, or none).
int ResponseGroupSlot(uint32_t group) {
  for(int i=0;group && i<5;++i) if(group==api->read_u32(kResponseGroups+i*4)) return i;
  return -1;
}
// The target for a group's level read: only a response group (slots 0-4; an
// activity spawner's, 5-11, reads the player) whose team has an entry
// (821D2EE8 gives -1 for team 4 and over 5; 823FCE40 would read +3844 then).
uint32_t ResponseTargetFor(uint32_t group) {
  const uint32_t obj=ResponseAvatar();
  return obj && ResponseGroupSlot(group)>=0 && Readable(group,456) && NotorietyEntry(api->read_u32(group+452))>=0 ? obj : 0;
}
const char* AvatarTag(uint32_t handle) { for(const Remote& r:remotes) if(handle && r.avatar==handle) return r.tag; return "another player"; }
WmlGuestFunction original_group_level=nullptr, original_group_timers=nullptr, original_police_budget=nullptr, original_gang_budget=nullptr, original_gang_dominant=nullptr,
                 original_spawn_ring=nullptr, original_pursuit_order=nullptr, original_pursuit=nullptr, original_heli_pursuit=nullptr, original_pursuit_level=nullptr, original_pursuit_manoeuvre=nullptr;
void GroupLevelHook(WmlContext* c,uint8_t* b) { PlayerSwap swap(ResponseTargetFor(uint32_t(api->get_r(c,3)))); original_group_level(c,b); }    // 823FCE40(group)
void GroupTimersHook(WmlContext* c,uint8_t* b) { PlayerSwap swap(ResponseTargetFor(uint32_t(api->get_r(c,3)))); original_group_timers(c,b); }  // 823FBFE0(group)
void PoliceBudgetHook(WmlContext* c,uint8_t* b) { PlayerSwap swap(ResponseAvatar()); original_police_budget(c,b); }  // 823FCC80(group)
void GangBudgetHook(WmlContext* c,uint8_t* b) { PlayerSwap swap(ResponseAvatar()); original_gang_budget(c,b); }      // 823FD008(group)
void GangDominantHook(WmlContext* c,uint8_t* b) { PlayerSwap swap(ResponseAvatar()); original_gang_dominant(c,b); }  // 823FCEB0(group)
// 8242A540(&position, &facing) at 8242ABA0's call (LR 8242AC94), for the group the spawners serve next.
void SpawnRingHook(WmlContext* c,uint8_t* b) {
  const int slot=api->get_lr(c)==0x8242AC94u ? ResponseGroupSlot(api->read_u32(0x8309A540)) : -1;
  PlayerSwap swap(slot>=0 ? ResponseAvatar() : 0);
  if(swap.on) {
    ++response_rings;
    static unsigned logged=0;
    if(logged++<10) { char line[120]; std::snprintf(line,sizeof(line),"Police response: group %d's spawn point picked around %s's character",slot,AvatarTag(response_handle)); ProbeLog(line); }
  }
  original_spawn_ring(c,b);
}
// 8242A228(group, car): which group orders the pursuit the two hooks below see
// (a response group's slot, or -1: an activity spawner's, or no target).
thread_local int pursuit_group=-1; thread_local uint32_t pursuit_car=0;
void PursuitOrderHook(WmlContext* c,uint8_t* b) {
  const int outer=pursuit_group; const uint32_t outer_car=pursuit_car;
  pursuit_group=response_handle ? ResponseGroupSlot(uint32_t(api->get_r(c,3))) : -1; pursuit_car=uint32_t(api->get_r(c,4));
  original_pursuit_order(c,b);
  pursuit_group=outer; pursuit_car=outer_car;
}
void PursuitLog(const char* what,uint32_t car) {   // the first steered orders
  static unsigned logged=0;
  if(logged++>=10) return;
  char line[160];
  std::snprintf(line,sizeof(line),"Police response: %s of group %d, car %08X, after %s's character",what,pursuit_group,Readable(car,72)?api->read_u32(car+68):0,AvatarTag(response_handle));
  ProbeLog(line);
}
// 825A91A0(ai, order) from 8242A228 (LR 8242A378): the order's handle, this player's, becomes the target's.
void PursuitHook(WmlContext* c,uint8_t* b) {
  if(const uint32_t obj=ResponseAvatar(); obj && pursuit_group>=0 && api->get_lr(c)==0x8242A378u) {
    const uint32_t order=uint32_t(api->get_r(c,4)), player=api->read_u32(0x8309ABEC);
    if(Readable(order,20) && Readable(player,72) && api->read_u32(order)==api->read_u32(player+68)) { api->write_u32(order,api->read_u32(obj+68)); ++response_steered; PursuitLog("pursuit order",pursuit_car); }
  }
  original_pursuit(c,b);
}
// 8259DCC8(helicopter, handle) from 8242A228 (LR 8242A2F4): the same for the helicopter.
void HeliPursuitHook(WmlContext* c,uint8_t* b) {
  if(const uint32_t obj=ResponseAvatar(); obj && pursuit_group>=0 && api->get_lr(c)==0x8242A2F4u) {
    const uint32_t player=api->read_u32(0x8309ABEC);
    if(Readable(player,72) && uint32_t(api->get_r(c,4))==api->read_u32(player+68)) { api->set_r(c,4,api->read_u32(obj+68)); ++response_steered; PursuitLog("helicopter pursuit order",uint32_t(api->get_r(c,3))); }
  }
  original_heli_pursuit(c,b);
}
// A car after the target's character (modes 4 / 10 / 11 of +50376, +50468 its
// handle) works at the character's levels, not this player's: 825A8828(car)
// and 825A5C00(car, target, &out) read the player for 82481AB8 alone.
uint32_t PursuingAvatar(uint32_t car) {
  if(!response_handle || !Readable(car,kVehicleSize)) return 0;
  const uint32_t mode=api->read_u32(car+50376);
  return (mode==4 || mode==10 || mode==11) && api->read_u32(car+50468)==response_handle ? ResponseAvatar() : 0;
}
void PursuitLevelHook(WmlContext* c,uint8_t* b) { PlayerSwap swap(PursuingAvatar(uint32_t(api->get_r(c,3)))); original_pursuit_level(c,b); }          // 825A8828(car)
void PursuitManoeuvreHook(WmlContext* c,uint8_t* b) { PlayerSwap swap(PursuingAvatar(uint32_t(api->get_r(c,3)))); original_pursuit_manoeuvre(c,b); }  // 825A5C00(car, target, &out)
// The game's vehicle list: *0x83289904, next at +50308 (821D1A20 walks it).
template<class F> void EachVehicle(F&& f) {
  const uint32_t head=api->read_u32(0x83289904);
  uint32_t v=head;
  for(unsigned guard=0;v && guard<1024 && Readable(v,kVehicleSize);++guard) {
    const uint32_t cur=v;
    v=api->read_u32(cur+50308);
    if(v==head) v=0;
    f(cur);
  }
}
// A helicopter after a handle: its AI block +54840 in mode (+684) 2, the
// pursuit (8259DCC8 / 825A0560), or 1, over the target (825A0F18 from the
// pursuit update 8259DD80; it keeps +160), with +160 the handle. 825A3000's
// mode 4 (target gone, or called off) leaves +160 as it was (2.1.4: the
// Police line counted mode 2 alone, 0 helicopters while four attacked).
bool HeliAfter(uint32_t v,uint32_t handle) { const uint32_t m=api->read_u32(v+54840+684); return (m==1 || m==2) && api->read_u32(v+54840+160)==handle; }
// 821D1A20's car pass for a character's handle: every car after it (modes 4 /
// 10 / 11 of +50376 with +50468 the handle, mode 3 with +51324), driven by the
// team (-1: any), is called off as the game calls off the player's at level 0;
// the helicopter after it (police) leaves with 825A3000, its own end of a
// pursuit. 821D1A20's skips: +120 bits 0x80000000 / 0x40000000 / 0x10000000 /
// 0x08000000 (gone, going, a mission's...), a car with +50300 == 1 and no
// seats (+46448 <= 0), no driver, one of another team or a mission's (+224 ==
// 2 with +3695 & 8).
void CallOffPursuit(GuestScratch& scratch,uint32_t handle,int32_t team,const char* why,const char* who) {
  unsigned cars=0, helis=0;
  EachVehicle([&](uint32_t v) {
    if((api->read_u32(v+120)&0xD8000000u) || (api->read_u8(v+50300)==1 && int32_t(api->read_u32(v+46448))<=0)) return;
    const uint32_t info=api->read_u32(v+50324);
    if(Readable(info,288) && api->read_u32(info+284)==6) {   // the helicopter: its own AI at +54840 (8259DCC8)
      if((team<0 || team==5) && HeliAfter(v,handle)) { scratch.ctx.r3.u64=v; scratch.Call(0x825A3000); ++helis; }
      return;
    }
    if(team>=0) {
      const uint32_t driver=ResolveHuman(api->read_u32(v+52316));
      if(!driver || int32_t(api->read_u32(driver+232))!=team || (api->read_u32(driver+224)==2 && (api->read_u8(driver+3695)&8))) return;
    }
    const uint32_t ai=v+50376, mode=api->read_u32(ai), car=api->read_u32(v+68);
    if((mode==4 || mode==10 || mode==11) && api->read_u32(ai+92)==handle) { scratch.ctx.r3.u64=ai; scratch.ctx.r4.u64=0; scratch.Call(0x825B9B10); }
    else if(mode==3 && api->read_u32(v+51324)==handle) api->write_u32(v+51324,0);
    else return;
    scratch.ctx.r3.u64=car; scratch.ctx.r4.u64=0; scratch.Call(0x825419F0);
    scratch.ctx.r3.u64=car; scratch.ctx.r4.u64=0; scratch.Call(0x825255E0);
    ++cars;
  });
  response_called_off+=cars; response_helis_off+=helis;
  static unsigned logged=0;
  if((cars || helis) && logged++<40) {
    char line[160];
    std::snprintf(line,sizeof(line),"Police response: %u cars and %u helicopters after %s's character called off (%s)",cars,helis,who?who:AvatarTag(handle),why);
    ProbeLog(line);
  }
}
// The cars after a handle now (modes 4 / 10 / 11 of +50376 with +50468 the handle: 825A91A0 stores 4, 17 and +92) and the helicopters (HeliAfter), for the Police line.
void CarsAfter(uint32_t handle,unsigned& cars,unsigned& helis) {
  cars=helis=0;
  EachVehicle([&](uint32_t v) {
    const uint32_t mode=api->read_u32(v+50376), info=api->read_u32(v+50324);
    if(Readable(info,288) && api->read_u32(info+284)==6) { if(HeliAfter(v,handle)) ++helis; }
    else if((mode==4 || mode==10 || mode==11) && api->read_u32(v+50468)==handle) ++cars;
  });
}
// The target's character goes (DestroyAvatar: before its release): the response after it is called off, this player is the target again.
void ResponseTargetGone(GuestScratch& scratch,uint32_t handle) {
  if(!handle || handle!=response_handle) return;
  CallOffPursuit(scratch,handle,-1,"their character goes");
  response_slot=-1; response_handle=0; response_since=MonoMs(); ++response_changes;
  static unsigned logged=0;
  if(logged++<40) ProbeLog((std::string("Police response: after this player, was after ")+AvatarTag(handle)+"'s character: it goes").c_str());
}
// A character's standing as the target: the police level first, the highest gang level after it.
int ResponseScore(uint32_t obj) {
  if(!Readable(obj,4252)) return 0;
  int gang=0;
  for(uint32_t g=0;g<4;++g) gang=std::max(gang,std::clamp<int32_t>(int32_t(api->read_u32(obj+3868+24*g)),0,5));
  return std::clamp<int32_t>(int32_t(api->read_u32(obj+3964)),0,5)*8+gang;
}
// Every update: the target (above). A target left has its units called off.
void UpdateResponseTarget(GuestScratch& scratch,uint32_t player,uint64_t now) {
  const bool on=hosting.load() && connected.load() && share_police && Readable(player,4252);
  struct Candidate { int slot; uint32_t handle; int score; float d2; };
  Candidate cands[kSlots]; int n=0; const Candidate* cur=nullptr;
  if(on) {
    const uint8_t me=my_slot.load();
    for(int s=0;s<kSlots;++s) {
      const Remote& r=remotes[s];
      if(s==me || !r.player_obj || !r.avatar || r.notoriety_on!=r.avatar) continue;   // (their levels on it, UpdateNotoriety)
      const uint32_t obj=AvatarObjOf(s);
      if(!obj || api->read_u32(obj+224)!=1) continue;
      bool here; { std::lock_guard lock(pose_mutex); here=Present(s) && (remote_in[s].pose.flags&kPoseHostWorld)!=0; }
      if(!here) continue;
      const float dx=api->read_f32(obj+20)-api->read_f32(player+20), dz=api->read_f32(obj+28)-api->read_f32(player+28);
      cands[n]={s,r.avatar,ResponseScore(obj),dx*dx+dz*dz};
      if(s==response_slot) cur=&cands[n];
      ++n;
    }
  }
  // Their character made again in one update (a death, out of range and back;
  // ResolveHuman failed on the old handle before DestroyAvatar could run): the
  // new one stays the target, the cars after the old handle are called off.
  if(cur && cur->handle!=response_handle) {
    if(response_handle) CallOffPursuit(scratch,response_handle,-1,"their character was made again",remotes[cur->slot].tag);
    response_handle=cur->handle;
  }
  const char* why=nullptr;   // why the current target (another player's character) is left
  if(response_slot>=0) {
    if(!on) why="session over";
    else if(!cur) why="their character is gone, or their game left this world";
    else if(!cur->score) why="no level left on their character";
    else if(cur->d2>230.0f*230.0f) why="over 230 m from this player";
  }
  const int32_t node=on ? int32_t(api->read_u32(player+21252)) : -1;   // this player's road node: -1 and 8242ABA0 starts no group
  if(response_slot>=0 && !why && node==-1) ++response_offroad;
  const int own=on ? ResponseScore(player) : 0;
  const int cur_score=response_slot>=0 ? (cur && !why ? cur->score : 0) : own;
  const Candidate* best=nullptr;
  for(int i=0;i<n;++i) if(&cands[i]!=cur && cands[i].score && cands[i].d2<=200.0f*200.0f && (!best || cands[i].score>best->score)) best=&cands[i];
  int next;
  if(why) next=best && best->score>own ? best->slot : -1;
  else {
    if(cur_score && now<response_since+10000) return;   // (kept 10 s at least)
    if(std::max(own,best?best->score:0)<=cur_score) return;
    next=best && best->score>own ? best->slot : -1;
    why=next>=0 ? "a higher level there" : "this player's level is higher";
  }
  const int old=response_slot; const uint32_t old_handle=response_handle;
  response_slot=next; response_handle=0; response_since=now; ++response_changes;
  for(int i=0;i<n;++i) if(cands[i].slot==next) response_handle=cands[i].handle;
  if(old>=0) CallOffPursuit(scratch,old_handle,-1,why,remotes[old].tag);
  static unsigned logged=0;
  if(logged++<40) {
    const int score=next>=0 ? best->score : own;
    char line[220];
    std::snprintf(line,sizeof(line),"Police response: after %s%s (police %d, gangs %d), was after %s%s: %s; this player's road node %d",next>=0?remotes[next].tag:"this player",next>=0?"'s character":"",score/8,score%8,
                  old>=0?remotes[old].tag:"this player",old>=0?"'s character":"",why,node);
    ProbeLog(line);
  }
}
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
  WorldPeopleLocals(ours,true);   // (the world stream's copies, 2.2)
  const uint32_t replica=ResolveHuman(replica_local);
  std::vector<uint32_t> remove;
  const uint32_t head=api->read_u32(kAmbientPeds);
  uint32_t obj=head;
  for(unsigned guard=0;obj && guard<128 && Readable(obj,4252);++guard) {
    const uint32_t cur=obj;
    obj=api->read_u32(cur+3600);
    if(obj==head) obj=0;
    if(cur==player || cur==replica || api->read_u32(cur+72)!=1 || AvatarSlotOf(cur)>=0) continue;
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
  if(!obj || obj==player || AvatarSlotOf(obj)>=0) return false;
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
  if(!obj || obj==player || AvatarSlotOf(obj)>=0) return false;
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
  uint32_t best=0; float best_d2=80.0f*80.0f;  // (they move away from where they spawned)
  char name[46];
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t cur=api->read_u32(kObjectTable+12+index*16);
    if(!Readable(cur,4252) || api->read_u32(cur+72)!=1 || (api->read_u32(cur+68)&0xffff)!=index) continue;
    if(cur==player || (api->read_u32(cur+120)&0xF0000000u) || AvatarSlotOf(cur)>=0) continue;
    if(SharedPedHostHandle(api->read_u32(cur+68))) continue;
    if(PlayerFollower(player,api->read_u32(cur+68))) continue;   // 2.1: never this player's homies (the minimap draws them purple)
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
// A model a copy waits for gets one extra reference, held while copies need
// it. 2.2: a people model (group 0) at level 0, where the game asks and
// where PeopleQuota keeps every slot: the pass 8250D3E0(group, level) pairs a
// level's requests only with that level's slots, and the top level had only
// the slots 8241BD40 last moved there (the "8 loaded" plateau of every
// client log, then 4-6, while slots sat idle at levels nobody asked at:
// notes/coop-2.1/model_refcount_investigation.md). Other groups (cars) keep
// the top level.
struct ModelPin { int level=0; uint64_t used=0; bool held=false; };   // held: a reference was taken (released with the pin)
bool pin_top=true;
bool keep_worn_models=true;  // mod.ini keep_worn_models: worn models keep their top-level request
std::unordered_map<uint32_t,ModelPin> model_pins; // model id -> pin (joiner, game thread)
unsigned pins_made=0, pins_released=0;
unsigned pins_people=0;   // (of pins_made: people models, at level 0)
void PinModel(GuestScratch& scratch,uint32_t id,uint64_t now) {
  auto it=model_pins.find(id);
  if(it!=model_pins.end()) { it->second.used=now; return; }
  const bool people=(id>>24)==0;
  const uint32_t entry=FindStreamEntry(id);
  int levels=entry ? int(api->read_u32(entry+8)) : (people ? 14 : 0);  // not asked for yet: people models have 14 levels
  if(!people && (levels<=1 || levels>32)) { model_pins[id]=ModelPin{0,now,false}; return; } // nothing higher to ask for
  // mod.ini pin_top (other groups): 1 = top level as before. 0 = the lowest.
  const int level=people ? 0 : pin_top ? levels-1 : 1;
  scratch.ctx.r3.u64=kStreamTable+(id>>24)*204; scratch.ctx.r4.u64=id;
  scratch.ctx.r5.u64=5; scratch.ctx.r6.u64=uint64_t(level); scratch.ctx.r7.u64=1;
  scratch.Call(0x8250C750);
  model_pins[id]=ModelPin{level,now,FindStreamEntry(id)!=0};   // (8250C750 takes nothing without a free request record)
  ++pins_made; if(people) ++pins_people;
}
void ReleasePins(GuestScratch& scratch,uint64_t now,bool all) {
  for(auto it=model_pins.begin();it!=model_pins.end();) {
    if(!all && now-it->second.used<4000) { ++it; continue; }
    if(it->second.held) {
      scratch.ctx.r3.u64=kStreamTable+(it->first>>24)*204; scratch.ctx.r4.u64=it->first;
      scratch.ctx.r5.u64=5; scratch.ctx.r6.u64=uint64_t(it->second.level); scratch.ctx.r7.u64=0;
      scratch.Call(0x8250CA48);
      ++pins_released;
    }
    it=model_pins.erase(it);
  }
}
// 2.2: the people group's per-level slot quota, the live table 0x827ACFFC
// (14 words; 8241CA80 copies it into the group's +196, 8241BD40 recomputes
// it from the kind-5 requests per level and never shrinks a level). Applied
// with 8250F780(0, table, 14): it copies each changed word to the group's
// array and sets the group's dirty bit (+200 0x80), so the next pass's
// balancer 8250D0A8 moves the surplus slots of each level to the levels below
// quota. On a client (co-op's pins at level 0, PinModel) level 0 gets every
// slot (the group's +0 count, people_models included) and the rest none,
// once a second when it differs; the game's own table (8 1 1 0 1 1 0 ...)
// goes back when the session ends.
std::array<uint32_t,14> quota_saved{}; bool quota_set=false; unsigned quota_writes=0;
void PeopleQuota(GuestScratch& scratch,bool on) {
  constexpr uint32_t kQuota=0x827ACFFC;
  const uint32_t group=kStreamTable;
  if((!on && !quota_set) || !Readable(kQuota,56) || !Readable(group,204) || (api->read_u8(group+200)&0x20)) return;
  std::array<uint32_t,14> want{};
  if(on) {
    const uint32_t slots=api->read_u32(group+0);
    if(!slots || slots>64) return;
    want[0]=slots;
  } else want=quota_saved;
  bool same=true;
  for(unsigned i=0;i<14;++i) same=same && api->read_u32(kQuota+i*4)==want[i];
  if(!same) {
    if(on && !quota_set) { for(unsigned i=0;i<14;++i) quota_saved[i]=api->read_u32(kQuota+i*4); quota_set=true; }
    char line[200]; std::string was;
    for(unsigned i=0;i<14;++i) { was+=" "+std::to_string(api->read_u32(kQuota+i*4)); api->write_u32(kQuota+i*4,want[i]); }
    scratch.ctx.r3.u64=0; scratch.ctx.r4.u64=kQuota; scratch.ctx.r5.u64=14;
    scratch.Call(0x8250F780);
    ++quota_writes;
    static unsigned logged=0;
    if(logged++<10) {
      std::snprintf(line,sizeof(line),"People-model slot quota %s: 827ACFFC was%s, now %u at level 0%s",on ? "set" : "put back",was.c_str(),want[0],on ? " and none above (co-op's pins are at level 0)" : " and the game's own above");
      ProbeLog(line);
    }
  }
  if(!on) quota_set=false;
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
  { std::vector<uint32_t> world; WorldPeopleLocals(world,false); for(uint32_t h:world) consider(ResolveHuman(h)); }   // (the world stream's, 2.2)
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
    const uint32_t h=TableObject(index,1,4252);
    if(!h) continue;
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
    // (the characters' references went with the request: a later release
    // would hit the next request made for the id)
    for(auto it=actor_models.begin();it!=actor_models.end();) it=it->second.id==id ? actor_models.erase(it) : std::next(it);
    scratch.ctx.r3.u64=group; scratch.ctx.r4.u64=id; scratch.ctx.r5.u64=5; scratch.ctx.r6.u64=0; scratch.ctx.r7.u64=1;
    scratch.Call(0x8250CA48);
    ++stale_models_cleared;
  }
}
// 2.1.4 guard, every 10 s: a model's references of kind 5 (entry+64) against
// what holds them here - its wearers (one per character made here, the
// local player's own) and its pin (PinModel, one). More is a reference
// nobody gives back (the leak ReleaseActorModel closes, or a path it
// misses): logged and let go, one release each. The players' own models are
// left alone (the game's own kind-5 references on them: 8247DDF0).
unsigned model_refs_excess=0;
void ModelRefGuard(GuestScratch& scratch) {
  const uint32_t group=kStreamTable;
  if(!Readable(group,204) || (api->read_u8(group+200)&0x20)) return;
  const uint32_t player=api->read_u32(0x8309ABEC);
  std::unordered_map<uint32_t,unsigned> wearers; std::vector<uint32_t> own;
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t h=TableObject(index,1,4252);
    if(!h) continue;
    const uint32_t desc=api->read_u32(h+228);
    if(!Readable(desc,612)) continue;
    const uint32_t id=api->read_u32(desc+608);
    ++wearers[id];
    if(h==player || AvatarSlotOf(h)>=0) own.push_back(id);
  }
  if(wearers.empty()) return;   // table not readable: leave everything alone
  std::vector<std::pair<uint32_t,unsigned>> excess;
  const uint32_t head=api->read_u32(group+48);
  unsigned guard=0;
  for(uint32_t e=head;e && Readable(e,72) && guard++<256;) {
    const uint32_t id=api->read_u32(e+4), kind5=api->read_u32(e+64);
    const auto w=wearers.find(id); const auto p=model_pins.find(id);
    const unsigned held=(w!=wearers.end() ? w->second : 0)+(p!=model_pins.end() && p->second.held ? 1 : 0);
    if(kind5>held && kind5<0x10000 && held && std::find(own.begin(),own.end(),id)==own.end()) excess.push_back({id,kind5-held});
    e=api->read_u32(e+28); if(e==head) break;
  }
  for(auto [id,n]:excess) {
    static unsigned logged=0;
    // (logged only this build: a reference the game itself holds on a model
    // only copies wear - a script's preload - would be let go too, and its
    // own release later would take the request below zero: immortal)
    if(logged++<20) { char line[160]; std::snprintf(line,sizeof(line),"Model %X: %u references of kind 5 more than its wearers and pin here",id,n); ProbeLog(line); }
    (void)scratch; model_refs_excess+=n;
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
    std::snprintf(t,sizeof(t)," [%u:%X %X %X %X %X L%u]",api->read_u32(sl+36),api->read_u32(sl+0),api->read_u32(sl+4),api->read_u32(sl+8),api->read_u32(sl+12),api->read_u32(sl+16),api->read_u32(sl+32));
    l+=t;
  }
  ProbeLog(l.c_str());
  // 2.1.4: the slots' levels (L, slot+32) and the per-level slot quota
  // (0x827ACFFC, 14 words, group 0; 8241BD40 recomputes it from the kind-5
  // requests per level, 8241CA80 copies it into the group): the pass
  // 8250D3E0(group, level) pairs a level's requests only with that level's
  // slots, and co-op asks at the top level (PinModel) - the "8 loaded" plateau.
  if(Readable(0x827ACFFC,56)) {
    std::string q="  slot quota per level 827ACFFC:";
    for(unsigned i=0;i<14;++i) { std::snprintf(t,sizeof(t)," %u",api->read_u32(0x827ACFFC+i*4)); q+=t; }
    ProbeLog(q.c_str());
  }
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
    { std::vector<uint32_t> world; WorldPeopleLocals(world,false); for(uint32_t h:world) if(const uint32_t m=model_of(h)) real.push_back(m); }   // (the world stream's copies, 2.2)
    const size_t before=candidates.size();
    candidates.erase(std::remove_if(candidates.begin(),candidates.end(),[&](uint32_t id){return std::find(real.begin(),real.end(),id)!=real.end();}),candidates.end());
    static unsigned logged=0;
    if(before!=candidates.size() && logged++<10) { char line[140]; std::snprintf(line,sizeof(line),"Model memory: %zu models the host's people wear here kept (not in the host's list)",before-candidates.size()); ProbeLog(line); }
    if(candidates.empty()) return;
  }
  const uint32_t player=api->read_u32(0x8309ABEC);
  for(uint32_t id:candidates) {
    std::vector<uint32_t> users; bool blocked=false;
    for(uint32_t index=0;index<4096 && !blocked;++index) {
      const uint32_t h=TableObject(index,1,4252);
      if(!h) continue;
      const uint32_t desc=api->read_u32(h+228);
      if(!Readable(desc,612) || api->read_u32(desc+608)!=id) continue;
      if(h==player || AvatarSlotOf(h)>=0 || api->read_u32(h+2496)) blocked=true;  // not people in cars (safer)
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
  auto effect=[&](uint32_t id,SharedPed& sp) {
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
  };
  for(auto& [id,sp]:shared_peds) effect(id,sp);
  ForWorldPeople(effect);   // (the world stream's copies, 2.2)
  for(auto it=joiner_effects.begin();it!=joiner_effects.end();) it=now-it->second.seen>10000 ? joiner_effects.erase(it) : std::next(it);
  if(now>=next_log && (effects_added || effects_removed)) {
    next_log=now+10000;
    char line[120]; std::snprintf(line,sizeof(line),"Attack arrows (in-game effects) from the host: %u put on copies, %u taken off, %zu listed now",effects_added,effects_removed,joiner_effects.size());
    ProbeLog(line); effects_added=0; effects_removed=0;
  }
}
void UpdateSharedPeds(GuestScratch& scratch,uint32_t player,uint64_t now) {
  const float d2=DistanceTo2(0);   // to the host
  // Not during a cutscene here: it brings its own characters (the copies
  // doubled them).
  const bool in_range=connected.load() && running.load() && share_peds &&
                      (WorldMode() ? WorldLeversOn() : peds_shared ? d2<200.0f*200.0f : d2<120.0f*120.0f);   // (2.2, world mode: as the traffic)
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
      // A few per update (1.63), as with the cars.
      static uint64_t next_drop=0;
      if(now<next_drop) return;
      next_drop=now+50;
      unsigned dropped=0;
      for(auto it=shared_peds.begin();it!=shared_peds.end() && dropped<3;) {
        if(ResolveHuman(it->second.handle)) { DropCopy(scratch,it->second); ++peds_removed; }
        it=shared_peds.erase(it); ++dropped;
      }
      if(!shared_peds.empty()) return;
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
  if(WorldMode()) {
    // 2.2 (mod.ini world = 1): the world stream shows the host's people here;
    // 2.1's copies go (three an update, 1.63) and none are made (the models
    // below are still looked after).
    peds.clear();
    unsigned dropped=0;
    for(auto it=shared_peds.begin();it!=shared_peds.end() && dropped<3;++dropped) { DropCopy(scratch,it->second); ++peds_removed; it=shared_peds.erase(it); }
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
    sp.seen=now; sp.sent_at=now; sp.flags=p.flags; sp.anim=p.anim; sp.host_action=p.action; sp.team=p.team;
    sp.follow.Add(time,p.pos,rows,now);
  }
  for(auto it=shared_peds.begin();it!=shared_peds.end();) {
    if(WorldMode()) break;   // (2.2: going, above)
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
    // 2.1: the host's team on whatever shows this person (and on the copy
    // waiting for its model behind a stand-in).
    {
      auto n=names.find(sp.model);
      const char* host_model=n!=names.end() ? n->second.c_str() : "";
      ShowHostTeam(obj,sp.team,sp.adopted?"own character taken over":sp.pending?"stand-in":sp.standin?"lookalike":"copy",host_model,player);
      if(sp.pending) ShowHostTeam(ResolveHuman(sp.pending),sp.team,"copy waiting for its model",host_model,player);
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
          actor_models.erase(sp.handle); NoteActorModel(sp.handle,id);   // (the first reference went with the dropped request)
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
      // combat here, and their own shots are dropped (FireHook): the host's
      // people's hits come across.
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
      if(WorldShown() && std::find(groups.begin(),groups.end(),0u)==groups.end()) groups.push_back(0);   // (the world stream's copies: the people group, 2.2)
      for(uint32_t g:groups) if(g<64) DropOwnModelRefs(scratch,g);
    }
  }
  if(now>=next_log) {
    next_log=now+10000;
    char line[480];
    {
      const uint32_t group=kStreamTable;
      unsigned loaded=0, waiting=0, guard=0;
      if(Readable(group,204)) { const uint32_t head=api->read_u32(group+48);
        for(uint32_t e=head;e && Readable(e,72) && guard++<256;) { if(api->read_u32(e)) ++loaded; else ++waiting; e=api->read_u32(e+28); if(e==head) break; } }
      ModelRefGuard(scratch);
      std::snprintf(line,sizeof(line),"People models here: %u loaded, %u waiting for room; %u unused models let go, %u not in the host's set made room, %u stuck slots freed; %u characters' references given back (%u for characters the game removed), %u over their wearers (seen, not let go this build)",
                    loaded,waiting,stale_models_cleared,models_evicted,slots_freed,actor_refs_released,actor_refs_swept,model_refs_excess); ProbeLog(line); stale_models_cleared=0; models_evicted=0; slots_freed=0; actor_refs_released=actor_refs_swept=model_refs_excess=0;
      { std::string l="  loaded:", w="  waiting:", h="  host has:"; char t[16];
        if(Readable(group,204)) { const uint32_t head=api->read_u32(group+48); unsigned g2=0;
          for(uint32_t e=head;e && Readable(e,72) && g2++<256;) { unsigned refs=0; for(unsigned k=0;k<7;++k) refs+=api->read_u32(e+44+k*4);
            std::snprintf(t,sizeof(t)," %X(%u)",api->read_u32(e+4),refs); (api->read_u32(e)?l:w)+=t; e=api->read_u32(e+28); if(e==head) break; } }
        for(uint32_t id:host_models) { std::snprintf(t,sizeof(t)," %X",id); h+=t; }
        ProbeLog((l+w+h).c_str()); LogPeopleSlots("  slots here:"); }
    }
    if(pins_made || pins_released) {
      std::snprintf(line,sizeof(line),"Model priority: %u models pinned (people %u at level 0, cars at the top level), %u released, %zu held; slot quota written %u times (level 0 holds every slot: %s)",
                    pins_made,pins_people,pins_released,model_pins.size(),quota_writes,quota_set ? "yes" : "no");
      ProbeLog(line); pins_made=pins_released=pins_people=quota_writes=0;
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
std::string PerfSectionsText(double seconds);  // below (UpdateBody)
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
      char line[640];
      std::snprintf(line,sizeof(line),"Perf: %.0f updates/s, mod %.2f ms/s (worst %.2f ms), packets in %.0f/s out %.0f/s, actors %u, person actions sent %u, joiner hits applied %u, people's hits on joiner %u, knockdowns/reactions sent %u played %u, car hits sent %u applied %u, own climbs of the other player blocked %u",
                    calls/seconds,busy_ms/seconds,worst_ms,packets_in.exchange(0)/seconds,packets_out.exchange(0)/seconds,
                    unsigned(avatar_count.load()+(replica_local?1:0)+(test_local?1:0)),ped_actions_sent.exchange(0),
                    hosting.load()?ped_hits_applied.exchange(0):0u,hits_from_people.exchange(0),ped_calls_sent.exchange(0),ped_calls_played.exchange(0),impacts_sent.exchange(0),impacts_applied.exchange(0),own_climbs_blocked);
      own_climbs_blocked=0;
      ProbeLog(line);
      ProbeLog(PerfSectionsText(seconds).c_str());
      if(ped_reacts_sent || ped_reacts_played || joiner_hit_reactions) {
        std::snprintf(line,sizeof(line),"Hit reactions: %u sent, %u played here, %u from the other player's hits",ped_reacts_sent,ped_reacts_played,joiner_hit_reactions);
        ProbeLog(line); ped_reacts_sent=ped_reacts_played=joiner_hit_reactions=0;
      }
      // v2: the session and the memory checks (Wine: QueryWorkingSetEx off).
      std::snprintf(line,sizeof(line),"Net: %d players (this game is player %d), %u other players shown here, %u car copies; %u reliable resends, %u old snapshots dropped, %u events dropped; memory checks to Windows %.0f/s%s",
                    player_count.load(),int(my_slot.load())+1,avatar_count.load(),unsigned(car_copies.size()),net_resent.exchange(0),net_dropped.exchange(0),
                    events_dropped.exchange(0),mem_os_checks.exchange(0)/seconds,qws_off.load()?" (QueryWorkingSetEx off)":"");
      ProbeLog(line);
      if(shots_sent.load() || shots_played.load() || shots_refused.load()) {
        // (Refused: why each waited last; "other" was never tried - the
        // character not updated or not made, or its queue full.)
        std::snprintf(line,sizeof(line),"Shots: %u sent, %u of other players' played here, %u refused here (lying %u, not set up %u, other gun %u, no ammo %u, fire refused %u, gun raising %u, in a car %u, other %u); throws %u, trigger releases %u played here",
                      shots_sent.exchange(0),shots_played.exchange(0),shots_refused.exchange(0),shots_held[kHeldLying].exchange(0),shots_held[kHeldNotSetUp].exchange(0),
                      shots_held[kHeldOtherGun].exchange(0),shots_held[kHeldNoAmmo].exchange(0),shots_held[kHeldFireRefused].exchange(0),shots_held[kHeldRaising].exchange(0),
                      shots_held[kHeldInCar].exchange(0),shots_held[kHeldOther].exchange(0),throws_played.exchange(0),releases_played.exchange(0));
        ProbeLog(line);
      }
      if(projs_sent.load() || projs_copied.load() || projs_refused.load() || booms_sent.load() || booms_played.load()) {
        std::snprintf(line,sizeof(line),"Projectiles: %u of ours sent, %u copies made here, %u of other players' own refused here; explosions %u sent, %u played here, %u copies' own held back",
                      projs_sent.exchange(0),projs_copied.exchange(0),projs_refused.exchange(0),booms_sent.exchange(0),booms_played.exchange(0),booms_held.exchange(0));
        ProbeLog(line);
      }
      if(parked_refused.load() || gang_skipped.load() || police_skipped.load() || roadblocks_skipped.load()) {
        const uint32_t me=api->read_u32(0x8309ABEC);
        std::snprintf(line,sizeof(line),"Own spawners while the host's world is shown: %u parked cars refused, %u gang manager runs skipped, %u police response runs and %u roadblock runs skipped (this player's police level %d, %u units of this game's own response here)",
                      parked_refused.exchange(0),gang_skipped.exchange(0),police_skipped.exchange(0),roadblocks_skipped.exchange(0),Readable(me,4252)?int32_t(api->read_u32(me+3964)):-1,OwnResponseUnits());
        ProbeLog(line);
      }
      // 2.1: notoriety per player (levels Carnales/Vice Kings/Rollerz/Playas/police).
      if(share_police) {
        const uint32_t me=api->read_u32(0x8309ABEC);
        const uint32_t packets_sent=notoriety_sent.exchange(0), packets_taken=notoriety_received.exchange(0), packets_old=notoriety_old.exchange(0);
        bool any=crimes_sent || crimes_received || crimes_dropped || crimes_refused || crimes_lost || crimes_witnessed || crimes_marked || busted_sent || busted_applied || busted_refused || forgivings || decays_skipped ||
                 response_slot>=0 || response_rings || response_steered || response_called_off || response_helis_off || response_changes || response_offroad || response_units_out || response_cars_out;
        std::string others;
        for(int s=0;s<kSlots;++s) if(remotes[s].notoriety_on) if(const uint32_t o=ResolveHuman(remotes[s].notoriety_on)) {
          others+=std::string(" ")+remotes[s].tag+" "+NotorietyText(o);
          for(uint32_t g=0;g<5;++g) any=any || api->read_u32(o+3868+24*g)!=0;
        }
        if(connected.load() && Readable(me,4252)) for(uint32_t g=0;g<5;++g) any=any || api->read_u32(me+3868+24*g)!=0;
        if(any && Readable(me,4252)) {
          char response[400]="";   // the host's response (hosting only)
          if(hosting.load()) {
            unsigned after_cars=0, after_helis=0;
            if(response_handle) CarsAfter(response_handle,after_cars,after_helis);
            std::snprintf(response,sizeof(response),"; response after %s: %u spawn points, %u pursuits steered, %u cars and %u helicopters after it now, %u cars and %u helicopters called off, %u target changes, "
                          "%u updates with this player off the roads (no new group then), %u of its units on foot and %u of its cars sent",
                          response_slot>=0 ? remotes[response_slot].tag : "this player",response_rings,response_steered,after_cars,after_helis,response_called_off,response_helis_off,response_changes,
                          response_offroad,response_units_out,response_cars_out);
          }
          char police[2200];
          std::snprintf(police,sizeof(police),"Police: levels here %s, on other players' characters:%s; notoriety packets %u sent, %u taken, %u old dropped; crimes of other players seen here %u (sent to their games), ours seen in theirs %u, dropped %u (another game counts them), refused %u, lost %u (the queue here was full), %u of theirs seen by this game's cops (sent witnessed), %u of ours queued witnessed here (the host's cops see our character: %u tellings sent, %u taken); "
                        "arrests: %u of other players' characters sent, %u of ours from theirs, %u refused (by a copy of the host's cops, or ours from theirs with our player not down, getting up, busted already or without a police level); %u forgivings (%u units told to stop), %u decay steps on other players' characters skipped%s",
                        NotorietyText(me).c_str(),others.empty()?" none":others.c_str(),packets_sent,packets_taken,packets_old,crimes_sent,crimes_received,crimes_dropped,crimes_refused,crimes_lost,crimes_witnessed,crimes_marked,seen_sent,seen_taken,busted_sent,busted_applied,busted_refused,forgivings,stops_told,decays_skipped,response);
          ProbeLog(police);
        }
        crimes_sent=crimes_received=crimes_dropped=crimes_refused=crimes_lost=crimes_witnessed=crimes_marked=seen_sent=seen_taken=busted_sent=busted_applied=busted_refused=forgivings=stops_told=decays_skipped=0;
        response_rings=response_steered=response_called_off=response_helis_off=response_changes=response_offroad=0;
      }
      if(copy_shots_blocked || copy_hits_zeroed || teams_written || crew_actions_blocked || crew_exits || crew_exits_refused || crew_hits_sent || crew_hits_applied.load() || crew_hits_empty || crew_hits_nocar || crew_hits_player || !crews_out.empty()) {
        std::snprintf(line,sizeof(line),"Copies of the host's people and traffic crews: %u own shots dropped (%u from a seat), %u of their own hits on this player zeroed, %u set to the host's team; "
                      "crews: %u own actions refused, %u own exits let through (asker logged), %u got out for this player's pull-outs, %u seen out of their seat, %u of this player's hits on them sent to the host, %u of other players' hits on them applied here "
                      "(%u found no one in that seat here, %u no such car here, %u a player in that seat)",
                      copy_shots_blocked,copy_shots_seated,copy_hits_zeroed,teams_written,crew_actions_blocked,crew_exits_refused,crew_exits,unsigned(crews_out.size()),crew_hits_sent,crew_hits_applied.exchange(0),crew_hits_empty,crew_hits_nocar,crew_hits_player);
        ProbeLog(line); copy_shots_blocked=copy_shots_seated=copy_hits_zeroed=teams_written=crew_actions_blocked=crew_exits=crew_exits_refused=crew_hits_sent=crew_hits_empty=crew_hits_nocar=crew_hits_player=0; crews_out.clear();
      }
      if(knockdowns_synced.load() || getups_synced.load() || knockdowns_refused.load() || stranded_found.load() || stranded_recovered.load() || getup_spots_moved.load() ||
         replayed_calls_dropped.load() || replayed_actions_dropped.load() || !ejection_cars_skipped.empty()) {
        std::snprintf(line,sizeof(line),"Knockdowns: %u of other players' applied here (%u refused attempts), %u get-ups, %u get-up spots set where they stand, %u stranded ragdolls, %u recovered; "
                      "their replayed calls %u and actions %u dropped (knockdown, get-up, or while down here); crash ejections skipped in %u cars of other games or with their characters",
                      knockdowns_synced.exchange(0),knockdowns_refused.exchange(0),getups_synced.exchange(0),getup_spots_moved.exchange(0),stranded_found.exchange(0),stranded_recovered.exchange(0),
                      replayed_calls_dropped.exchange(0),replayed_actions_dropped.exchange(0),unsigned(ejection_cars_skipped.size()));
        ProbeLog(line);
      }
      ejection_cars_skipped.clear();
      // 2.1: car damage on every game (it was only in the client's shared
      // traffic line, and its skipped count was a running total).
      const unsigned cars_waiting=CarsWaitingForPhysics();   // (now, not this window's arrivals)
      if(car_damage_applied || car_smoke_failed || car_hp_applied || car_wrecks_applied || car_sirens_applied || detonations_window ||
         car_hp_blocked || car_pushes_blocked || own_cars_wrecked || their_cars_wrecked || car_wreck_nophys || car_wreck_nodef || car_wrecks_refused || cars_waiting) {
        char cars[896];
        std::snprintf(cars,sizeof(cars),"Car damage: %u smoke/fire changes applied (%u refused), %u hit point changes, %u wrecks replayed (%u without the explosion, %u of them after waiting for physics here; put off: %u tries without physics here, %u cars waiting for it now, %u without an explosion, %u refused), %u siren/siren light changes; "
                      "on cars other games drive, skipped here: %u local explosions/hits (%s), %u hit point losses (%s), %u explosion pushes, %u wrecked here anyway; own cars wrecked %u",
                      car_damage_applied,car_smoke_failed,car_hp_applied,car_wrecks_applied,car_wrecks_quiet,car_wrecks_waited,car_wreck_nophys,cars_waiting,car_wreck_nodef,car_wrecks_refused,car_sirens_applied,
                      detonations_window,TopCallers(detonate_callers,true).c_str(),car_hp_blocked,TopCallers(car_hp_callers,false).c_str(),car_pushes_blocked,their_cars_wrecked,own_cars_wrecked);
        ProbeLog(cars);
        car_damage_applied=car_smoke_failed=car_hp_applied=car_wrecks_applied=car_wrecks_quiet=car_wrecks_waited=car_sirens_applied=0;
        car_wreck_nophys=car_wreck_nodef=car_wrecks_refused=0;
        detonations_window=car_hp_blocked=car_pushes_blocked=their_cars_wrecked=own_cars_wrecked=0;
      }
      // The car copies' model references (CreateVehicle: 82532360, kind 5,
      // noted at v+50304 since 2.2 so the destructor 825324D0 gives them back):
      // the vehicle group's kind-5 count against the copies here, and per
      // model of a copy its kind-5 count against the cars of it here (copies,
      // the game's own) - flat when they come back.
      if(car_model_refs_taken || car_model_refs_total) {
        unsigned entries=0, loaded=0, kind5=0, copies=unsigned(car_copies.size());
        for(auto& [cid,sc]:shared_cars) if(sc.car) ++copies;
        const uint32_t group=kStreamTable+car_model_group*204;
        if(car_model_group<64 && Readable(group,204)) { const uint32_t head=api->read_u32(group+48); unsigned guard=0;
          for(uint32_t e=head;e && Readable(e,72) && guard++<4096;) { ++entries; if(api->read_u32(e)) ++loaded; kind5+=api->read_u32(e+64); e=api->read_u32(e+28); if(e==head) break; } }
        std::snprintf(line,sizeof(line),"Car models here: %u references taken by copies made this window (%u since the start), group %u now %u entries (%u loaded) with %u references of kind 5, %u car copies here",
                      car_model_refs_taken,car_model_refs_total,car_model_group,entries,loaded,kind5,copies);
        ProbeLog(line); car_model_refs_taken=0;
        struct CarModel { unsigned copies=0, others=0, noted=0; };
        std::map<uint32_t,CarModel> models;
        for(uint32_t index=0;index<4096;++index) {
          const uint32_t v=TableObject(index,5,kVehicleSize);
          if(!v) continue;
          const uint32_t info=api->read_u32(v+50324);
          if(!Readable(info,4)) continue;
          CarModel& m=models[api->read_u32(info)];
          const uint32_t h=api->read_u32(v+68);
          if(IsSharedCarHandle(h) || IsCarCopyHandle(h)) { ++m.copies; if(int32_t(api->read_u32(v+50304))!=-1) ++m.noted; } else ++m.others;
        }
        std::string s; unsigned over=0, shown=0; char t[48];
        for(auto& [mid,m]:models) {
          if(!m.copies) continue;
          const uint32_t e=FindStreamEntry(mid);
          const unsigned k5=e ? api->read_u32(e+64) : 0;
          if(k5>m.copies+m.others+1) ++over;
          if(shown++<16) { std::snprintf(t,sizeof(t)," %X %u (%u, %u; noted %u)",mid,k5,m.copies,m.others,m.noted); s+=t; }
        }
        std::snprintf(line,sizeof(line),"Car models: per model of a copy here its kind-5 references (copies, this game's own; copies noted at +50304):%s; %u with more references than its cars + 1",
                      s.empty() ? " none" : s.c_str(),over);
        ProbeLog(line);
      }
      // 2.1.4: the other players' arrows on the maps (FollowersHook, PauseMapHook, Team0DotHook).
      if(markers_minimap.load() || markers_pausemap.load() || dots_skipped.load() || marker_calls_failed.load()) {
        std::snprintf(line,sizeof(line),"Markers: other players' arrows drawn %u on the minimap (%u of them at its edge), %u on the pause map (%u at its edge); %u purple dots of their characters skipped; %u draw calls failed since the start",
                      markers_minimap.exchange(0),markers_minimap_edge.exchange(0),markers_pausemap.exchange(0),markers_pausemap_edge.exchange(0),dots_skipped.exchange(0),marker_calls_failed.load());
        ProbeLog(line);
      }
#ifdef WHOMPAYS_EOS
      if(const int kind=eos::link_kind.load()) {
        std::snprintf(line,sizeof(line),"Link: %s; largest queues: outgoing %.1f KB (%u packets), incoming %.1f KB (%u packets); %u packets refused by the full outgoing queue",
                      kind==1 ? "direct" : "through Epic's relay servers",link_out_bytes.exchange(0)/1024.0,link_out_packets.exchange(0),
                      link_in_bytes.exchange(0)/1024.0,link_in_packets.exchange(0),eos::send_refused.exchange(0));
        ProbeLog(line);
      }
#endif
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
  { std::lock_guard lock(pose_mutex); p=remote_in[0].pose; }   // the host's
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
  const bool in_car=ResolveVehicle(api->read_u32(player+2496))!=0;
  const bool overlap=d<0.6f && std::abs(dy)<1.5f && p.vehicle<0 && !in_car;
  if(!overlap) overlap_since=0; else if(!overlap_since) overlap_since=now;
  const bool step_aside=overlap_since && now-overlap_since>3000;
  const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316;
  // v2: inside another client the same way (both put on one spot): the
  // higher slot steps 1.5 m to its own right.
  const int me=my_slot.load();
  {
    static uint64_t client_overlap_since=0;
    bool client_overlap=false;
    for(int s=1;s<kSlots && !client_overlap && !in_car;++s) {
      if(s==me || me<=s || !Present(s)) continue;
      PlayerPose q;
      { std::lock_guard lock(pose_mutex); q=remote_in[s].pose; }
      const float qx=q.pos[0]-local_x.load(), qy=q.pos[1]-local_y.load(), qz=q.pos[2]-local_z.load();
      client_overlap=q.sequence && q.vehicle<0 && qx*qx+qz*qz<0.6f*0.6f && std::abs(qy)<1.5f;
    }
    if(!client_overlap) client_overlap_since=0; else if(!client_overlap_since) client_overlap_since=now;
    if(client_overlap_since && now-client_overlap_since>3000) {
      client_overlap_since=0;
      api->write_f32(pos,local_x.load()+api->read_f32(player+32)*1.5f);
      api->write_f32(pos+4,local_y.load()+0.1f);
      api->write_f32(pos+8,local_z.load()+api->read_f32(player+40)*1.5f);
      api->write_f32(pos+12,0);
      for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,api->read_f32(player+32+i*4));
      const bool ok=MoveHuman(scratch,player,pos,matrix,0,true);
      ProbeLog(ok?"Standing inside another player for 3 s: stepped aside":"Standing inside another player for 3 s: step aside refused");
      return;
    }
  }
  // This player's spot by the host: 1.5 m (in a car 4 m) to the host's right
  // for the first client, left for the second, then further out for the
  // others (v1 put every client on the same spot), facing the same way.
  const int k=(me>=1 && me<kSlots) ? me-1 : 0;
  const float side=(k&1) ? -1.0f : 1.0f, rank=float(1+k/2);
  const float near_enough=std::max(6.0f,1.5f*rank+2.0f);
  float target[3]={p.pos[0]+p.rows[0]*1.5f*side*rank, p.pos[1]+0.1f, p.pos[2]+p.rows[2]*1.5f*side*rank};
  float rows[9]={p.rows[0],0,p.rows[2], 0,1,0, p.rows[6],0,p.rows[8]};
  if(p.vehicle>=0) { target[0]=p.vpos[0]+p.vrows[0]*4.0f*side*rank; target[1]=p.vpos[1]+0.5f; target[2]=p.vpos[2]+p.vrows[2]*4.0f*side*rank; }
  const float tx=target[0]-local_x.load(), tz=target[2]-local_z.load(), td=std::sqrt(tx*tx+tz*tz);
  // Joining: brought next to the host once (the host's character only
  // appears within 40 m, so a joiner far away saw nobody).
  const bool joining=join_move && d>near_enough;
  if(join_move && d<=near_enough) join_move=false;
  if(!active && !step_aside && !joining) { pending=false; return; }
  if(!pending && !step_aside && !joining && !(d>150.0f && now>=next_catch_up)) return;
  pending=false; next_catch_up=now+10000; overlap_since=0; join_move=false;
  if(!step_aside && (td<1.0f || (d>=0.6f && d<near_enough))) return;
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
    // Timed mission help only. The host's own interaction prompts ("Kick the
    // door open") come here too, untimed or with a long hold, and showed on
    // the joiner as if they were the joiner's (1.62).
    float duration=0; std::memcpy(&duration,&e.a[0],4);
    const bool timed=duration>0.5f && duration<=30.0f;
    const uint32_t lr=api->get_lr(c);
    static unsigned logged=0;
    if(logged<40) {
      ++logged;
      std::string t; for(uint32_t i=0;i<e.length && i<60;++i) t+=char(e.text[i]<128 ? e.text[i] : '?');
      char line[200]; std::snprintf(line,sizeof(line),"Help here (from %08X, %.1f s): %s - %s",lr,duration,t.c_str(),timed?"sent to the joiner":"kept here");
      ProbeLog(line);
    }
    if(timed) QueueHud(e);
  } else if(HoldLocalHud()) return;
  original_help(c,b);
}
void MissionEndHook(WmlContext* c,uint8_t* b) {
  if(connected.load() && hosting.load() && share_missions) {
    // Only with a mission running (82215360's own first test, 0x82B3106C):
    // the player's death calls it "failed" in free roam too, and every death
    // of the host showed "Host's mission failed" on the clients (2.1.4).
    if(MissionActive()) {
      HudEvent e; e.kind=kHudMissionEnd; e.a[0]=uint32_t(api->get_r(c,3));
      e.length=ReadWide(uint32_t(api->get_r(c,4)),e.text,300);
      QueueHud(e);
      char line[80]; std::snprintf(line,sizeof(line),"Mission ended here (%s)",e.a[0]?"passed":"failed"); ProbeLog(line);
    } else {
      static unsigned logged=0;
      if(logged++<20) ProbeLog("Mission end message not forwarded: no mission (the host died)");
    }
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
    } else if(e.kind==kHudMissionEnd) {
      // The host's mission never ran here (1.63): at least say how it went.
      Notify(e.a[0] ? "Host's mission complete" : "Host's mission failed");
      ProbeLog("Host's mission ended, but it wasn't running here: shown as a message");
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
  const float px=api->read_f32(player+20), pz=api->read_f32(player+28);
  std::string l=std::string("Near here (")+when+"), not the host's:"; unsigned n=0, copies=0;
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t cur=api->read_u32(kObjectTable+12+index*16);
    if(!Readable(cur,4252) || api->read_u32(cur+72)!=1 || (api->read_u32(cur+68)&0xffff)!=index) continue;
    if(cur==player || AvatarSlotOf(cur)>=0) continue;
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
    // What 82212728 checks before it starts anything: the mission's record
    // (0x82B313A8, 100 bytes each, +44 kind: 0 mission, 3 stronghold), its
    // state entry (0x82B31088, 8 bytes: hash, flags; 0x80 = available in
    // this save) and the "busy" byte 0x8370DB32. A stronghold or mission the
    // joiner's own save hasn't opened yet never started here (1.62: the
    // host's strongholds, banner, objectives and end screen never showed).
    const uint32_t count=api->read_u32(0x82B31084);
    uint32_t record=0, state=0;
    for(uint32_t i=0;i<count && i<512;++i) {
      if(!record && api->read_u32(0x82B313A8+i*100)==hash) record=0x82B313A8+i*100;
      if(!state && api->read_u32(0x82B31088+i*8)==hash) state=0x82B31088+i*8;
    }
    const uint32_t kind=record ? api->read_u32(record+44) : 0xffffffffu;
    const uint8_t flags=state ? api->read_u8(state+4) : 0;
    const bool opened_by_us=state && !(flags&0x80);
    if(opened_by_us) api->write_u8(state+4,uint8_t(flags|0x80));
    ++applying_remote;
    scratch.ctx.r3.u64=hash;
    const bool called=scratch.Call(0x82212728);
    --applying_remote;
    if(opened_by_us) api->write_u8(state+4,flags);
    const bool started=called && api->read_u32(0x82B3106C)!=0;
    char line[200];
    std::snprintf(line,sizeof(line),"Host's mission %08X: %s (kind %d, flags %02X%s, busy %u)",hash,
      started ? "started here too" : "didn't start here (trying again)",int(kind),flags,
      opened_by_us ? ", opened for co-op" : "",api->read_u8(0x8370DB32));
    static uint32_t logged_fail=0;
    if(started || logged_fail++<6) ProbeLog(line);
    if(started) logged_fail=0;
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
  static unsigned hidden_total=0;
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t cur=api->read_u32(kObjectTable+12+index*16);
    if(!Readable(cur,4252) || api->read_u32(cur+72)!=1 || (api->read_u32(cur+68)&0xffff)!=index) continue;
    if(cur==player || AvatarSlotOf(cur)>=0) continue;
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
// 1.61: while the host is paused the joiner's game is paused for real (the
// game's own pause / unpause, counter 0x8370DAE4): its world stops and the
// joiner can't move, as in the host's game. A pause the joiner's own menu asks
// for meanwhile counts normally; when the host resumes the counter goes back
// to 0 (the joiner's world never stays paused for the other player).
bool remote_pause_on=false;   // game thread: this game is paused because the host is
bool pause_passthrough=false; // game thread: our own pause call
void PauseHook(WmlContext* c,uint8_t* b) {
  if(connected.load() && running.load() && !hosting.load() && !pause_passthrough && !remote_pause_on) {
    static unsigned logged=0;
    if(logged++<20) { char line[100]; std::snprintf(line,sizeof(line),"Pause skipped (the world keeps running for the other player), from %08X",api->get_lr(c)); ProbeLog(line); }
    return;
  }
  original_pause(c,b);
}
// The pause menu (game state 6) is open: anywhere on the state stack
// (822C04D0 tests the top; 82209B68 indexes 0..depth, -1 = empty).
bool PauseMenuOpen() {
  const int32_t depth=int32_t(api->read_u32(0x827D5780));
  for(int32_t i=0;i<=depth && i<32;++i) if(api->read_u32(0x82B30E78+uint32_t(i)*4)==6) return true;
  return false;
}
// 823494D0 tells the pause menu's tabs what they run over: 3 System Link
// lobby, 1 (byte 0x8370E927), 2 a paused world or the game's own multiplayer,
// 0 otherwise. The MAP tab (822B6548) and others take Back, close and the tab
// switches (822C06B8) only on 2, so a client's menu over its running world
// (PauseHook) could never be closed (2.0 test: stuck on the map). It gets the
// game's multiplayer answer, as its menu there runs over a running world too.
WmlGuestFunction original_menu_context=nullptr;
void MenuContextHook(WmlContext* c,uint8_t* b) {
  original_menu_context(c,b);
  if(connected.load() && running.load() && !hosting.load() && uint32_t(api->get_r(c,3))==0 && PauseMenuOpen()) {
    api->set_r(c,3,2);
    static unsigned logged=0;
    if(logged++<5) ProbeLog("Pause menu over a running world: given the multiplayer answer (2)");
  }
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
void ReleaseRemotePause(GuestScratch& scratch,const char* why);
void UpdateHostPause(GuestScratch& scratch,uint64_t now) {
  // (diagnostics) the game's sound pause count (82100C18(-1) raises it, as
  // the pause does): sound went strange for the joiner after prompts.
  { static int32_t last=0; const int32_t v=int32_t(api->read_u32(0x8370CF84));
    if(v!=last) { static unsigned n=0; if(n++<30) { char line[100]; std::snprintf(line,sizeof(line),"Sound pause count now %d (was %d)",v,last); ProbeLog(line); } last=v; } }
  // Not while this player's own pause menu is open: switching the controls
  // on under the open menu left it stuck on screen (it couldn't be closed,
  // while the player walked around). Applied once the menu is closed.
  if(controls_held && api->read_u32(0x839E0DF8)!=0x82FFB84Cu) {
    scratch.Call(0x824D92F0); controls_held=false;  // an older hold still on
    ProbeLog("Controls back on");
  }
  // Not during a cutscene here (the shared cutscene must keep playing).
  const bool hold=HostPauseHold(now) && (remote_pause_on || !CutscenePlaying());
  if(hold && !remote_pause_on) {
    pause_passthrough=true;
    scratch.ctx.r3.u64=1;  // sound too, as the pause menu does
    scratch.Call(0x8220C778);
    pause_passthrough=false;
    remote_pause_on=true;
    char line[100]; std::snprintf(line,sizeof(line),"Host paused: this game paused too (counter %d)",int32_t(api->read_u32(0x8370DAE4)));
    ProbeLog(line);
  } else if(!hold && remote_pause_on) {
    ReleaseRemotePause(scratch,"Host resumed");
  }
}
// Back to an unpaused world: every pause this game holds (ours, and one the
// joiner's own menu took while the host was paused) is let go.
void ReleaseRemotePause(GuestScratch& scratch,const char* why) {
  remote_pause_on=false;
  for(int i=0;i<8 && int32_t(api->read_u32(0x8370DAE4))>0;++i) {
    scratch.ctx.r3.u64=1;
    scratch.Call(0x8220C868);
  }
  char line[120]; std::snprintf(line,sizeof(line),"%s: this game unpaused (counter %d)",why,int32_t(api->read_u32(0x8370DAE4)));
  ProbeLog(line);
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

// Perf sections (1.60): where the mod's time goes, logged with the Perf line.
struct PerfSection { const char* name; double ms; };
PerfSection perf_sections[40]; unsigned perf_section_count=0;
struct SectionTimer {
  LARGE_INTEGER a; PerfSection* s;
  explicit SectionTimer(const char* name) : s(nullptr) {
    for(unsigned i=0;i<perf_section_count;++i) if(perf_sections[i].name==name) { s=&perf_sections[i]; break; }
    if(!s && perf_section_count<40) { s=&perf_sections[perf_section_count++]; s->name=name; s->ms=0; }
    QueryPerformanceCounter(&a);
  }
  ~SectionTimer() {
    if(!s) return;
    static LARGE_INTEGER f{}; if(!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER b; QueryPerformanceCounter(&b);
    s->ms+=double(b.QuadPart-a.QuadPart)*1000.0/double(f.QuadPart);
  }
};
#define PERF_SECTION(n) SectionTimer perf_timer_##__LINE__(n)
std::string PerfSectionsText(double seconds) {
  std::vector<PerfSection> v(perf_sections,perf_sections+perf_section_count);
  std::sort(v.begin(),v.end(),[](const PerfSection& x,const PerfSection& y){return x.ms>y.ms;});
  std::string out="Perf parts (ms/s):"; char t[64];
  for(size_t i=0;i<v.size() && i<10;++i) { std::snprintf(t,sizeof(t)," %s %.1f",v[i].name,v[i].ms/seconds); out+=t; }
  for(unsigned i=0;i<perf_section_count;++i) perf_sections[i].ms=0;
  return out;
}

void WorldGameTick(GuestScratch& scratch,uint32_t player,uint64_t now);   // (the shared area mirrored, 2.2: with the network thread)
void WorldMemberTick(GuestScratch& scratch,uint32_t player,uint64_t now);
void WorldRegistryOff();
void WorldLeversOff();
void WorldMemberReset();
extern bool world_on, world_member_live;
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
  // (After a Stop the shared cars and people, other players' characters and
  // car copies still go a few at a time: v1 stopped after the first update
  // and left a client without its own traffic until a restart.)
  const bool leftovers=traffic_shared || peds_shared || avatars_cleaning || !car_copies.empty() || WorldCopiesLeft() ||
                       std::any_of(remotes.begin(),remotes.end(),[](const Remote& r){ return r.avatar!=0; });
  if (!running.load() && !cleanup_requested.load() && !leftovers && !toggle_test && !toggle_copy && !debug && watches.empty()) return;
  const uint32_t sp=static_cast<uint32_t>(api->get_r(raw,1));
  if (sp<0x5000 || !Readable(sp-0x5000,0x5000)) return;
  GuestScratch scratch(raw);
  const uint64_t now=MonoMs();
  // Session over, or this game's slot changed (a client that lost the host
  // can come back with another one): every other player's character and car
  // copy goes, over as many updates as it takes (one still in a car gets out
  // first), before players are shown again.
  {
    static uint8_t ran_as=coopnet::kNoSlot;
    const uint8_t me_now=my_slot.load();
    if(connected.load() && me_now!=coopnet::kNoSlot) {
      if(ran_as!=coopnet::kNoSlot && me_now!=ran_as) { avatars_cleaning=true; ProbeLog("This game's slot changed: other players' characters made again"); }
      ran_as=me_now;
    }
    const bool over=cleanup_requested.exchange(false) || !connected.load();
    if(over) {
      avatars_cleaning=true;
      if(world_on) WorldRegistryOff();   // (2.2: a Stop or a lost host too, review A2-4)
      WorldLeversOff();
      PeopleQuota(scratch,false);   // (2.2: the game's own people-model slot quota back; nothing when it was never set)
      if(world_member_live) { WorldMemberReset(); world_member_live=false; }   // (the next session's area starts over: review A3-2)
      ShowMissionMarkers(scratch);
      if(controls_held) { scratch.Call(0x824D92F0); controls_held=false; ProbeLog("Session over: controls back on"); }
      if(remote_pause_on) ReleaseRemotePause(scratch,"Session over");
      if(replica_local) DestroyReplica(scratch);
    }
    { static uint64_t next_quota=0;   // (2.2: a client's people-model slots all at level 0, with its pins)
      if(!over && !hosting.load() && now>=next_quota) { next_quota=now+1000; PeopleQuota(scratch,true); } }
    if(avatars_cleaning) {
      bool any=false;
      for(Remote& r:remotes) if(r.avatar) { DestroyAvatar(scratch,r); any=any || r.avatar; }
      if(!any) {
        if(!car_copies.empty()) CleanCarCopies(scratch,now,true);   // (cars after the characters are out of them)
        for(Remote& r:remotes) { r.generation=~0u; r.notoriety_on=0; r.next_notoriety=r.next_busted=0; }   // made fresh when shown again (their notoriety here with them, 2.1)
        avatars_cleaning=false;
      }
      avatar_count=0;
    }
  }
  // The other player's character is a real player object (from the game's
  // player pool). After a cutscene the game can take it for the local player
  // (both players saw only the other one's character and couldn't move):
  // the local player is put back.
  // (By handle: ResolveHuman never resolves the object the game calls the
  // local player, so v1's check of the resolved character never matched.)
  {
    static uint32_t own_player=0;
    const uint32_t cur=api->read_u32(0x8309ABEC);
    const uint32_t cur_handle=Readable(cur,72) ? api->read_u32(cur+68) : 0;
    const bool taken=IsAvatarHandle(cur_handle);
    if(connected.load() && taken && own_player && own_player!=cur && Readable(own_player,4252) && api->read_u32(own_player+72)==1) {
      api->write_u32(0x8309ABEC,own_player);
      static unsigned logged=0;
      if(logged++<10) { char line[160]; std::snprintf(line,sizeof(line),"The game took another player's character for ours (%08X): our player %08X put back",cur,own_player); ProbeLog(line); }
    } else if(Readable(cur,4252) && !taken) own_player=cur;
    static uint32_t last_head=0; static bool was_cut=false;
    const bool cut=CutscenePlaying();
    const uint32_t head=api->read_u32(0x8309AC54);
    if(connected.load() && (head!=last_head || (was_cut && !cut))) {
      static unsigned logged=0;
      if(logged++<20) { char line[160]; std::snprintf(line,sizeof(line),"Players: local %08X, active list head %08X, other players shown %u%s",api->read_u32(0x8309ABEC),head,avatar_count.load(),(was_cut&&!cut)?" (cutscene just ended)":""); ProbeLog(line); }
    }
    last_head=head; was_cut=cut;
  }
  const uint32_t player=api->read_u32(0x8309ABEC);
  if(traffic_shared && (!connected.load() || !running.load()) && Readable(player,4252)) UpdateSharedTraffic(scratch,player,now);
  if(peds_shared && (!connected.load() || !running.load()) && Readable(player,4252)) UpdateSharedPeds(scratch,player,now);
  if(WorldCopiesLeft() && (!connected.load() || !running.load()) && Readable(player,4252)) WorldMemberTick(scratch,player,now);   // (2.2: the world stream's copies fade and go)
  if (!Readable(player,4252) || api->read_u32(player+72)!=1) return;
  local_x=api->read_f32(player+20); local_y=api->read_f32(player+24); local_z=api->read_f32(player+28);
  { PERF_SECTION("local pose"); CaptureLocalPose(player); }
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
  { PERF_SECTION("notices/watch"); ShowNotices(scratch); WatchActors(now); }
  if(!running.load()) { diagnostics_done=false; own_police_off=false; return; }
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
  { PERF_SECTION("avatar"); if(connected.load() && !avatars_cleaning) { UpdateAvatars(scratch,now); if(share_police) UpdateNotoriety(scratch,player,now); } }
  if(share_police) UpdateResponseTarget(scratch,player,now);   // (the host's response target, 2.1; one left is called off)
  { PERF_SECTION("mission follow"); if(connected.load() && !hosting.load()) UpdateMissionFollow(scratch,player,now); }
  if(connected.load() && !hosting.load() && share_missions) HideMissionMarkers(scratch,now);
  if(!hosting.load()) UpdateHostPause(scratch,now);
  { PERF_SECTION("unstick"); if(connected.load()) UnstickPlayer(scratch,player); }
  if(connected.load() && !hosting.load() && share_missions && host_mission.load()) UpdateMissionIntro(scratch);
  if(connected.load() && !hosting.load()) ApplyHostHud(scratch);
  { PERF_SECTION("mission mirror"); if(connected.load() && !hosting.load()) UpdateMissionMirror(scratch,player,now); }
  if(connected.load() && !hosting.load()) UpdateSharedCutscene(scratch,now);
  { PERF_SECTION("capture traffic"); if(connected.load() && hosting.load()) CaptureTraffic(scratch,player,now); }
  // This player's own car: its look goes to the other game (whose copy of it
  // is made from the type alone).
  { PERF_SECTION("car look");
if(connected.load()) {
    // (only this game's own car: a copy goes by its owner's look)
    const uint32_t own=ResolveVehicle(api->read_u32(player+2496));
    uint32_t owner=0;
    if(own) { CarId(api->read_u32(own+68),owner); if(owner==my_slot.load()) ShareCarLook(scratch,own,now); }
  }
  }
  { PERF_SECTION("capture people"); if(connected.load() && hosting.load() && share_peds) CapturePeds(player,now); }
  { PERF_SECTION("world"); WorldGameTick(scratch,player,now); }   // (2.2: the registry on every game, the owner's stream on the host)
  mod_stage.store("shared traffic");
  { PERF_SECTION("shared traffic"); if(!hosting.load()) UpdateSharedTraffic(scratch,player,now); }
  mod_stage.store("shared people");
  { PERF_SECTION("shared people"); if(!hosting.load()) { UpdateSharedPeds(scratch,player,now); UpdatePedEffects(scratch,now); } }
  SweepActorModels(scratch,now);   // (characters the game removed give their model reference back, 2.1.4)
  RemoveOwnResponse(scratch,player);   // (once, as the host's world comes)
  mod_stage.store("events");
  { PERF_SECTION("replica"); if(connected.load() && !hosting.load()) UpdateReplica(scratch,now); }
  { PERF_SECTION("events"); if(connected.load()) ApplyEvents(scratch,now); }
  { PERF_SECTION("projectiles"); if(connected.load()) ApplyProjectiles(scratch,now); }
  if(debug_cars && connected.load()) AuditCarLists(player,now);
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
              sizeof(coop::NativePacket) <= kEosMaxPacket && sizeof(NotorietyPacket) <= kEosMaxPacket, "a co-op packet is too big for EOS P2P (1170 bytes)");
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

// ---- Network thread (co-op v2) ----
// One coopnet::Session (net.h): the host is slot 0 and relays; up to 11
// clients. Every message names its sender's slot. Each payload is one of the
// packets above (magic included); the session adds the sender, the target
// and, for reliable ones, acks and resends (v1 sent events 3 times instead).
enum : uint8_t {
  kMsgPose=coopnet::kFirstUserType,   // PosePacket: everyone, unreliable, 30 a second
  kMsgEvent,                          // EventPacket: e.target, reliable
  kMsgChat,                           // ChatPacket: everyone, reliable
  kMsgWorn, kMsgMorph, kMsgExtras,    // appearance: everyone, unreliable, every second
  kMsgCarLook,                        // CarLookPacket: everyone, unreliable, cycling
  // The host's world, host -> everyone:
  kMsgTraffic, kMsgTrafficNames, kMsgPeds, kMsgPedLook,  // unreliable
  kMsgHud, kMsgCutscene,                                 // reliable
  kMsgMissionId, kMsgMissionState, kMsgNative,           // unreliable
  kMsgProj,                           // ProjPacket: everyone, reliable (a projectile made / exploded)
  kMsgNotoriety,                      // NotorietyPacket: everyone, reliable at a change of a level, min or max, else once a second unreliable (2.1)
  // 2.2, the shared area mirrored (world.h; the owner is the host): owner -> one member unless said
  kMsgWorld,                          // WWS1: UpdateHeader, CarRec / PedRec / SeatedRec records; unreliable, 20 a second within the member's budget
  kMsgWorldCreate,                    // WWC1: CreateRec and its tail (name, look, paint, crew, the first record); reliable, at most 4 a tick
  kMsgWorldRemove,                    // WWR1: RemoveRec; reliable, only to the members the id was created on
  kMsgWorldAdmit,                     // WWA1: AdmitRec and its tail; member -> owner, unreliable, once a second and on change
  kMsgWorldDigest,                    // WWD1: DigestRec and the ids; reliable, every 10 s
  kMsgOwnerMap,                       // WOM1: OwnerMapRec; host -> everyone, reliable when a slot's owner changes, else once a second unreliable
};
static_assert(coopworld::kMaxMessage==coopnet::kMaxMessage && coopworld::kMaxSlots==coopnet::kMaxSlots,"world.h and net.h agree");
// A v1 co-op (1.71 and older) says this; it can't join a v2 game.
constexpr char kV1Hello[] = "WHOMPAYS_COOP_V3_HELLO";

// ---- The shared area mirrored (co-op 2.2, step 2: the owner side) ----
// docs/COOP_V2.md section 11. Every game keeps a registry (world.h) of the
// cars and people its own game simulates, fed where the game makes and
// removes them (its multiplayer registers every object from the constructors
// through 82352D38 when both MP bytes are set, research Q 2.1; co-op hooks
// the same points and never sets those bytes):
//  - cars: 82569700(source, ...) after the call (VehicleCreateHook). r3 at its
//    17 call sites: 2 traffic or 4 (the traffic creator 82411310 passes 4 for
//    its police cars), 4 the response (82411118) and gangs' cars (825611D8), 5
//    parked (8241E378), 6 special (824224F0), 7 scripts and cheats (6
//    callers), 3 co-op's own copies (making_car; the MP creators 823BA888 /
//    823C2B78 / 823C8880 / 823E7A68 don't run in story mode) and 9 the El
//    Train (8242D248, under the train manager 8242EFB0): left out, counted;
//  - people: 8247A6D0(human, -, park) after the call: the activation. It puts
//    the human on the active list 0x8309ABB8 (+3592/+3596) when +200 bits 0
//    and 1 are set, else (r5 = 1) on the inactive list 0x8309ABB0, and
//    registers type 6 with 82352D38 in MP. Called by the constructor 82477B78
//    (r5 = 1, only when +216 0x4000 says the model is set up; else 8247A8E0
//    parks the human inactive), by 8247A9F8 (the people group's model-loaded
//    callback, +140 of its config in 82185498: every inactive human of that
//    model with +3619 bit 0x08 clear; r5 = 0, after 8244B268) and by 8240B228
//    (a queued mission character once 8250F3C8 says its model is in; r5 = 0).
//    So one human can come again (a model unloaded and back: 8247AA70 parks
//    it, 8247A9F8 brings it back): the add is idempotent and counted as a
//    re-activation. Its kind is +4216, written before (82477B78; report L: 1
//    ambient, 2 traffic crews, 4 police response, 6 special, 8 hood, 0/3/7
//    system, MP and script characters);
//  - removal: 824789A8 (HumanDestroyHook) and 825324D0 (VehicleRemoveHook)
//    when the game really removes the object (a copy kept is not removed).
// Filled from the object table when the session starts, checked against it
// every 10 s (the audit: what the hooks missed). The host (the owner in 2.2)
// streams it to the members of its area - the clients that show the world
// stream within member_join_m of its player (out past member_leave_m: the
// host's rule on the poses, step 4) - 20 times a second through world.h's
// scheduler, within a budget per member (mod.ini world_upload_kbps shared by
// the members). The members' side is the next section (step 3); 2.1's
// streams go only to the clients that don't show the world stream (step 4).
namespace cw=coopworld;
unsigned world_upload_kbps=1000;   // mod.ini world_upload_kbps
bool world_on=false;               // game thread: the registry is filled, for world_slot
uint8_t world_slot=coopnet::kNoSlot;
cw::Registry world_reg;            // game thread (hooks, UpdateBody)
cw::Scheduler world_sched;
std::array<cw::Member,cw::kMaxSlots> world_members;   // owner: its view of each member (game thread)
bool world_any_member=false;       // owner: some client is in its area (the hooks' test, game thread)
uint16_t world_epoch=0;            // OwnerMap epoch: +1 whenever a slot's owner changes (in 2.2: it joins or leaves the host's area)
// Why an entry doesn't stream right now (kEntryInactive while not 0).
enum : uint8_t { kWhyStreams=0, kWhyInactive, kWhyNoMission, kWhyNamed };
// Per entry (by handle: the registry holds this game's own objects only), what its records need between ticks.
struct WorldObj {
  bool classified=false, named=false;
  uint8_t why=kWhyStreams;
  uint8_t hp=255, damage=0, pflags=0, team=0xFF, weapon=0, drive=0; uint32_t occupant=0;   // as last seen: a resting object sends a change at once (drive: brake, reversing, horn)
  uint8_t shots=0; bool hit_ok=false, blank=false, burst=false; float hit[3]{}; uint64_t last_shot=0, horn_at=0;   // Package C: the rounds it fired (running count), the last one's hit point and kind; the horn
  float pos[3]{}; uint64_t pos_at=0; float vel[2]{};   // people: x / z velocity from the last two positions
  uint64_t rest_since=0;           // a car a player left: back to the owner's game 1 s after it rests
};
std::unordered_map<uint32_t,WorldObj> world_objs;      // game thread
void WorldDriveBits(uint32_t v,uint32_t handle,uint64_t now,uint8_t& bits,uint8_t& drive);   // (Package C, with the describers)
std::unordered_map<uint32_t,uint64_t> world_fading;    // handles whose fade-out began (out of the registry already) -> when
std::unordered_set<uint32_t> world_faded_back;         // handles still here 30 s after their fade began (the fade ended without a delete: counted if the audit adds them back)
std::unordered_set<uint32_t> world_mission_cars;       // cars a script made while a mission ran: left out until it is over
std::atomic<uint32_t> world_link_inflight[kSlots]{}, world_link_rtt[kSlots]{};   // network thread: reliable bytes not acked, smoothed RTT (ms) per slot
std::array<uint64_t,kSlots> world_budget_sum{};       // owner: the budget each member had over the 10 s window (bytes)
struct WorldNearIn { uint8_t cars=0, people=0; uint64_t at=0; };
std::array<WorldNearIn,kSlots> world_near_in{};       // owner: the copies each member shows within 50 m of its camera (its last WWA1)
bool world_first_member=true;                          // owner: "the first member in the area" not logged yet this session
// The host's member rule (step 4, spec 3.1/3): mod.ini member_join_m / member_leave_m (2D, from the poses), 2 s / 5 s.
unsigned world_join_m=150, world_leave_m=200;
std::array<uint64_t,kSlots> world_near_since{}, world_far_since{}, world_in_since{};
std::array<float,kSlots> world_member_d{};             // owner: each client's distance from this player (2D) at the last tick
// The area's viewpoints (Package B): [0] this game's own (camera eye
// 0x827D9778 +44, forward +104, player +20), then one per member from its
// pose (PosePacket.pos, .eye, .cam[6..8]); refreshed by WorldOwnerTick (20
// Hz, game thread; the spawner hooks run in the game's update on the same
// thread: no lock).
struct Viewpoint { int slot=-1; float pos[3]{}, eye[3]{}, fwd[3]{0,0,1}; float speed=0; uint64_t at=0, next_parked=0, next_people=0; bool full_cars=false, full_people=false; };
std::array<Viewpoint,kSlots> world_views; int world_view_count=0;   // [0] = this game
bool world_spawns_on=false;        // owner: members with a viewpoint, the host's stream on, no cutscene (WorldSpawnsOn)
unsigned world_member_spawns=7;    // mod.ini member_spawns: 1 parked, 2 traffic, 4 people (0 = keep-alive only)
float world_member_density=1.0f;   // mod.ini member_density: the wanted counts x (1 + density x members), capped 12 / 26
void WorldViewsRefresh(uint32_t player,uint64_t now);
struct WorldCounts { unsigned made_cars=0, made_people=0, removed_cars=0, removed_people=0, reactivated=0, fades=0, trains=0, mission_cars=0, audit_added=0, audit_removed=0, taken=0, returned=0, fade_back=0; };
WorldCounts world_n;               // 10 s (game thread)
struct WorldSent { unsigned creates=0, create_bytes=0, records=0, messages=0, record_bytes=0, removes_gone=0, removes_area=0, other_bytes=0, digests=0, digest_ids=0, skipped=0, admits=0; };
std::array<WorldSent,kSlots> world_sent;   // per member, 10 s (game thread)
// Game thread <-> network thread.
struct WorldSend { uint8_t type=0, target=0; bool reliable=false; std::vector<uint8_t> bytes; };
std::mutex world_mutex;
std::vector<WorldSend> world_out;          // planned messages, sent by SendDue in order (before the events)
struct WorldAdmitIn { uint8_t origin=0; cw::AdmitMsg msg; };
std::vector<WorldAdmitIn> world_admits_in; // WWA1 from members
std::atomic<uint32_t> world_link_backlog[kSlots]{}, world_link_dropped[kSlots]{};   // network thread: the session's LinkStats per slot
std::atomic<uint32_t> world_eos_backlog{0};       // network thread: Epic's outgoing queue (all peers)
std::atomic<uint32_t> world_in_count[6]{}, world_in_bytes{0}, world_admits_bad{0}, world_out_full{0};   // member: what came from the host by type (step 2: counted only)

void WorldQueue(uint8_t type,uint8_t target,bool reliable,const void* data,size_t n) {
  std::lock_guard lock(world_mutex);
  if(world_out.size()>=4096) { ++world_out_full; return; }   // (the network thread fell behind)
  const uint8_t* p=static_cast<const uint8_t*>(data);
  world_out.push_back({type,target,reliable,std::vector<uint8_t>(p,p+n)});
}
uint8_t WorldCarKind(uint32_t source) { return source==2 ? cw::kKindTraffic : source==5 ? cw::kKindParked : source==4 ? cw::kKindResponseCar : source==6 ? cw::kKindSpecialCar : cw::kKindScriptCar; }
uint8_t WorldPersonKind(uint32_t obj) {
  switch(api->read_u32(obj+4216)) {
    case 1: return cw::kKindAmbient;      case 2: return cw::kKindCrew;          case 4: return cw::kKindResponseUnit;
    case 6: return cw::kKindSpecialPerson; case 8: return cw::kKindHood;          default: return cw::kKindMissionPerson;   // 0/3/7
  }
}
bool PersonActive(uint32_t obj) { return (api->read_u32(obj+200)&3)==3; }   // (8247A6D0's test for the active list)
// Not this game's own: co-op's copies of other games' cars and people, the players' characters, test characters.
bool WorldLeftOut(uint32_t obj,uint32_t handle,bool car) {
  if(car) return IsSharedCarHandle(handle) || taken_cars.count(handle);
  return IsPlayerObject(obj) || obj==api->read_u32(0x8309ABEC) || AvatarSlotOf(obj)>=0 || handle==test_local || handle==replica_local || obj==player_copy || OurCharacter(handle);
}
void WorldCarMade(uint32_t handle,uint32_t source) {   // VehicleCreateHook, after the game made it
  if(!world_on || making_car || !connected.load() || my_slot.load()!=world_slot) return;
  const uint32_t v=ResolveVehicle(handle);
  if(!v || WorldLeftOut(v,handle,true)) return;
  cw::Entry& e=world_reg.Add(world_slot,handle,WorldCarKind(source),uint8_t(source),MonoMs());
  if(e.activations>1) return;
  ++world_n.made_cars;
  if(source==9) { e.flags|=cw::kEntryExcluded; ++world_n.trains; }                          // trains as records are 2.3 (C1)
  else if(source==7 && share_missions && MissionActive()) { e.flags|=cw::kEntryExcluded; world_mission_cars.insert(handle); ++world_n.mission_cars; }   // a mission's: each game's own mission makes it (P1-4); streamed once the mission is over (review A2-11)
}
WmlGuestFunction original_human_activate=nullptr;
void HumanActivateHook(WmlContext* c,uint8_t* b) {
  const uint32_t obj=uint32_t(api->get_r(c,3)), lr=uint32_t(api->get_lr(c));
  original_human_activate(c,b);
  if(!world_on || making_actor || !connected.load() || my_slot.load()!=world_slot || !Readable(obj,4252) || api->read_u32(obj+72)!=1) return;
  { static uint32_t seen[4]{}; static unsigned n=0;   // (which of its callers reach the hook: 82477B78, 8247A9F8, 8240B228)
    if(n<4 && std::find(seen,seen+n,lr)==seen+n) { seen[n++]=lr; char line[100]; std::snprintf(line,sizeof(line),"World registry: 8247A6D0 reached from %08X",lr); ProbeLog(line); } }
  const uint32_t handle=api->read_u32(obj+68);
  if(ResolveHuman(handle)!=obj || world_fading.count(handle) || (api->read_u32(obj+120)&0xC0000000u) || WorldLeftOut(obj,handle,false)) return;
  if(!PersonActive(obj) && world_reg.Find(world_slot,handle)) return;   // (8247A9F8 passes every inactive human of the model; one still inactive is no re-activation: review A2-15)
  const cw::Entry& e=world_reg.Add(world_slot,handle,WorldPersonKind(obj),0,MonoMs());
  if(e.activations>1) ++world_n.reactivated; else ++world_n.made_people;
}
void WorldObjectGone(uint32_t obj,bool car) {   // HumanDestroyHook / VehicleRemoveHook: the game removes it now
  if(!world_on || !Readable(obj,72)) return;
  const uint32_t handle=api->read_u32(obj+68);
  world_objs.erase(handle);
  if(world_fading.erase(handle)) return;   // (out of the registry since its fade began)
  if(world_reg.Remove(world_slot,handle)) ++(car ? world_n.removed_cars : world_n.removed_people);
}

// The object table (4096 slots) against the registry: everything this game
// has at the session start (fill) and every 10 s (audit: what the hooks
// missed, both ways). A car made before the hooks saw it is named by where it
// is: the traffic list, a parked record (0x83AD346C, 108 bytes, count
// *0x8309A48C, the car's handle at +96), a response squad, an El Train car by
// its type (vehicles.xtbl row 65, sp_eltrain_01), else a script's. People only
// once active (the hook adds the others when they are).
void WorldScan(uint64_t now,bool fill) {
  const uint8_t me=world_slot;
  std::unordered_set<uint32_t> traffic, parked;
  { const uint32_t head=api->read_u32(kTrafficList); uint32_t node=head;
    for(unsigned guard=0;node && guard<64 && Readable(node,12);++guard) { traffic.insert(api->read_u32(node+8)); node=api->read_u32(node); if(node==head) break; } }
  { const uint32_t n=std::min<uint32_t>(api->read_u32(0x8309A48C),4096);
    if(n && Readable(0x83AD346C,n*108)) for(uint32_t i=0;i<n;++i) if(const uint32_t h=api->read_u32(0x83AD346C+i*108+96)) parked.insert(h); }
  std::vector<uint32_t> response_cars; OwnResponseUnits(nullptr,&response_cars);
  std::unordered_set<cw::Key> seen;
  unsigned added=0;
  for(uint32_t index=0;index<4096;++index) {
    const uint32_t o=api->read_u32(kObjectTable+12+index*16);
    if(!o || !Readable(o+68,8) || (api->read_u32(o+68)&0xffff)!=index) continue;
    const uint32_t type=api->read_u32(o+72), handle=api->read_u32(o+68);
    if(api->read_u32(o+120)&0xC0000000u) continue;   // queued for deletion: 823FE4A8 sets 0xC0000000 and the object stays in the table until 823FDBE8 runs (review A2-7)
    if(type==5) {
      if(ResolveVehicle(handle)!=o || WorldLeftOut(o,handle,true)) continue;
      seen.insert(cw::MakeKey(me,handle));
      if(world_reg.Find(me,handle) || world_fading.count(handle)) continue;
      const uint32_t source=VehicleType(o)==65 ? 9 : traffic.count(handle) ? 2 : parked.count(handle) ? 5 :
                            std::count(response_cars.begin(),response_cars.end(),handle) ? 4 : 7;
      cw::Entry& e=world_reg.Add(me,handle,WorldCarKind(source),uint8_t(source),now);
      if(source==9) e.flags|=cw::kEntryExcluded;
      else if(source==7 && share_missions && MissionActive()) { e.flags|=cw::kEntryExcluded; world_mission_cars.insert(handle); }
      if(world_faded_back.erase(handle)) ++world_n.fade_back;   // (its fade ended without a delete: review A2-10)
      ++added;
    } else if(type==1) {
      if(ResolveHuman(handle)!=o || WorldLeftOut(o,handle,false)) continue;
      const bool known=world_reg.Find(me,handle)!=nullptr;
      if(!known && !PersonActive(o)) continue;
      seen.insert(cw::MakeKey(me,handle));
      if(known || world_fading.count(handle)) continue;
      world_reg.Add(me,handle,WorldPersonKind(o),0,now);
      if(world_faded_back.erase(handle)) ++world_n.fade_back;
      ++added;
    }
  }
  std::vector<cw::Key> gone;
  for(auto& [k,e]:world_reg.Entries()) if(!seen.count(k)) gone.push_back(k);
  for(cw::Key k:gone) { world_reg.Remove(cw::OwnerOf(k),cw::IdOf(k)); world_objs.erase(cw::IdOf(k)); }
  if(!fill) { world_n.audit_added+=added; world_n.audit_removed+=unsigned(gone.size()); }
  for(auto it=world_fading.begin();it!=world_fading.end();) {
    const bool here=ResolveHuman(it->first) || ResolveVehicle(it->first);
    if(here && now-it->second<=30000) { ++it; continue; }
    if(here && world_faded_back.size()<512) world_faded_back.insert(it->first);   // (the fade ended without a delete: 82441B00 / 82566ED8 clear the bits, review A2-10)
    it=world_fading.erase(it);
  }
  for(auto it=world_faded_back.begin();it!=world_faded_back.end();) it=ResolveHuman(*it) || ResolveVehicle(*it) ? std::next(it) : world_faded_back.erase(it);
}
void WorldRegistryOn(uint64_t now) {
  world_reg.Clear(); world_objs.clear(); world_fading.clear(); world_faded_back.clear(); world_mission_cars.clear(); world_n=WorldCounts{}; world_sent={};
  world_budget_sum={}; world_near_in={}; world_first_member=true;
  for(auto& m:world_members) m=cw::Member{};
  world_slot=my_slot.load(); world_on=true;
  WorldScan(now,true);
  unsigned cars=0, people=0;
  for(auto& [k,e]:world_reg.Entries()) ++(cw::IsCar(e.kind) ? cars : people);
  char line[240];
  std::snprintf(line,sizeof(line),"World registry on (slot %d): %u cars and %u people of this game's own at the start (object table); MP net time *0x83061944 = %u (probe only)",
                int(world_slot),cars,people,api->read_u32(0x83061944));
  ProbeLog(line);
}
void WorldRegistryOff() {
  world_reg.Clear(); world_objs.clear(); world_fading.clear(); world_faded_back.clear(); world_mission_cars.clear();
  for(auto& m:world_members) m=cw::Member{};
  world_on=false; world_slot=coopnet::kNoSlot; world_any_member=false; WorldProjEnded(0);
  world_spawns_on=false; world_view_count=0;
  ProbeLog("World registry off (session over)");
}

// kEventAuthority to the area's members: a player got into a car of this
// game's (their pose carries it from now on; the owner sends no record for
// it, F2) or the car is back with this game's own AI.
void WorldAuthority(const cw::Entry& e,bool driven,uint8_t was) {
  Event ev{kEventAuthority,{e.owner,e.id,e.authority,driven?1u:0u},0};
  for(int s=0;s<kSlots;++s) if(world_members[s].in_area && e.offer[s].created) QueueEventTo(ev,uint8_t(s));   // (a member without the car learns it from the create: review A2-8)
  static unsigned logged=0;
  if(logged++<30) {
    char line[160];
    if(driven) std::snprintf(line,sizeof(line),"World (owner): car %08X driven by slot %u now: its pose carries it (kEventAuthority)",e.id,unsigned(e.authority));
    else std::snprintf(line,sizeof(line),"World (owner): car %08X back with this game (at rest 1 s with nobody in it; slot %u had it)",e.id,unsigned(was));
    ProbeLog(line);
  }
}
// A fade the game began on one of its objects (82537D40: +53976 0x40000000,
// clock +54436, ms +54444, and every occupant's below; 824517A0: +212 0x80,
// clock +2488, ms +2492; the clock both store is *0x827AA6E0, less
// 1800000000 past that): the time left, for the members' fade.
uint16_t FadeLeft(uint32_t start,uint32_t ms) {
  if(!ms || ms>30000) return cw::kDefaultFadeMs;
  int32_t clock=int32_t(api->read_u32(0x827AA6E0));
  if(clock>1800000000) clock-=1800000000;
  return uint16_t(std::clamp<int32_t>(int32_t(ms)-(clock-int32_t(start)),0,int32_t(ms)));
}
// The registry's positions, flags, crews and authority, from the game objects (the owner, every tick).
void WorldRefresh(uint32_t player,uint64_t now) {
  const uint8_t me=world_slot;
  // The players' cars here: this player's, each other player's character's
  // (and its seat), the car one is getting into. The driver's pose carries a
  // car; a car with players as passengers only, the first of them.
  uint32_t in_car[kSlots]{}, seat[kSlots]{}, entering[kSlots]{};
  in_car[me]=api->read_u32(player+2496); seat[me]=api->read_u32(player+2500);
  for(int s=0;s<kSlots;++s) if(s!=me) {
    if(const uint32_t av=AvatarObjOf(s)) if(ResolveVehicle(api->read_u32(av+2496))) { in_car[s]=api->read_u32(av+2496); seat[s]=api->read_u32(av+2500); }
    if(remotes[s].enter_car && now<remotes[s].enter_until) entering[s]=remotes[s].enter_car;
  }
  auto driver_of=[&](uint32_t h,uint32_t v) {
    if(!h) return -1;
    for(int s=0;s<kSlots;++s) if(in_car[s]==h && seat[s]==0) return s;
    int p=-1;
    for(int s=0;s<kSlots && p<0;++s) if(in_car[s]==h || entering[s]==h) p=s;
    if(p<0) return -1;
    // A passenger or one getting in only while nobody but a player is at the
    // wheel: with this game's NPC driving, its records go on (review A2-9).
    const uint32_t d=SeatOccupant(v,0);
    return !d || d==api->read_u32(player+68) || IsAvatarHandle(d) ? p : -1;
  };
  const bool mission=share_missions && MissionActive();
  std::vector<std::pair<cw::Key,uint16_t>> faded;
  for(auto& [k,e]:world_reg.Entries()) {
    WorldObj& w=world_objs[e.id];
    if(cw::IsCar(e.kind)) {
      const uint32_t v=ResolveVehicle(e.id);
      if(!v) continue;   // (the audit takes it out)
      if(!w.classified) { w.classified=true; const int32_t t=VehicleType(v); e.model=uint16_t(t<0 ? 0 : t); if(!share_traffic) e.flags|=cw::kEntryExcluded; }
      if((e.flags&cw::kEntryExcluded) && !MissionActive() && world_mission_cars.erase(e.id)) { e.flags&=uint8_t(~cw::kEntryExcluded); e.changed_at=now; }   // (a mission's car left after the mission: streamed now)
      for(int i=0;i<3;++i) e.pos[i]=api->read_f32(v+20+i*4);
      if(api->read_u32(v+53976)&0x40000000u) { faded.push_back({k,FadeLeft(api->read_u32(v+54436),api->read_u32(v+54444))}); continue; }
      w.why=(api->read_u32(v+120)&0x04000000u) ? kWhyStreams : kWhyInactive;   // (its chunk unloaded: parked, no physics, S 7a)
      float speed2=0;
      if(const uint32_t vel=VehicleVelocityAddress(v)) for(int i=0;i<3;++i) { const float f=api->read_f32(vel+i*4); speed2+=f*f; }
      const bool rest=speed2<0.3f*0.3f;   // (no physics: at rest)
      const uint8_t hp=CarHealth(v), dmg=uint8_t(CarDamage(v)|CarSiren(v)); const uint32_t occ=OccupantHandle(v,0);
      uint8_t bits=0, drive=0; WorldDriveBits(v,e.id,now,bits,drive);
      const uint8_t act=uint8_t(bits|(drive&cw::kDriveHorn));   // (a car stopped at a light shows its brake lights: Package C)
      if(hp!=w.hp || dmg!=w.damage || occ!=w.occupant || act!=w.drive) { e.changed_at=now; w.hp=hp; w.damage=dmg; w.occupant=occ; w.drive=act; }
      e.flags=uint8_t((e.flags&~(cw::kEntryRest|cw::kEntryInactive))|(rest ? cw::kEntryRest : 0)|(w.why ? cw::kEntryInactive : 0));
      // Authority (F2): a player in it or getting in; back 1 s after it rests with nobody.
      if(const int d=driver_of(e.id,v); d>=0) {
        w.rest_since=0;
        if(!(e.flags&cw::kEntryPlayerCar) || e.authority!=d) { const uint8_t was=e.authority; e.authority=uint8_t(d); e.flags|=cw::kEntryPlayerCar; e.changed_at=now; WorldAuthority(e,true,was); ++world_n.taken; }
      } else if(e.flags&cw::kEntryPlayerCar) {
        if(!rest) w.rest_since=0;
        else if(!w.rest_since) w.rest_since=now;
        else if(now-w.rest_since>=1000) { const uint8_t was=e.authority; e.authority=me; e.flags&=uint8_t(~cw::kEntryPlayerCar); e.changed_at=now; w.rest_since=0; WorldAuthority(e,false,was); ++world_n.returned; }
      }
    } else {
      const uint32_t o=ResolveHuman(e.id);
      if(!o) continue;
      if(!w.classified) {
        w.classified=true;
        const uint32_t desc=api->read_u32(o+228);
        if(Readable(desc,612)) e.model=uint16_t(api->read_u32(desc+608)&0xFFFF);   // (the people group's model id: what WWA1 lists)
        char name[46];
        if(ModelName(o,name,sizeof(name))) {
          e.name=name;
          w.named=std::strncmp(name,"PD_",3) && name[0] && name[1] && name[2]=='_' && name[3] && name[3]!='X';   // (CapturePeds' story characters)
        }
        if(!share_peds) e.flags|=cw::kEntryExcluded;
      }
      e.kind=WorldPersonKind(o);   // (82400F10 moves people between kinds: a script's released to the street)
      for(int i=0;i<3;++i) e.pos[i]=api->read_f32(o+20+i*4);
      if((api->read_u32(o+212)&0x80) && api->read_u32(o+2492)) { faded.push_back({k,FadeLeft(api->read_u32(o+2488),api->read_u32(o+2492))}); continue; }
      // Mission and script characters as 2.1 sends them (only while a mission
      // runs); story characters on the street never (they stay at the church
      // stairs after a mission, unseen here: CapturePeds).
      w.why=!PersonActive(o) ? kWhyInactive : e.kind==cw::kKindMissionPerson && !mission ? kWhyNoMission : w.named && e.kind!=cw::kKindMissionPerson ? kWhyNamed : kWhyStreams;
      const uint32_t car=api->read_u32(o+2496);
      const bool seated=car && ResolveVehicle(car);
      e.car=seated ? car : 0; e.seat=seated ? uint8_t(std::min<uint32_t>(api->read_u32(o+2500),15)) : 0;
      e.flags=uint8_t((e.flags&~(cw::kEntryRest|cw::kEntryInactive))|(seated ? cw::kEntryRest : 0)|(w.why ? cw::kEntryInactive : 0));
      if(w.pos_at && now>w.pos_at) {
        const float dt=float(now-w.pos_at)/1000.0f;
        if(dt<1.0f) { w.vel[0]=(e.pos[0]-w.pos[0])/dt; w.vel[1]=(e.pos[2]-w.pos[2])/dt; }
      }
      std::memcpy(w.pos,e.pos,sizeof(w.pos)); w.pos_at=now;
      e.extra=uint8_t(world_actions && w.last_shot && now-w.last_shot<=1000 ? sizeof(cw::FireExt) : 0);   // (its records carry a fire part: Package C)
      if(seated) {   // a seated person goes at 2 Hz unless this changes
        const uint8_t team=TeamOf(o), weapon=uint8_t(std::clamp<int32_t>(HandWeapon(o)+1,0,255)), f=uint8_t((api->read_u32(o+780)!=0 ? 1 : 0) | (PersonDead(o) ? 2 : 0));
        if(team!=w.team || weapon!=w.weapon || f!=w.pflags || car!=w.occupant) { e.changed_at=now; w.team=team; w.weapon=weapon; w.pflags=f; w.occupant=car; }
      }
    }
  }
  for(auto& [k,ms]:faded) {
    const bool car=cw::IsCar(world_reg.Entries().at(k).kind);
    world_reg.Remove(cw::OwnerOf(k),cw::IdOf(k),ms);
    world_fading[cw::IdOf(k)]=now; world_objs.erase(cw::IdOf(k));
    ++world_n.fades; ++(car ? world_n.removed_cars : world_n.removed_people);
  }
}

// ---- The shared area: actions and effects (co-op 2.2, Package C) ----
// docs/COOP_V2.md section 11, notes/coop-2.2/plan_C.md. What the host's
// people and cars do shows on every member's screen the way the game's MP
// mirrors its NPCs (823A8240: a fire byte, a burn byte and the aim in the NPC
// state, not an event per shot). mod.ini world_actions = 0 turns it off on
// this game, sent and applied alike.
//  - Gunfire: the host counts the rounds of its world's people in FireHook
//    (a round re-arms the refire timer, ammo +24, or uses one, +16) with the
//    first pellet's hit point (BulletRayHook); their records carry the gun
//    raised and, for a second after a shot, a FireExt (the running count,
//    trigger held, blank, the aim point). The member plays them on the copy as
//    other players' shots are played (TryRemoteShots): a single-trigger gun
//    one 82470A50(copy, 0, &aim, blank) per round, an automatic one with the
//    trigger held (a call each update once the refire timer is due) and the
//    release 82474738 + 82476B70; under fire_replay_depth (it hurts no one)
//    and copy_fire_replay_depth (no car takes the hit: DetonateHook); within
//    150 m of the camera, 8 calls an update at most; a seated copy fires
//    blanks (r6 = 1: flash and sound, no ray).
//  - Driver inputs: CarRec's brake and reversing bits and its drive byte
//    (steering, horn, the engine bit). The member answers the brake getter
//    82533DA8 (r3 handle, f1 = E+408; the tail lights' 10-frame average
//    82527ED8 reads it) and the reversing test 82535C00 (r3 the vehicle; the
//    reverse lights 8255ACA0) with the host's values for a copy, holds the
//    horn with 825241F0 each update (825231A0 stops it on a frame nobody
//    called it) and writes E+412 so the physics' steering (E+412 + E+416, the
//    packer 82583810) is the host's.
//  - Burning: PedRec flag 5 lights the copy with 8245E3A0(copy, 0, 0, 0, 0.0)
//    as 823A8240 does (its refusal bits +120 0xD0000000 masked for the call)
//    and 8245E0B8(handle, 0) puts it out; the member's own ignites of copies
//    are refused (IgniteHook). 4 burn slots without the MP byte (0x8309A9E8,
//    24 bytes, +0 the handle): a full set loses a dead or unseen one, else
//    the one furthest from the camera - never this player or another
//    player's character for a copy.
//  - Explosions: a prop's death (82138AE8 from 823FAA38, LR 823FAC68) and a
//    car's later second explosion (8252B870, LR 8252B9B0) go to the members
//    within 200 m (kEventExplosion) and are played there under
//    world_boom_depth (a prop once per game: 120 s by its uid); a member's
//    own second explosion of a copy car is held (the host's comes). Rockets
//    and throws of the host's people go as WPJ1 with the shooter's id to each
//    member; a copy's own replayed launch is refused there.
//  - A member's bullet on a car of the host's world goes to the host
//    (kEventCarDamage, spec A/9: 825397E0's arguments, the weapon as its
//    table index) and is applied there with the member's character as the
//    attacker; the host's replay of that round leaves the car alone.
struct WorldActOwner {
  unsigned rounds=0, bursts=0, blanks=0, hits=0, aim_far=0, guns_past=0, fire_records=0, fire_bytes=0;
  std::unordered_set<uint32_t> shooters;
  unsigned car_records=0, car_phys=0, brake=0, reverse=0, horn=0, steer=0, forced_on=0, forced_off=0;
  unsigned booms=0, booms_props=0, booms_cars=0, skip_wreck=0, skip_proj=0, skip_queue=0, skip_replay=0, skip_far=0, skip_other=0, bad_def=0;
  unsigned projs=0, proj_booms=0;
  unsigned car_hits=0, car_hits_gone=0, car_hits_driven=0, car_hits_bad=0, replay_car_hits=0;
};
WorldActOwner world_ao;                 // host, 10 s (game thread)
enum : uint8_t { kCopyFireNotSetUp=0, kCopyFireLying, kCopyFireOtherGun, kCopyFireNoAmmo, kCopyFireRaising, kCopyFireRefused, kCopyFireOld, kCopyFireFar, kCopyFireCap, kCopyFireReasons };
const char* const kCopyFireNames[kCopyFireReasons]={"not set up","lying","other gun","no ammo","raising","refused","too old","past 150 m","frame cap"};
enum : uint8_t { kBurnCar=0, kBurnKeptOwn, kBurn3694, kBurnDead, kBurnWater, kBurnOther, kBurnReasons };
const char* const kBurnNames[kBurnReasons]={"in a car","slots full: this player's or a player's character would go out","+3694 0x80","dead","under water","other"};
enum : uint8_t { kIgniteBlast=0, kIgnitePatch, kIgniteSpread, kIgniteCar, kIgniteOther, kIgniteKinds };
struct WorldActMember {
  unsigned counted=0, played=0, singles=0, held_frames=0, releases=0, blanks=0, raises=0, waited[kCopyFireReasons]{};
  unsigned brake=0, reverse=0, horn_frames=0, steer_writes=0, steer_nophys=0, engine_writes=0, e416_n=0; double e416=0;
  unsigned lit=0, put_out=0, relit=0, masked=0, burn_refused[kBurnReasons]{}, ignites_refused[kIgniteKinds]{};
  unsigned booms=0, props=0, props_here=0, props_missing=0, cars=0, cars_missing=0, here_already=0, own_act=0, held_again=0, held_replay=0, chain=0, copy_car_held=0, bad_def=0;
  unsigned car_hits_sent=0, car_hits_unnamed=0;
  double fire_ms=0; unsigned fire_calls=0;
};
WorldActMember world_am;                // member, 10 s (game thread)
// Member: the car copies' brake and reversing bits for the getter hooks
// (82533DA8 / 82535C00, whatever thread asks): per handle index the handle
// << 32 | the CarRec bits, 0 when the copy's inputs are older than a second;
// the game thread keeps when each was set.
std::atomic<uint64_t> world_drive_slot[4096]{};
std::atomic<uint32_t> world_drive_n{0}, world_brake_answers{0}, world_reverse_answers{0};
std::unordered_map<uint32_t,uint64_t> world_drive;   // member (game thread): a copy's handle with a slot set -> when
// Member: props exploded here (their uid, *(*(mover+216)+40)) -> when, and whether it was the host's played here.
struct PropBoom { uint64_t at=0; bool replayed=false; };
std::unordered_map<uint32_t,PropBoom> world_prop_boom;
// Host: its people's projectiles sent to the area (the handle -> the shooter's id), so the explosion goes too after the person is gone.
std::unordered_map<uint32_t,uint32_t> world_proj_sent;
constexpr uint32_t kExplosionDefs=0x829ACEB8, kExplosionDefCount=0x8370D114, kExplosionDefSize=140;   // 140-byte defs (research S)

// ---- Gunfire, the host ----
// FireHook for a person of the world stream while the area has members: the
// call runs here with its rounds counted and its first pellet's hit point
// kept (BulletRayHook by the shooter, r5). A seated person's record goes at
// the band rate while it fires (its resting 2 Hz is for a car that sits).
bool WorldFireCapture(WmlContext* c,uint8_t* b,uint32_t human) {
  if(!world_on || !world_actions || !world_any_member || !Readable(human,4252)) return false;
  const uint32_t handle=api->read_u32(human+68);
  cw::Entry* e=world_reg.Find(world_slot,handle);
  if(!e || cw::IsCar(e->kind)) return false;
  const uint32_t ammo=EquippedAmmo(human), blank=uint32_t(api->get_r(c,6));
  const uint32_t rounds=ammo ? api->read_u32(ammo+16) : 0, timer=ammo ? api->read_u32(ammo+24) : 0;
  capture_shooter=human; hit_valid=false;
  original_fire(c,b);
  capture_shooter=0;
  if(!ammo || !Readable(ammo,28) || (api->read_u32(ammo+16)==rounds && api->read_u32(ammo+24)==timer)) return true;
  const uint64_t now=MonoMs();
  WorldObj& w=world_objs[handle];
  const unsigned n=unsigned(std::clamp<int32_t>(int32_t(rounds-api->read_u32(ammo+16)),1,4));
  const uint32_t info=api->read_u32(ammo+8);
  w.shots=uint8_t(w.shots+n); w.last_shot=now; w.blank=blank!=0; w.hit_ok=hit_valid;
  w.burst=Readable(info,kWeaponInfoSize) && api->read_u32(info+96)!=0;   // (Trigger_Type: an automatic gun)
  if(hit_valid) std::memcpy(w.hit,hit_point,sizeof(w.hit));
  if(e->Seated()) e->changed_at=now;
  world_ao.rounds+=n; if(w.burst) world_ao.bursts+=n; if(w.blank) world_ao.blanks+=n; if(hit_valid) ++world_ao.hits;
  if(world_ao.shooters.size()<256) world_ao.shooters.insert(handle);
  return true;
}
// A person's fire part while it fired in the last second (never in a create).
bool WorldFireExt(const cw::Entry& e,uint32_t o,const float ref[3],uint64_t now,cw::FireExt& f) {
  if(!world_actions || !e.extra) return false;   // (exactly when WorldRefresh charged it this tick)
  const auto it=world_objs.find(e.id);
  if(it==world_objs.end() || !it->second.last_shot) return false;
  const WorldObj& w=it->second;
  f=cw::FireExt{};
  f.fire=uint8_t((w.shots&cw::kFireCount)|(w.burst && now-w.last_shot<=250 ? cw::kFireHeld : 0)|(w.blank ? cw::kFireBlank : 0));
  if(e.Seated() && api->read_u32(o+516)==3) f.fire|=cw::kFireDriveBy;
  if(w.hit_ok) { if(cw::QPos(ref,w.hit,f.aim)) f.fire|=cw::kFireAim; else ++world_ao.aim_far; }
  return true;
}

// ---- Driver inputs, the host ----
// 825241F0(handle): the horn (its voice v+53788, v+53976 |= 0x4008, the
// driver's AI told). Noted for the car's records (150 ms).
WmlGuestFunction original_horn=nullptr;
void HornHook(WmlContext* c,uint8_t* b) {
  const uint32_t handle=uint32_t(api->get_r(c,3));
  original_horn(c,b);
  if(world_on && world_any_member && hosting.load()) if(const auto it=world_objs.find(handle); it!=world_objs.end()) it->second.horn_at=MonoMs();
}
// The brake and reversing bits (CarRec.damage 6-7) and the drive byte of a
// car of this game's (E = *(v+224) while byte v+50300 == 1: physics up).
void WorldDriveBits(uint32_t v,uint32_t handle,uint64_t now,uint8_t& bits,uint8_t& drive) {
  bits=0; drive=0;
  if(!world_actions) return;
  float steer=0;
  if(api->read_u8(v+50300)==1) {
    const uint32_t E=api->read_u32(v+224);
    if(Readable(E,484)) {
      if(api->read_f32(v+53500)>0.2f) bits|=cw::kCarBrake;   // (8255A730's test of the average for a car the player doesn't drive)
      const uint32_t info=api->read_u32(v+50324);
      if(int8_t(api->read_u8(E+180))!=0 && Readable(info,288) && api->read_u32(info+284)!=6) bits|=cw::kCarReverse;   // (82535C00)
      const float s=api->read_f32(E+412)+api->read_f32(E+416);
      if(std::isfinite(s)) steer=std::clamp(s,-1.0f,1.0f);
    }
  }
  drive=cw::QSteer(steer);
  if(api->read_u32(v+53976)&0x08000000u) drive|=cw::kDriveEngine;
  if(const auto it=world_objs.find(handle); it!=world_objs.end() && it->second.horn_at && now-it->second.horn_at<150) drive|=cw::kDriveHorn;
}

// ---- Explosions, the host's side and the member's guards ----
// Kinds of kEventExplosion's source (a[0] bits 16-19).
enum : uint8_t { kBoomProp=1, kBoomCarSecond=2 };
// The defs table index of an explosion (82138AE8's r3), or -1.
int32_t ExplosionDefIndex(uint32_t def) {
  const uint32_t n=api->read_u32(kExplosionDefCount);
  if(def<kExplosionDefs || (def-kExplosionDefs)%kExplosionDefSize || n>1024) return -1;
  const uint32_t i=(def-kExplosionDefs)/kExplosionDefSize;
  return i<n ? int32_t(i) : -1;
}
inline uint32_t PackDir(const float* f) {   // three signed 10-bit values x 511
  uint32_t out=0;
  for(int i=0;i<3;++i) out|=uint32_t(std::clamp<int32_t>(int32_t(std::lround((std::isfinite(f[i]) ? f[i] : 0.0f)*511)),-511,511)&0x3FF)<<(10*i);
  return out;
}
inline void UnpackDir(uint32_t p,float* f) { for(int i=0;i<3;++i) { int32_t v=int32_t((p>>(10*i))&0x3FF); if(v&0x200) v-=0x400; f[i]=float(v)/511.0f; } }
// A prop's uid (*(*(mover+216)+40)) and sub (+224), research S.
bool MoverId(uint32_t mover,uint32_t& uid,uint32_t& sub) {
  if(!Readable(mover,228)) return false;
  const uint32_t rec=api->read_u32(mover+216);
  if(!Readable(rec,44)) return false;
  uid=api->read_u32(rec+40); sub=api->read_u32(mover+224)&0xFF;
  return true;
}
// 82138AE8 (ExplosionHook, before the explosion runs). Host: a prop's death
// or a car's second explosion near a member goes to the members (each game
// plays explosions on its own player, R 3.0: the owner's world's ones too).
// Member: false holds a local one the host's world sends instead - a copy
// car's own explosion (not the wreck replay 82542650, LR 82542BFC) and a
// prop the host's explosion already broke here.
bool WorldExplosion(WmlContext* c) {
  const uint32_t lr=uint32_t(api->get_lr(c)), def=uint32_t(api->get_r(c,3)), source=uint32_t(api->get_r(c,4));
  const uint64_t now=MonoMs();
  if(!hosting.load()) {
    if(world_boom_depth) {   // (one of the host's played here: a prop it breaks on the way is broken here)
      uint32_t uid, sub;
      if(lr==0x823FAC68u && MoverId(source,uid,sub)) { world_prop_boom[uid]={now,true}; ++world_am.chain; }
      return true;
    }
    if(!WorldLeversOn() || !WorldActionsWith(0)) return true;
    if(lr==0x823FAC68u) {
      uint32_t uid, sub;
      if(!MoverId(source,uid,sub)) return true;
      if(copy_fire_replay_depth) { ++world_am.held_replay; return false; }   // (broken by a copy's replayed round: the host's own round decides, its explosion comes; 823FAA38 shows its fallback here)
      const auto it=world_prop_boom.find(uid);
      if(it!=world_prop_boom.end() && it->second.replayed && now-it->second.at<120000) { ++world_am.held_again; return false; }   // (823FAA38 then shows only the def's effect, its own fallback)
      world_prop_boom[uid]={now,false};
      return true;
    }
    if(lr!=0x82542BFCu && source && Readable(source,72) && api->read_u32(source+72)==5 && WorldCarId(api->read_u32(source+68))) { ++world_am.copy_car_held; return false; }
    return true;
  }
  if(!world_on || !world_any_member || replaying_proj) return true;
  uint8_t kind=0;
  if(lr==0x823FAC68u) kind=kBoomProp;
  else if(lr==0x8252B9B0u) kind=kBoomCarSecond;
  else {
    if(lr==0x8252CBACu) ++world_ao.skip_wreck;                                    // (the member's wreck replay 82542650 explodes)
    else if(lr==0x825BF5C0u || lr==0x825BF6B0u) ++world_ao.skip_proj;             // (projectiles: WPJ1)
    else if(lr==0x8213926Cu) ++world_ao.skip_queue;                               // (the deferred queue: sent at its first call)
    else if(lr==0x82542BFCu || lr==0x8238BF44u || lr==0x8238BF84u) ++world_ao.skip_replay;
    else ++world_ao.skip_other;
    return true;
  }
  const int32_t index=ExplosionDefIndex(def);
  const uint32_t rows=uint32_t(api->get_r(c,6)), pos=uint32_t(api->get_r(c,7)), sp=uint32_t(api->get_r(c,1));
  if(index<0 || !Readable(rows,36) || !Readable(pos,12)) { ++world_ao.bad_def; return true; }
  Event e{kEventExplosion,{},0};
  uint32_t flags=0;
  if(kind==kBoomProp) {
    uint32_t uid=0, sub=0;
    if(MoverId(source,uid,sub)) { flags|=1; e.a[6]=uid; e.a[7]=sub&0xFF; }
  } else {
    const cw::Entry* car=source && Readable(source,72) ? world_reg.Find(world_slot,api->read_u32(source+68)) : nullptr;
    if(!car || !cw::IsCar(car->kind)) { ++world_ao.skip_other; return true; }
    e.a[5]=car->id;   // (the call's +92 is the car's info +24: the member takes its copy's own)
    (void)sp;
  }
  if(api->read_u8(sp+103)) flags|=2;
  e.a[0]=uint32_t(index)|uint32_t(kind)<<16|uint32_t(world_slot&15)<<20|flags<<24;
  float p[3], f[3];
  for(int i=0;i<3;++i) { p[i]=api->read_f32(pos+i*4); f[i]=api->read_f32(rows+24+i*4); }
  std::memcpy(&e.a[1],p,12); e.a[4]=PackDir(f);
  // The attacker: this player, another player's character (their slot), a person of the world stream (its id).
  const uint32_t attacker=uint32_t(api->get_r(c,5)), player=api->read_u32(0x8309ABEC);
  if(attacker && attacker==player) e.a[7]|=1u<<8;
  else if(const int s=attacker ? AvatarSlotOf(attacker) : -1; s>=0) e.a[7]|=2u<<8|uint32_t(s)<<16;
  else if(attacker && Readable(attacker,72)) {
    const uint32_t h=api->read_u32(attacker+68);
    if(const cw::Entry* pe=world_reg.Find(world_slot,h); pe && !cw::IsCar(pe->kind) && kind==kBoomProp) { e.a[7]|=3u<<8; e.a[5]=h; }
  }
  bool sent=false;
  for(int s=0;s<kSlots;++s) if(WorldMemberWithActions(s) && cw::Dist(p,world_members[s].eye)<=200.0f) { QueueEventTo(e,uint8_t(s)); sent=true; }
  if(!sent) { ++world_ao.skip_far; return true; }
  ++world_ao.booms; ++(kind==kBoomProp ? world_ao.booms_props : world_ao.booms_cars);
  return true;
}
bool WorldMemberWithActions(int slot) { return slot>=0 && slot<kSlots && world_members[slot].in_area && WorldActionsWith(slot); }
// Host: the people of its world stream fire rockets and throw too: their
// projectiles go to each member of the area (WPJ1 with the shooter's id).
uint32_t WorldProjShooter(uint32_t owner) {
  if(!owner || !world_on || !world_actions || !world_any_member || !hosting.load()) return 0;
  const cw::Entry* e=world_reg.Find(world_slot,owner);
  return e && !cw::IsCar(e->kind) ? owner : 0;
}
void WorldProjOut(const ProjPacket& p) {
  if(p.kind==1) { if(world_proj_sent.size()>256) world_proj_sent.clear(); world_proj_sent[p.id]=p.shooter; ++world_ao.projs; }
  else { world_proj_sent.erase(p.id); ++world_ao.proj_booms; }
  std::lock_guard lock(event_mutex);
  for(int s=0;s<kSlots;++s) if(WorldMemberWithActions(s) && proj_out.size()<512) proj_out.push_back({uint8_t(s),p});
}
void WorldProjEnded(uint32_t proj_handle) { if(proj_handle) world_proj_sent.erase(proj_handle); else world_proj_sent.clear(); }
// Host: a projectile of one of its people sent when it was made (the shooter's id), else 0.
uint32_t WorldProjSent(uint32_t proj_handle) { const auto it=world_proj_sent.find(proj_handle); return it!=world_proj_sent.end() ? it->second : 0; }

// ---- A member's bullets on the host's cars (spec A/9) ----
// DetonateHook, before anything else. Member: 825397E0 from the bullet hit
// 82536270 (LR 82536470, the caller's r1+84 = 2) by this player on a car
// copy of the host's world: its arguments go to the host (r6 the weapon's
// info as its table index, r7 the hit point, r8 the surface normal, r9 the
// push, r10 the damage; f1 / f2 are the call's constants) and the copy takes
// nothing here (the host's damage stream brings the result). Host: the replay
// of that member's round (TryRemoteShots) on a car of its world is left to
// the member's event (the member's screen decides the hit).
bool WorldCarHit(WmlContext* c,uint32_t v) {
  if(!world_actions || uint32_t(api->get_lr(c))!=0x82536470u || !Readable(v,72)) return false;
  const uint32_t sp=uint32_t(api->get_r(c,1)), attacker=uint32_t(api->get_r(c,4)), handle=api->read_u32(v+68);
  if(api->read_u32(sp+84)!=2) return false;
  if(!hosting.load()) {
    if(replicated_depth || fire_replay_depth || !attacker || attacker!=api->read_u32(0x8309ABEC) || !WorldActionsWith(0)) return false;
    const uint32_t id=WorldCarId(handle);
    if(!id) return false;
    const int32_t w=WeaponIndex(uint32_t(api->get_r(c,6)));
    const uint32_t pos=uint32_t(api->get_r(c,7)), normal=uint32_t(api->get_r(c,8)), push=uint32_t(api->get_r(c,9));
    if(w<0 || w>255 || !Readable(pos,12) || !Readable(push,12)) { ++world_am.car_hits_unnamed; return false; }   // (blocked as before: the copy decides nothing)
    float n[3]={0,0,0}, f[3];
    const bool has_normal=Readable(normal,12);   // (the hit record's +24, 8246FD18's: read as 3 floats)
    for(int i=0;i<3;++i) { if(has_normal) n[i]=api->read_f32(normal+i*4); f[i]=api->read_f32(push+i*4); }
    Event e{kEventCarDamage,{id,uint32_t(w)|uint32_t(std::clamp<int32_t>(int32_t(api->get_r(c,10)),0,0xFFFFFF))<<8},0};
    for(int i=0;i<3;++i) e.a[2+i]=api->read_u32(pos+i*4);
    e.a[5]=PackDir(n); std::memcpy(&e.a[6],&f[0],4); std::memcpy(&e.a[7],&f[1],4); e.f=f[2];
    QueueEventTo(e,0); ++world_am.car_hits_sent;
    return true;
  }
  if(!fire_replay_depth || !world_on) return false;
  const int s=attacker ? AvatarSlotOf(attacker) : -1;
  const cw::Entry* e=world_reg.Find(world_slot,handle);
  if(!WorldMemberWithActions(s) || !e || !cw::IsCar(e->kind) || (e->flags&cw::kEntryPlayerCar)) return false;   // (a car a player drives: its own replays decide, as in 2.1)
  ++world_ao.replay_car_hits;
  return true;
}
// Host: a member's bullet hit (kEventCarDamage) on a car of its world, with
// the member's character as the attacker, through the game's own 825397E0
// (its hit points, smoke, fire, the wreck 8252BF38 with its script calls).
void WorldCarDamageEvent(GuestScratch& scratch,const Event& e,uint32_t avatar) {
  if(!world_actions || !world_on) return;
  const cw::Entry* car=world_reg.Find(world_slot,e.a[0]);
  const uint32_t v=car && cw::IsCar(car->kind) ? ResolveVehicle(e.a[0]) : 0;
  if(!v) { ++world_ao.car_hits_gone; return; }
  if((car->flags&cw::kEntryPlayerCar) || OtherGamesCar(v)) { ++world_ao.car_hits_driven; return; }   // (a player drives it: the replays of the round decide there, as in 2.1)
  const uint32_t base=api->read_u32(kWeaponInfos), count=api->read_u32(kWeaponCount), w=e.a[1]&0xFF;
  float pos[3], n[3], f[3]={0,0,0};
  std::memcpy(pos,&e.a[2],12); UnpackDir(e.a[5],n); std::memcpy(&f[0],&e.a[6],4); std::memcpy(&f[1],&e.a[7],4); f[2]=e.f;
  bool ok=avatar && base && count<=255 && w<count;
  for(int i=0;i<3;++i) ok=ok && std::isfinite(pos[i]) && std::isfinite(f[i]) && std::abs(f[i])<100000;
  if(!ok) { ++world_ao.car_hits_bad; return; }
  const uint32_t d=(scratch.data()+2900+15)&~15u;
  for(int i=0;i<3;++i) { api->write_f32(d+i*4,pos[i]); api->write_f32(d+16+i*4,n[i]); api->write_f32(d+32+i*4,f[i]); }
  api->write_f32(d+12,0); api->write_f32(d+28,0); api->write_f32(d+44,0);
  const uint32_t sp=scratch.ctx.r1.u32;
  api->write_u32(sp+84,2); api->write_u32(sp+92,d+32); api->write_u8(sp+103,0); api->write_u8(sp+119,0);
  scratch.ctx.r3.u64=v; scratch.ctx.r4.u64=avatar; scratch.ctx.r5.u64=0; scratch.ctx.r6.u64=base+w*kWeaponInfoSize;
  scratch.ctx.r7.u64=d; scratch.ctx.r8.u64=d+16; scratch.ctx.r9.u64=d+32; scratch.ctx.r10.u64=e.a[1]>>8;
  scratch.ctx.f1.f64=api->read_f32(0x820380CC); scratch.ctx.f2.f64=api->read_f32(0x8208A354);   // (82536270's constants for the call)
  const uint32_t was=applying_vehicle; applying_vehicle=v;
  ++replicated_depth; scratch.Call(0x825397E0); --replicated_depth;
  applying_vehicle=was;
  ++world_ao.car_hits;
  static unsigned logged=0;
  if(logged++<10) { char line[160]; std::snprintf(line,sizeof(line),"World (owner): slot %u's bullet hit car %08X here (weapon %u, %u damage)",unsigned(e.origin),e.a[0],w,e.a[1]>>8); ProbeLog(line); }
}

// The 10 s lines (plan_C): the owner's, with members in its area.
void WorldActionsOwnerLog(double secs) {
  WorldActOwner& o=world_ao;
  char line[700];
  if(o.rounds || o.fire_records) {
    std::snprintf(line,sizeof(line),"World (owner) gunfire: rounds %u by %zu people (bursts %u, blanks %u, hit points %u, aim past 655 m %u, guns past index 62 %u), fire parts in %u records (%.1f KB)",
                  o.rounds,o.shooters.size(),o.bursts,o.blanks,o.hits,o.aim_far,o.guns_past,o.fire_records,o.fire_bytes/1024.0);
    ProbeLog(line);
  }
  if(o.car_records) {
    std::snprintf(line,sizeof(line),"World (owner) driving: car records with brake %u, reverse %u, horn %u, steering %u (of %u with physics, %u in all); lights forced on %u / off %u",
                  o.brake,o.reverse,o.horn,o.steer,o.car_phys,o.car_records,o.forced_on,o.forced_off);
    ProbeLog(line);
  }
  if(o.booms || o.skip_far || o.skip_wreck || o.skip_proj || o.skip_queue || o.projs || o.proj_booms || o.bad_def) {
    std::snprintf(line,sizeof(line),"World (owner) explosions: sent %u (props %u, cars' second %u), skipped as wrecks %u / projectiles %u / queue %u / replays %u / far %u / other %u, bad def %u; NPC projectiles sent %u, exploded %u",
                  o.booms,o.booms_props,o.booms_cars,o.skip_wreck,o.skip_proj,o.skip_queue,o.skip_replay,o.skip_far,o.skip_other,o.bad_def,o.projs,o.proj_booms);
    ProbeLog(line);
  }
  if(o.car_hits || o.car_hits_gone || o.car_hits_driven || o.car_hits_bad || o.replay_car_hits) {
    std::snprintf(line,sizeof(line),"World (owner) members' bullets on its cars: applied %u (car gone %u, a player drives it %u, bad %u); their replayed rounds' car hits left to their events %u",
                  o.car_hits,o.car_hits_gone,o.car_hits_driven,o.car_hits_bad,o.replay_car_hits);
    ProbeLog(line);
  }
  (void)secs;
  world_ao=WorldActOwner{};
}

// The records (world.h), relative to ref. False: the offset saturates (more
// than 655 m from ref), skipped and counted (M9).
bool DescribeCar(uint32_t v,const cw::Entry& e,const float ref[3],cw::CarRec& r) {
  float pos[3], rows[9], vel[3]{}, ang[3]{};
  for(int i=0;i<3;++i) pos[i]=api->read_f32(v+20+i*4);
  for(int i=0;i<9;++i) rows[i]=api->read_f32(v+32+i*4);
  r.id=e.id; r.model=e.model;
  if(!cw::QPos(ref,pos,r.pos)) return false;
  cw::QRot(rows,r.rot);
  if(const uint32_t va=VehicleVelocityAddress(v)) for(int i=0;i<3;++i) { vel[i]=api->read_f32(va+i*4); ang[i]=api->read_f32(va+16+i*4); }   // (no physics: zero)
  for(int i=0;i<3;++i) { r.vel[i]=cw::QVel(vel[i]); r.ang[i]=cw::QAng(ang[i]); }
  r.damage=uint8_t(CarDamage(v)|CarSiren(v)); r.hp=CarHealth(v);   // (CarDamage never carries the fade bit 0x40000000)
  uint8_t bits=0;
  WorldDriveBits(v,e.id,MonoMs(),bits,r.drive);   // (Package C: brake lights, reversing, steering, horn, the engine bit)
  r.damage|=bits;
  return true;
}
// PedRec flags: dead from the hit points (+1912), never the fade bits (C2).
uint8_t WorldPedFlags(uint32_t o,const cw::Entry& e) {
  const float fx=api->read_f32(o+56), fz=api->read_f32(o+64);
  const uint32_t f216=api->read_u32(o+216), f120=api->read_u32(o+120);
  uint8_t f=0;
  if(api->read_f32(o+32)*fz-api->read_f32(o+40)*fx>=0) f|=cw::kPedSign;
  if(e.kind==cw::kKindMissionPerson) f|=cw::kPedMission;
  if(PersonDead(o)) f|=cw::kPedDead;
  if(f216&0x20000000u) f|=cw::kPedCrouch;
  if(api->read_u32(o+780)) f|=cw::kPedRagdoll;
  if(f216&0x00400000u) f|=cw::kPedBurning;
  if(f120&0x28000000u) f|=cw::kPedHidden;   // hidden (0x08000000) or a cutscene actor (0x20000000): shown again by the member, never removed (F10)
  return f;
}
// The weapon byte (kWeapon*): the gun's index + 1 (past 62 sent as 0, counted) and the gun raised (+216 0x80000000).
uint8_t WorldWeapon(uint32_t o) {
  const int32_t w=HandWeapon(o);
  if(w>=62) ++world_ao.guns_past;
  return uint8_t((w>=0 && w<62 ? w+1 : 0)|((api->read_u32(o+216)&0x80000000u) ? cw::kWeaponRaised : 0));
}
// ext: its fire part when it fired in the last second (none in a create).
bool DescribePerson(uint32_t o,const cw::Entry& e,const float ref[3],cw::PedRec& r,cw::FireExt* ext=nullptr) {
  float pos[3];
  for(int i=0;i<3;++i) pos[i]=api->read_f32(o+20+i*4);
  r.id=e.id;
  if(!cw::QPos(ref,pos,r.pos)) return false;
  const float fwd[3]={api->read_f32(o+56),api->read_f32(o+60),api->read_f32(o+64)};
  r.yaw=cw::QYaw(cw::YawOf(fwd));
  r.flags=WorldPedFlags(o,e);
  r.state=uint8_t(std::min<uint32_t>(api->read_u32(o+516),15)<<4 | std::min<uint32_t>(api->read_u32(o+520),15));
  r.anim=AnimSet(o); r.action=CurrentPedAction(o);
  const auto it=world_objs.find(e.id);
  r.vel[0]=cw::QPedVel(it!=world_objs.end() ? it->second.vel[0] : 0); r.vel[1]=cw::QPedVel(it!=world_objs.end() ? it->second.vel[1] : 0);
  r.team=TeamOf(o); r.weapon=WorldWeapon(o);
  if(ext && WorldFireExt(e,o,ref,MonoMs(),*ext)) r.weapon|=cw::kWeaponFire;
  return true;
}
void DescribeSeated(uint32_t o,const cw::Entry& e,cw::SeatedRec& r,const float* ref=nullptr,cw::FireExt* ext=nullptr) {
  r.id=e.id; r.car=e.car; r.seat=e.seat; r.team=TeamOf(o); r.weapon=WorldWeapon(o); r.flags=uint8_t(WorldPedFlags(o,e)|cw::kPedSeated);
  if(ext && ref && WorldFireExt(e,o,ref,MonoMs(),*ext)) r.weapon|=cw::kWeaponFire;
}
// A create (WWC1) with its tail: the name and look of a person (ReadLook,
// the 8238B0B8 block), the paint of a car (ReadCarLook, CarLookPacket's
// content: a game call, read once a tick per car whoever it goes to, review
// A2-16), a car's crew, the first record.
bool WorldCreate(GuestScratch& scratch,cw::Entry& e,const float ref[3],uint64_t now,std::vector<uint8_t>& out,std::unordered_map<uint32_t,std::vector<uint8_t>>& paints) {
  cw::CreateMsg c;
  c.head.id=e.id; c.head.time=uint32_t(now); c.head.kind=e.kind; c.head.model=e.model; c.head.team=0xFF;
  std::memcpy(c.head.ref,ref,sizeof(c.head.ref));   // (the first record's positions are from here)
  c.head.authority=uint8_t(e.authority|((e.flags&cw::kEntryPlayerCar) ? cw::kAuthorityPlayer : 0));   // (a car a player drives: their pose carries it, no records)
  if(cw::IsCar(e.kind)) {
    const uint32_t v=ResolveVehicle(e.id);
    cw::CarRec r;
    if(!v || !DescribeCar(v,e,ref,r)) return false;
    auto [pt,fresh]=paints.try_emplace(e.id);
    if(fresh) pt->second=ReadCarLook(scratch,v);
    if(pt->second.size()<=cw::kMaxPaint) c.paint=pt->second;
    for(auto& [k,p]:world_reg.Entries()) if(p.Seated() && p.car==e.id && p.Streams() && c.crew.size()<8) c.crew.push_back(p.id);
    c.first.assign(reinterpret_cast<const uint8_t*>(&r),reinterpret_cast<const uint8_t*>(&r)+sizeof(r));
  } else {
    const uint32_t o=ResolveHuman(e.id);
    if(!o) return false;
    c.head.team=TeamOf(o);
    c.name=e.name.substr(0,cw::kMaxName);
    uint8_t look[127];
    c.look.assign(look,look+ReadLook(o,look,sizeof(look)));
    if(e.Seated() && world_reg.Find(e.owner,e.car)) {
      cw::SeatedRec r; DescribeSeated(o,e,r);
      c.head.car=e.car; c.head.seat=e.seat;
      c.first.assign(reinterpret_cast<const uint8_t*>(&r),reinterpret_cast<const uint8_t*>(&r)+sizeof(r));
    } else {
      cw::PedRec r;
      if(!DescribePerson(o,e,ref,r)) return false;
      c.first.assign(reinterpret_cast<const uint8_t*>(&r),reinterpret_cast<const uint8_t*>(&r)+sizeof(r));
    }
  }
  out.resize(cw::kMaxMessage);
  const size_t n=cw::WriteCreate(c,out.data(),out.size());
  out.resize(n);
  if(n) e.create_bytes=uint16_t(n);   // (what the scheduler charges for it from now on)
  return n>0;
}
// WOM1: the host's area (its own slot as the area id) and who is in it.
void WorldOwnerMap(bool reliable) {
  cw::OwnerMapRec m{};
  cw::SetMagic(m.magic,"WOM1"); m.epoch=world_epoch;
  for(int s=0;s<kSlots;++s) {
    const bool in=s==world_slot || world_members[s].in_area;
    m.slots[s].area=in ? world_slot : cw::kSolo; m.slots[s].owner=in ? world_slot : uint8_t(s);
  }
  WorldQueue(kMsgOwnerMap,coopnet::kEveryone,reliable,&m,sizeof(m));
  for(int s=0;s<kSlots;++s) if(world_members[s].in_area) world_sent[s].other_bytes+=sizeof(m);
}

// The owner's tick, 20 a second (the host in 2.2): membership, the
// registry's state, the plan per member, and the messages it asks for,
// described from the game objects and queued for the network thread.
void WorldOwnerTick(GuestScratch& scratch,uint32_t player,uint64_t now) {
  static uint64_t next=0, last=0, next_map=0;
  if(now<next) return;
  // On a 50 ms grid that catches up (a 30 fps host ticks after 66 and 33 ms:
  // 20 a second, review A2-1); each tick the plain budget, more only after a
  // stall (at most 4 ticks' worth).
  const bool stall=!next || now>=next+2*cw::kTickMs;
  const uint64_t elapsed=stall && last ? std::clamp<uint64_t>(now-last,cw::kTickMs,4*cw::kTickMs) : cw::kTickMs;
  next=stall ? now+cw::kTickMs : next+cw::kTickMs;
  last=now;
  const uint8_t me=world_slot;
  // What the members hold and declined (WWA1, step 3 sends them).
  {
    std::vector<WorldAdmitIn> in;
    { std::lock_guard lock(world_mutex); in.swap(world_admits_in); }
    for(auto& a:in) if(a.origin<kSlots && a.origin!=me) {
      world_sched.OnAdmit(now,world_reg,world_members[a.origin],a.origin,me,a.msg); ++world_sent[a.origin].admits;
      world_near_in[a.origin]={a.msg.head.near_cars,a.msg.head.near_people,now};
    }
  }
  // Membership (step 4: the host's own rule on the poses, spec 3.1/3): a
  // client that shows the world stream (kPoseWorldStream: mod.ini world = 1
  // there and this game's OwnerMap seen) joins within member_join_m of this
  // player (2D, PosePacket.pos) after 2 s in a row and leaves past
  // member_leave_m after 5 s; at once when its pose is 3 s old, it stops
  // showing the stream or it left the session.
  const float px=api->read_f32(player+20), pz=api->read_f32(player+28);
  int members=0; bool changed=false;
  for(int s=0;s<kSlots;++s) {
    cw::Member& m=world_members[s];
    bool world=false; float eye[3]{}, fwd[3]{0,0,1}, d=1e9f;
    m.present=s!=me && Present(s);
    if(m.present) {
      std::lock_guard lock(pose_mutex);
      const RemoteIn& ri=remote_in[s];
      const PlayerPose& p=ri.pose;
      world=p.sequence && (ri.pose_at>now || now-ri.pose_at<3000) && (p.flags&kPoseWorldStream);
      std::memcpy(eye,p.eye,sizeof(eye));
      if(!eye[0] && !eye[1] && !eye[2]) std::memcpy(eye,p.pos,sizeof(eye));   // (no camera in the pose: its character)
      const float n=std::sqrt(p.cam[6]*p.cam[6]+p.cam[7]*p.cam[7]+p.cam[8]*p.cam[8]);
      if(n>0.1f) for(int i=0;i<3;++i) fwd[i]=p.cam[6+i]/n;   // (the camera's forward row)
      if(p.sequence) d=std::sqrt((p.pos[0]-px)*(p.pos[0]-px)+(p.pos[2]-pz)*(p.pos[2]-pz));
    }
    world_member_d[s]=d;
    bool in=m.in_area;
    const char* why="";
    if(!world) { in=false; world_near_since[s]=world_far_since[s]=0; why=!m.present ? "left the session" : "no world stream from it (or its pose is 3 s old)"; }
    else if(!m.in_area) {
      world_far_since[s]=0;
      if(d<float(world_join_m)) { if(!world_near_since[s]) world_near_since[s]=now; if(now-world_near_since[s]>=2000) in=true; }
      else world_near_since[s]=0;
    } else {
      world_near_since[s]=0;
      if(d>float(world_leave_m)) { if(!world_far_since[s]) world_far_since[s]=now; if(now-world_far_since[s]>=5000) { in=false; why="past member_leave_m for 5 s"; } }
      else world_far_since[s]=0;
    }
    if(in!=m.in_area) {
      changed=true;
      if(in) world_in_since[s]=now;
      static unsigned logged=0;
      if(logged++<40) {
        char line[200];
        if(in) std::snprintf(line,sizeof(line),"World (owner): slot %d joined the area at %.0f m from this player (within %u m for 2 s; epoch %u)",s,d,world_join_m,unsigned(world_epoch+1));
        else std::snprintf(line,sizeof(line),"World (owner): slot %d left the area at %.0f m from this player (%s; epoch %u)",s,d<1e8f ? d : -1.0f,why,unsigned(world_epoch+1));
        ProbeLog(line);
      }
      if(in && world_first_member) {
        world_first_member=false;
        unsigned cars=0, people=0;
        for(auto& [k,e]:world_reg.Entries()) if(e.Streams()) ++(cw::IsCar(e.kind) ? cars : people);
        char line[200];
        std::snprintf(line,sizeof(line),"World (owner): the first member in the area: slot %d; %u cars and %u people to catch up at 4 creates a tick, budget %u B/s per member",
                      s,cars,people,cw::TickBudget(world_upload_kbps,1)*20);
        ProbeLog(line);
      }
    }
    m.in_area=in;
    if(in) { ++members; std::memcpy(m.eye,eye,sizeof(eye)); std::memcpy(m.fwd,fwd,sizeof(fwd)); }
  }
  world_any_member=members>0;
  WorldViewsRefresh(player,now);   // (Package B: the members' viewpoints for the spawner hooks)
  if(changed) { ++world_epoch; WorldOwnerMap(true); next_map=now+1000; }
  else if(now>=next_map) { next_map=now+1000; WorldOwnerMap(false); }
  WorldRefresh(player,now);
  bool any=members>0;
  for(const cw::Member& m:world_members) any=any || m.was_in_area;
  if(!any) { world_reg.TakeRemovals(); return; }   // (nobody holds anything)
  // Budgets: world_upload_kbps shared by the members (6-24 KB/s each) for the
  // time since the last tick; half for 2 s when a link's backlog passes 16 KB
  // or it drops snapshots (the session's LinkStats; Epic's own queue too).
  const uint32_t tick_budget=cw::TickBudget(world_upload_kbps,members);
  // A link falls behind (review A2-3): its unreliable backlog after a flush
  // or its reliable bytes not acked past 16 KB, dropped snapshots, its RTT
  // over twice its lowest of the last 10 s (and 250 ms), or Epic's queue (one
  // for all peers) past 16 KB.
  static uint32_t last_dropped[kSlots]{}, rtt_low[kSlots]{};
  static uint64_t rtt_window=0;
  if(now>=rtt_window) { rtt_window=now+10000; for(uint32_t& r:rtt_low) r=0; }
  for(int s=0;s<kSlots;++s) {
    cw::Member& m=world_members[s];
    m.budget=uint32_t(uint64_t(tick_budget)*elapsed/cw::kTickMs);
    const uint32_t dropped=world_link_dropped[s].load(), rtt=world_link_rtt[s].load();
    if(rtt && (!rtt_low[s] || rtt<rtt_low[s])) rtt_low[s]=rtt;
    const bool behind=std::max({world_link_backlog[s].load(),world_link_inflight[s].load(),world_eos_backlog.load()})>cw::kBackpressureBacklog ||
                      dropped>last_dropped[s] || (rtt_low[s] && rtt>std::max(250u,2*rtt_low[s]));
    if(m.in_area) { if(behind) cw::Scheduler::Backpressure(now,m); world_budget_sum[s]+=now<m.halved_until ? m.budget/2 : m.budget; }
    last_dropped[s]=dropped;
  }
  // The stream point: the camera (0x827D9778 +44), where the game's own
  // stream point 82485DB0 starts and what its despawn radii are measured from.
  float ref[3];
  for(int i=0;i<3;++i) ref[i]=api->read_f32(0x827D9778+44+i*4);
  if(!std::isfinite(ref[0]) || !std::isfinite(ref[1]) || !std::isfinite(ref[2])) for(int i=0;i<3;++i) ref[i]=api->read_f32(player+20+i*4);
  auto plans=world_sched.Plan(now,world_reg,world_members,ref);
  // Each object's record once a tick, whoever it goes to.
  struct Rec { bool ok=false; cw::CarRec car{}; cw::PedRec ped{}; cw::SeatedRec seated{}; cw::FireExt ext{}; };
  std::unordered_map<cw::Key,Rec> recs;
  auto record=[&](const cw::Planned& u)->const Rec& {
    auto [it,fresh]=recs.try_emplace(u.key);
    if(!fresh) return it->second;
    Rec& r=it->second;
    const cw::Entry* e=world_reg.Find(cw::OwnerOf(u.key),cw::IdOf(u.key));
    if(!e) return r;
    if(cw::IsCar(e->kind)) {
      const uint32_t v=ResolveVehicle(e->id);
      if(v) r.ok=DescribeCar(v,*e,ref,r.car);
      if(r.ok && world_actions) {   // (Package C's driving line)
        ++world_ao.car_records;
        if(api->read_u8(v+50300)==1) { ++world_ao.car_phys; world_ao.steer+=(r.car.drive&cw::kDriveSteer)!=0; }
        world_ao.brake+=(r.car.damage&cw::kCarBrake)!=0; world_ao.reverse+=(r.car.damage&cw::kCarReverse)!=0; world_ao.horn+=(r.car.drive&cw::kDriveHorn)!=0;
        const uint32_t forced=api->read_u32(v+53980);
        world_ao.forced_on+=(forced&0x00040000u)!=0; world_ao.forced_off+=(forced&0x00020000u)!=0;
      }
    }
    else if(const uint32_t o=ResolveHuman(e->id)) {
      if(u.seated) { DescribeSeated(o,*e,r.seated,ref,&r.ext); r.ok=true; }
      else r.ok=DescribePerson(o,*e,ref,r.ped,&r.ext);
    }
    return r;
  };
  std::vector<uint8_t> buf;
  std::unordered_map<uint32_t,std::vector<uint8_t>> paints;   // (a car's paint once a tick)
  for(int s=0;s<kSlots;++s) {
    const cw::MemberPlan& p=plans[s];
    WorldSent& sent=world_sent[s];
    // Creates, removes, then the records (reliable before unreliable, F7).
    for(const cw::Planned& c:p.creates) {
      cw::Entry* e=world_reg.Find(cw::OwnerOf(c.key),cw::IdOf(c.key));
      if(!e || !WorldCreate(scratch,*e,ref,now,buf,paints)) { if(e) e->offer[s].created=false; ++sent.skipped; continue; }   // (offered again next tick)
      WorldQueue(kMsgWorldCreate,uint8_t(s),true,buf.data(),buf.size());
      ++sent.creates; sent.create_bytes+=unsigned(buf.size());
    }
    for(const cw::PlannedRemove& r:p.removes) {
      cw::RemoveRec m{}; cw::SetMagic(m.magic,"WWR1");
      m.id=cw::IdOf(r.key); m.time=uint32_t(now); m.fade_ms=r.fade_ms; m.reason=r.reason;
      WorldQueue(kMsgWorldRemove,uint8_t(s),true,&m,sizeof(m));
      ++(r.reason==cw::kRemoveAreaLeft ? sent.removes_area : sent.removes_gone); sent.other_bytes+=sizeof(m);
    }
    if(!p.updates.empty()) {
      std::vector<cw::CarRec> cars; std::vector<cw::PedRec> peds; std::vector<cw::SeatedRec> seated; std::vector<cw::FireExt> peds_ext, seated_ext;
      for(const cw::Planned& u:p.updates) {
        const Rec& r=record(u);
        if(!r.ok) { ++sent.skipped; continue; }
        if(cw::IsCar(u.kind)) cars.push_back(r.car);
        else if(u.seated) { seated.push_back(r.seated); seated_ext.push_back(r.ext); if(r.seated.weapon&cw::kWeaponFire) { ++world_ao.fire_records; world_ao.fire_bytes+=sizeof(cw::FireExt); } }
        else { peds.push_back(r.ped); peds_ext.push_back(r.ext); if(r.ped.weapon&cw::kWeaponFire) { ++world_ao.fire_records; world_ao.fire_bytes+=sizeof(cw::FireExt); } }
      }
      cw::UpdateHeader head{}; head.owner=me; head.epoch=world_epoch; head.time=uint32_t(now); std::memcpy(head.ref,ref,sizeof(ref));
      for(const auto& m:cw::PackUpdates(head,cars,peds,seated,peds_ext,seated_ext)) {
        WorldQueue(kMsgWorld,uint8_t(s),false,m.data(),m.size());
        ++sent.messages; sent.record_bytes+=unsigned(m.size());
      }
      sent.records+=unsigned(cars.size()+peds.size()+seated.size());
    }
    if(p.digest_due) {
      for(const auto& m:cw::WriteDigest(me,uint32_t(now),p.digest)) { WorldQueue(kMsgWorldDigest,uint8_t(s),true,m.data(),m.size()); sent.other_bytes+=unsigned(m.size()); }
      ++sent.digests; sent.digest_ids=unsigned(p.digest.size());
    }
  }
}

// Every 10 s: the registry (every game) and, on the host, what each member
// was sent (the 2.2 owner line, spec section 6); on a client what came from
// the host (step 2: counted, not applied).
void WorldBLog(double secs);   // (Package B: content around the members, further down)
void WorldLog(uint64_t now) {
  static uint64_t window=0;
  const double secs=window && now>window ? (now-window)/1000.0 : 10.0;
  window=now;
  unsigned car_kind[6]{}, person_kind[6]{}, cars=0, people=0, trains=0, mission_cars=0, cars_inactive=0, player_cars=0, seated=0, why[4]{};
  for(auto& [k,e]:world_reg.Entries()) {
    const auto it=world_objs.find(e.id);
    const uint8_t w=it!=world_objs.end() ? it->second.why : kWhyStreams;
    if(cw::IsCar(e.kind)) {
      ++cars; ++car_kind[std::min<unsigned>(e.kind,5)];
      if(e.flags&cw::kEntryExcluded) { if(e.source==9) ++trains; else if(e.source==7) ++mission_cars; }
      else if(w) ++cars_inactive;
      if(e.flags&cw::kEntryPlayerCar) ++player_cars;
    } else {
      ++people; ++person_kind[std::min<unsigned>(e.kind-cw::kKindAmbient,5)];
      if(e.Seated()) ++seated;
      if(!(e.flags&cw::kEntryExcluded)) ++why[w];
    }
  }
  char line[900];
  const bool host=hosting.load();
  std::string area="registry";
  int members=0;
  if(host) {
    std::string slots;
    for(int s=0;s<kSlots;++s) if(world_members[s].in_area) { slots+=" "+std::to_string(s); ++members; }
    area="area: "+std::to_string(members)+" members (slots"+(slots.empty() ? std::string(" none") : slots)+", epoch "+std::to_string(world_epoch)+")";
  }
  std::snprintf(line,sizeof(line),"World (%s): %s; set: cars %u (traffic %u, parked %u, response %u, special %u, script %u; trains left out %u, mission cars left out %u, inactive %u, players' cars %u), "
                "people %u (ambient %u, traffic crews %u, response %u, hood %u, special %u, mission/script %u; seated %u; story characters left out %u, mission/script outside a mission %u, inactive %u); "
                "audit +%u/-%u against the object table (%u back after a fade); made cars %u / people %u, removed cars %u / people %u (%u of them as their fade began), re-activated people %u (the game's own, from the hooks)",
                host ? "owner" : "this game's own, a client",area.c_str(),
                cars,car_kind[cw::kKindTraffic],car_kind[cw::kKindParked],car_kind[cw::kKindResponseCar],car_kind[cw::kKindSpecialCar],car_kind[cw::kKindScriptCar],trains,mission_cars,cars_inactive,player_cars,
                people,person_kind[0],person_kind[1],person_kind[2],person_kind[3],person_kind[4],person_kind[5],seated,why[kWhyNamed],why[kWhyNoMission],why[kWhyInactive],
                world_n.audit_added,world_n.audit_removed,world_n.fade_back,world_n.made_cars,world_n.made_people,world_n.removed_cars,world_n.removed_people,world_n.fades,world_n.reactivated);
  ProbeLog(line);
  if(host) {
    for(int s=0;s<kSlots;++s) {
      cw::Member& m=world_members[s];
      WorldSent& sent=world_sent[s];
      if(s==world_slot || (!m.in_area && !m.ticks && !sent.creates && !sent.removes_area && !Present(s))) continue;
      unsigned offered_cars=0, offered_people=0, declined=0, waiting=0;
      for(auto& [k,e]:world_reg.Entries()) {
        const cw::Offer& o=e.offer[s];
        if(o.created) ++(cw::IsCar(e.kind) ? offered_cars : offered_people);
        else if(o.declined_until>now) ++declined;
        else if(e.Streams() && m.in_area) ++waiting;
      }
      // Achieved rates per band for the moving objects (targets 20 / 10 / 2 Hz), and the resting ones (2 Hz) apart:
      // sends per object per second over the ticks that ran (review A2-1).
      auto hz=[&](int b) { const uint32_t n=m.band[b].objects-m.band[b].rest_objects; return n ? double(m.band[b].planned-m.band[b].rest_planned)*m.ticks/(double(n)*secs) : 0.0; };
      const uint32_t rest_n=m.band[0].rest_objects+m.band[1].rest_objects+m.band[2].rest_objects;
      const double rest_hz=rest_n ? double(m.band[0].rest_planned+m.band[1].rest_planned+m.band[2].rest_planned)*m.ticks/(double(rest_n)*secs) : 0.0;
      char held[80];
      if(m.cars_cap==0xFFFF) std::snprintf(held,sizeof(held),"? (no WWA1 yet)");
      else std::snprintf(held,sizeof(held),"cars %u/%u, people %u/%u",m.cars_held,m.cars_cap,m.people_held,m.people_cap);
      const unsigned bytes=sent.create_bytes+sent.record_bytes+sent.other_bytes;
      char where[96];
      if(world_member_d[s]<1e8f) std::snprintf(where,sizeof(where),"%s at %.0f m",m.in_area ? "in the area" : "not in the area",world_member_d[s]);
      else std::snprintf(where,sizeof(where),"%s",m.in_area ? "in the area" : "not in the area");
      std::snprintf(line,sizeof(line),"World (owner) slot %d (%s; 2.1's traffic and people streams to it: %s): offered %u (cars %u, people %u), held %s, declined %u (re-offered %u), created %u (%.1f KB; catch-up left %u, deferred for the budget %u), removed %u (gone %u, area %u), "
                    "near %.1f Hz / mid %.1f Hz / far %.1f Hz (resting %.1f Hz; %u ticks), %u records in %u messages, bytes %.1f KB/s of budget %.1f KB/s (plan %.1f KB/s), backpressure halvings %u, digest sent %u (%u ids), records skipped %u (past 655 m %u), WWA1 %u",
                    s,where,(legacy_streams.load()>>s&1) ? "still sent" : "off",offered_cars+offered_people,offered_cars,offered_people,held,declined,m.reoffers,sent.creates,sent.create_bytes/1024.0,waiting,m.over_create_budget,
                    sent.removes_gone+sent.removes_area,sent.removes_gone,sent.removes_area,hz(0),hz(1),hz(2),rest_hz,m.ticks,sent.records,sent.messages,bytes/1024.0/secs,
                    world_budget_sum[s]/1024.0/secs,m.bytes/1024.0/secs,m.halvings,sent.digests,sent.digest_ids,sent.skipped,m.skipped_far,sent.admits);
      ProbeLog(line);
      // World match from its WWA1 (two clocks: the member's count when it sent it, this game's now).
      if(m.in_area && world_near_in[s].at && now-world_near_in[s].at<3000) {
        unsigned nc=0, np=0;
        for(auto& [k,e]:world_reg.Entries()) if(e.Streams() && cw::Dist(e.pos,m.eye)<=50.0f) ++(cw::IsCar(e.kind) ? nc : np);
        std::snprintf(line,sizeof(line),"World (owner) match slot %d (its WWA1, two clocks): within 50 m of its camera this game has cars %u / people %u, it shows %u / %u",
                      s,nc,np,unsigned(world_near_in[s].cars),unsigned(world_near_in[s].people));
        ProbeLog(line);
      }
      m.ResetStats(); sent=WorldSent{}; world_budget_sum[s]=0;
    }
    if(player_cars || world_n.taken || world_n.returned) {
      std::string who;
      for(auto& [k,e]:world_reg.Entries()) if(e.flags&cw::kEntryPlayerCar) who+=" "+std::to_string(e.authority);
      std::snprintf(line,sizeof(line),"World (owner) authority: %u cars driven by players now (slots%s), %u taken, %u returned to this game",player_cars,who.c_str(),world_n.taken,world_n.returned);
      ProbeLog(line);
    }
    if(const uint32_t full=world_out_full.exchange(0)) { std::snprintf(line,sizeof(line),"World (owner): %u messages not queued (the network thread fell behind)",full); ProbeLog(line); }
    if(members) { WorldBLog(secs); WorldActionsOwnerLog(secs); }
    else world_ao=WorldActOwner{};
  } else if(!world_stream) {
    uint32_t n[6]; for(int i=0;i<6;++i) n[i]=world_in_count[i].exchange(0);
    const uint32_t bytes=world_in_bytes.exchange(0);
    if(bytes) {
      std::snprintf(line,sizeof(line),"World (member, mod.ini world = 0: counted, not applied): from the host %u update messages, %u creates, %u removes, %u digests, %u owner maps, %.1f KB/s",
                    n[0],n[1],n[2],n[4],n[5],bytes/1024.0/secs);
      ProbeLog(line);
    }
  }
  if(const uint32_t bad=world_admits_bad.exchange(0)) { std::snprintf(line,sizeof(line),"World: %u malformed WWA1 dropped",bad); ProbeLog(line); }
  world_n=WorldCounts{};
  uint32_t a,r,q; world_reg.TakeCounts(a,r,q);   // (world.h's own counts: the per-kind ones above are logged)
}

// ---- The shared area mirrored (co-op 2.2, step 3: the members' side) ----
// docs/COOP_V2.md section 11. A client in the host's area shows what the
// host's world stream sends (mod.ini world = 1; with 0, or while the host
// streams nothing - no OwnerMap from it for 3 s - 2.1's streams show the
// host's world as before):
//  - membership is the host's (step 4: WorldOwnerTick, from the poses; this
//    client's pose says it shows the world stream, kPoseWorldStream); the
//    host's OwnerMap (WOM1) says who is in. While in (and until the last copy
//    here is gone) this game's own traffic, people, hood and special spawns,
//    trains and police stay off (UpdateSharedTraffic / UpdateSharedPeds /
//    HoodSpawnsHook / WorldTrains / OwnPoliceOff), and none of 2.1's copies
//    are made;
//  - one table of copies keyed (owner slot, id) with a reverse index by this
//    game's handle (world_copies / world_local), next to 2.1's maps;
//  - admission is this game's (spec 3.6): cars up to 32 less this player's
//    own, one per other player and 4 spare (26 with two players), people up
//    to 56 (75 less 11 players and 8 spare), within the live counts
//    (vehicles 0x83710238 of 32, humans 0x8309ABAC of 75) when a copy is
//    made; a person whose model isn't loaded here is declined while the
//    people group's slots (0x83E876B0 +0: 12, one kept for the players) are
//    all wanted by the copies here; when full a new copy nearer than the
//    farthest held one of its kind by 20 m (people 10 m) takes its place (that
//    one fades), else the new one is declined; a crew member goes with its
//    car. Declined, evicted and lost ids go back in WWA1; the host offers them
//    again after 3 s while there is room;
//  - creates, 4 per update at most, with co-op's own sequences: CreateVehicle
//    and the paint, CreateActor and the look block, crews seated with
//    SeatInVehicle; a driver whose model hasn't loaded after 2 s in a moving
//    car gets a lookalike (LoadedLookalike);
//  - updates (WWS1): per id the newest owner time only; records before their
//    create held 1 s, after a remove dropped; cars as the game's MP frame
//    moves a remote car (823BB068): the body (+244) woken with 826CD960 and
//    its velocities set through its motion (+88; vtable +88 linear, +92
//    angular) towards the path, placed (82535C68) only when far off or turned
//    round; a car without physics (+50300 != 1, which 823BB068 leaves alone)
//    has its transform written as 82535C68 writes one (+20, +32..+64), at
//    most twice a second unless the record moved; hit points, smoke, fire,
//    wreck and siren through ApplyCarDamage / ApplyCarSiren; people on 2.1's
//    path follower, with their flags (hidden and cutscene people hidden, not
//    removed; dead from the owner's hit points), team, animation set and gun;
//  - removes (WWR1) fade the copy (cars 82537D40(v, ms, 1), people
//    824517A0(h, ms, 1)) and co-op destroys it when the fade is over (the
//    game's own deleters never reach a copy: the traffic housekeeping
//    82410B80 walks the traffic list only, and HumanDestroyHook lets the
//    corpse clean-up's ask through only for a fading copy); a copy not shown
//    yet goes at once; a car a player sits in is let go;
//  - kEventAuthority: a car a player drives gets no records here until the
//    host says it is back with its game;
//  - WWA1 to the host every second and soon after a decline; the digest
//    (WWD1) is diffed against the table every 10 s (the proof line).
std::atomic<uint64_t> world_map_seen{0};   // network thread: the host's last world message (it streams its world)
uint64_t world_wait_until=0;               // game thread: a new session waits this long for the host's OwnerMap before 2.1's streams show its world
struct WorldIn { uint8_t type=0; uint64_t at=0; std::vector<uint8_t> bytes; };
std::vector<WorldIn> world_in;             // host -> this member, in order (world_mutex)
std::atomic<uint32_t> world_in_full{0};
enum : uint8_t { kCopyQueued=0, kCopyMade, kCopyFading };
// Why an id is declined here (WWA1): the pools, a model that can't load, its car not held, evicted for a nearer one, the game removed the copy, the create failed, lost (the digest).
enum : uint8_t { kDeclinePool=0, kDeclineModel, kDeclineCrew, kDeclineRoom, kDeclineGone, kDeclineFailed, kDeclineLost, kDeclineReasons };
const char* const kDeclineNames[kDeclineReasons]={"pool","model","car","made room","gone here","create failed","lost"};
// A copy's gunfire (Package C): the owner's round count as last taken, the
// rounds waiting, the trigger held, the aim.
struct CopyFire {
  uint8_t seen=0xFF;                 // the owner's count (kFireCount) as last taken; 0xFF none yet
  bool quiet=false;                  // a record without a fire part came since (the count can't have moved meanwhile)
  uint64_t last_record=0;            // the newest record of the copy (a gap of 1.5 s takes no backlog)
  bool held=false, holding=false, aim_ok=false, blank=false, driveby=false;
  uint8_t reason=0;                  // the last wait of the round in front (a dropped round is counted by it)
  uint64_t ext_at=0, next_raise=0;
  float aim[3]{};
  std::deque<uint64_t> shots;        // single rounds waiting (at most 4), when they came
};
struct WorldCopy {
  uint8_t owner=0, kind=0, state=kCopyQueued, seat=0, team=0xFF, weapon=0, pstate=0;   // weapon: the record's byte (kWeapon*)
  uint8_t driven=coopnet::kNoSlot;      // a car: the slot whose pose carries it (kEventAuthority, the create): no records here
  uint64_t driven_at=0;                 // when driven was set here (a record from the owner past 1 s later means it is back: review A3-4)
  uint32_t id=0, car=0, local=0;        // car: a seated person's car id; local: the copy's handle here (0: not made)
  uint16_t model=0;                     // vehicle table index / people model id
  std::string name; std::vector<uint8_t> look, paint;
  uint32_t create_time=0, last_time=0;  // owner clock: the create, the newest record taken
  uint64_t queued_at=0, made_at=0, deadline=0, next_paint=0, next_damage=0, next_write=0, next_seat=0, next_exit=0, next_standin=0;
  unsigned tries=0, paint_tries=0, exits=0;
  float pos[3]{}, rows[9]{1,0,0,0,1,0,0,0,1}, vel[3]{}, ang[3]{}, written[3]{};
  uint8_t damage=0, hp=255;
  bool paint_done=false, standin=false, was_driven=false, was_seated=false, held_logged=false, past_logged=false, waiting_counted=false;
  Follower follow;        // a car's path (a person's is ped.follow)
  ImpactWatch impact;
  SharedPed ped;          // a person: 2.1's state of a copy of the host's people (ped.handle is local)
  GunState gun;
  // Package C: a person's fire and burning; a car's driver inputs (CarRec damage bits 6-7, drive) from the newest record.
  CopyFire fire;
  uint64_t next_burn=0; bool burn_lit=false;
  uint8_t drive_bits=0, drive=0; uint64_t drive_at=0; bool drive_steered=true;
};
void WorldFireIn(WorldCopy& c,uint8_t weapon,const cw::FireExt* x,const float* ref,uint64_t now);   // (Package C, further down)
void WorldBurn(GuestScratch& scratch,WorldCopy& c,uint32_t obj,uint64_t now);
void WorldDriveApply(GuestScratch& scratch,WorldCopy& c,uint32_t v,uint64_t now);
void WorldCopyFire(GuestScratch& scratch,uint64_t now);
void WorldDriveClear(uint32_t local);
void WorldActionsMemberLog(double secs);
std::map<cw::Key,WorldCopy> world_copies;          // game thread
std::unordered_map<uint32_t,cw::Key> world_local;  // this game's handle -> key
std::deque<cw::Key> world_queue;                   // admitted, not made yet (in the host's order: nearest first)
struct WorldDeclined { uint64_t at=0; float pos[3]{}; uint8_t kind=0, why=0; };
std::map<cw::Key,WorldDeclined> world_declined;
std::vector<uint32_t> world_report;                // declined, evicted and lost ids for the next WWA1
std::unordered_map<cw::Key,std::pair<uint32_t,uint64_t>> world_tomb;   // removed here: the remove's owner time, when (later records of it are dropped for 5 s)
struct WorldEarly { uint64_t at=0; uint32_t time=0; float ref[3]{}; uint8_t type=0; uint8_t rec[sizeof(cw::CarRec)]{}; };
std::unordered_map<cw::Key,WorldEarly> world_early;   // records for ids whose create hasn't come yet (held 1 s)
cw::DigestParts world_digest;
struct WorldArea { bool in=false, have=false; uint16_t epoch=0; uint64_t map_at=0, joined_at=0; } world_area;
struct WorldMemberCounts {
  unsigned offered[2]{}, declined[2][kDeclineReasons]{}, made[2]{}, faded[2]{}, destroyed[2]{}, at_once=0, let_go=0, past_deadline=0, gone[2]{};
  unsigned creates=0, held_for_car=0, replaced=0, adopted=0, updates[2]{}, old=0, early=0, early_used=0, early_expired=0, after_remove=0, declined_records=0, epoch=0;
  unsigned removes_gone=0, removes_area=0, removes_digest=0, placements=0, writes=0, steered=0, woken=0, standins=0, seated=0, seat_refused=0, exits=0, taken=0, returned=0, bad=0, refused_asks=0, let_through=0;
  uint64_t car_ms=0;   // car copies made x ms (placements per car per second)
} world_m;
std::vector<int32_t> world_ages;     // this window: apply time - owner time per WWS1 (ms, this clock's offset in it)
int32_t world_age_offset=INT32_MAX;  // this window: the least receive time - owner time (one clock)
struct WorldMatch { unsigned n=0, held=0, declined=0, model_wait=0, chunk=0; std::vector<uint32_t> missing, extra; bool have=false; } world_match;
bool world_admit_due=false;
bool world_member_live=false;     // game thread: a session in which this client may show the world stream (its start resets the area)
bool world_rejoin_wait=false;     // game thread: this game left the area on its own (no OwnerMap): out until the host's map has it out too (review A3-1)
// Package B's pass line: the copies shown near this camera (people 50 m, cars
// 100 m) once a second, by this player's distance to the host's (under 40 m,
// 100-150 m), summed over the session.
struct WorldBand { double people=0, cars=0; unsigned seconds=0; };
WorldBand world_band_near, world_band_far;
uint64_t world_rejoin_since=0;
std::vector<WorldCopy> world_graveyard;   // copies a create replaced: destroyed in the per-update budget (review A3-6)

// The host's world stream still counts as there `since` its last message: 3 s,
// for as long as the host's pause holds (HostPauseHold: its update, which
// sends the OwnerMap, doesn't run), and 10 s while its state packets still
// come (its network thread runs, its game thread is busy: a load or a stall)
// (review A3-1).
bool WorldHostHold(uint64_t since,uint64_t now) {
  if(since>=now || now-since<3000 || HostPauseHold(now)) return true;
  return now-since<10000 && now-host_state_seen.load()<1000;
}
bool WorldMode() {   // (game thread)
  if(!world_stream || !connected.load() || hosting.load()) return false;
  const uint64_t seen=world_map_seen.load(), now=MonoMs();
  return seen ? WorldHostHold(seen,now) : now<world_wait_until;
}
bool WorldShown() { return WorldMode() && world_area.in; }
bool WorldLeversOn() { return WorldMode() && (world_area.in || !world_copies.empty()); }
bool WorldCopiesLeft() { return !world_copies.empty(); }
// This client shows the host's world stream (mod.ini world = 1 and the host's
// OwnerMap within 3 s): its pose says so (kPoseWorldStream), and the host
// decides from the poses whether it is in the area (step 4).
// While this game left the area on its own it says it doesn't, until the
// host's OwnerMap has it out (the host then starts its offers over).
bool WorldStreamSeen() {
  if(!world_stream || !connected.load() || hosting.load() || world_rejoin_wait) return false;
  const uint64_t seen=world_map_seen.load();
  return seen && WorldHostHold(seen,MonoMs());
}
WorldCopy* WorldAt(uint32_t local) {
  if(!local) return nullptr;
  const auto it=world_local.find(local);
  if(it==world_local.end()) return nullptr;
  const auto c=world_copies.find(it->second);
  return c==world_copies.end() || c->second.local!=local ? nullptr : &c->second;
}
WorldCopy* WorldFind(uint8_t owner,uint32_t id) { const auto it=world_copies.find(cw::MakeKey(owner,id)); return it==world_copies.end() ? nullptr : &it->second; }
void WorldIndex(WorldCopy& c,uint32_t local) {
  if(c.local) world_local.erase(c.local);
  c.local=local; c.ped.handle=local;
  if(local) world_local[local]=cw::MakeKey(c.owner,c.id);
}
uint32_t WorldPersonId(uint32_t local,bool seated) { const WorldCopy* c=WorldAt(local); return c && !cw::IsCar(c->kind) && (seated || !c->car) ? c->id : 0; }
uint32_t WorldCarId(uint32_t local) { const WorldCopy* c=WorldAt(local); return c && cw::IsCar(c->kind) ? c->id : 0; }
uint32_t WorldPersonLocal(uint32_t id) { const WorldCopy* c=WorldFind(0,id); return c && !cw::IsCar(c->kind) ? c->local : 0; }
uint32_t WorldCarLocal(uint32_t id) { const WorldCopy* c=WorldFind(0,id); return c && cw::IsCar(c->kind) ? c->local : 0; }
bool WorldCrewSeat(uint32_t local,uint32_t& car,uint32_t& seat) { const WorldCopy* c=WorldAt(local); if(!c || cw::IsCar(c->kind) || !c->car) return false; car=c->car; seat=c->seat; return true; }
SharedPed* WorldPerson(uint32_t id) { WorldCopy* c=WorldFind(0,id); return c && !cw::IsCar(c->kind) && c->local && c->state==kCopyMade ? &c->ped : nullptr; }
void WorldPeopleLocals(std::vector<uint32_t>& out,bool standins) {
  for(auto& [k,c]:world_copies) if(!cw::IsCar(c.kind) && c.local && (standins || !c.standin)) out.push_back(c.local);
}
void ForWorldPeople(const std::function<void(uint32_t,SharedPed&)>& fn) {
  for(auto& [k,c]:world_copies) if(!cw::IsCar(c.kind) && c.local && c.state==kCopyMade) fn(c.id,c.ped);
}
// HumanDestroyHook: a copy being faded lets the game's own ask through (its corpse clean-up 824793C0 asks once the fade bits are set).
bool WorldFading(uint32_t local) { const WorldCopy* c=WorldAt(local); return c && c->state==kCopyFading; }
// Host: a person of its world stream (seated: crews too): its events go to the members, their hits on it come here.
bool WorldSends(uint32_t handle,bool seated) {
  if(!world_on || !hosting.load() || std::none_of(world_members.begin(),world_members.end(),[](const cw::Member& m){ return m.in_area; })) return false;
  const cw::Entry* e=world_reg.Find(world_slot,handle);
  return e && !cw::IsCar(e->kind) && e->Streams() && (seated || !e->Seated());
}

void WorldEye(float out[3]) {
  for(int i=0;i<3;++i) out[i]=api->read_f32(0x827D9778+44+i*4);
  if(!std::isfinite(out[0]) || !std::isfinite(out[1]) || !std::isfinite(out[2])) { out[0]=local_x.load(); out[1]=local_y.load(); out[2]=local_z.load(); }
}
float WorldDist(const float* a,const float* b) { const float dx=a[0]-b[0], dy=a[1]-b[1], dz=a[2]-b[2]; return std::sqrt(dx*dx+dy*dy+dz*dz); }
// The pools (spec 3.6/27).
unsigned WorldCarCap() { return 32-unsigned(std::min(26,1+std::max(0,player_count.load()-1)+4)); }
unsigned WorldPeopleCap() { return 56; }
int WorldCarRoom() { return 30-int(api->read_u32(0x83710238)); }      // the vehicle pool (count 0x83710238 of 32), 2 spare
int WorldPeopleRoom() { return 67-int(api->read_u32(0x8309ABAC)); }   // the humans (0x8309ABAC of 75), 8 spare
constexpr unsigned kWorldModelSlots=11;   // the people group's 12 slots, one kept for the players
// A person's model is loaded here (the people group's request for it holds its data).
bool WorldModelLoaded(uint16_t model) { const uint32_t e=FindStreamEntry(model); return e && api->read_u32(e)!=0; }
bool WorldModelRoom(uint16_t model) {
  if(WorldModelLoaded(model)) return true;
  std::vector<uint16_t> wanted;
  for(auto& [k,c]:world_copies) if(!cw::IsCar(c.kind) && c.state!=kCopyFading && std::find(wanted.begin(),wanted.end(),c.model)==wanted.end()) wanted.push_back(c.model);
  return std::find(wanted.begin(),wanted.end(),model)!=wanted.end() || wanted.size()<kWorldModelSlots;
}
// A person's rows from its heading and orientation sign (as 2.1's people records).
void WorldPersonRows(float yaw,uint8_t flags,float rows[9]) {
  const float fx=std::sin(yaw), fz=std::cos(yaw), sign=(flags&cw::kPedSign) ? 1.0f : -1.0f;
  rows[0]=sign*fz; rows[1]=0; rows[2]=-sign*fx; rows[3]=0; rows[4]=1; rows[5]=0; rows[6]=fx; rows[7]=0; rows[8]=fz;
}
// The records' fields into the copy (a queued copy is made where the newest one says).
void WorldCarRecord(WorldCopy& c,const cw::CarRec& r,const float* ref,uint32_t time,uint64_t now) {
  // The owner sends no records for a car a player has: one past 1 s after it
  // was named driven here means the "back" event was lost or came first (review A3-4).
  if(c.driven!=coopnet::kNoSlot && int32_t(time-c.create_time)>0 && now-c.driven_at>1000) { c.driven=coopnet::kNoSlot; c.follow.Reset(); ++world_m.returned; }
  cw::UnqPos(ref,r.pos,c.pos); cw::UnqRot(r.rot,c.rows);
  for(int i=0;i<3;++i) { c.vel[i]=cw::UnqVel(r.vel[i]); c.ang[i]=cw::UnqAng(r.ang[i]); }
  c.damage=uint8_t(r.damage&0x3F); c.hp=r.hp;
  c.drive_bits=uint8_t(r.damage&(cw::kCarBrake|cw::kCarReverse)); c.drive=r.drive; c.drive_at=now; c.drive_steered=false;   // (Package C)
  if(c.state==kCopyMade && c.driven==coopnet::kNoSlot) c.follow.Add(time,c.pos,c.rows,now);
  ++world_m.updates[0];
}
void WorldPedRecord(WorldCopy& c,const cw::PedRec& r,const float* ref,uint32_t time,uint64_t now) {
  cw::UnqPos(ref,r.pos,c.pos); WorldPersonRows(cw::UnqYaw(r.yaw),r.flags,c.rows);
  c.car=0; c.seat=0; c.team=r.team; c.weapon=r.weapon; c.pstate=r.state;
  SharedPed& sp=c.ped;
  sp.flags=r.flags; sp.anim=r.anim; sp.host_action=r.action; sp.team=r.team; sp.seen=sp.sent_at=now;
  if(c.state==kCopyMade) {
    if(c.was_seated) { sp.follow.Reset(); c.was_seated=false; }
    sp.follow.Add(time,c.pos,c.rows,now);
  }
  ++world_m.updates[1];
}
void WorldSeatedRecord(WorldCopy& c,const cw::SeatedRec& r,uint64_t now) {
  c.car=r.car; c.seat=r.seat; c.team=r.team; c.weapon=r.weapon;
  if(const WorldCopy* cc=WorldFind(c.owner,r.car)) { std::memcpy(c.pos,cc->pos,sizeof(c.pos)); std::memcpy(c.rows,cc->rows,sizeof(c.rows)); }
  SharedPed& sp=c.ped;
  sp.flags=r.flags; sp.team=r.team; sp.seen=sp.sent_at=now;
  c.was_seated=true;
  ++world_m.updates[1];
}
void WorldTakeRecord(WorldCopy& c,uint8_t type,const uint8_t* rec,const float* ref,uint32_t time,uint64_t now) {
  if(type==0) { cw::CarRec r; std::memcpy(&r,rec,sizeof(r)); WorldCarRecord(c,r,ref,time,now); }
  else if(type==1) { cw::PedRec r; std::memcpy(&r,rec,sizeof(r)); WorldPedRecord(c,r,ref,time,now); }
  else { cw::SeatedRec r; std::memcpy(&r,rec,sizeof(r)); WorldSeatedRecord(c,r,now); }
}
// A record's copy (nullptr: held for its create, after its remove, declined, or older than the newest taken).
WorldCopy* WorldRecordCopy(cw::Key key,bool car,uint8_t type,const void* rec,const float* ref,uint32_t time,uint64_t now) {
  const auto it=world_copies.find(key);
  if(it==world_copies.end()) {
    if(const auto t=world_tomb.find(key); t!=world_tomb.end() && int32_t(time-t->second.first)<=0) { ++world_m.after_remove; return nullptr; }
    if(world_declined.count(key)) { ++world_m.declined_records; return nullptr; }
    WorldEarly& e=world_early[key];
    if(!e.at || int32_t(time-e.time)>0) { e.at=now; e.time=time; std::memcpy(e.ref,ref,sizeof(e.ref)); e.type=type; std::memcpy(e.rec,rec,type==0 ? sizeof(cw::CarRec) : type==1 ? sizeof(cw::PedRec) : sizeof(cw::SeatedRec)); }
    ++world_m.early;
    return nullptr;
  }
  WorldCopy& c=it->second;
  if(c.state==kCopyFading || cw::IsCar(c.kind)!=car) return nullptr;
  if(int32_t(time-c.last_time)<=0) { ++world_m.old; return nullptr; }
  c.last_time=time;
  return &c;
}

// An id declined (or evicted, or lost) here: told to the host in the next WWA1.
void WorldDecline(cw::Key key,uint8_t kind,uint8_t why,const float* pos,uint64_t now) {
  WorldDeclined& d=world_declined[key];
  d.at=now; d.kind=kind; d.why=why; std::memcpy(d.pos,pos,sizeof(d.pos));
  if(world_report.size()<512) world_report.push_back(cw::IdOf(key));
  ++world_m.declined[cw::IsCar(kind) ? 0 : 1][why];
  world_admit_due=true;
  static unsigned logged=0;
  if((why==kDeclinePool || why==kDeclineRoom) && logged<20) {
    ++logged;
    unsigned held=0;
    for(auto& [k,c]:world_copies) held+=cw::IsCar(c.kind)==cw::IsCar(kind) && c.state!=kCopyFading;
    char line[160];
    std::snprintf(line,sizeof(line),"World (member): full: %s %u/%u, %s id %08X",cw::IsCar(kind) ? "cars" : "people",held,cw::IsCar(kind) ? WorldCarCap() : WorldPeopleCap(),
                  why==kDeclineRoom ? "evicted (a nearer one came)" : "declined",cw::IdOf(key));
    ProbeLog(line);
  }
}
// A copy goes: faded, then destroyed when the fade is over (spec 3.10). A
// copy not made yet or not shown goes at once; a car a player sits in or gets
// into is let go (it stays as that player's: a car copy of the host's).
void WorldFade(GuestScratch& scratch,WorldCopy& c,uint16_t fade_ms,uint64_t now) {
  if(c.state==kCopyFading) return;
  const bool car=cw::IsCar(c.kind);
  c.state=kCopyFading; c.deadline=now;
  if(!c.local) return;
  if(car) {
    const uint32_t v=ResolveVehicle(c.local);
    const uint32_t player=api->read_u32(0x8309ABEC);
    if(!v) return;
    if((Readable(player,4252) && api->read_u32(player+2496)==c.local) || PlayerSitsIn(c.local) || PlayerGettingIn(c.local,now)) {
      // (this player's: 2.1's taken car, named as the host's; another player's character's: a car copy that player's pose drives)
      if(Readable(player,4252) && api->read_u32(player+2496)==c.local) taken_cars[c.local]=c.id;
      else { CarCopy& cc=car_copies[CarKey(c.owner,c.id)]; cc=CarCopy{}; cc.local=c.local; cc.type=c.model; cc.used=now; cc.applied_look=c.paint; }
      WorldIndex(c,0); ++world_m.let_go;
      return;
    }
    if(fade_ms) {
      scratch.ctx.r3.u64=v; scratch.ctx.r4.u64=fade_ms; scratch.ctx.r5.u64=1;
      scratch.Call(0x82537D40);
      c.deadline=now+fade_ms+50; ++world_m.faded[0];
      // Its crew here fades with it (82537D40 fades the occupants it lists in its seat slots).
      for(auto& [k,p]:world_copies) if(!cw::IsCar(p.kind) && p.car==c.id && p.state!=kCopyFading) if(const uint32_t o=ResolveHuman(p.local)) if(api->read_u32(o+2496)==c.local) {
        scratch.ctx.r3.u64=o; scratch.ctx.r4.u64=fade_ms; scratch.ctx.r5.u64=1;
        scratch.Call(0x824517A0);
        p.state=kCopyFading; p.deadline=c.deadline; ++world_m.faded[1];
      }
    } else ++world_m.at_once;
    return;
  }
  const uint32_t o=ResolveHuman(c.local);
  if(!o) return;
  if(!fade_ms || !(api->read_u32(o+216)&0x4000) || (api->read_u32(o+120)&0x08000000u)) { ++world_m.at_once; return; }   // (not shown: at once, the game's own rule)
  scratch.ctx.r3.u64=o; scratch.ctx.r4.u64=fade_ms; scratch.ctx.r5.u64=1;
  scratch.Call(0x824517A0);
  c.deadline=now+fade_ms+50; ++world_m.faded[1];
}
// Destroyed now (a car's crew copies first, as 2.1 removed a car's: its own removal 825324D0 takes its occupants).
void WorldDestroy(GuestScratch& scratch,WorldCopy& c) {
  if(!c.local) return;
  if(cw::IsCar(c.kind)) {
    if(ResolveVehicle(c.local)) {
      if(PlayerSitsIn(c.local)) { taken_cars[c.local]=c.id; ++world_m.let_go; WorldIndex(c,0); return; }   // (got in since)
      for(auto& [k,p]:world_copies) if(!cw::IsCar(p.kind) && p.local) if(const uint32_t o=ResolveHuman(p.local)) if(api->read_u32(o+2496)==c.local) {
        DestroyObject(scratch,p.local); ++world_m.destroyed[1];
        WorldIndex(p,0); p.state=kCopyFading; p.deadline=0;
      }
      DestroyObject(scratch,c.local); ++world_m.destroyed[0];
    }
  } else if(ResolveHuman(c.local)) { DestroyObject(scratch,c.local); ++world_m.destroyed[1]; }
  WorldIndex(c,0);
}
// Area left (the OwnerMap, no OwnerMap for 3 s, the session over): every copy fades.
void WorldLeaveArea(GuestScratch& scratch,uint64_t now,const char* why) {
  unsigned n=0;
  for(auto& [k,c]:world_copies) if(c.state!=kCopyFading) { WorldFade(scratch,c,cw::kDefaultFadeMs,now); ++n; }
  world_queue.clear(); world_declined.clear(); world_report.clear(); world_early.clear(); world_digest=cw::DigestParts{};
  world_area.in=false;
  static unsigned logged=0;
  if(logged++<20) { char line[160]; std::snprintf(line,sizeof(line),"World (member): area left (%s, epoch %u): %u copies fading",why,unsigned(world_area.epoch),n); ProbeLog(line); }
}

// A car of the host's this game shows already (a car copy a player's pose
// made - UpdateRemoteCar, for a car the world stream hasn't made yet - or one
// this player took while away): taken into the table, not a second car.
bool WorldAdoptCar(WorldCopy& c,uint64_t now) {
  uint32_t have=0;
  if(const auto cc=car_copies.find(CarKey(c.owner,c.id)); cc!=car_copies.end() && ResolveVehicle(cc->second.local)) { have=cc->second.local; car_copies.erase(cc); }
  else for(auto t=taken_cars.begin();t!=taken_cars.end();++t) if(t->second==c.id && ResolveVehicle(t->first)) { have=t->first; taken_cars.erase(t); break; }
  if(!have) return false;
  WorldIndex(c,have); c.state=kCopyMade; c.made_at=now; c.paint_done=true;
  ++world_m.adopted;
  return true;
}
// The host's messages (game thread, in the order they came).
void WorldOnCreate(GuestScratch& scratch,const WorldIn& m,uint64_t now) {
  cw::CreateMsg c;
  if(!cw::ReadCreate(m.bytes.data(),m.bytes.size(),c)) { ++world_m.bad; return; }
  const uint8_t owner=0;   // (the host: the only owner in 2.2)
  const cw::Key key=cw::MakeKey(owner,c.head.id);
  const bool car=cw::IsCar(c.head.kind);
  ++world_m.offered[car ? 0 : 1];
  world_tomb.erase(key); world_declined.erase(key);
  // Still held here: the owner's handle came back (a new generation, or the area left and joined again while it faded): the old copy goes now.
  if(const auto it=world_copies.find(key); it!=world_copies.end()) {
    if(it->second.local && world_graveyard.size()<256) world_graveyard.push_back(std::move(it->second));   // (destroyed 4 an update with the faded ones: review A3-6)
    world_copies.erase(it); ++world_m.replaced;
  }
  WorldCopy w;
  w.owner=owner; w.id=c.head.id; w.kind=c.head.kind; w.car=c.head.car; w.seat=c.head.seat; w.model=c.head.model; w.team=c.head.team;
  w.name=c.name; w.look=c.look; w.paint=c.paint; w.create_time=w.last_time=c.head.time; w.queued_at=now;
  const uint8_t type=car ? 0 : c.head.car ? 2 : 1;
  WorldTakeRecord(w,type,c.first.data(),c.head.ref,c.head.time,now);
  --world_m.updates[car ? 0 : 1];   // (the create's own record)
  const bool player_car=car && (c.head.authority&cw::kAuthorityPlayer);
  if(player_car) { w.driven=uint8_t(c.head.authority&0x7F); w.driven_at=now; }
  w.ped.follow.delay=130;   // (people come at 2-20 Hz: 2.1's margin)
  w.follow.delay=60;
  // Admission (spec 3.6/28). A player's car always: its pose names it.
  if(!player_car) {
    uint8_t why=0xFF;
    if(!car && w.car) { const WorldCopy* cc=WorldFind(owner,w.car); if(!cc || cc->state==kCopyFading) why=kDeclineCrew; }
    if(why==0xFF && !car && !WorldModelRoom(w.model)) why=kDeclineModel;
    if(why==0xFF) {
      unsigned held=0;
      for(auto& [k,o]:world_copies) held+=cw::IsCar(o.kind)==car && o.state!=kCopyFading;
      if(held>=(car ? WorldCarCap() : WorldPeopleCap())) {
        // Full: the farthest held one of its kind (not a crew member, not a car
        // a player has, not one this player stands next to) gives way to one
        // nearer by 20 m (people 10 m), else this one is declined.
        float eye[3]; WorldEye(eye);
        const float d_new=WorldDist(w.pos,eye);
        WorldCopy* farthest=nullptr; float d_far=0;
        for(auto& [k,o]:world_copies) {
          if(cw::IsCar(o.kind)!=car || o.state==kCopyFading || (!car && o.car) || (car && (o.driven!=coopnet::kNoSlot || PlayerSitsIn(o.local)))) continue;
          const float d=WorldDist(o.pos,eye);
          if(d>d_far) { d_far=d; farthest=&o; }
        }
        if(farthest && d_far>d_new+(car ? 20.0f : 10.0f)) {
          // its crew goes with it
          if(car) for(auto& [k,p]:world_copies) if(!cw::IsCar(p.kind) && p.car==farthest->id && p.state!=kCopyFading) { WorldDecline(cw::MakeKey(owner,p.id),p.kind,kDeclineRoom,p.pos,now); WorldFade(scratch,p,cw::kDefaultFadeMs,now); }
          WorldDecline(cw::MakeKey(owner,farthest->id),farthest->kind,kDeclineRoom,farthest->pos,now);
          WorldFade(scratch,*farthest,cw::kDefaultFadeMs,now);
        } else why=kDeclinePool;
      }
    }
    if(why!=0xFF) { WorldDecline(key,w.kind,why,w.pos,now); return; }
  }
  WorldCopy& cp=world_copies.emplace(key,std::move(w)).first->second;
  if(const auto e=world_early.find(key); e!=world_early.end()) {   // a record that came before its create
    if(int32_t(e->second.time-cp.create_time)>0) { cp.last_time=e->second.time; WorldTakeRecord(cp,e->second.type,e->second.rec,e->second.ref,e->second.time,now); ++world_m.early_used; }
    world_early.erase(e);
  }
  if(car && WorldAdoptCar(cp,now)) return;
  world_queue.push_back(key);
}
void WorldOnRemove(GuestScratch& scratch,const WorldIn& m,uint64_t now) {
  cw::RemoveRec r;
  if(m.bytes.size()!=sizeof(r)) { ++world_m.bad; return; }
  std::memcpy(&r,m.bytes.data(),sizeof(r));
  if(!cw::HasMagic(r.magic,"WWR1")) { ++world_m.bad; return; }
  const cw::Key key=cw::MakeKey(0,r.id);
  world_tomb[key]={r.time,now};
  world_declined.erase(key); world_early.erase(key);
  const auto it=world_copies.find(key);
  if(it==world_copies.end()) return;
  ++(r.reason==cw::kRemoveAreaLeft ? world_m.removes_area : world_m.removes_gone);
  static bool first=true;
  if(first && it->second.local) {
    first=false;
    char line[160]; std::snprintf(line,sizeof(line),"World (member): the first remove applied: %s %08X (reason %u, fade %u ms; copy %08X)",cw::IsCar(it->second.kind) ? "car" : "person",r.id,unsigned(r.reason),unsigned(r.fade_ms),it->second.local);
    ProbeLog(line);
  }
  WorldFade(scratch,it->second,r.fade_ms,now);
}
void WorldOnUpdate(const WorldIn& m,uint64_t now) {
  cw::UpdateMsg u;
  if(!cw::ReadUpdate(m.bytes.data(),m.bytes.size(),u) || u.head.owner!=0) { ++world_m.bad; return; }
  if(world_area.have && int16_t(u.head.epoch-world_area.epoch)<0) { ++world_m.epoch; return; }   // (from before the OwnerMap's last change)
  // Update age on this clock: (apply - owner time) less the least (receive - owner time) of the window.
  world_age_offset=std::min(world_age_offset,int32_t(uint32_t(m.at)-u.head.time));
  if(world_ages.size()<8192) world_ages.push_back(int32_t(uint32_t(now)-u.head.time));
  for(const cw::CarRec& r:u.cars) if(WorldCopy* c=WorldRecordCopy(cw::MakeKey(0,r.id),true,0,&r,u.head.ref,u.head.time,now)) WorldCarRecord(*c,r,u.head.ref,u.head.time,now);
  for(size_t i=0;i<u.peds.size();++i) { const cw::PedRec& r=u.peds[i];
    if(WorldCopy* c=WorldRecordCopy(cw::MakeKey(0,r.id),false,1,&r,u.head.ref,u.head.time,now)) { WorldPedRecord(*c,r,u.head.ref,u.head.time,now); WorldFireIn(*c,r.weapon,&u.peds_ext[i],u.head.ref,now); } }
  for(size_t i=0;i<u.seated.size();++i) { const cw::SeatedRec& r=u.seated[i];
    if(WorldCopy* c=WorldRecordCopy(cw::MakeKey(0,r.id),false,2,&r,u.head.ref,u.head.time,now)) { WorldSeatedRecord(*c,r,now); WorldFireIn(*c,r.weapon,&u.seated_ext[i],u.head.ref,now); } }
}
// The set digest against the table (the proof line, spec section 6): missing
// ids (a create this game doesn't have) go back as declined, so the host
// offers them again; extra ones (held here, gone there) are removed.
void WorldOnDigest(GuestScratch& scratch,const WorldIn& m,uint64_t now) {
  if(!world_digest.Add(m.bytes.data(),m.bytes.size())) return;
  std::vector<uint32_t> owner_ids=world_digest.Ids(), mine, held;
  const uint32_t time=world_digest.time;
  world_digest=cw::DigestParts{};
  std::sort(owner_ids.begin(),owner_ids.end());
  for(auto& [k,c]:world_copies) if(c.state!=kCopyFading) held.push_back(c.id);
  mine=held;
  for(auto& [k,d]:world_declined) mine.push_back(cw::IdOf(k));
  std::sort(held.begin(),held.end()); std::sort(mine.begin(),mine.end());
  std::vector<uint32_t> missing, extra, unused;
  cw::DigestDiff(owner_ids,mine,missing,unused);
  cw::DigestDiff(owner_ids,held,unused,extra);
  WorldMatch& wm=world_match;
  wm=WorldMatch{}; wm.have=true; wm.n=unsigned(owner_ids.size());
  for(uint32_t id:owner_ids) {
    if(std::binary_search(held.begin(),held.end(),id)) {
      ++wm.held;
      const WorldCopy* c=WorldFind(0,id);
      if(c && !cw::IsCar(c->kind)) { const uint32_t o=ResolveHuman(c->local); if(!o || !(api->read_u32(o+216)&0x4000)) ++wm.model_wait; }
    } else if(const auto d=world_declined.find(cw::MakeKey(0,id)); d!=world_declined.end()) { ++wm.declined; if(d->second.why==kDeclineGone) ++wm.chunk; }
  }
  for(uint32_t id:extra) if(const auto it=world_copies.find(cw::MakeKey(0,id)); it!=world_copies.end()) {
    world_tomb[it->first]={time,now};
    WorldFade(scratch,it->second,cw::kDefaultFadeMs,now); ++world_m.removes_digest;
  }
  for(uint32_t id:missing) { const float zero[3]{}; WorldDecline(cw::MakeKey(0,id),0,kDeclineLost,zero,now); }
  // Declined ids the host no longer lists are forgotten.
  for(auto it=world_declined.begin();it!=world_declined.end();) it=std::binary_search(owner_ids.begin(),owner_ids.end(),cw::IdOf(it->first)) ? std::next(it) : world_declined.erase(it);
  wm.missing.assign(missing.begin(),missing.begin()+std::min<size_t>(missing.size(),8));
  wm.extra.assign(extra.begin(),extra.begin()+std::min<size_t>(extra.size(),8));
  char line[400], ids[200]="";
  auto list=[](const std::vector<uint32_t>& v,size_t total,char* out,size_t n) { size_t o=0; out[0]=0; for(uint32_t id:v) o+=size_t(std::snprintf(out+o,n-o,"%s%08X",o?" ":"",id)); if(total>v.size()) std::snprintf(out+o,n-o," +%zu",total-v.size()); };
  char miss[120], ext[120];
  list(wm.missing,missing.size(),miss,sizeof(miss)); list(wm.extra,extra.size(),ext,sizeof(ext));
  (void)ids;
  std::snprintf(line,sizeof(line),"World set match: held %u of %u (declined %u, model-wait %u, chunk %u) = %.0f%%; missing %zu [%s]; extra %zu [%s]",
                wm.held,wm.n,wm.declined,wm.model_wait,wm.chunk,wm.n ? 100.0*(wm.held+wm.declined)/wm.n : 100.0,missing.size(),miss,extra.size(),ext);
  ProbeLog(line);
}
void WorldOnMap(GuestScratch& scratch,const WorldIn& m,uint64_t now) {
  cw::OwnerMapRec r;
  if(m.bytes.size()!=sizeof(r)) { ++world_m.bad; return; }
  std::memcpy(&r,m.bytes.data(),sizeof(r));
  if(!cw::HasMagic(r.magic,"WOM1")) { ++world_m.bad; return; }
  // An older one is dropped; one far behind (over 1000 epochs) is a new host's (review A3-2).
  if(world_area.have && int16_t(r.epoch-world_area.epoch)<0 && int16_t(world_area.epoch-r.epoch)<=1000) return;
  world_area.have=true; world_area.epoch=r.epoch; world_area.map_at=now;
  const uint8_t me=my_slot.load();
  bool in=me<kSlots && r.slots[me].area!=cw::kSolo && r.slots[me].owner==0;
  if(world_rejoin_wait) {   // (left on its own: in again only after the host has had it out)
    if(in) in=false;
    else {
      world_rejoin_wait=false;
      static unsigned logged=0;
      if(logged++<20) ProbeLog("World (member): the host's OwnerMap has this game out too: it can join again");
    }
  }
  if(in==world_area.in) return;
  if(in) {
    world_area.in=true; world_area.joined_at=now; world_admit_due=true;
    static unsigned logged=0;
    if(logged++<20) { char line[160]; std::snprintf(line,sizeof(line),"World (member): area joined (epoch %u): the host's world stream is shown here; %zu copies still fading",unsigned(r.epoch),world_copies.size()); ProbeLog(line); }
  } else WorldLeaveArea(scratch,now,"the host's OwnerMap");
}
void WorldAuthorityEvent(const Event& e,uint64_t now) {
  (void)now;
  WorldCopy* c=WorldFind(uint8_t(e.a[0]),e.a[1]);
  if(!c || !cw::IsCar(c->kind)) return;
  const uint8_t was=c->driven;
  c->driven=e.a[3] ? uint8_t(e.a[2]) : coopnet::kNoSlot;
  if(e.a[3]) c->driven_at=MonoMs();
  if(was==c->driven) return;
  c->follow.Reset();
  ++(e.a[3] ? world_m.taken : world_m.returned);
  static unsigned logged=0;
  if(logged++<30) {
    char line[140];
    if(e.a[3]) std::snprintf(line,sizeof(line),"World (member): car %08X (copy %08X) driven by slot %u: no records for it",e.a[1],c->local,e.a[2]);
    else std::snprintf(line,sizeof(line),"World (member): car %08X (copy %08X) back with the host's game: its records again",e.a[1],c->local);
    ProbeLog(line);
  }
}

// The body's velocities the way the game's MP frame sets a remote car's
// (823BB068): the body (+244) woken with 826CD960 (which returns at once for
// an awake body - its island +92, +40 set - or a fixed one, +153; with no
// island it would write through 0), then its motion's (+88) setters, vtable
// +88 the linear and +92 the angular velocity (16-byte vectors).
void SetCarVelocity(GuestScratch& scratch,uint32_t v,const float* lin,const float* ang) {
  const uint32_t body=api->read_u32(v+244);
  if(!Readable(body,160)) return;
  const uint32_t motion=api->read_u32(body+88);
  if(!Readable(motion,240)) return;
  const uint32_t vt=api->read_u32(motion);
  if(!Readable(vt,96)) return;
  const uint32_t island=api->read_u32(body+92);
  if(island && Readable(island,44) && !api->read_u8(island+40) && !api->read_u8(body+153)) { scratch.ctx.r3.u64=body; scratch.Call(0x826CD960); ++world_m.woken; }
  const uint32_t vec=(scratch.data()+3600+15)&~15u;   // (the setters load with lvx: 16-byte aligned, as 823BB068's stack slots; review A3-8)
  for(unsigned i=0;i<3;++i) { api->write_f32(vec+i*4,lin[i]); api->write_f32(vec+16+i*4,ang[i]); }
  api->write_f32(vec+12,0); api->write_f32(vec+28,0);
  scratch.ctx.r3.u64=motion; scratch.ctx.r4.u64=vec; scratch.Call(api->read_u32(vt+88));
  scratch.ctx.r3.u64=motion; scratch.ctx.r4.u64=vec+16; scratch.Call(api->read_u32(vt+92));
}
// A car copy along its path (spec 3.7/34): steered with velocities, placed
// only when far off (8 m + 0.35 s of its speed) or turned round (rows off by
// about 60 degrees); without physics its transform is written (at most twice
// a second unless the record moved). Resting where it should be: left asleep.
void WorldSteerCar(GuestScratch& scratch,WorldCopy& c,uint32_t v,uint64_t now) {
  if(!c.follow.count) return;
  float path_pos[3], rows[9], vel[3];
  c.follow.Sample(uint32_t(int64_t(now)-c.follow.offset-c.follow.delay),path_pos,rows,vel);
  if(c.vel[0]||c.vel[1]||c.vel[2]) std::memcpy(vel,c.vel,sizeof(vel));
  float e[3], dist2=0, align=0, cur[9];
  for(unsigned i=0;i<3;++i) { e[i]=path_pos[i]-api->read_f32(v+20+i*4); dist2+=e[i]*e[i]; }
  for(unsigned i=0;i<9;++i) { cur[i]=api->read_f32(v+32+i*4); align+=rows[i]*cur[i]; }   // (3 when the same)
  const float speed=std::sqrt(vel[0]*vel[0]+vel[1]*vel[1]+vel[2]*vel[2]);
  if(api->read_u8(v+50300)!=1) {
    if(dist2<0.05f*0.05f && align>2.999f) return;
    const float mx=path_pos[0]-c.written[0], my=path_pos[1]-c.written[1], mz=path_pos[2]-c.written[2];
    if(now<c.next_write && mx*mx+my*my+mz*mz<0.25f) return;
    for(unsigned i=0;i<3;++i) api->write_f32(v+20+i*4,path_pos[i]);
    for(unsigned i=0;i<9;++i) api->write_f32(v+32+i*4,rows[i]);
    std::memcpy(c.written,path_pos,sizeof(c.written)); c.next_write=now+500;
    ++world_m.writes;
    return;
  }
  const float snap=8.0f+speed*0.35f;
  if(dist2>snap*snap || align<2.0f) {
    const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316;
    for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,path_pos[i]);
    api->write_f32(pos+12,0);
    for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,rows[i]);
    PlaceVehicle(scratch,v,pos,matrix);
    if(api->read_u8(v+50300)==1) SetCarVelocity(scratch,v,vel,c.ang);
    ++world_m.placements;
    return;
  }
  const float spin=std::fabs(c.ang[0])+std::fabs(c.ang[1])+std::fabs(c.ang[2]);
  if(speed<0.2f && spin<0.05f && dist2<0.3f*0.3f && align>2.99f) return;
  float w[3]={0,0,0};
  for(unsigned a=0;a<3;++a) {
    const float* cv=cur+a*3; const float* t=rows+a*3;
    w[0]+=0.5f*(cv[1]*t[2]-cv[2]*t[1]);
    w[1]+=0.5f*(cv[2]*t[0]-cv[0]*t[2]);
    w[2]+=0.5f*(cv[0]*t[1]-cv[1]*t[0]);
  }
  float lin[3], ang[3];
  for(unsigned i=0;i<3;++i) { lin[i]=vel[i]+std::clamp(e[i]*5.0f,-15.0f,15.0f); ang[i]=c.ang[i]+std::clamp(w[i]*5.0f,-3.0f,3.0f); }
  SetCarVelocity(scratch,v,lin,ang);
  ++world_m.steered;
}

// Made: a car with its paint, a person with its look (once set up), a crew member seated in its car.
bool WorldMake(GuestScratch& scratch,WorldCopy& c,uint64_t now,uint32_t player) {
  if(cw::IsCar(c.kind)) {
    const uint32_t h=CreateVehicle(scratch,int32_t(c.model),c.pos,c.rows);
    const uint32_t v=ResolveVehicle(h);
    if(!v) return false;
    if(VehicleType(v)!=int32_t(c.model)) ++car_type_mismatch;
    PinModel(scratch,api->read_u32(kVehicleInfo+uint32_t(c.model)*kVehicleInfoSize),now);   // (its model at the top priority, as 2.1's copies)
    ForgetCarDamage(h);
    WorldIndex(c,h); c.state=kCopyMade; c.made_at=now;
    if(const uint32_t va=VehicleVelocityAddress(v)) for(unsigned i=0;i<3;++i) { api->write_f32(va+i*4,c.vel[i]); api->write_f32(va+16+i*4,c.ang[i]); }
    c.follow.Add(c.last_time,c.pos,c.rows,now);
    if(c.paint.empty() || ApplyCarLook(scratch,v,c.paint)) c.paint_done=true; else c.next_paint=now+500;
    ++world_m.made[0];
    return true;
  }
  uint32_t v=0;
  if(c.car) {
    const WorldCopy* cc=WorldFind(c.owner,c.car);
    v=cc && cc->state==kCopyMade ? ResolveVehicle(cc->local) : 0;
    if(!v) return false;
    if(const uint32_t occ=OccupantHandle(v,c.seat)) if(occ==api->read_u32(player+68) || IsAvatarHandle(occ)) return false;   // (a player sits there here)
    std::memcpy(c.pos,cc->pos,sizeof(c.pos)); std::memcpy(c.rows,cc->rows,sizeof(c.rows));
  }
  const uint32_t data=scratch.data(), pos=data+1300, matrix=data+1316, name=data+1400;
  for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,v ? api->read_f32(v+20+i*4) : c.pos[i]);
  api->write_f32(pos+12,0);
  for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,v ? api->read_f32(v+32+i*4) : c.rows[i]);
  const uint32_t definition=LookupModel(scratch,c.name.c_str(),name);
  if(!definition) return false;
  const uint32_t h=CreateActor(scratch,definition,pos,matrix,"World person",true);
  const uint32_t o=ResolveHuman(h);
  if(!o) return false;
  WorldIndex(c,h); c.state=kCopyMade; c.made_at=now;
  SharedPed& sp=c.ped;
  sp.model=0; sp.dead=false; sp.look_applied=c.look.empty(); sp.look_hidden=false; sp.loading_since=0; sp.follow.Reset();
  if(c.team<=6) { api->write_u32(o+232,c.team); ++teams_written; }   // (as set_team 824DB708 stores it)
  MakePassive(o,!(sp.flags&cw::kPedMission));
  if(v) {
    ++replicated_depth; const bool seated=SeatInVehicle(scratch,o,v,c.seat); --replicated_depth;
    if(seated) ++world_m.seated; else { ++world_m.seat_refused; c.next_seat=now+1000; }
    c.was_seated=true;
  } else sp.follow.Add(c.last_time,c.pos,c.rows,now);
  ++world_m.made[1];
  return true;
}

// A person copy, every update (2.1's copies' rules: UpdateSharedPeds).
// Returns false when the copy is lost here (to be reported).
bool WorldApplyPerson(GuestScratch& scratch,WorldCopy& c,uint32_t obj,uint32_t player,uint64_t now) {
  SharedPed& sp=c.ped;
  ShowHostTeam(obj,sp.team,c.standin ? "world stand-in" : c.car ? "world crew" : "world copy",c.name.c_str(),player);
  const bool set_up=(api->read_u32(obj+216)&0x4000)!=0;
  const WorldCopy* cc=c.car ? WorldFind(c.owner,c.car) : nullptr;
  const uint32_t v=cc && cc->state==kCopyMade ? ResolveVehicle(cc->local) : 0;
  if(!set_up) {
    if(!sp.loading_since) sp.loading_since=now;
    const uint32_t desc=api->read_u32(obj+228);
    const uint32_t mid=Readable(desc,612) ? api->read_u32(desc+608) : 0;
    if(mid) PinModel(scratch,mid,now);
    // The request can be dropped when the group had no free record then: asked again (once).
    if(mid && !sp.rerequested && now-sp.loading_since>2000) {
      sp.rerequested=true;
      char info[160];
      if(StreamState(mid,info,sizeof(info))==0) {
        scratch.ctx.r3.u64=kStreamTable+(mid>>24)*204; scratch.ctx.r4.u64=mid;
        scratch.ctx.r5.u64=5; scratch.ctx.r6.u64=0; scratch.ctx.r7.u64=1;
        scratch.Call(0x8250C750);
        actor_models.erase(c.local); NoteActorModel(c.local,mid);
        ++streams_rerequested;
      }
    }
    // A driver whose model hasn't loaded in 2 s while its car moves: a lookalike at the wheel (spec 3.6/31: an empty driver's seat in a moving car shows).
    if(v && c.seat==0 && !c.standin && now-sp.loading_since>=2000 && now>=c.next_standin && now>=actors_blocked_until) {
      const float sp2=cc->vel[0]*cc->vel[0]+cc->vel[1]*cc->vel[1]+cc->vel[2]*cc->vel[2];
      if(sp2>1.0f) {
        c.next_standin=now+3000;
        if(const uint32_t definition=LoadedLookalike(scratch,c.name.c_str(),player)) {
          const uint32_t pos=scratch.data()+1300, matrix=scratch.data()+1316;
          for(unsigned i=0;i<3;++i) api->write_f32(pos+i*4,api->read_f32(v+20+i*4));
          api->write_f32(pos+12,0);
          for(unsigned i=0;i<9;++i) api->write_f32(matrix+i*4,api->read_f32(v+32+i*4));
          const uint32_t made=CreateActor(scratch,definition,pos,matrix,"World driver stand-in",true);
          if(const uint32_t m=ResolveHuman(made)) {
            DestroyObject(scratch,c.local);
            WorldIndex(c,made);
            ++replicated_depth; const bool seated=SeatInVehicle(scratch,m,v,0); --replicated_depth;
            if(!seated) { DestroyObject(scratch,made); WorldIndex(c,0); return false; }
            c.standin=true; sp.look_applied=true; sp.loading_since=0; ++world_m.standins; ++seat_standins;
            MakePassive(m);
            return true;
          }
        }
      }
    }
    if(now-sp.loading_since>8000 && !sp.stuck_logged) {
      sp.stuck_logged=true;
      static unsigned logged=0;
      if(logged++<25) {
        char line[200], info[160]="?";
        if(mid) StreamState(mid,info,sizeof(info));
        std::snprintf(line,sizeof(line),"Copy never finished loading: %s (world %s %08X; its model: %s)",c.name.c_str(),c.car ? "crew member" : "person",c.id,info);
        ProbeLog(line);
      }
    }
    // Not set up in 20 s: let go (the host offers it again in 3 s, maybe with its model in by then).
    if(now-sp.loading_since>20000) { DestroyObject(scratch,c.local); WorldIndex(c,0); return false; }
    return true;
  }
  sp.loading_since=0;
  if(!c.standin && keep_worn_models) { const uint32_t desc=api->read_u32(obj+228); if(Readable(desc,612)) if(const uint32_t mid=api->read_u32(desc+608)) PinModel(scratch,mid,now); }
  // Died in the host's game (its hit points, never the fade bits): dies here too and lies where it fell.
  if((sp.flags&cw::kPedDead) && !sp.dead) { KillCopy(scratch,obj); sp.dead=true; }
  if(sp.dead) return true;
  // The look, once set up (hidden until it is on, at most 3 s: a new copy shows its model's own random clothes first);
  // hidden like the host's (hidden or a cutscene actor there) and while a cutscene plays here (it brings its own people).
  if(!sp.ready_since) sp.ready_since=now;
  if(!sp.look_applied && !c.look.empty()) { sp.look_applied=ApplyLook(scratch,c.local,c.look.data(),unsigned(c.look.size())); if(sp.look_applied) ++peds_looks; }
  const bool hide=(sp.flags&cw::kPedHidden) || CutscenePlaying() || (!sp.look_applied && now-sp.ready_since<3000);
  SetCharacterHidden(scratch,obj,hide);
  MakePassive(obj,!(sp.flags&cw::kPedMission));
  WorldBurn(scratch,c,obj,now);   // (Package C: the host's person's burning; never in a car, 8245E3A0's own refusal)
  const uint32_t in_car=api->read_u32(obj+2496);
  if(c.car) {
    // Seated there: seated here (in its car, once that is here and no player is in that seat or has the car).
    if(!v || in_car==cc->local) return true;
    if(std::any_of(remotes.begin(),remotes.end(),[&](const Remote& r){ return r.pending_victim==c.local; })) return true;   // (pulled out by a player here)
    if(api->read_u32(player+2496)==cc->local || PlayerSitsIn(cc->local) || PlayerGettingIn(cc->local,now) || now<c.next_seat || ResolveVehicle(in_car)) return true;
    c.next_seat=now+1000;
    if(const uint32_t occ=OccupantHandle(v,c.seat)) if(occ!=c.local) return true;
    ++replicated_depth; const bool seated=SeatInVehicle(scratch,obj,v,c.seat); --replicated_depth;
    if(seated) ++world_m.seated; else ++world_m.seat_refused;
    return true;
  }
  // On foot there, still in a car here: out (not while a player pulls it out here).
  if(ResolveVehicle(in_car)) {
    if(std::any_of(remotes.begin(),remotes.end(),[&](const Remote& r){ return r.pending_victim==c.local; }) || now<c.next_exit || c.exits>=4) return true;
    c.next_exit=now+1500; ++c.exits;
    ++replicated_depth; OrderExitVehicle(scratch,obj); --replicated_depth;
    ++world_m.exits;
    return true;
  }
  c.exits=0;
  if(now>=sp.next_crouch) { sp.next_crouch=now+250; MatchCrouch(scratch,obj,(sp.flags&cw::kPedCrouch)!=0); }
  // The gun in hand (the weapon byte's index + 1; 0 none), as other players'
  // characters get theirs: settled (400 ms to another, 1.5 s to none) and
  // kept while its rounds wait (Package C; WorldCopyFire matches it for a round).
  {
    const uint32_t want=c.weapon&cw::kWeaponGun;
    GunState& g=c.gun;
    if(want!=g.pose_want) { g.pose_want=want; g.pose_since=now; }
    const bool busy=!c.fire.shots.empty() || c.fire.holding || now<g.hold_until;
    if(want!=uint32_t(HandWeapon(obj)+1) && !(want==0 && busy) && now-g.pose_since>=(want ? 400u : 1500u)) MatchWeapon(scratch,obj,want,g,true);
  }
  // What the host's person is playing (2.1's sync_idle) and its animation set (applied once per change).
  if(sync_idle && sp.host_action!=0xFFFF && now>=sp.next_action_sync && api->read_u32(obj+756)!=sp.host_action && sp.action_misses<4) {
    sp.next_action_sync=now+1500;
    const bool known=sp.action_args_for==sp.host_action;
    scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=sp.host_action;
    scratch.ctx.r5.u64=known?sp.action_args[0]:0; scratch.ctx.r6.u64=known?sp.action_args[1]:1; scratch.ctx.r7.u64=known?sp.action_args[2]:0;
    ++replicated_depth; const bool played=scratch.Call(0x82444F88) && scratch.ctx.r3.u32; --replicated_depth;
    if(played) { ++ped_actions_synced; sp.action_until=now+4000; sp.action_misses=0; } else ++sp.action_misses;
  }
  if(sp.host_action==0xFFFF) sp.action_misses=0;
  if(sp.anim!=sp.anim_seen) { sp.anim_seen=sp.anim; sp.anim_since=now; }
  const bool drifted=sp.anim==sp.anim_applied && api->read_u32(obj+952)!=sp.anim && now>=sp.next_anim+5000 && sp.follow.moving;
  if(sp.anim!=0xFF && (sp.anim!=sp.anim_applied || drifted) && now-sp.anim_since>=1500 && now>=sp.action_until) {
    sp.anim_applied=sp.anim; sp.next_anim=now+1000;
    api->write_u32(obj+952,sp.anim);
    scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=uint64_t(int64_t(-1)); scratch.ctx.r5.u64=uint64_t(int64_t(-1)); scratch.ctx.r6.u64=0;
    ++replicated_depth; scratch.Call(0x82440200); --replicated_depth;
    sp.follow.next_order=0; sp.follow.moving=true;
    ++peds_anims;
  }
  if(CutscenePlaying() || !sp.follow.count) return true;
  if(now<sp.action_until) {
    // Back to the path as soon as the host's person moves off.
    float pp[3], rr[9], vv[3];
    const float speed=sp.follow.Sample(uint32_t(int64_t(now)-sp.follow.offset-sp.follow.delay),pp,rr,vv);
    const float gx=pp[0]-api->read_f32(obj+20), gz=pp[2]-api->read_f32(obj+28);
    if(speed<0.5f && gx*gx+gz*gz<1.0f) return true;
    sp.action_until=0;
  }
  {
    // Standing still there (a bench, an idle): no walk orders.
    float pp[3], rr[9], vv[3];
    const float speed=sp.follow.Sample(uint32_t(int64_t(now)-sp.follow.offset-sp.follow.delay),pp,rr,vv);
    const float gx=pp[0]-api->read_f32(obj+20), gz=pp[2]-api->read_f32(obj+28);
    if(speed<0.3f && gx*gx+gz*gz<0.8f*0.8f) return true;
  }
  sp.follow.Drive(scratch,obj,now,nullptr);
  return true;
}
// Every frame: a person on the path at that moment (small corrections; 2.1's SmoothSharedPeds).
void SmoothWorldPeople(uint64_t now) {
  for(auto& [k,c]:world_copies) {
    if(cw::IsCar(c.kind) || c.state!=kCopyMade || c.car || CutscenePlaying()) continue;
    SharedPed& sp=c.ped;
    if(sp.dead || now<sp.action_until || !sp.follow.count || sp.follow.offset==INT64_MAX) continue;
    const uint32_t obj=ResolveHuman(c.local);
    if(!obj || !(api->read_u32(obj+216)&0x4000) || ResolveVehicle(api->read_u32(obj+2496))) continue;
    float pos[3], rows[9], vel[3];
    sp.follow.Sample(uint32_t(int64_t(now)-sp.follow.offset-sp.follow.delay),pos,rows,vel);
    const float dx=pos[0]-api->read_f32(obj+20), dy=pos[1]-api->read_f32(obj+24), dz=pos[2]-api->read_f32(obj+28);
    if(dx*dx+dy*dy+dz*dz>4.0f) continue;
    for(unsigned i=0;i<3;++i) api->write_f32(obj+20+i*4,pos[i]);
    for(unsigned i=0;i<9;++i) api->write_f32(obj+32+i*4,rows[i]);
  }
}
// A car copy, every update.
void WorldApplyCar(GuestScratch& scratch,WorldCopy& c,uint32_t v,uint32_t player,uint64_t now) {
  if(!c.paint_done && now>=c.next_paint) { if(ApplyCarLook(scratch,v,c.paint) || ++c.paint_tries>=6) c.paint_done=true; else c.next_paint=now+500; }
  // A player's: this player in it or getting in, another player's character
  // here, or a player the host named (kEventAuthority): that pose moves it.
  const bool driven=api->read_u32(player+2496)==c.local || PlayerSitsIn(c.local) || PlayerGettingIn(c.local,now) || c.driven!=coopnet::kNoSlot;
  if(driven) { if(!c.was_driven) { c.was_driven=true; c.follow.Reset(); WorldDriveClear(c.local); } return; }
  if(c.was_driven) { c.was_driven=false; c.follow.Reset(); }
  if(!WatchImpact(v,c.impact,c.id,now)) { WorldSteerCar(scratch,c,v,now); AfterSteer(v,c.impact); }
  if(now>=c.next_damage) { c.next_damage=now+250; ApplyCarDamage(scratch,v,c.damage,c.hp); ApplyCarSiren(scratch,v,c.damage); }
  WorldDriveApply(scratch,c,v,now);   // (Package C: brake and reverse lights, horn, steering)
}

// WWA1 (spec 3.6/29): what this game holds of each kind and its caps (the
// live pools when they are the tighter), the people models loaded here, the
// chunks loaded here by list index, the ids declined, evicted or lost since
// the last one (32 a message), and the copies shown near the camera.
uint64_t WorldChunkMask() {
  // 0x829A97E8: +0 count, record pointers from +16; 8211E380(id) finds the
  // record whose byte +8 is id and says loaded when its +20 bit 0x80 is
  // clear (cntlzw of the masked byte: 1 for 0). Bit i = list index i.
  const int32_t n=int32_t(api->read_u32(0x829A97E8));
  if(n<=0 || n>256 || !Readable(0x829A97E8+16,uint32_t(n)*4)) return ~0ull;
  uint64_t mask=0;
  std::string high;
  for(int32_t i=0;i<n;++i) {
    const uint32_t r=api->read_u32(0x829A97E8+16+uint32_t(i)*4);
    if(!Readable(r,24)) continue;
    if(i<64 && !(api->read_u8(r+20)&0x80)) mask|=1ull<<i;
    if(api->read_u8(r+8)>=64 && high.size()<200) high+=" "+std::to_string(api->read_u8(r+8))+"@"+std::to_string(i);
  }
  static bool logged=false;
  if(!logged) {
    logged=true;
    char line[320]; std::snprintf(line,sizeof(line),"World chunks here: %d in the list (bit = list index, %s); chunk ids >= 64 seen:%s",n,n>64 ? "past 64 not in the mask" : "all in the mask",high.empty() ? " none" : high.c_str());
    ProbeLog(line);
  }
  return mask;
}
void WorldNear(unsigned& cars,unsigned& people) {
  float eye[3]; WorldEye(eye);
  cars=people=0;
  for(auto& [k,c]:world_copies) {
    if(c.state!=kCopyMade || !c.local) continue;
    if(cw::IsCar(c.kind)) { if(const uint32_t v=ResolveVehicle(c.local)) { const float p[3]={api->read_f32(v+20),api->read_f32(v+24),api->read_f32(v+28)}; cars+=WorldDist(p,eye)<=50.0f; } }
    else if(const uint32_t o=ResolveHuman(c.local)) {
      if(!(api->read_u32(o+216)&0x4000) || (api->read_u32(o+120)&0x08000000u)) continue;
      const float p[3]={api->read_f32(o+20),api->read_f32(o+24),api->read_f32(o+28)}; people+=WorldDist(p,eye)<=50.0f;
    }
  }
}
void WorldSendAdmit(uint64_t now) {
  (void)now;
  cw::AdmitMsg a;
  unsigned cars=0, people=0;
  for(auto& [k,c]:world_copies) ++(cw::IsCar(c.kind) ? cars : people);
  a.head.cars_held=uint16_t(cars); a.head.people_held=uint16_t(people);
  a.head.cars_cap=uint16_t(WorldCarRoom()<=0 ? std::min(cars,WorldCarCap()) : WorldCarCap());
  a.head.people_cap=uint16_t(WorldPeopleRoom()<=0 ? std::min(people,WorldPeopleCap()) : WorldPeopleCap());
  a.head.chunks=WorldChunkMask();
  static unsigned last_model=0, last_chunk=0;
  const unsigned dm=world_m.declined[1][kDeclineModel], dc=world_m.declined[0][kDeclineGone]+world_m.declined[1][kDeclineGone];
  a.head.dropped_model=uint8_t(std::min(255u,dm-std::min(dm,last_model))); a.head.dropped_chunk=uint8_t(std::min(255u,dc-std::min(dc,last_chunk)));
  unsigned nc=0, np=0; WorldNear(nc,np);
  a.head.near_cars=uint8_t(std::min(255u,nc)); a.head.near_people=uint8_t(std::min(255u,np));
  {
    const uint32_t group=kStreamTable;
    if(Readable(group,204)) {
      const uint32_t head=api->read_u32(group+48); unsigned guard=0;
      for(uint32_t e=head;e && Readable(e,72) && guard++<256 && a.models.size()<cw::kMaxModels;) {
        const uint32_t id=api->read_u32(e+4);
        if(api->read_u32(e) && id<0x10000) a.models.push_back(uint16_t(id));
        e=api->read_u32(e+28); if(e==head) break;
      }
    }
  }
  const size_t n=std::min(world_report.size(),cw::kMaxDeclined);
  a.declined.assign(world_report.begin(),world_report.begin()+n);
  uint8_t buf[cw::kMaxMessage];
  const size_t len=cw::WriteAdmit(a,buf,sizeof(buf));
  if(!len) return;
  WorldQueue(kMsgWorldAdmit,0,false,buf,len);
  world_report.erase(world_report.begin(),world_report.begin()+n);
}

// The 10 s lines (spec section 6).
extern unsigned hood_skipped; extern int world_trains_saved;
void WorldMemberLog(uint64_t now) {
  static uint64_t last=0;
  const double seconds=last ? std::max(1.0,(now-last)/1000.0) : 10.0;
  last=now;
  unsigned held[2]{}, made[2]{}, fading=0, waiting=0, queued=0, standins=0, driven=0;
  for(auto& [k,c]:world_copies) {
    const int i=cw::IsCar(c.kind) ? 0 : 1;
    ++held[i];
    if(c.state==kCopyQueued) ++queued; else if(c.state==kCopyFading) ++fading; else ++made[i];
    if(c.standin) ++standins;
    if(i==0 && c.state==kCopyMade && (c.driven!=coopnet::kNoSlot || c.was_driven)) ++driven;
    if(i==1 && c.state==kCopyMade) if(const uint32_t o=ResolveHuman(c.local)) if(!(api->read_u32(o+216)&0x4000)) ++waiting;
  }
  WorldMemberCounts& n=world_m;
  auto declined=[&](int i) { unsigned t=0; for(unsigned w=0;w<kDeclineReasons;++w) t+=n.declined[i][w]; return t; };
  auto reasons=[&](int i) { std::string s; for(unsigned w=0;w<kDeclineReasons;++w) if(n.declined[i][w]) s+=std::string(s.empty() ? "" : ", ")+kDeclineNames[w]+" "+std::to_string(n.declined[i][w]); return s.empty() ? std::string("none") : s; };
  unsigned loaded=0, model_waiting=0, bodies=0;
  {
    const uint32_t group=kStreamTable;
    std::vector<uint32_t> players;
    for(int s=-1;s<kSlots;++s) { const uint32_t o=s<0 ? api->read_u32(0x8309ABEC) : AvatarObjOf(s); if(Readable(o,4252)) { const uint32_t d=api->read_u32(o+228); if(Readable(d,612)) players.push_back(api->read_u32(d+608)); } }
    if(Readable(group,204)) { const uint32_t head=api->read_u32(group+48); unsigned guard=0;
      for(uint32_t e=head;e && Readable(e,72) && guard++<256;) { if(api->read_u32(e)) { ++loaded; if(std::count(players.begin(),players.end(),api->read_u32(e+4))) ++bodies; } else ++model_waiting; e=api->read_u32(e+28); if(e==head) break; } }
  }
  const double car_s=n.car_ms/1000.0;
  char line[1400];
  std::snprintf(line,sizeof(line),"World (member of slot 0's area, epoch %u%s): held cars %u of %u offered (declined %u: %s), people %u of %u (declined %u: %s); now cars %u, people %u (%u queued, %u fading, %u driven by players); "
                "copies made %u cars / %u people (%u taken over), faded %u / %u, destroyed %u / %u (at once %u, past deadline %u), let go %u, gone here %u / %u; stand-ins %u (drivers after 2 s, %u now), waiting for a model %u, models loaded %u/12 (player bodies %u, %u waiting); "
                "creates applied %u (held for a car %u, replaced %u), updates applied %u cars / %u people (old %u, before the create %u: used %u, expired %u; after a remove %u, declined %u, old epoch %u), removes %u (gone %u, area %u, digest %u); "
                "placements %.2f per car per second (%u placed, %u steered, %u woken, %u written without physics); crews seated %u (refused %u), out of a car %u; authority taken %u / back %u; bytes in %.1f KB/s; malformed %u; "
                "this game's hood / special spawn passes skipped %u, trains %s",
                unsigned(world_area.epoch),world_area.in ? "" : ", not in it",held[0]+0,n.offered[0],declined(0),reasons(0).c_str(),held[1],n.offered[1],declined(1),reasons(1).c_str(),made[0],made[1],queued,fading,driven,
                n.made[0],n.made[1],n.adopted,n.faded[0],n.faded[1],n.destroyed[0],n.destroyed[1],n.at_once,n.past_deadline,n.let_go,n.gone[0],n.gone[1],n.standins,standins,waiting,loaded,bodies,model_waiting,
                n.creates,n.held_for_car,n.replaced,n.updates[0],n.updates[1],n.old,n.early,n.early_used,n.early_expired,n.after_remove,n.declined_records,n.epoch,n.removes_gone+n.removes_area+n.removes_digest,n.removes_gone,n.removes_area,n.removes_digest,
                car_s>0 ? n.placements/car_s : 0.0,n.placements,n.steered,n.woken,n.writes,n.seated,n.seat_refused,n.exits,n.taken,n.returned,world_in_bytes.exchange(0)/1024.0/seconds,n.bad,
                hood_skipped,world_trains_saved>=0 ? "off" : "as the game has them");
  hood_skipped=0;
  ProbeLog(line);
  // Update age, one clock (spec F4): apply time less the owner time, less the window's least receive delay.
  if(!world_ages.empty() && world_age_offset!=INT32_MAX) {
    std::vector<int32_t> a=world_ages;
    for(int32_t& x:a) x-=world_age_offset;
    std::sort(a.begin(),a.end());
    std::snprintf(line,sizeof(line),"World update age: p50 %d ms, p95 %d ms (%zu update messages; the least receive delay as zero)",a[a.size()/2],a[std::min(a.size()-1,a.size()*95/100)],a.size());
    ProbeLog(line);
  }
  world_ages.clear(); world_age_offset=INT32_MAX;
  // World match, one clock: within 50 m of this camera what the host lists (the records held and the creates declined) against what shows here.
  {
    float eye[3]; WorldEye(eye);
    unsigned lc=0, lp=0, sc=0, spp=0, fade=0, wait=0, dec=0;
    for(auto& [k,c]:world_copies) {
      if(WorldDist(c.pos,eye)>50.0f) continue;
      const bool car=cw::IsCar(c.kind);
      if(c.state!=kCopyFading) ++(car ? lc : lp);
      if(c.state==kCopyFading) { ++fade; continue; }
      bool shown=false;
      if(c.state==kCopyMade && car) shown=ResolveVehicle(c.local)!=0;
      else if(c.state==kCopyMade) if(const uint32_t o=ResolveHuman(c.local)) shown=(api->read_u32(o+216)&0x4000) && !(api->read_u32(o+120)&0x08000000u);
      if(shown) ++(car ? sc : spp); else ++wait;
    }
    for(auto& [k,d]:world_declined) if(WorldDist(d.pos,eye)<=50.0f && d.why!=kDeclineLost) { ++(cw::IsCar(d.kind) ? lc : lp); ++dec; }
    std::snprintf(line,sizeof(line),"World match: within 50 m of me the host lists cars %u / people %u, I show %u / %u (fading %u, waiting %u, declined %u)",lc,lp,sc,spp,fade,wait,dec);
    ProbeLog(line);
  }
  // Model references (spec 3.6/30): per people model, all its references against co-op's pin and the copies wearing it here.
  {
    std::unordered_map<uint32_t,unsigned> wearing;
    for(auto& [k,c]:world_copies) if(!cw::IsCar(c.kind)) if(const uint32_t o=ResolveHuman(c.local)) { const uint32_t d=api->read_u32(o+228); if(Readable(d,612)) ++wearing[api->read_u32(d+608)]; }
    std::string s; unsigned over=0; char t[48];
    const uint32_t group=kStreamTable;
    if(Readable(group,204)) { const uint32_t head=api->read_u32(group+48); unsigned guard=0;
      for(uint32_t e=head;e && Readable(e,72) && guard++<64;) {
        const uint32_t id=api->read_u32(e+4);
        unsigned refs=0; for(unsigned k=0;k<7;++k) refs+=api->read_u32(e+44+k*4);
        const auto p=model_pins.find(id); const unsigned pins=p!=model_pins.end() && p->second.held ? 1 : 0;
        const auto w=wearing.find(id); const unsigned copies=w!=wearing.end() ? w->second : 0;
        if(refs>pins+copies+1) ++over;
        if(s.size()<600) { std::snprintf(t,sizeof(t)," %X %u (%u, %u)",id,refs,pins,copies); s+=t; }
        e=api->read_u32(e+28); if(e==head) break; } }
    std::snprintf(line,sizeof(line),"World models: per id refs (pins, copies):%s; %u with more refs than pins + copies + 1",s.empty() ? " none" : s.c_str(),over);
    ProbeLog(line);
  }
  // Package B's pass line: what this game shows near its camera far from the host against next to it (plan_B 8: people >= 60%, cars >= 80%).
  {
    const WorldBand& a=world_band_near; const WorldBand& f=world_band_far;
    const double a0=a.seconds ? a.people/a.seconds : 0, b0=a.seconds ? a.cars/a.seconds : 0, a1=f.seconds ? f.people/f.seconds : 0, b1=f.seconds ? f.cars/f.seconds : 0;
    std::snprintf(line,sizeof(line),"World (member) B: near me (people 50 m / cars 100 m of my eye) by distance to the host: <40 m: people %.1f, cars %.1f (%u s); 100-150 m: people %.1f, cars %.1f (%u s); session ratio people a1/a0 = %.0f%%, cars b1/b0 = %.0f%% (needs n0, n1 >= 60 s)",
                  a0,b0,a.seconds,a1,b1,f.seconds,a0>0 ? 100.0*a1/a0 : 0.0,b0>0 ? 100.0*b1/b0 : 0.0);
    ProbeLog(line);
  }
  if(const uint32_t full=world_in_full.exchange(0)) { std::snprintf(line,sizeof(line),"World (member): %u messages from the host dropped (the game thread fell behind)",full); ProbeLog(line); }
  if(world_actions) WorldActionsMemberLog(seconds);
  world_m=WorldMemberCounts{};
}

// A session's start and end (review A3-2): the area, its epoch, declines,
// tombs, early records and the digest start over; copies still here fade
// and go as they are (WorldMemberTick).
void WorldMemberReset() {
  world_area=WorldArea{}; world_wait_until=0; world_rejoin_wait=false; world_rejoin_since=0;
  world_queue.clear(); world_declined.clear(); world_report.clear(); world_tomb.clear(); world_early.clear(); world_digest=cw::DigestParts{};
  world_match=WorldMatch{}; world_admit_due=false; world_band_near=world_band_far=WorldBand{};
  for(auto& [h,at]:world_drive) world_drive_slot[h&0xFFF].store(0);
  world_drive.clear(); world_drive_n=0; world_prop_boom.clear();
  for(auto it=world_copies.begin();it!=world_copies.end();) it=it->second.state==kCopyQueued ? world_copies.erase(it) : std::next(it);   // (never made: nothing to fade)
}
// Game thread, every update on a client (and after the session while copies are left).
void WorldMemberTick(GuestScratch& scratch,uint32_t player,uint64_t now) {
  const bool live=connected.load() && running.load() && !hosting.load() && my_slot.load()<kSlots && world_stream;
  if(live && !world_member_live) { WorldMemberReset(); world_wait_until=now+3000; }
  world_member_live=live;
  std::vector<WorldIn> in;
  { std::lock_guard lock(world_mutex); in.swap(world_in); }
  if(!live) in.clear();
  for(const WorldIn& m:in) {
    if(world_area.in && (m.type==kMsgWorld || m.type==kMsgWorldCreate)) world_area.map_at=now;   // (the stream comes: the area holds, review A3-1)
    switch(m.type) {
      case kMsgWorldCreate: WorldOnCreate(scratch,m,now); break;
      case kMsgWorldRemove: WorldOnRemove(scratch,m,now); break;
      case kMsgWorld: WorldOnUpdate(m,now); break;
      case kMsgWorldDigest: WorldOnDigest(scratch,m,now); break;
      case kMsgOwnerMap: WorldOnMap(scratch,m,now); break;
      default: break;
    }
  }
  if(world_area.in && (!live || !WorldMode() || !WorldHostHold(world_area.map_at,now))) {
    WorldLeaveArea(scratch,now,!live ? "session over" : "nothing from the host's world stream for 3 s (10 s while its state packets come, as long as it is paused)");
    if(live) { world_rejoin_wait=true; world_rejoin_since=now; }   // (the host must see this game out before it is in again: review A3-1)
  }
  if(world_rejoin_wait && now-world_rejoin_since>15000) world_rejoin_wait=false;   // (safety: the host never showed it out)
  for(auto it=world_early.begin();it!=world_early.end();) if(now-it->second.at>1000) { ++world_m.early_expired; it=world_early.erase(it); } else ++it;
  for(auto it=world_tomb.begin();it!=world_tomb.end();) it=now-it->second.second>5000 ? world_tomb.erase(it) : std::next(it);
  // Creates, 4 an update at most (spec 3.4/20), after 2.1's copies are gone (a member that just joined the area).
  const bool old_copies=(!shared_cars.empty() || !shared_peds.empty()) && now-world_area.joined_at<3000;
  unsigned made=0;
  for(auto it=world_queue.begin();it!=world_queue.end() && made<4 && !old_copies;) {
    const auto f=world_copies.find(*it);
    if(f==world_copies.end() || f->second.state!=kCopyQueued) { it=world_queue.erase(it); continue; }
    WorldCopy& c=f->second;
    const bool car=cw::IsCar(c.kind);
    if(!car && c.car) {
      const WorldCopy* cc=WorldFind(c.owner,c.car);
      if(!cc || cc->state==kCopyFading) { WorldDecline(f->first,c.kind,kDeclineCrew,c.pos,now); world_copies.erase(f); it=world_queue.erase(it); continue; }
      if(cc->state!=kCopyMade) { if(!c.held_logged) { c.held_logged=true; ++world_m.held_for_car; } ++it; continue; }
    }
    if(!car && now<actors_blocked_until) { ++it; continue; }
    // The live pools: made when there is room (fading copies hold theirs until they go); declined after 3 s of waiting.
    if((car ? WorldCarRoom() : WorldPeopleRoom())<=0) {
      if(now-c.queued_at>3000) { WorldDecline(f->first,c.kind,kDeclinePool,c.pos,now); world_copies.erase(f); it=world_queue.erase(it); continue; }
      ++it; continue;
    }
    if(car && WorldAdoptCar(c,now)) { it=world_queue.erase(it); continue; }   // (a player's pose made it meanwhile)
    ++made;
    if(WorldMake(scratch,c,now,player)) {
      ++world_m.creates;
      static bool first=true;
      if(first) {
        first=false;
        char line[200]; std::snprintf(line,sizeof(line),"World (member): the first create applied: %s %08X as %08X (kind %u, model %u%s%s)",car ? "car" : "person",c.id,c.local,unsigned(c.kind),unsigned(c.model),c.name.empty() ? "" : ", ",c.name.c_str());
        ProbeLog(line);
      }
      it=world_queue.erase(it); continue;
    }
    if(++c.tries>=3) { WorldDecline(f->first,c.kind,kDeclineFailed,c.pos,now); world_copies.erase(f); it=world_queue.erase(it); continue; }
    ++it;
  }
  // Every copy: applied, lost, or (fading) destroyed at its deadline, 4 an update (1.63: many removals in one update froze the game).
  static uint64_t last=0;
  const uint64_t step=last && now>last ? std::min<uint64_t>(now-last,200) : 0;
  last=now;
  unsigned destroys=0;
  while(!world_graveyard.empty() && destroys<4) { WorldDestroy(scratch,world_graveyard.back()); world_graveyard.pop_back(); ++destroys; }
  std::vector<cw::Key> gone;
  for(auto& [k,c]:world_copies) {
    if(c.state==kCopyQueued) continue;
    if(c.state==kCopyFading) {
      if(!c.local) { gone.push_back(k); continue; }
      if(now<c.deadline) continue;
      if(now>c.deadline+1000 && !c.past_logged) { c.past_logged=true; ++world_m.past_deadline; }
      if(destroys>=4) continue;
      ++destroys;
      WorldDestroy(scratch,c);
      gone.push_back(k);
      continue;
    }
    const bool car=cw::IsCar(c.kind);
    const uint32_t obj=car ? ResolveVehicle(c.local) : ResolveHuman(c.local);
    bool ok=obj!=0;
    if(ok && !live) { WorldFade(scratch,c,cw::kDefaultFadeMs,now); continue; }
    if(ok && car) { WorldApplyCar(scratch,c,obj,player,now); world_m.car_ms+=step; }
    else if(ok) ok=WorldApplyPerson(scratch,c,obj,player,now);
    if(!ok) {
      // Removed here by the game (its region or chunk checks; a copy whose model never loaded): told to the host, which offers it again.
      ++world_m.gone[car ? 0 : 1];
      WorldDecline(k,c.kind,c.local ? kDeclineGone : kDeclineModel,c.pos,now);
      if(c.local) WorldIndex(c,0);
      gone.push_back(k);
    }
  }
  for(cw::Key k:gone) world_copies.erase(k);
  { PERF_SECTION("world fire"); if(live) WorldCopyFire(scratch,now); }   // (Package C: the host's people's rounds on their copies)
  { static uint64_t next_prune=0;
    if(now>=next_prune) {
      next_prune=now+1000;
      for(auto it=world_drive.begin();it!=world_drive.end();) {
        if(now-it->second<=1000) { ++it; continue; }
        uint64_t s=world_drive_slot[it->first&0xFFF].load();
        if(uint32_t(s>>32)==it->first) world_drive_slot[it->first&0xFFF].compare_exchange_strong(s,0);
        it=world_drive.erase(it);
      }
      world_drive_n=uint32_t(world_drive.size());
      for(auto it=world_prop_boom.begin();it!=world_prop_boom.end();) it=now-it->second.at>120000 ? world_prop_boom.erase(it) : std::next(it);
    } }
  // The pass line's counts (Package B), once a second while in the area.
  static uint64_t next_band=0;
  if(live && world_area.in && now>=next_band) {
    next_band=now+1000;
    float h[3];
    if(RemotePos(0,h)) {
      const float dx=h[0]-local_x.load(), dz=h[2]-local_z.load(), d=std::sqrt(dx*dx+dz*dz);
      WorldBand* band=d<40.0f ? &world_band_near : d>=100.0f && d<150.0f ? &world_band_far : nullptr;
      if(band) {
        float eye[3]; WorldEye(eye);
        unsigned people=0, cars=0;
        for(auto& [k,c]:world_copies) {
          if(c.state!=kCopyMade || !c.local) continue;
          if(cw::IsCar(c.kind)) { if(const uint32_t v=ResolveVehicle(c.local)) { float p[3]; for(int i=0;i<3;++i) p[i]=api->read_f32(v+20+i*4); cars+=WorldDist(p,eye)<=100.0f; } }
          else if(const uint32_t o=ResolveHuman(c.local)) {
            if(!(api->read_u32(o+216)&0x4000) || (api->read_u32(o+120)&0x08000000u)) continue;
            float p[3]; for(int i=0;i<3;++i) p[i]=api->read_f32(o+20+i*4); people+=WorldDist(p,eye)<=50.0f;
          }
        }
        band->people+=people; band->cars+=cars; ++band->seconds;
      }
    }
  }
  // WWA1: once a second, and 200 ms after a decline.
  static uint64_t next_admit=0, last_admit=0;
  if(live && world_area.in && (now>=next_admit || (world_admit_due && now-last_admit>=200))) { next_admit=now+1000; last_admit=now; world_admit_due=false; WorldSendAdmit(now); }
  static uint64_t next_log=0;
  if(now>=next_log) {
    const bool first=!next_log;
    next_log=now+10000;
    if(!first && (live || !world_copies.empty()) && (world_area.have || world_m.offered[0] || world_m.offered[1])) WorldMemberLog(now);
  }
}

// ---- The shared area mirrored (co-op 2.2, step 4: the cut-over) ----
// docs/COOP_V2.md section 11. The host decides who is in its area from the
// poses (WorldOwnerTick); 2.1's traffic and people streams (CaptureTraffic /
// CapturePeds and their packets) go only to the clients that don't show the
// world stream (mod.ini world = 0 there, or no OwnerMap seen yet; kept
// switchable for the test), and none are captured when no client needs them.
// A member's levers of spec 3.8/36 that A adds: the hood and special spawns
// (824290D0) and new trains (0x827ACF87) off, and on the host no new trains
// while it has members (3.8/37).

// Host, every update: the clients 2.1's streams still go to.
void WorldLegacyStreams(uint64_t now) {
  uint16_t mask=0;
  for(int s=1;s<kSlots;++s) {
    if(!Present(s)) continue;
    bool world=false;
    if(world_stream) {
      { std::lock_guard lock(pose_mutex); const RemoteIn& ri=remote_in[s]; world=ri.pose.sequence && (ri.pose_at>now || now-ri.pose_at<3000) && (ri.pose.flags&kPoseWorldStream); }
      if(!world && world_near_in[s].at && now-world_near_in[s].at<3000) world=true;   // (its WWA1)
    }
    if(!world) mask|=uint16_t(1u<<s);
  }
  const uint16_t was=legacy_streams.exchange(mask);
  if(was==mask) return;
  static unsigned logged=0;
  for(int s=1;s<kSlots && logged<40;++s) if(((was^mask)>>s&1) && Present(s)) {
    ++logged;
    char line[200];
    std::snprintf(line,sizeof(line),"World (owner): 2.1's traffic and people streams to slot %d %s",s,
                  (mask>>s&1) ? (world_stream ? "on (it doesn't show the world stream: mod.ini world = 0 there, or no OwnerMap yet)" : "on (mod.ini world = 0 here)") : "off (it shows the world stream)");
    ProbeLog(line);
  }
}
// UpdateAvatar: another player's character is kept to 130 m (its clothing
// unloads further out); a member of the host's area (the host: a member; a
// member: the host's) to member_leave_m + 30 m.
float AvatarKeepRadius(int slot) {
  if(hosting.load() ? (slot>=0 && slot<kSlots && world_members[slot].in_area) : (slot==0 && WorldShown())) return float(world_leave_m+30);
  return 130.0f;
}
// Trains (spec 3.8/36-37, review C1): the train manager 8242EFB0 starts one
// (8242E3D0) only while spawn_trains 0x827ACF87 is set (lbz +10407 off
// 0x827AA6E0). Trains aren't mirrored (source 9 left out), so none starts on a
// member while it shows the host's area, nor on the host while it has members;
// the running ones finish their route. Put back as it was (a script's value
// written meanwhile wins).
int world_trains_saved=-1;
void WorldTrains(bool off) {
  if(off) {
    const uint8_t v=api->read_u8(0x827ACF87);
    if(world_trains_saved<0 || v) { if(world_trains_saved<0) ProbeLog("World: new trains off here (spawn_trains 0x827ACF87 = 0) while the area is shared"); world_trains_saved=v; }
    if(v) api->write_u8(0x827ACF87,0);
  } else if(world_trains_saved>=0) {
    api->write_u8(0x827ACF87,uint8_t(world_trains_saved)); world_trains_saved=-1;
    ProbeLog("World: trains back as they were");
  }
}
void WorldLeversOff() { WorldTrains(false); }   // (the session over: UpdateBody)
// 824290D0 (one caller, the world update 824131F0): the spawn-region manager,
// hood gang members and cops (82427548, kinds 8 / 1) and the special spawns.
// The game's multiplayer skips all of it on a client (its MP byte); a member
// of the host's area does too (the host's are shown). Its own enable bytes
// 0x83AE3A90 / 91 are left alone (mission scripts own them, review M1).
WmlGuestFunction original_hood_spawns=nullptr;
unsigned hood_skipped=0;
void HoodSpawnsHook(WmlContext* c,uint8_t* b) {
  if(connected.load() && !hosting.load() && WorldLeversOn()) { ++hood_skipped; return; }
  original_hood_spawns(c,b);
}
std::string SlotName(int slot);
// The vehicle table (review A2-12): cars go by their index in it (WWC1, the
// poses), so a game with a vehicles.xtbl mod shows other models. Every game
// says what it has when a player joins; a difference is logged and shown.
bool VehicleTable(uint32_t& rows,uint32_t& hash) {
  rows=api->read_u32(kVehicleInfoCount); hash=2166136261u;
  if(!rows || rows>1024 || !Readable(kVehicleInfo,rows*kVehicleInfoSize)) return false;
  for(uint32_t i=0;i<rows;++i) { hash^=api->read_u32(kVehicleInfo+i*kVehicleInfoSize+120); hash*=16777619u; }
  return true;
}
void WorldVehicleTableEvent(const Event& e) {
  static uint32_t seen[kSlots]{};   // per slot: the hash taken (0: none yet)
  if(e.origin<kSlots && e.a[4]) {   // (2.2 Package C: 2 its world_actions on, 1 off)
    const bool on=e.a[4]==2;
    if(world_actions_of[e.origin]!=on) { char line[120]; std::snprintf(line,sizeof(line),"World actions: slot %u has mod.ini world_actions = %d: Package C %s with it",unsigned(e.origin),on ? 1 : 0,on && world_actions ? "on" : "off"); ProbeLog(line); }
    world_actions_of[e.origin]=on;
  }
  uint32_t rows=0, hash=0;
  if(e.origin>=kSlots || !VehicleTable(rows,hash) || seen[e.origin]==(e.a[1]|1)) return;
  seen[e.origin]=e.a[1]|1;
  const bool same=rows==e.a[0] && hash==e.a[1];
  char line[300];
  std::snprintf(line,sizeof(line),"Vehicle table: slot %u has %u rows (hash %08X), this game %u (%08X): %s",unsigned(e.origin),e.a[0],e.a[1],rows,hash,
                same ? "the same" : "DIFFERENT - cars of the other game show as other models here (a vehicles.xtbl mod on one side)");
  ProbeLog(line);
  // 2.2 Package C: explosions go by their def index, guns by their weapon index.
  const uint32_t defs=api->read_u32(kExplosionDefCount), guns=api->read_u32(kWeaponCount);
  if(e.a[2] || e.a[3]) {
    std::snprintf(line,sizeof(line),"Explosion defs / weapons: slot %u has %u / %u, this game %u / %u: %s",unsigned(e.origin),e.a[2],e.a[3],defs,guns,
                  e.a[2]==defs && e.a[3]==guns ? "the same" : "DIFFERENT - the other game's explosions and guns show as others here");
    ProbeLog(line);
  }
  if(!same) Notify("Co-op: "+SlotName(e.origin)+"'s vehicle table differs from yours (a vehicles mod?): their cars may show as other models.");
}
void WorldSendVehicleTable() {   // (every update while connected: when the player count goes up)
  static int last=0;
  const int n=player_count.load();
  if(n<=last) { last=n; return; }
  last=n;
  uint32_t rows=0, hash=0;
  if(!VehicleTable(rows,hash)) return;
  QueueEvent(Event{kEventVehicleTable,{rows,hash,api->read_u32(kExplosionDefCount),api->read_u32(kWeaponCount),world_actions ? 2u : 1u},0});
}

// ---- The shared area: content around every member (co-op 2.2, Package B) ----
// docs/COOP_V2.md section 11. The engine fills the world only around the
// viewpoint its spawners read: parked cars 80 m (2D) from the player
// *0x8309ABEC, traffic 130-140 m and people 90 m from the camera 0x827D9778
// (research S 0.2). So on the host each spawner also runs for one member's
// viewpoint at a time (notes/coop-2.2/plan_B.md), its own radii unchanged and
// its caps shared by the area (one street of 12 cars, 26 people, 6 parked):
//  - parked: after the game's 8241D930 (the queue's spawns) the hook runs
//    8241D930 and 8241DC78(0) (the 6 nearest candidates within 80 m, 8241DAE0)
//    once more with *0x8309ABEC on a stand-in holding the member's position
//    (+20/+24/+28: all that 8241D930 and 8241DAE0 read of it; everything
//    else in their 61-function closure reads no player or camera), the real
//    player back around 8241E378 (the spawn) and 8241D210 (state changes:
//    model references, the car's destroy). 8241D930's "beyond 80 m" re-states
//    (8241D210 with LR 8241DA14 / 8241DA28, states 0 / 1) are vetoed for a
//    record within 80 m of any viewpoint, and the removal collector 8241D6B0's
//    array (0x8309A444, {record, d2} x 6, count 0x8309A474: the only input of
//    8241D5C0, which makes room) loses the records near a member;
//  - traffic: 82412298(out, &centre, &dir, f1 speed, r7 ahead), the spot
//    query of 824129C8 (one call, LR 82412F30; it reads no camera and the
//    player only for its highway flag), gets a member's eye (written to the
//    caller's free r1+80), forward (r5, the caller's copy at r1+96), speed and
//    "ahead" on a member's turn: the member furthest below its share of the
//    wanted count, the host's own at least every other tick;
//  - people: after 8240FBB0 (on the updates its own spawn tick runs: the
//    timer 0x82832744 not run out, *0x8370FEBC set) 8240F148(1, !moving)
//    with the camera's eye (+44) and forward row (+104) the member's: its
//    picker 8240EC88 and the spawn 8240DD40 read only those of the camera
//    (and the view's FOV); the constructor 82479AB0 (LR 8240E178) and the
//    set-up 820E9550 (LR 8240E1AC), whose closures reach the camera code, run
//    with the real pose. Never the make-room path for a member (8240E448);
//  - keep-alive: 8240FA90 (people, 90 m 2D of the camera) and 82412878 (cars,
//    130-140 m 3D) say "keep" for one within 90 m / 140 m of a member's eye;
//    8240E338 (make room's "removable") says no for a person near a member;
//    the slot allocator's evict functions 823FFA48 (people) / 823FFC48 (cars),
//    which take the record with the largest +24 (distance squared from the
//    host's player: a member's first), take an unprotected one, or none -
//    except for a priority-0 creation (mission, script, co-op copy: the
//    category table 0x82038858 +0), which is never refused;
//  - the wanted counts 824194A8 (traffic, <= 12) / 82419208 (people, <= 26)
//    are raised by member_density per member: the area's street is shared.
// mod.ini member_spawns (1 parked, 2 traffic, 4 people; 0 = keep-alive only)
// and member_density; every hook does nothing without members (single player
// and 2.1 unchanged).
const char kPerfBKeep[]="B keep";   // (one name for the keep-alive hooks' Perf part)
struct WorldMadeFor { unsigned parked=0, traffic=0, traffic_view=0, people=0, people_gone=0; };
std::array<WorldMadeFor,kSlots> world_made_for{};     // per member, 10 s
struct WorldBCounts {
  unsigned parked_passes=0, parked_vetoed=0, removals_kept=0, traffic_ticks=0, member_turns=0, turns_zero=0, people_passes=0, people_full=0;
  unsigned kept_people=0, kept_cars=0, make_room_spared=0, evictions_refused=0, evict_protected_taken=0, evict_covered=0;
  uint64_t member_spots=0;
} world_b;
int traffic_turn_slot=-1;          // the member whose turn the traffic spots of this update were (-1: the host's own)
int parked_pass_slot=-1;           // the member whose parked pass runs
uint32_t traffic_wanted=0, traffic_wanted_base=0, people_wanted=0, people_wanted_base=0;   // the last wanted counts (boosted, the game's)
float world_prev_eye[kSlots][3]{}; uint32_t world_prev_time[kSlots]{}; float world_prev_speed[kSlots]{};
bool WorldSpawnsOn() { return world_spawns_on && connected.load() && hosting.load(); }
inline float Dist2D(const float* a,const float* b) { const float dx=a[0]-b[0], dz=a[2]-b[2]; return std::sqrt(dx*dx+dz*dz); }
// Within r of a member's viewpoint (index >= 1): 2D on x/z or 3D, to its eye or its character.
bool NearMember(const float* p,float r,bool planar,bool eye) {
  for(int i=1;i<world_view_count;++i) {
    const float* q=eye ? world_views[i].eye : world_views[i].pos;
    if((planar ? Dist2D(p,q) : WorldDist(p,q))<=r) return true;
  }
  return false;
}
void ObjPos(uint32_t o,float* p) { for(int i=0;i<3;++i) p[i]=api->read_f32(o+20+i*4); }

// The viewpoints (WorldOwnerTick, 20 Hz): this game's camera and player,
// then every member whose pose is under 500 ms old. A member's forward speed
// from its car's velocity, else from its camera's moves between poses.
void WorldViewsRefresh(uint32_t player,uint64_t now) {
  Viewpoint& own=world_views[0];
  own.slot=world_slot; own.at=now;
  for(int i=0;i<3;++i) { own.eye[i]=api->read_f32(0x827D9778+44+i*4); own.fwd[i]=api->read_f32(0x827D9778+104+i*4); own.pos[i]=api->read_f32(player+20+i*4); }
  int n=1;
  for(int s=0;s<kSlots;++s) {
    if(s==world_slot || !world_members[s].in_area) continue;
    PlayerPose p; uint64_t at=0;
    { std::lock_guard lock(pose_mutex); p=remote_in[s].pose; at=remote_in[s].pose_at; }
    if(!p.sequence || (at<now && now-at>500)) continue;
    bool ok=true;
    for(int i=0;i<3;++i) ok=ok && std::isfinite(p.pos[i]) && std::isfinite(p.eye[i]) && std::abs(p.pos[i])<100000 && std::abs(p.eye[i])<100000;
    if(!ok) continue;
    Viewpoint& v=world_views[n];
    const bool same=v.slot==s;
    v.slot=s; v.at=now;
    std::memcpy(v.pos,p.pos,sizeof(v.pos));
    std::memcpy(v.eye,p.eye,sizeof(v.eye));
    if(!v.eye[0] && !v.eye[1] && !v.eye[2]) std::memcpy(v.eye,p.pos,sizeof(v.eye));
    std::memcpy(v.fwd,world_members[s].fwd,sizeof(v.fwd));
    if(!same) { v.next_parked=v.next_people=0; }
    float speed=world_prev_speed[s];
    if(p.vehicle>=0) speed=p.vvel[0]*v.fwd[0]+p.vvel[1]*v.fwd[1]+p.vvel[2]*v.fwd[2];
    else if(p.time!=world_prev_time[s] && world_prev_time[s]) {
      const float dt=float(int32_t(p.time-world_prev_time[s]))/1000.0f;
      if(dt>0.0f && dt<=1.0f) speed=((v.eye[0]-world_prev_eye[s][0])*v.fwd[0]+(v.eye[1]-world_prev_eye[s][1])*v.fwd[1]+(v.eye[2]-world_prev_eye[s][2])*v.fwd[2])/dt;
    }
    if(p.time!=world_prev_time[s]) { std::memcpy(world_prev_eye[s],v.eye,sizeof(v.eye)); world_prev_time[s]=p.time; }
    v.speed=world_prev_speed[s]=std::isfinite(speed) ? std::clamp(speed,-100.0f,100.0f) : 0.0f;
    const cw::Member& m=world_members[s];
    v.full_cars=m.cars_cap!=0xFFFF && m.cars_held>=m.cars_cap;
    v.full_people=m.people_cap!=0xFFFF && m.people_held>=m.people_cap;
    ++n;
  }
  for(int i=n;i<kSlots;++i) world_views[i].slot=-1;
  world_view_count=n;
  const bool on=hosting.load() && connected.load() && world_on && world_stream && n>1 && !CutscenePlaying();
  if(on && !world_spawns_on) {
    static bool logged=false;
    if(!logged) {
      logged=true;
      char line[260];
      std::snprintf(line,sizeof(line),"World (owner) B: hooks on (parked/traffic/people = %u/%u/%u, member_density %.1f); traffic ring *0x82832724 = %.1f m, removal radii *0x83AD2640 / 44 = %.1f / %.1f m",
                    world_member_spawns&1,(world_member_spawns>>1)&1,(world_member_spawns>>2)&1,world_member_density,api->read_f32(0x82832724),api->read_f32(0x83AD2640),api->read_f32(0x83AD2644));
      ProbeLog(line);
    }
  }
  world_spawns_on=on;
}
void FirstPass(const char* kind,int slot) {
  static unsigned seen=0;
  const unsigned bit=kind[0]=='p' && kind[1]=='a' ? 1 : kind[0]=='t' ? 2 : 4;
  if(seen&bit) return;
  seen|=bit;
  char line[120]; std::snprintf(line,sizeof(line),"World (owner) B: the first %s pass for a member (slot %d)",kind,slot); ProbeLog(line);
}

// ---- Parked cars (B3)
// A parked record's position: its spot's (rec+4 -> +4) or its own (rec+8).
bool ParkedRecPos(uint32_t rec,float* p) {
  if(!Readable(rec,108)) return false;
  const uint32_t spot=api->read_u32(rec+4);
  const uint32_t at=spot ? spot+4 : rec+8;
  if(!Readable(at,12)) return false;
  for(int i=0;i<3;++i) p[i]=api->read_f32(at+i*4);
  return true;
}
bool ParkedNearAnyViewpoint(uint32_t rec,int* slot) {
  float p[3];
  if(!ParkedRecPos(rec,p)) return false;
  for(int i=0;i<world_view_count;++i) if(Dist2D(p,world_views[i].pos)<=80.0f) { if(slot) *slot=world_views[i].slot; return true; }
  return false;
}
// One member per update, each at most every 200 ms, in turn.
Viewpoint* NextParkedView(uint64_t now) {
  static int rr=0;
  for(int k=1;k<world_view_count;++k) {
    const int i=1+(rr+k-1)%(world_view_count-1);
    if(now>=world_views[i].next_parked) { rr=i; return &world_views[i]; }
  }
  return nullptr;
}
WmlGuestFunction original_parked_pass=nullptr, original_parked_state=nullptr, original_parked_removals=nullptr;
// 8241D930() (one caller, 824131F0): the queue's spawns. The game's own pass
// first; then one member's (8241D930, then 8241DC78(0): new candidates
// around it) under the stand-in.
void ParkedPassHook(WmlContext* c,uint8_t* b) {
  if(parked_member_depth) { original_parked_pass(c,b); return; }
  traffic_turn_slot=-1;   // (824129C8 ran before in this update: its creations are over)
  original_parked_pass(c,b);
  if(!WorldSpawnsOn() || !(world_member_spawns&1)) return;
  const uint64_t now=MonoMs();
  Viewpoint* v=NextParkedView(now);
  if(!v) return;
  const uint32_t sp=uint32_t(api->get_r(c,1));
  if(sp<0x5000 || !Readable(sp-0x5000,0x5000)) return;
  PERF_SECTION("B parked");
  GuestScratch scratch(c);   // (its data() lies above the stack the calls below use: r1 - 0x2000)
  const uint32_t stand=scratch.data()+0x100;
  std::memset(api->guest_pointer(stand),0,0x60);   // (+68 handle 0, +72 type 0: anything that resolved it fails safe)
  for(int i=0;i<3;++i) api->write_f32(stand+20+4*i,v->pos[i]);
  parked_real_player=api->read_u32(0x8309ABEC);
  {
    PlayerSwap swap(stand); ++parked_member_depth; parked_pass_slot=v->slot;
    scratch.Call(0x8241D930);                          // the queue: cars within 80 m of the member
    scratch.ctx.r3.u64=0; scratch.Call(0x8241DC78);    // new candidates around it
    --parked_member_depth; parked_pass_slot=-1;
  }
  parked_real_player=0;
  v->next_parked=now+200; ++world_b.parked_passes;
  FirstPass("parked",v->slot);
}
// 8241D210(rec, state): 14 callers. 8241D930's "beyond 80 m" re-states (LR
// 8241DA14 / 8241DA28) are vetoed for a record within 80 m of any viewpoint
// (so one pass's dequeue doesn't undo another's); everything it does runs
// with the real player.
void ParkedStateHook(WmlContext* c,uint8_t* b) {
  const uint32_t lr=uint32_t(api->get_lr(c)), rec=uint32_t(api->get_r(c,3)), state=uint32_t(api->get_r(c,4));
  if((lr==0x8241DA14u || lr==0x8241DA28u) && state<=1 && WorldSpawnsOn()) {
    int slot=-1;
    if(ParkedNearAnyViewpoint(rec,&slot)) {
      ++world_b.parked_vetoed;
      static unsigned logged=0;
      if(logged++<10) { float p[3]{}; ParkedRecPos(rec,p); char line[160]; std::snprintf(line,sizeof(line),"World (owner) B: parked re-state vetoed for record %08X (spot %08X, %.0f %.0f) near slot %d",rec,Readable(rec,8) ? api->read_u32(rec+4) : 0u,p[0],p[2],slot); ProbeLog(line); }
      return;
    }
  }
  PlayerSwap unswap(parked_member_depth ? parked_real_player : 0);
  original_parked_state(c,b);
}
// 8241D6B0() (824131F0 and the reset 82413398): the records 8241D5C0 may
// make room from; those near a member are taken out (it makes room only from
// this array).
void ParkedRemovalsHook(WmlContext* c,uint8_t* b) {
  original_parked_removals(c,b);
  if(!WorldSpawnsOn()) return;
  constexpr uint32_t kArray=0x8309A444, kCount=0x8309A474;
  const uint32_t n=std::min<uint32_t>(api->read_u32(kCount),6);
  uint32_t kept=0;
  for(uint32_t i=0;i<n;++i) {
    const uint32_t rec=api->read_u32(kArray+i*8); const float d2=api->read_f32(kArray+i*8+4);
    float p[3];
    if(ParkedRecPos(rec,p) && NearMember(p,80.0f,true,false)) { ++world_b.removals_kept; continue; }
    api->write_u32(kArray+kept*8,rec); api->write_f32(kArray+kept*8+4,d2); ++kept;
  }
  if(kept!=n) api->write_u32(kCount,kept);
}

// ---- Traffic (B4)
// The traffic cars within 140 m of each viewpoint's eye (the list 0x8309A374, <= 12 nodes).
void TrafficNear(unsigned* nearby) {
  for(int i=0;i<world_view_count;++i) nearby[i]=0;
  const uint32_t head=api->read_u32(kTrafficList); uint32_t node=head;
  for(unsigned guard=0;node && guard<64 && Readable(node,12);++guard) {
    if(const uint32_t v=ResolveVehicle(api->read_u32(node+8))) { float p[3]; ObjPos(v,p); for(int i=0;i<world_view_count;++i) nearby[i]+=WorldDist(p,world_views[i].eye)<=140.0f; }
    node=api->read_u32(node); if(node==head) break;
  }
}
// Whose turn the spot query is: the member furthest below its share of the
// wanted count (not a full one), the host's own at least every other tick
// and on a tie.
const Viewpoint* TrafficTurn() {
  static bool owner_last=false;
  if(!owner_last) { owner_last=true; return nullptr; }
  unsigned nearby[kSlots]{}; TrafficNear(nearby);
  const int share=int((std::max<uint32_t>(traffic_wanted,1)+world_view_count-1)/world_view_count);
  int best=-1, best_def=0;
  for(int i=1;i<world_view_count;++i) { const int def=share-int(nearby[i]); if(!world_views[i].full_cars && def>best_def) { best_def=def; best=i; } }
  if(best<0 || share-int(nearby[0])>=best_def) return nullptr;
  owner_last=false;
  return &world_views[best];
}
WmlGuestFunction original_traffic_spots=nullptr, original_traffic_despawn=nullptr, original_traffic_wanted=nullptr;
// 82412298(out[32 x 16 B], &centre, &dir, f1 speed, r7 ahead) -> spot count (one caller, 824129C8, LR 82412F30).
void TrafficSpotsHook(WmlContext* c,uint8_t* b) {
  ++world_b.traffic_ticks;
  const Viewpoint* v=api->get_lr(c)==0x82412F30u && WorldSpawnsOn() && share_traffic && (world_member_spawns&2) ? TrafficTurn() : nullptr;
  traffic_turn_slot=-1;
  if(!v) { original_traffic_spots(c,b); return; }
  // The caller's frame (824129C8, 768 B): r1+80..+91 is free from here on (its
  // despawn loop's byte is done); r5 is its copy of the camera's forward at
  // r1+96, rewritten by its spot loop right after: both take the member's.
  const uint32_t sp=uint32_t(api->get_r(c,1)), centre=sp+80, dir=uint32_t(api->get_r(c,5));
  if(!Readable(centre,12) || dir!=sp+96) { original_traffic_spots(c,b); return; }
  for(int i=0;i<3;++i) { api->write_f32(centre+4*i,v->eye[i]); api->write_f32(dir+4*i,v->fwd[i]); }
  api->set_r(c,4,centre); api->set_f(c,1,v->speed); api->set_r(c,7,v->speed>2.2352f ? 1 : 0);
  traffic_turn_slot=v->slot;
  original_traffic_spots(c,b);
  const uint32_t spots=uint32_t(api->get_r(c,3));
  world_b.member_spots+=spots; ++world_b.member_turns; if(!spots) ++world_b.turns_zero;
  FirstPass("traffic",v->slot);
}
// 82412878(&visible, car, &camera+44, far, &dir or 0) -> 1 remove (824129C8's despawn loop, LR 82412C9C).
void TrafficDespawnHook(WmlContext* c,uint8_t* b) {
  const uint32_t car=uint32_t(api->get_r(c,4));
  original_traffic_despawn(c,b);
  if(!(api->get_r(c,3)&0xFF) || !WorldSpawnsOn() || !Readable(car,32)) return;
  PERF_SECTION(kPerfBKeep);
  float p[3]; ObjPos(car,p);
  if(NearMember(p,140.0f,false,true)) { api->set_r(c,3,0); ++world_b.kept_cars; }
}
// 824194A8() -> traffic wanted, 82419208() -> people wanted: raised for the members (capped at the kinds' limits).
uint32_t BoostWanted(uint32_t base,uint32_t cap) {
  if(!base || !WorldSpawnsOn() || world_member_density<=0.0f) return base;
  const float w=float(base)*(1.0f+world_member_density*float(world_view_count-1));
  return std::min<uint32_t>(cap,uint32_t(std::lround(w)));
}
void TrafficWantedHook(WmlContext* c,uint8_t* b) {
  original_traffic_wanted(c,b);
  traffic_wanted_base=uint32_t(api->get_r(c,3));
  traffic_wanted=BoostWanted(traffic_wanted_base,12);
  if(traffic_wanted!=traffic_wanted_base) api->set_r(c,3,traffic_wanted);
}
// What a car the game makes was made for (VehicleCreateHook): a member's parked pass (source 5) or its traffic turn (source 2).
void WorldMadeForCar(uint32_t handle,uint32_t source) {
  if(!world_spawns_on || !handle) return;
  const Viewpoint* view=nullptr;
  if(source==5 && parked_member_depth) { if(parked_pass_slot>=0) ++world_made_for[parked_pass_slot].parked; return; }
  if(source==2 && traffic_turn_slot>=0) {
    const int slot=traffic_turn_slot;
    for(int i=1;i<world_view_count;++i) if(world_views[i].slot==slot) view=&world_views[i];
    ++world_made_for[slot].traffic;
    if(view) if(const uint32_t v=ResolveVehicle(handle)) {
      float p[3]; ObjPos(v,p);
      const float d=WorldDist(p,view->eye);
      if(d<140.0f && cw::InCone(view->eye,view->fwd,p,d)) ++world_made_for[slot].traffic_view;
    }
  }
}

// ---- People (B5)
// The camera's pose is a member's for one game call: +44..+52 eye and
// +104..+112 forward of 0x827D9778 (all that the people pickers read of it),
// back on every path, never across an update.
struct CameraPoseSwap {
  const bool on; float was[6]{};
  CameraPoseSwap(const float* eye,const float* fwd) : on(eye!=nullptr) {
    if(!on) return;
    for(int i=0;i<3;++i) { was[i]=api->read_f32(0x827D9778+44+4*i); was[3+i]=api->read_f32(0x827D9778+104+4*i); }
    for(int i=0;i<3;++i) { api->write_f32(0x827D9778+44+4*i,eye[i]); api->write_f32(0x827D9778+104+4*i,fwd[i]); }
  }
  ~CameraPoseSwap() { if(on) for(int i=0;i<3;++i) { api->write_f32(0x827D9778+44+4*i,was[i]); api->write_f32(0x827D9778+104+4*i,was[3+i]); } }
  CameraPoseSwap(const CameraPoseSwap&)=delete; CameraPoseSwap& operator=(const CameraPoseSwap&)=delete;
};
thread_local const CameraPoseSwap* people_pass=nullptr;   // the member pass running (its was[] = the real pose)
int people_pass_slot=-1;
// 8240FBB0's own spawn tick (8240FCCC-8240FD48): *0x8370FEBC set and the
// shared timer 0x82832744 not run out against the clock *0x827AA6E0 (a
// negative timer counts as not run out; 900000000 ms of wrap).
bool PeopleSpawnTick() {
  if(!api->read_u32(0x8370FEBC)) return false;
  const int32_t t=int32_t(api->read_u32(0x82832744));
  if(t<0) return true;
  const int32_t clock=int32_t(api->read_u32(0x827AA6E0));
  return t>clock ? t-clock<900000000 : clock-t>=900000000;
}
// Ambient people within 90 m (2D) of each viewpoint's eye (the list 0x8309ABB4, +3600).
void PeopleNear(unsigned* nearby) {
  for(int i=0;i<world_view_count;++i) nearby[i]=0;
  const uint32_t head=api->read_u32(kAmbientPeds); uint32_t o=head;
  for(unsigned guard=0;o && guard<128 && Readable(o,3604);++guard) {
    float p[3]; ObjPos(o,p);
    for(int i=0;i<world_view_count;++i) nearby[i]+=Dist2D(p,world_views[i].eye)<=90.0f;
    o=api->read_u32(o+3600); if(o==head) break;
  }
}
// A member below its share of the people wanted (not full), each at most every 250 ms, 10 passes a second in all.
Viewpoint* NextPeopleView(uint64_t now) {
  static uint64_t next_any=0;
  if(now<next_any) return nullptr;
  unsigned nearby[kSlots]{}; PeopleNear(nearby);
  const int share=int((std::max<uint32_t>(people_wanted,1)+world_view_count-1)/world_view_count);
  Viewpoint* best=nullptr; int best_def=0;
  for(int i=1;i<world_view_count;++i) {
    Viewpoint& v=world_views[i];
    const int def=share-int(nearby[i]);
    if(!v.full_people && now>=v.next_people && def>best_def) { best_def=def; best=&v; }
  }
  if(best) next_any=now+100;
  return best;
}
WmlGuestFunction original_people_update=nullptr, original_people_despawn=nullptr, original_people_wanted=nullptr, original_make_room_test=nullptr,
                 original_human_create=nullptr, original_ped_setup=nullptr;
// 8240FBB0() (one caller, 824131F0): the people's despawn and spawn pass.
void PeopleUpdateHook(WmlContext* c,uint8_t* b) {
  const bool tick=WorldSpawnsOn() && share_peds && (world_member_spawns&4) && PeopleSpawnTick();   // (read before: the pass may re-arm it)
  original_people_update(c,b);
  if(!tick) return;
  const uint64_t now=MonoMs();
  Viewpoint* v=NextPeopleView(now);
  if(!v) return;
  const uint32_t sp=uint32_t(api->get_r(c,1));
  if(sp<0x5000 || !Readable(sp-0x5000,0x5000)) return;
  PERF_SECTION("B people");
  GuestScratch scratch(c);
  scratch.Call(0x82419208); const int32_t wanted=int32_t(scratch.ctx.r3.u32);
  scratch.Call(0x8240DB70); const int32_t have=int32_t(scratch.ctx.r3.u32);
  v->next_people=now+250;
  if(have>=wanted || api->read_u32(0x8309ABAC)>=75) { ++world_b.people_full; return; }   // (never the make-room path for a member)
  {
    CameraPoseSwap cam(v->eye,v->fwd); people_pass=&cam; people_pass_slot=v->slot;
    scratch.ctx.r3.u64=1; scratch.ctx.r4.u64=v->speed>2.2352f ? 0 : 1; scratch.Call(0x8240F148);
    people_pass=nullptr; people_pass_slot=-1;
  }
  ++world_b.people_passes;
  FirstPass("people",v->slot);
}
// The constructor 82479AB0 (11 callers; the people spawn's at LR 8240E178)
// and the set-up 820E9550 (LR 8240E1AC) in a member pass: the real camera.
void PassCalls(const char* what,uint32_t lr,uint32_t want) {   // (which calls a member pass reaches: plan_B 10)
  static uint32_t seen[8]{}; static unsigned n=0;
  if(lr==want || n>=8 || std::find(seen,seen+n,lr)!=seen+n) return;
  seen[n++]=lr;
  char line[140]; std::snprintf(line,sizeof(line),"World (owner) B: %s reached from %08X in a member's people pass (not the spawn's own call)",what,lr); ProbeLog(line);
}
void HumanCreateHook(WmlContext* c,uint8_t* b) {
  const uint32_t lr=uint32_t(api->get_lr(c));
  if(!people_pass) { original_human_create(c,b); return; }
  PassCalls("82479AB0",lr,0x8240E178u);
  const int slot=people_pass_slot;
  { CameraPoseSwap unswap(lr==0x8240E178u ? people_pass->was : nullptr,people_pass->was+3); original_human_create(c,b); }
  if(lr!=0x8240E178u || slot<0) return;
  const uint32_t o=uint32_t(api->get_r(c,3));
  if(!o) return;
  // 8243EB60 (the region check, inside the constructor) queues a person made
  // in a chunk this game hasn't loaded for deletion (+120 0xC0000000).
  if(Readable(o,124) && (api->read_u32(o+120)&0xC0000000u)) {
    ++world_made_for[slot].people_gone;
    static unsigned logged=0;
    if(logged++<10) { char line[140]; std::snprintf(line,sizeof(line),"World (owner) B: people made for slot %d deleted at birth: no region here (chunk unloaded on this game)",slot); ProbeLog(line); }
  } else ++world_made_for[slot].people;
}
void PedSetupHook(WmlContext* c,uint8_t* b) {
  const uint32_t lr=uint32_t(api->get_lr(c));
  if(!people_pass) { original_ped_setup(c,b); return; }
  PassCalls("820E9550",lr,0x8240E1ACu);
  CameraPoseSwap unswap(lr==0x8240E1ACu ? people_pass->was : nullptr,people_pass->was+3);
  original_ped_setup(c,b);
}
// 8240FA90(&visible out, person) -> 1 beyond 90 m (2D) of the camera (8240FBB0's despawn loop).
void PeopleDespawnHook(WmlContext* c,uint8_t* b) {
  const uint32_t o=uint32_t(api->get_r(c,4));
  original_people_despawn(c,b);
  if(!(api->get_r(c,3)&0xFF) || !WorldSpawnsOn() || !Readable(o,32)) return;
  PERF_SECTION(kPerfBKeep);
  float p[3]; ObjPos(o,p);
  if(NearMember(p,90.0f,true,true)) { api->set_r(c,3,0); ++world_b.kept_people; }
}
// 8240E338(person) -> 1 removable (one caller, the make-room 8240E448): not in a member pass, never one near a member.
void MakeRoomTestHook(WmlContext* c,uint8_t* b) {
  const uint32_t o=uint32_t(api->get_r(c,3));
  original_make_room_test(c,b);
  if(!(api->get_r(c,3)&0xFF)) return;
  if(people_pass) { api->set_r(c,3,0); return; }
  if(!WorldSpawnsOn() || !Readable(o,32)) return;
  float p[3]; ObjPos(o,p);
  if(NearMember(p,90.0f,true,true)) { api->set_r(c,3,0); ++world_b.make_room_spared; }
}
void PeopleWantedHook(WmlContext* c,uint8_t* b) {
  original_people_wanted(c,b);
  people_wanted_base=uint32_t(api->get_r(c,3));
  people_wanted=BoostWanted(people_wanted_base,26);
  if(people_wanted!=people_wanted_base) api->set_r(c,3,people_wanted);
}

// ---- The slot allocator (B6)
// 823FFF78(sys, kind, level): a creation of `kind` found the pool full; it
// tries the categories of a lower priority (higher number) through their
// evict functions. 823FFA48 (people) / 823FFC48 (cars) (r3 = the category's
// lists, +0 people / +4 cars; r4 = the pass, 1: not a visible one) take the
// valid record ({+0 next, +12 handle, +20 state, +24 score}) with the largest
// score, the distance squared from the host's player. Records near a member
// get the lowest score for the call; with nothing else there the creation is
// refused, unless its priority is 0 (category table 0x82038858 + 28 x kind:
// a mission's, a script's, co-op's copies), which always gets its slot.
thread_local int evict_kind=-1;
WmlGuestFunction original_evict_request=nullptr, original_evict_people=nullptr, original_evict_cars=nullptr;
void EvictRequestHook(WmlContext* c,uint8_t* b) {
  const int outer=evict_kind; evict_kind=int(api->get_r(c,4)&0xFF);
  original_evict_request(c,b);
  evict_kind=outer;
}
void EvictCover(WmlContext* c,uint8_t* b,WmlGuestFunction original,bool cars) {
  if(!WorldSpawnsOn() || evict_kind<0 || evict_kind>=10) { original(c,b); return; }
  PERF_SECTION(kPerfBKeep);
  const uint32_t lists=uint32_t(api->get_r(c,3)); const bool pass=(api->get_r(c,4)&0xFF)!=0;
  const uint32_t head=Readable(lists,8) ? api->read_u32(lists+(cars ? 4 : 0)) : 0;
  std::vector<std::pair<uint32_t,float>> covered;   // record, its score
  bool other=false;
  uint32_t rec=head;
  for(unsigned guard=0;rec && guard<256 && Readable(rec,28);++guard) {
    const uint32_t h=api->read_u32(rec+12), state=api->read_u32(rec+20);
    const uint32_t o=h && (h&0xFFFF)<4096 ? api->read_u32(kObjectTable+12+(h&0xFFFF)*16) : 0;
    const bool valid=o && Readable(o,232) && api->read_u32(o+68)==h && api->read_u32(o+72)==(cars ? 5u : 1u) && (cars || api->read_u32(o+224)==2) &&
                     !(api->read_u32(o+120)&0x80000000u) && state && !(pass && state==2);
    if(valid) {
      float p[3]; ObjPos(o,p);
      if(cars ? NearMember(p,140.0f,false,true) : NearMember(p,90.0f,true,true)) covered.push_back({rec,api->read_f32(rec+24)}); else other=true;
    }
    rec=api->read_u32(rec); if(rec==head) break;
  }
  if(covered.empty()) { original(c,b); return; }
  if(other) {
    for(auto& [r,score]:covered) api->write_f32(r+24,-1e30f);
    original(c,b);
    for(auto& [r,score]:covered) if(Readable(r,28)) api->write_f32(r+24,score);
    ++world_b.evict_covered;
    return;
  }
  if(api->read_u32(0x82038858+28*uint32_t(evict_kind))==0) { original(c,b); ++world_b.evict_protected_taken; return; }
  api->set_r(c,3,0); ++world_b.evictions_refused;
  static unsigned logged=0;
  if(logged++<10) { char line[140]; std::snprintf(line,sizeof(line),"World (owner) B: eviction refused for kind %d (category's %s: all near a member)",evict_kind,cars ? "cars" : "people"); ProbeLog(line); }
}
void EvictPeopleHook(WmlContext* c,uint8_t* b) { EvictCover(c,b,original_evict_people,false); }
void EvictCarsHook(WmlContext* c,uint8_t* b) { EvictCover(c,b,original_evict_cars,true); }

void InstallWorldSpawnHooks() {
  api->hook(0x8240FA90,PeopleDespawnHook,&original_people_despawn);
  api->hook(0x82412878,TrafficDespawnHook,&original_traffic_despawn);
  api->hook(0x823FFF78,EvictRequestHook,&original_evict_request);
  api->hook(0x823FFA48,EvictPeopleHook,&original_evict_people);
  api->hook(0x823FFC48,EvictCarsHook,&original_evict_cars);
  api->hook(0x8240E338,MakeRoomTestHook,&original_make_room_test);
  api->hook(0x8241D930,ParkedPassHook,&original_parked_pass);
  api->hook(0x8241D210,ParkedStateHook,&original_parked_state);
  api->hook(0x8241D6B0,ParkedRemovalsHook,&original_parked_removals);
  api->hook(0x82412298,TrafficSpotsHook,&original_traffic_spots);
  api->hook(0x8240FBB0,PeopleUpdateHook,&original_people_update);
  api->hook(0x82479AB0,HumanCreateHook,&original_human_create);
  api->hook(0x820E9550,PedSetupHook,&original_ped_setup);
  api->hook(0x824194A8,TrafficWantedHook,&original_traffic_wanted);
  api->hook(0x82419208,PeopleWantedHook,&original_people_wanted);
}

// The 10 s lines (plan_B 8): the owner's, and per member.
void WorldBLog(double secs) {
  (void)secs;
  unsigned n1=0, near_members=0;
  {
    const uint32_t head=api->read_u32(0x8309A47C); uint32_t rec=head;   // (state 1: the candidates)
    for(unsigned guard=0;rec && guard<4096 && Readable(rec,108);++guard) {
      ++n1; float p[3];
      if(ParkedRecPos(rec,p) && NearMember(p,80.0f,true,false)) ++near_members;
      rec=api->read_u32(rec+64); if(rec==head) break;
    }
  }
  unsigned made_parked=0, made_traffic=0, traffic_view=0, made_people=0, people_gone=0;
  for(const WorldMadeFor& m:world_made_for) { made_parked+=m.parked; made_traffic+=m.traffic; traffic_view+=m.traffic_view; made_people+=m.people; people_gone+=m.people_gone; }
  std::string slots;
  for(int i=1;i<world_view_count;++i) slots+=" "+std::to_string(world_views[i].slot);
  char line[900];
  const WorldBCounts& b=world_b;
  std::snprintf(line,sizeof(line),"World (owner) B: viewpoints %d (members %d: slots%s); wanted cars %u (boosted from %u) / people %u (from %u); parked: member passes %u (candidates near members %u, %u state-1 records), made for members %u, re-states vetoed %u, removals kept %u; "
                "traffic: turns %u of %u ticks (0 spots %u), made for members %u (in their view %u); people: member passes %u (full %u), made %u (deleted by the region check at birth %u); kept alive people %u / cars %u; make room spared %u; evictions refused %u (protected taken for a priority-0 creation %u, covered %u)",
                world_view_count,std::max(0,world_view_count-1),slots.empty() ? " none" : slots.c_str(),traffic_wanted,traffic_wanted_base,people_wanted,people_wanted_base,b.parked_passes,near_members,n1,made_parked,b.parked_vetoed,b.removals_kept,
                b.member_turns,b.traffic_ticks,b.turns_zero,made_traffic,traffic_view,b.people_passes,b.people_full,made_people,people_gone,b.kept_people,b.kept_cars,b.make_room_spared,b.evictions_refused,b.evict_protected_taken,b.evict_covered);
  ProbeLog(line);
  const uint64_t now=MonoMs();
  for(int s=0;s<kSlots;++s) {
    if(s==world_slot || !world_members[s].in_area) continue;
    const float* eye=world_members[s].eye;
    unsigned people=0, cars=0, people_s=0, cars_s=0;
    for(auto& [k,e]:world_reg.Entries()) {
      const bool car=cw::IsCar(e.kind);
      if(WorldDist(e.pos,eye)>(car ? 100.0f : 50.0f)) continue;
      ++(car ? cars : people);
      if(e.Streams()) ++(car ? cars_s : people_s);
    }
    const WorldMadeFor& m=world_made_for[s];
    std::snprintf(line,sizeof(line),"World (owner) B slot %d: %.0f m from this player (in since %.0f s); near its eye (people 50 m / cars 100 m): this game has %u / %u (registry; %u / %u streaming); made for it parked %u / traffic %u / people %u in 10 s",
                  s,world_member_d[s],world_in_since[s] ? (now-world_in_since[s])/1000.0 : 0.0,people,cars,people_s,cars_s,m.parked,m.traffic,m.people);
    ProbeLog(line);
  }
  world_b=WorldBCounts{}; world_made_for={};
}

// Game thread, every update while connected: the registry on and off, the
// audit, the owner's tick (the host), the 10 s lines.
// ---- Gunfire, the member ----
// A record's fire part into the copy's state. The owner's count is exact
// while records keep coming: a record without a fire part says the person
// hasn't fired for a second (the count can't have moved since), so the first
// round after quiet is played too; after a gap with no records at all (or
// the copy's first fire part) only from there on.
void WorldFireIn(WorldCopy& c,uint8_t weapon,const cw::FireExt* x,const float* ref,uint64_t now) {
  if(!WorldActionsWith(0)) return;
  CopyFire& f=c.fire;
  const bool gap=!f.last_record || now-f.last_record>1500;   // (no record of it for 1.5 s: what happened meanwhile is unknown)
  f.last_record=now;
  if(!(weapon&cw::kWeaponFire) || !x) { if(gap) f.seen=0xFF; f.quiet=true; return; }   // (quiet for a second; after a gap the count is unknown)
  const uint8_t count=x->fire&cw::kFireCount;
  unsigned n=0;
  if(gap) n=0;                                    // (after a gap, or the copy's first record: from here on)
  else if(f.seen==0xFF) n=f.quiet ? 1 : 0;        // (its first fire part after quiet records: at least this round)
  else n=unsigned((count-f.seen)&15);             // (records kept coming: the count is exact)
  f.seen=count; f.ext_at=now; f.quiet=false;
  f.blank=(x->fire&cw::kFireBlank)!=0; f.driveby=(x->fire&cw::kFireDriveBy)!=0; f.aim_ok=(x->fire&cw::kFireAim)!=0;
  if(f.aim_ok) cw::UnqPos(ref,x->aim,f.aim);
  world_am.counted+=n;
  const uint32_t want=weapon&cw::kWeaponGun, base=api->read_u32(kWeaponInfos), guns=api->read_u32(kWeaponCount);
  const bool burst=want && base && guns<=255 && want<=guns && api->read_u32(base+(want-1)*kWeaponInfoSize+96)!=0;
  auto push=[&](unsigned k) { for(unsigned i=0;i<k;++i) { if(f.shots.size()>=4) { f.shots.pop_front(); ++world_am.waited[kCopyFireOld]; } f.shots.push_back(now); } };
  if(!burst) push(std::min(n,4u));
  else { f.held=(x->fire&cw::kFireHeld)!=0; if(n && !f.held) push(std::min(n,2u)); }
}
// Why a round of a copy waits (or 0xFF: ready): not set up, lying or
// getting up, past 150 m of the camera, the gun (MatchWeapon), its ammo, the
// "run 180" turn stopped, raised first on foot (82449708).
uint8_t CopyFireGate(GuestScratch& scratch,WorldCopy& c,uint32_t obj,const float* eye,bool seated) {
  if(!(api->read_u32(obj+216)&0x4000)) return kCopyFireNotSetUp;
  if(api->read_u32(obj+780) || api->read_u32(obj+516)==8) return kCopyFireLying;
  float p[3]; for(int i=0;i<3;++i) p[i]=api->read_f32(obj+20+i*4);
  if(WorldDist(p,eye)>150.0f) return kCopyFireFar;
  const uint32_t want=c.weapon&cw::kWeaponGun;
  if(!want || !MatchWeapon(scratch,obj,want,c.gun,true)) return kCopyFireOtherGun;
  c.gun.hold_until=MonoMs()+1500;
  const uint32_t ammo=EquippedAmmo(obj);
  if(!ammo) return kCopyFireNoAmmo;
  // Never empty here: this game would reload or click on its own (R 5).
  if(int32_t(api->read_u32(ammo+16))<2) api->write_u32(ammo+16,2);
  if(int32_t(api->read_u32(ammo+20))<2) api->write_u32(ammo+20,2);
  if(seated) return 0xFF;
  auto is_turn=[](uint32_t a) { return a==335 || a==336; };
  if(is_turn(api->read_u32(obj+756))) { scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=1; scratch.ctx.f1.f64=0.1; ++replicated_depth; scratch.Call(0x82445778); --replicated_depth; }
  if(is_turn(api->read_u32(obj+764))) { scratch.ctx.r3.u64=obj; scratch.ctx.f1.f64=0.1; ++replicated_depth; scratch.Call(0x82445908); --replicated_depth; }
  if(is_turn(api->read_u32(obj+772))) { scratch.ctx.r3.u64=obj; scratch.ctx.f1.f64=0.1; ++replicated_depth; scratch.Call(0x824459A0); --replicated_depth; }
  if(!(api->read_u32(obj+216)&0x80000000u)) {
    scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=1; scratch.ctx.r5.u64=0;
    ++replicated_depth; scratch.Call(0x82449708); --replicated_depth;
    ++world_am.raises;
  }
  return 0xFF;
}
// One call of the copy's fire function as a replay (no damage, no car hit).
void CopyFireCall(GuestScratch& scratch,WorldCopy& c,uint32_t obj,uint32_t blank) {
  const uint32_t aim=scratch.data()+2440;
  if(c.fire.aim_ok) for(int i=0;i<3;++i) api->write_f32(aim+i*4,c.fire.aim[i]);
  scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=0; scratch.ctx.r5.u64=c.fire.aim_ok ? aim : 0; scratch.ctx.r6.u64=blank;
  ++fire_replay_depth; ++replicated_depth; ++copy_fire_replay_depth;
  scratch.Call(0x82470A50);
  --copy_fire_replay_depth; --replicated_depth; --fire_replay_depth;
  ++world_am.fire_calls;
  if(blank) ++world_am.blanks;
}
// The one-shot line: the first round of each reason not played.
void CopyFireLog(const WorldCopy& c,uint8_t why) {
  static uint32_t logged_reasons=0; static unsigned logged=0;
  if(logged>=20 || (logged_reasons>>why&1)) return;
  logged_reasons|=1u<<why; ++logged;
  const uint32_t want=c.weapon&cw::kWeaponGun, base=api->read_u32(kWeaponInfos);
  char name[32]="?";
  if(want && base) { const uint32_t info=base+(want-1)*kWeaponInfoSize; if(Readable(api->read_u32(info),16)) std::snprintf(name,sizeof(name),"%.31s",static_cast<const char*>(api->guest_pointer(api->read_u32(info)))); }
  char line[200];
  std::snprintf(line,sizeof(line),"World copy %08X (%s): round of %s not played: %s",c.id,c.name.c_str(),name,kCopyFireNames[why]);
  ProbeLog(line);
}
// Every update (member): the copies' rounds, trigger holds and releases,
// and the raised gun, 8 fire calls an update at most.
void WorldCopyFire(GuestScratch& scratch,uint64_t now) {
  if(!world_actions || world_copies.empty()) return;
  LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
  const unsigned calls0=world_am.fire_calls;
  float eye[3]; WorldEye(eye);
  for(auto& [k,c]:world_copies) {
    if(cw::IsCar(c.kind) || c.state!=kCopyMade || !c.local) continue;
    CopyFire& f=c.fire;
    const bool raise=(c.weapon&cw::kWeaponRaised) && !c.car;
    if(f.shots.empty() && !f.held && !f.holding && !raise) continue;
    const uint32_t obj=ResolveHuman(c.local);
    if(!obj || c.ped.dead) {
      if(obj && f.holding) {   // (its trigger let go, as below)
        ++replicated_depth; scratch.ctx.r3.u64=obj; scratch.Call(0x82474738); scratch.ctx.r3.u64=obj; scratch.Call(0x82476B70); --replicated_depth;
        api->write_u8(obj+3592,uint8_t(api->read_u8(obj+3592)&~0x40)); ++world_am.releases;
      }
      f.shots.clear(); f.held=f.holding=false; continue;
    }
    const bool seated=ResolveVehicle(api->read_u32(obj+2496))!=0;
    const uint32_t blank=(f.blank || (seated && api->read_u32(obj+516)!=3)) ? 1 : 0;   // (a seated copy: flash and sound only; drive-by mode on copies is for later)
    // Raised while the host's person has it up (the 2.5 s hold, renewed every 2 s).
    if(raise && now>=f.next_raise && HandWeapon(obj)>=0 && !api->read_u32(obj+780) && (api->read_u32(obj+216)&0x4000)) {
      f.next_raise=now+2000;
      scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=1; scratch.ctx.r5.u64=0;
      ++replicated_depth; scratch.Call(0x82449708); --replicated_depth;
      ++world_am.raises;
    }
    // Single rounds, in order; one older than 750 ms is dropped with its last wait.
    while(!f.shots.empty()) {
      if(now-f.shots.front()>750) { f.shots.pop_front(); ++world_am.waited[f.reason]; CopyFireLog(c,f.reason); continue; }
      if(world_am.fire_calls-calls0>=8) { f.reason=kCopyFireCap; break; }
      const uint8_t why=CopyFireGate(scratch,c,obj,eye,seated);
      if(why==kCopyFireFar) { world_am.waited[kCopyFireFar]+=unsigned(f.shots.size()); f.shots.clear(); break; }
      if(why!=0xFF) { f.reason=why; break; }
      const uint32_t ammo=EquippedAmmo(obj);
      api->write_u8(ammo+12,uint8_t(api->read_u8(ammo+12)&~0xA0));
      const uint32_t t=WeaponClock();
      api->write_u32(ammo+24,t); api->write_u32(ammo+40,0);
      const uint32_t rounds=api->read_u32(ammo+16);
      CopyFireCall(scratch,c,obj,blank);
      if(api->read_u32(obj+212)&0x00080000u) { api->write_u32(obj+212,api->read_u32(obj+212)&~0x00080000u); f.reason=kCopyFireRaising; break; }   // (deferred while it raised the gun: next update)
      if(api->read_u32(ammo+24)!=t || api->read_u32(ammo+16)!=rounds) { ++world_am.played; ++world_am.singles; f.shots.pop_front(); continue; }
      f.reason=kCopyFireRefused; break;
    }
    // An automatic gun: the trigger held while the host's person fires (a call when the refire timer is due), then the release.
    const bool held=f.held && now-f.ext_at<300;
    if(held) {
      if(world_am.fire_calls-calls0>=8) { ++world_am.waited[kCopyFireCap]; continue; }
      const uint8_t why=CopyFireGate(scratch,c,obj,eye,seated);
      if(why!=0xFF) { ++world_am.waited[why]; continue; }
      const uint32_t ammo=EquippedAmmo(obj);
      if(!f.holding) { f.holding=true; api->write_u32(ammo+24,WeaponClock()); api->write_u8(obj+3592,uint8_t(api->read_u8(obj+3592)|0x40)); }
      if(int32_t(WeaponClock()-api->read_u32(ammo+24))<0) continue;   // (not due: no call)
      api->write_u8(ammo+12,uint8_t(api->read_u8(ammo+12)&~0xA0));
      const uint32_t rounds=api->read_u32(ammo+16), t=api->read_u32(ammo+24);
      CopyFireCall(scratch,c,obj,blank);
      if(api->read_u32(obj+212)&0x00080000u) api->write_u32(obj+212,api->read_u32(obj+212)&~0x00080000u);
      if(api->read_u32(ammo+24)!=t || api->read_u32(ammo+16)!=rounds) { ++world_am.played; ++world_am.held_frames; }
    } else if(f.holding) {
      f.holding=false; f.held=false;
      if(EquippedAmmo(obj)) {
        ++replicated_depth;
        scratch.ctx.r3.u64=obj; scratch.Call(0x82474738);
        scratch.ctx.r3.u64=obj; scratch.Call(0x82476B70);
        --replicated_depth;
      }
      api->write_u8(obj+3592,uint8_t(api->read_u8(obj+3592)&~0x40));
      ++world_am.releases;
    } else f.held=false;   // (stale before it was ever held here)
  }
  LARGE_INTEGER t1, fq; QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
  if(world_am.fire_calls!=calls0) world_am.fire_ms+=double(t1.QuadPart-t0.QuadPart)*1000.0/double(fq.QuadPart);
}

// ---- Driver inputs, the member ----
// The copies' lights ask these: 82533DA8(handle) -> f1 the brake pedal (E+408;
// 0 without physics), 82535C00(vehicle) -> r3 reversing ((int8)E+180, 0
// without physics or for an info+284 == 6 car). A copy with the host's inputs
// from the last second answers the host's (a lock-free slot per handle).
WmlGuestFunction original_brake_getter=nullptr, original_reverse_getter=nullptr;
bool WorldDriveOf(uint32_t handle,uint8_t& bits) {
  const uint64_t s=handle ? world_drive_slot[handle&0xFFF].load(std::memory_order_relaxed) : 0;
  if(!s || uint32_t(s>>32)!=handle) return false;
  bits=uint8_t(s);
  return true;
}
void BrakeGetterHook(WmlContext* c,uint8_t* b) {
  uint8_t bits;
  if(world_drive_n.load(std::memory_order_relaxed) && WorldDriveOf(uint32_t(api->get_r(c,3)),bits)) { api->set_f(c,1,(bits&cw::kCarBrake) ? 1.0 : 0.0); ++world_brake_answers; return; }
  original_brake_getter(c,b);
}
void ReverseGetterHook(WmlContext* c,uint8_t* b) {
  const uint32_t v=uint32_t(api->get_r(c,3));
  uint8_t bits;
  if(world_drive_n.load(std::memory_order_relaxed) && v && api->read_u8(v+50300)==1 && WorldDriveOf(api->read_u32(v+68),bits)) { api->set_r(c,3,(bits&cw::kCarReverse) ? 1 : 0); ++world_reverse_answers; return; }
  original_reverse_getter(c,b);
}
// A car copy a player drives now (or gone): the getters answer the game's own again.
void WorldDriveClear(uint32_t local) {
  uint64_t s=world_drive_slot[local&0xFFF].load();
  if(local && uint32_t(s>>32)==local) world_drive_slot[local&0xFFF].compare_exchange_strong(s,0);
  if(world_drive.erase(local)) world_drive_n=uint32_t(world_drive.size());
}
// Every update for a car copy (not one a player drives): the inputs the
// getters answer with, the engine bit both lights need, the horn held, and
// E+412 so the physics steers as the host's car (E+412 + E+416).
void WorldDriveApply(GuestScratch& scratch,WorldCopy& c,uint32_t v,uint64_t now) {
  if(!WorldActionsWith(0) || !c.drive_at || now-c.drive_at>=1000) return;
  world_drive_slot[c.local&0xFFF].store(uint64_t(c.local)<<32|c.drive_bits);
  if(world_drive.insert_or_assign(c.local,now).second) world_drive_n=uint32_t(world_drive.size());
  const bool brake=(c.drive_bits&cw::kCarBrake)!=0, reverse=(c.drive_bits&cw::kCarReverse)!=0;
  if(!c.drive_steered) { world_am.brake+=brake; world_am.reverse+=reverse; }
  const uint32_t f=api->read_u32(v+53976);
  const bool engine=(c.drive&cw::kDriveEngine)!=0;
  if((brake || reverse) && engine!=((f&0x08000000u)!=0)) { api->write_u32(v+53976,engine ? f|0x08000000u : f&~0x08000000u); ++world_am.engine_writes; }
  if((c.drive&cw::kDriveHorn) && now-c.drive_at<250 && !CutscenePlaying()) { scratch.ctx.r3.u64=c.local; scratch.Call(0x825241F0); ++world_am.horn_frames; }
  if(c.drive_steered) return;   // (once per record)
  c.drive_steered=true;
  if(api->read_u8(v+50300)!=1) { ++world_am.steer_nophys; return; }
  const uint32_t E=api->read_u32(v+224);
  if(!Readable(E,484)) return;
  const float own=api->read_f32(E+416);
  if(!std::isfinite(own)) return;
  api->write_f32(E+412,std::clamp(cw::UnqSteer(c.drive)-own,-1.0f,1.0f));
  ++world_am.steer_writes; world_am.e416+=std::fabs(own); ++world_am.e416_n;
}

// ---- Burning, the member ----
// 8245E3A0(victim, -, attacker, flag, f1): sets a person alight. On a member
// a copy of the host's people burns only as the host's does: its own
// explosions (82458A50), fire patches (82142E18), spreading (8245ED40) and
// burning cars (82529838) here don't light one (r3 = 0: refused, as the game's
// own refusals return).
WmlGuestFunction original_ignite=nullptr;
void IgniteHook(WmlContext* c,uint8_t* b) {
  const uint32_t victim=uint32_t(api->get_r(c,3));
  if(!burn_replay_depth && connected.load() && !hosting.load() && WorldActionsWith(0) && victim && Readable(victim,72) && WorldPersonId(api->read_u32(victim+68),true)) {
    const uint32_t lr=uint32_t(api->get_lr(c));
    const uint8_t k=lr==0x82458EA8u ? kIgniteBlast : lr==0x82143080u ? kIgnitePatch : lr==0x8245F334u ? kIgniteSpread :
                    (lr==0x82529940u || lr==0x82529B28u || lr==0x82529C88u || lr==0x82529DC8u) ? kIgniteCar : kIgniteOther;
    ++world_am.ignites_refused[k];
    api->set_r(c,3,0); return;
  }
  original_ignite(c,b);
}
// The burn slot 8245E3A0 would free for a new one when all 4 (12 with the MP
// byte) are taken: a dead or gone one first, else the furthest from the
// camera (its on-screen test 82632A00 isn't repeated here); 0 while one is free.
uint32_t BurnSlotToGo() {
  const unsigned n=api->read_u8(0x8370E9F6) ? 12 : 4;
  uint32_t pick=0; float d_far=-1;
  float eye[3]; WorldEye(eye);
  for(unsigned i=0;i<n;++i) {
    const uint32_t h=api->read_u32(0x8309A9E8+i*24);
    if(!h) return 0;
    const uint32_t o=ResolveHuman(h);
    if(!o || PersonDead(o)) return h;
    float p[3]; for(int k=0;k<3;++k) p[k]=api->read_f32(o+20+k*4);
    const float d=WorldDist(p,eye);
    if(d>d_far) { d_far=d; pick=h; }
  }
  return pick;
}
// Per update for a copy on foot that is set up: the host's burning flag on it.
void WorldBurn(GuestScratch& scratch,WorldCopy& c,uint32_t obj,uint64_t now) {
  if(!WorldActionsWith(0)) return;
  const bool want=(c.ped.flags&cw::kPedBurning)!=0, burning=(api->read_u32(obj+216)&0x00400000u)!=0;
  if(want==burning) return;
  if(!want) {
    scratch.ctx.r3.u64=c.local; scratch.ctx.r4.u64=0;
    ++burn_replay_depth; ++replicated_depth; scratch.Call(0x8245E0B8); --replicated_depth; --burn_replay_depth;
    ++world_am.put_out; c.burn_lit=false;
    return;
  }
  if(now<c.next_burn) return;
  c.next_burn=now+1000;
  uint8_t why=0xFF;
  if(ResolveVehicle(api->read_u32(obj+2496))) why=kBurnCar;
  else if(const uint32_t out=BurnSlotToGo()) { const uint32_t player=api->read_u32(0x8309ABEC); if((Readable(player,72) && out==api->read_u32(player+68)) || IsAvatarHandle(out)) why=kBurnKeptOwn; }
  const uint32_t f120=api->read_u32(obj+120);
  if(why==0xFF) {
    if(f120&0xD0000000u) ++world_am.masked;
    api->write_u32(obj+120,f120&~0xD0000000u);
    scratch.ctx.r3.u64=obj; scratch.ctx.r4.u64=0; scratch.ctx.r5.u64=0; scratch.ctx.r6.u64=0; scratch.ctx.f1.f64=0.0;
    ++burn_replay_depth; ++replicated_depth; scratch.Call(0x8245E3A0); --replicated_depth; --burn_replay_depth;
    api->write_u32(obj+120,(api->read_u32(obj+120)&~0xD0000000u)|(f120&0xD0000000u));
    if(api->read_u32(obj+216)&0x00400000u) { ++world_am.lit; if(c.burn_lit) ++world_am.relit; c.burn_lit=true; return; }
    why=(api->read_u8(obj+3694)&0x80) ? kBurn3694 : PersonDead(obj) ? kBurnDead : (api->read_u8(obj+124)&0x80) ? kBurnWater : kBurnOther;
  }
  ++world_am.burn_refused[why];
  static unsigned logged=0;
  if(logged++<10) {
    char line[200];
    std::snprintf(line,sizeof(line),"World copy %08X not set alight: %s (+120 %08X, +216 %08X, +3694 %02X, +2496 %08X)",c.id,kBurnNames[why],f120,api->read_u32(obj+216),unsigned(api->read_u8(obj+3694)),api->read_u32(obj+2496));
    ProbeLog(line);
  }
}

void InstallWorldActionHooks() {
  api->hook(0x825241F0,HornHook,&original_horn);
  api->hook(0x82533DA8,BrakeGetterHook,&original_brake_getter);
  api->hook(0x82535C00,ReverseGetterHook,&original_reverse_getter);
  api->hook(0x8245E3A0,IgniteHook,&original_ignite);
}
// Member: an explosion of the host's world (kEventExplosion), played as the
// game plays one (82138AE8: def, source, attacker, rows, pos; the MP byte is
// 0 so r9 changes nothing) under world_boom_depth: this player takes its
// damage, knockdown and fire here; copies take none (DamageHook, IgniteHook,
// the car hooks); a prop it breaks here is noted.
void WorldExplosionEvent(GuestScratch& scratch,const Event& e,uint64_t now) {
  if(!WorldActionsWith(0) || !WorldShown()) return;
  const uint32_t index=e.a[0]&0xFFFF, kind=(e.a[0]>>16)&15, flags=e.a[0]>>24;
  if(index>=api->read_u32(kExplosionDefCount) || api->read_u32(kExplosionDefCount)>1024) { ++world_am.bad_def; return; }
  float pos[3], fwd[3];
  std::memcpy(pos,&e.a[1],12); UnpackDir(e.a[4],fwd);
  if(!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]) || std::abs(pos[0])>100000 || std::abs(pos[2])>100000) { ++world_am.bad_def; return; }
  // The attacker here: the host's character, another player's, a copy of the host's person; this player's own act is here already.
  uint32_t attacker=0;
  const uint32_t akind=(e.a[7]>>8)&0xFF, aslot=(e.a[7]>>16)&15;
  if(akind==1) attacker=AvatarObjOf(0);
  else if(akind==2) { if(aslot==my_slot.load()) { ++world_am.own_act; return; } attacker=AvatarObjOf(int(aslot)); }
  else if(akind==3) attacker=ResolveHuman(WorldPersonLocal(e.a[5]));
  uint32_t source=0;
  if(kind==kBoomProp) {
    if(flags&1) {
      const uint32_t uid=e.a[6];
      const auto it=world_prop_boom.find(uid);
      if(it!=world_prop_boom.end() && now-it->second.at<120000) { ++world_am.here_already; return; }
      world_prop_boom[uid]={now,true};
      for(uint32_t sub:{e.a[7]&0xFF,255u}) {
        scratch.ctx.r3.u64=uid; scratch.ctx.r4.u64=sub;
        if(scratch.Call(0x824995B0) && scratch.ctx.r3.u32) { source=scratch.ctx.r3.u32; break; }
      }
    }
    ++world_am.props; ++(source ? world_am.props_here : world_am.props_missing);
  } else if(kind==kBoomCarSecond) {
    source=ResolveVehicle(WorldCarLocal(e.a[5]));
    ++world_am.cars; if(!source) ++world_am.cars_missing;
  } else { ++world_am.bad_def; return; }
  if(source && source==api->read_u32(0x8309ABEC)) source=0;   // (never this player: the area pass skips the source)
  // Rows from the forward row and world up (+32 right, +44 up, +56 forward).
  float up[3]={0,1,0}, right[3]={up[1]*fwd[2]-up[2]*fwd[1],up[2]*fwd[0]-up[0]*fwd[2],up[0]*fwd[1]-up[1]*fwd[0]};
  float n=std::sqrt(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]);
  if(n<1e-3f) { right[0]=1; right[1]=0; right[2]=0; n=1; }
  for(float& v:right) v/=n;
  const float fl=std::sqrt(fwd[0]*fwd[0]+fwd[1]*fwd[1]+fwd[2]*fwd[2]);
  if(fl<1e-3f) { fwd[0]=0; fwd[1]=0; fwd[2]=1; } else for(float& v:fwd) v/=fl;
  up[0]=fwd[1]*right[2]-fwd[2]*right[1]; up[1]=fwd[2]*right[0]-fwd[0]*right[2]; up[2]=fwd[0]*right[1]-fwd[1]*right[0];
  const uint32_t d=(scratch.data()+2800+15)&~15u, rows=d+16;
  for(int i=0;i<3;++i) { api->write_f32(d+i*4,pos[i]); api->write_f32(rows+i*4,right[i]); api->write_f32(rows+12+i*4,up[i]); api->write_f32(rows+24+i*4,fwd[i]); }
  api->write_f32(d+12,0);
  const uint32_t sp=scratch.ctx.r1.u32;
  uint32_t word92=0;   // (8252B870 passes its car's info +24: the copy's own here)
  if(kind==kBoomCarSecond && source) { const uint32_t info=api->read_u32(source+50324); if(Readable(info,28)) word92=api->read_u32(info+24); }
  api->write_u32(sp+84,0); api->write_u32(sp+92,word92); api->write_u8(sp+103,uint8_t(flags&2 ? 1 : 0)); api->write_u8(sp+111,0);
  scratch.ctx.r3.u64=kExplosionDefs+index*kExplosionDefSize; scratch.ctx.r4.u64=source; scratch.ctx.r5.u64=attacker;
  scratch.ctx.r6.u64=rows; scratch.ctx.r7.u64=d; scratch.ctx.r8.u64=0; scratch.ctx.r9.u64=0; scratch.ctx.r10.u64=0;
  ++world_boom_depth; scratch.Call(0x82138AE8); --world_boom_depth;
  ++world_am.booms;
  static unsigned logged=0;
  if(logged++<10) {
    char line[200];
    std::snprintf(line,sizeof(line),"World explosion from the host: def %u (kind %u) at %.1f %.1f %.1f, source %s, attacker %s",index,kind,pos[0],pos[1],pos[2],
                  source ? "found" : "none",akind==1 ? "the host's character" : akind==2 ? "a player's character" : akind==3 ? (attacker ? "a copy of the host's person" : "a person not here") : "none");
    ProbeLog(line);
  }
}
// The member's 10 s lines (plan_C), with WorldMemberLog.
void WorldActionsMemberLog(double secs) {
  WorldActMember& m=world_am;
  char line[900];
  const unsigned* w=m.waited;
  std::snprintf(line,sizeof(line),"World (member) gunfire: rounds played %u (single %u, held frames %u, releases %u, blanks %u) of %u counted from the host; waited/dropped: not set up %u, lying %u, other gun %u, no ammo %u, raising %u, refused %u, too old %u, past 150 m %u, frame cap %u; "
                "guns given %u (carried %u, failed %u), raises %u; cost %.2f ms/s (%.0f us a call)",
                m.played,m.singles,m.held_frames,m.releases,m.blanks,m.counted,w[kCopyFireNotSetUp],w[kCopyFireLying],w[kCopyFireOtherGun],w[kCopyFireNoAmmo],w[kCopyFireRaising],w[kCopyFireRefused],w[kCopyFireOld],w[kCopyFireFar],w[kCopyFireCap],
                copy_guns_given,copy_guns_carried,copy_guns_failed,m.raises,m.fire_ms/secs,m.fire_calls ? 1000.0*m.fire_ms/m.fire_calls : 0.0);
  ProbeLog(line);
  std::snprintf(line,sizeof(line),"World (member) driving: brake shown %u (getter answers %u), reverse shown %u (answers %u), horns %u frames, steer written %u (no physics %u, mean |E+416| %.2f), engine bit written %u",
                m.brake,world_brake_answers.exchange(0),m.reverse,world_reverse_answers.exchange(0),m.horn_frames,m.steer_writes,m.steer_nophys,m.e416_n ? m.e416/m.e416_n : 0.0,m.engine_writes);
  ProbeLog(line);
  const unsigned* r=m.burn_refused; const unsigned* g=m.ignites_refused;
  std::snprintf(line,sizeof(line),"World (member) burning: lit %u, put out %u, relit %u; refused %u (in a car %u, slots full kept own %u, +120 masked %u, +3694 %u, dead %u, water %u, other %u); own ignites of copies refused %u (explosions %u, fire patches %u, spreading %u, cars %u, other %u)",
                m.lit,m.put_out,m.relit,r[kBurnCar]+r[kBurnKeptOwn]+r[kBurn3694]+r[kBurnDead]+r[kBurnWater]+r[kBurnOther],r[kBurnCar],r[kBurnKeptOwn],m.masked,r[kBurn3694],r[kBurnDead],r[kBurnWater],r[kBurnOther],
                g[0]+g[1]+g[2]+g[3]+g[4],g[kIgniteBlast],g[kIgnitePatch],g[kIgniteSpread],g[kIgniteCar],g[kIgniteOther]);
  ProbeLog(line);
  std::snprintf(line,sizeof(line),"World (member) explosions: played %u (props %u with the prop here %u, without %u; cars' second %u, copy missing %u), skipped here already %u / own act %u, held again here %u, held from copies' rounds %u, chain props recorded %u, local copy-car explosions held %u, bad def %u; "
                "NPC projectile copies made %u (exploded %u), copies' own launches refused %u; this player's bullets on the host's cars sent %u (weapon not named %u), copies' replayed rounds on cars skipped %u",
                m.booms,m.props,m.props_here,m.props_missing,m.cars,m.cars_missing,m.here_already,m.own_act,m.held_again,m.held_replay,m.chain,m.copy_car_held,m.bad_def,copy_proj_made,copy_proj_booms,copy_launches_refused,m.car_hits_sent,m.car_hits_unnamed,copy_car_hits_skipped);
  ProbeLog(line);
  world_am=WorldActMember{}; copy_guns_given=copy_guns_carried=copy_guns_failed=copy_launches_refused=copy_car_hits_skipped=copy_proj_made=copy_proj_booms=0;
}


void WorldGameTick(GuestScratch& scratch,uint32_t player,uint64_t now) {
  const bool on=connected.load() && running.load() && my_slot.load()<kSlots;
  if(!on) { if(world_on) WorldRegistryOff(); WorldLeversOff(); return; }   // (a member's copies left fade and go: UpdateBody)
  if(!world_on || my_slot.load()!=world_slot) WorldRegistryOn(now);
  WorldSendVehicleTable();
  if(hosting.load()) {
    if(world_stream) WorldOwnerTick(scratch,player,now);   // (mod.ini world = 0: no world stream; its clients show 2.1's streams)
    WorldLegacyStreams(now);
    WorldTrains(world_stream && std::any_of(world_members.begin(),world_members.end(),[](const cw::Member& m){ return m.in_area; }));
  } else {
    world_reg.TakeRemovals();   // (a client's own registry streams to nobody: review A2-5)
    { PERF_SECTION("world apply"); WorldMemberTick(scratch,player,now); }
    WorldTrains(WorldLeversOn());
  }
  static uint64_t next_log=0;
  if(now>=next_log) {
    const bool first=!next_log;
    next_log=now+10000;
    if(!first) { WorldScan(now,false); WorldLog(now); }
  }
}

// A slot is a new player (or left): what arrived from the last one goes.
void ResetRemoteIn(int slot,const std::string& name) {
  std::lock_guard lock(pose_mutex);
  RemoteIn& in=remote_in[slot];
  const uint32_t generation=in.generation+1;
  in=RemoteIn{};
  in.generation=generation; in.name=name;
}
std::string SlotName(int slot) {
  std::lock_guard lock(pose_mutex);
  return remote_in[slot].name.empty() ? "Player "+std::to_string(slot+1) : remote_in[slot].name;
}

// The host's world as last received (sequence numbers included): forgotten
// for a new host, or the next host's packets were dropped as old for as long
// as the last one had run (network thread; StartSession before it starts).
uint32_t native_last_received=0;   // network thread
void ResetHostWorldIn() {
  {
    std::lock_guard lock(traffic_mutex);
    for(auto& part:traffic_in) part=TrafficIn{};
    peds_in=PedsIn{};
    traffic_in_names.clear(); ped_looks_in.clear();
  }
  { std::lock_guard lock(cutscene_mutex); cutscene_in=CutsceneStart{}; cutscene_in_new=false; }
  native_last_received=0;
}
// Session events: joins, leaves, refusals (network thread).
void OnSessionMessage(coopnet::Session& sess,const coopnet::Received& m,bool host) {
  const int o=m.origin;
  switch(m.type) {
    case coopnet::kJoinOk: {
      for(int s=0;s<kSlots;++s) ResetRemoteIn(s,sess.Present(s) ? sess.Name(s) : std::string());
      ResetHostWorldIn();
      char line[120]; std::snprintf(line,sizeof(line),"V2 joined as slot %d (%d players)",int(sess.MySlot()),sess.PlayerCount());
      ProbeLog(line);
      Notify("Co-op: connected. You're in "+sess.Name(0)+"'s game ("+std::to_string(sess.PlayerCount())+" players).");
      return;
    }
    case coopnet::kJoinDeny: {
      const std::string why(m.data.begin(),m.data.end());
      ProbeLog(("V2 join refused: "+why).c_str());
      SetStatus(("Join refused: "+why).c_str());
      Notify("Co-op: couldn't join - "+why+".");
      return;
    }
    case coopnet::kPeerJoined: {
      if(o<0 || o>=kSlots) return;
      const std::string name(m.data.begin(),m.data.end());
      ResetRemoteIn(o,name);
      char line[160]; std::snprintf(line,sizeof(line),"V2 player joined: %s as slot %d (%d players)",name.c_str(),o,sess.PlayerCount());
      ProbeLog(line);
      Notify("Co-op: "+(name.empty() ? std::string("a player") : name)+" joined ("+std::to_string(sess.PlayerCount())+" players).");
      return;
    }
    case coopnet::kPeerLeft: {
      if(o<0 || o>=kSlots) return;
      const std::string name=SlotName(o);
      ResetRemoteIn(o,std::string());
      if(!host && o==0) {
        ResetHostWorldIn();
        ProbeLog("V2 lost the host");
        Notify("Co-op: lost the connection to the host. Trying to get back in...");
        for(int s=1;s<kSlots;++s) ResetRemoteIn(s,std::string());
      } else {
        char line[120]; std::snprintf(line,sizeof(line),"V2 player left: %s (slot %d)",name.c_str(),o);
        ProbeLog(line);
        Notify("Co-op: "+name+" left.");
      }
      return;
    }
    default: return;
  }
}

// What another game sent (network thread).
void OnGameMessage(const coopnet::Received& m,bool host) {
  const int o=m.origin;
  if(o<0 || o>=kSlots || o==my_slot.load()) return;
  const char* packet=reinterpret_cast<const char*>(m.data.data());
  const size_t count=m.data.size();
  const bool from_host=o==0 && !host;   // the host's world only from the host
  switch(m.type) {
    case kMsgPose: {
      if(count!=sizeof(PosePacket)) return;
      PosePacket p; std::memcpy(&p,packet,sizeof(p));
      bool valid=true;
      for(float v:p.pos) valid=valid && std::isfinite(v) && std::abs(v)<100000;
      for(float v:p.rows) valid=valid && std::isfinite(v) && std::abs(v)<=1.5f;
      valid=valid && p.look_length<=sizeof(p.look) && p.vehicle>=-1 && p.vehicle<256 && p.seat<16;
      for(float v:p.vpos) valid=valid && std::isfinite(v) && std::abs(v)<100000;
      for(float v:p.vrows) valid=valid && std::isfinite(v) && std::abs(v)<=1.5f;
      for(float v:p.vvel) valid=valid && std::isfinite(v) && std::abs(v)<1000;
      for(float v:p.vang) valid=valid && std::isfinite(v) && std::abs(v)<100;
      valid=valid && p.vowner<uint32_t(kSlots);
      for(float v:p.eye) valid=valid && std::isfinite(v) && std::abs(v)<100000;
      for(float v:p.cam) valid=valid && std::isfinite(v) && std::abs(v)<=1.5f;
      if(!valid) return;
      p.model[63]=0;
      for(char& c:p.model) if(c && (c<32 || c>126)) c='_';
      std::lock_guard lock(pose_mutex);
      RemoteIn& in=remote_in[o];
      if(p.sequence<=in.last_pose) return;
      in.last_pose=p.sequence;
      PlayerPose& pose=in.pose;
      pose.sequence=p.sequence;
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
      std::memcpy(pose.eye,p.eye,sizeof(pose.eye)); std::memcpy(pose.cam,p.cam,sizeof(pose.cam));
      in.pose_new=true; in.pose_at=MonoMs();
      return;
    }
    case kMsgEvent: {
      if(count!=sizeof(EventPacket)) return;
      EventPacket ep; std::memcpy(&ep,packet,sizeof(ep));
      Event e{ep.kind,{},ep.f}; std::memcpy(e.a,ep.a,sizeof(e.a));
      e.origin=uint8_t(o);
      std::lock_guard lock(event_mutex);
      if(events_in.size()<1024) events_in.push_back(e); else ++events_dropped;
      return;
    }
    case kMsgChat: {
      if(count!=sizeof(ChatPacket)) return;
      ChatPacket c; std::memcpy(&c,packet,sizeof(c));
      c.name[15]=0; c.text[159]=0;
      std::string name(c.name), text(c.text);
      for(char& ch:name) if(uint8_t(ch)<32) ch=' ';
      for(char& ch:text) if(uint8_t(ch)<32) ch=' ';
      if(name.empty()) name=SlotName(o);
      std::lock_guard lock(chat_mutex);
      chat_in.push_back(name+"\t"+text);
      while(chat_in.size()>50) chat_in.pop_front();
      return;
    }
    case kMsgMorph: {
      if(count!=sizeof(MorphPacket)) return;
      MorphPacket mp; std::memcpy(&mp,packet,sizeof(mp));
      std::vector<float> values(mp.values,mp.values+128);
      std::lock_guard lock(pose_mutex);
      if(values!=remote_in[o].morphs) { remote_in[o].morphs=values; remote_in[o].morphs_new=true; }
      return;
    }
    case kMsgExtras: {
      if(count!=sizeof(ExtrasPacket)) return;
      ExtrasPacket x; std::memcpy(&x,packet,sizeof(x));
      if(!x.length || x.length>sizeof(x.bytes) || (x.length&7)) return;
      std::vector<uint8_t> bytes(x.bytes,x.bytes+x.length);
      std::lock_guard lock(pose_mutex);
      if(bytes!=remote_in[o].extras) { remote_in[o].extras=bytes; remote_in[o].extras_new=true; }
      return;
    }
    case kMsgWorn: {
      if(count!=sizeof(WornPacket)) return;
      WornPacket w; std::memcpy(&w,packet,sizeof(w));
      if(!w.length || w.length>sizeof(w.bytes)) return;
      std::vector<uint8_t> bytes(w.bytes,w.bytes+w.length);
      std::lock_guard lock(pose_mutex);
      if(bytes!=remote_in[o].worn) { remote_in[o].worn=bytes; remote_in[o].worn_new=true; }
      return;
    }
    case kMsgProj: {
      if(count!=sizeof(ProjPacket)) return;
      ProjIn in{uint8_t(o),{}}; std::memcpy(&in.p,packet,sizeof(in.p));
      if(in.p.kind!=1 && in.p.kind!=2) return;
      std::lock_guard lock(event_mutex);
      if(proj_in.size()<256) proj_in.push_back(in);
      return;
    }
    case kMsgNotoriety: {
      // Clamped: levels, min and max 0..5 with min <= level <= max (the game
      // indexes tables with the level), points finite and not negative.
      if(count!=sizeof(NotorietyPacket)) return;
      NotorietyPacket p; std::memcpy(&p,packet,sizeof(p));
      Notoriety n;
      for(int g=0;g<5;++g) {
        n.min[g]=std::min<uint8_t>(p.min[g],5); n.max[g]=std::clamp<uint8_t>(p.max[g],n.min[g],5);
        n.level[g]=std::clamp<uint8_t>(p.level[g],n.min[g],n.max[g]);
        n.points[g]=std::isfinite(p.points[g]) && p.points[g]>0.0f ? std::min(p.points[g],1000000.0f) : 0.0f;
      }
      std::lock_guard lock(pose_mutex);
      RemoteIn& in=remote_in[o];
      if(!p.sequence) return;
      if(in.notoriety_sequence && int32_t(p.sequence-in.notoriety_sequence)<0) { ++notoriety_old; return; }   // (a refresh from before the last change)
      in.notoriety_sequence=p.sequence; in.notoriety=n; in.notoriety_new=true; ++notoriety_received;
      return;
    }
    case kMsgCarLook: {
      if(count!=sizeof(CarLookPacket)) return;
      CarLookPacket t; std::memcpy(&t,packet,sizeof(t));
      if(!t.length || t.length>sizeof(t.look)) return;
      std::lock_guard lock(car_look_mutex);
      if(car_looks_in.size()>1024) car_looks_in.clear();
      car_looks_in[CarKey(uint32_t(o),t.id)].assign(t.look,t.look+t.length);
      return;
    }
    case kMsgWorldAdmit: {   // (2.2) a member's WWA1: what it holds and declined, for the owner's tick
      if(!host) return;
      coopworld::AdmitMsg a;
      if(!coopworld::ReadAdmit(m.data.data(),count,a)) { ++world_admits_bad; return; }
      std::lock_guard lock(world_mutex);
      if(world_admits_in.size()<64) world_admits_in.push_back({uint8_t(o),std::move(a)});
      return;
    }
    default: break;
  }
  if(!from_host) return;
  switch(m.type) {
    case kMsgTraffic: {
      if(count!=sizeof(TrafficPacket)) return;
      TrafficPacket t; std::memcpy(&t,packet,sizeof(t));
      if(t.part>=kTrafficParts || t.count>kTrafficPerPacket) return;
      bool valid=true;
      for(unsigned i=0;i<t.count;++i) {
        for(float v:t.cars[i].pos) valid=valid && std::isfinite(v) && std::abs(v)<100000;
        for(float v:t.cars[i].rows) valid=valid && std::isfinite(v) && std::abs(v)<=1.5f;
        for(float v:t.cars[i].vel) valid=valid && std::isfinite(v) && std::abs(v)<1000;
        for(float v:t.cars[i].ang) valid=valid && std::isfinite(v) && std::abs(v)<100;
      }
      if(!valid) return;
      std::lock_guard lock(traffic_mutex);
      TrafficIn& in=traffic_in[t.part];
      if(int32_t(t.sequence-in.sequence)>0) {
        in.sequence=t.sequence; in.time=t.time; in.received=MonoMs();
        in.cars.assign(t.cars,t.cars+t.count);
      }
      return;
    }
    case kMsgTrafficNames: {
      if(count!=sizeof(TrafficNamesPacket)) return;
      TrafficNamesPacket t; std::memcpy(&t,packet,sizeof(t));
      std::lock_guard lock(traffic_mutex);
      for(unsigned i=0;i<t.count && i<16;++i) {
        t.names[i].model[45]=0;
        traffic_in_names[t.names[i].index]=t.names[i].model;
      }
      return;
    }
    case kMsgPeds: {
      if(count!=sizeof(PedPacket)) return;
      PedPacket t; std::memcpy(&t,packet,sizeof(t));
      if(t.count>kPedsPerPacket) return;
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
      return;
    }
    case kMsgPedLook: {
      if(count!=sizeof(PedLookPacket)) return;
      PedLookPacket t; std::memcpy(&t,packet,sizeof(t));
      if(!t.length || t.length>sizeof(t.look)) return;
      std::lock_guard lock(traffic_mutex);
      if(ped_looks_in.size()>512) ped_looks_in.clear();
      ped_looks_in[t.id].assign(t.look,t.look+t.length);
      return;
    }
    case kMsgHud: {
      if(count!=sizeof(HudPacket)) return;
      HudPacket hp; std::memcpy(&hp,packet,sizeof(hp));
      if(hp.length>=300) return;
      HudEvent e; e.kind=hp.kind; std::memcpy(e.a,hp.a,sizeof(e.a)); e.length=hp.length; std::memcpy(e.text,hp.text,sizeof(e.text));
      std::lock_guard lock(hud_mutex);
      if(hud_in.size()<256) hud_in.push_back(e);
      return;
    }
    case kMsgMissionId: {
      if(count!=sizeof(MissionIdPacket)) return;
      MissionIdPacket mi; std::memcpy(&mi,packet,sizeof(mi));
      host_mission_hash=mi.hash;
      return;
    }
    case kMsgCutscene: {
      if(count!=sizeof(CutscenePacket)) return;
      CutscenePacket cp; std::memcpy(&cp,packet,sizeof(cp));
      cp.name[63]=0;
      std::lock_guard lock(cutscene_mutex);
      if(cp.sequence!=cutscene_in.sequence) {
        cutscene_in.sequence=cp.sequence; cutscene_in.r4=cp.r4; cutscene_in.r5=cp.r5; cutscene_in.r6=cp.r6; cutscene_in.r7=cp.r7;
        std::memcpy(cutscene_in.name,cp.name,64); cutscene_in_new=true;
      }
      return;
    }
    case kMsgMissionState: {
      if(count!=sizeof(MissionStatePacket)) return;
      MissionStatePacket t; std::memcpy(&t,packet,sizeof(t));
      host_mission=t.mission!=0; host_cutscene=t.cutscene!=0; host_paused=(t.pad&1) ? (t.pad&2)!=0 : t.paused!=0; host_state_seen=MonoMs();
      { std::lock_guard lock(time_mutex); std::memcpy(host_time,t.time,16); host_time_new=true; }
      return;
    }
    case kMsgNative: {
      if(count!=sizeof(coop::NativePacket)) return;
      coop::NativePacket p; std::memcpy(&p,packet,sizeof(p));
      if(!coop::ValidateNpc(p,count) || p.sequence<=native_last_received) return;
      native_last_received=p.sequence;
      { std::lock_guard lock(probe_mutex); native_in=p; native_pending=true; }
      ++native_received;
      return;
    }
    case kMsgWorld: case kMsgWorldCreate: case kMsgWorldRemove: case kMsgWorldDigest: case kMsgOwnerMap: {
      // 2.2: the host's world stream, applied on the game thread in the order
      // it came (WorldMemberTick); mod.ini world = 0: counted, not applied.
      ++world_in_count[m.type-kMsgWorld]; world_in_bytes+=uint32_t(count);
      world_map_seen=MonoMs();   // (the host streams its world: its OwnerMap, or the stream itself, review A3-1)
      if(!world_stream) return;
      std::lock_guard lock(world_mutex);
      if(world_in.size()>=4096) { ++world_in_full; return; }
      world_in.push_back({uint8_t(m.type),MonoMs(),m.data});
      return;
    }
    default: return;
  }
}

// What this game sends, on the same schedule as v1 (network thread).
struct SendState {
  uint64_t next_pose=0, next_tenth=0, next_second=0;
  uint32_t sent_pose=0, sent_peds=0, sent_cutscene=0, sent_native=0, chat_seq=0;
  size_t car_look_cursor=0, name_cursor=0, ped_look_cursor=0;
  uint32_t sent_notoriety=0; uint64_t next_notoriety=0;   // (2.1)
};
void SendDue(coopnet::Session& sess,SendState& st,bool host,uint64_t now) {
  using coopnet::kEveryone;
  auto send=[&](uint8_t type,const void* data,size_t n,bool reliable,uint8_t target=kEveryone) {
    if(sess.Send(target,type,data,uint16_t(n),reliable)) ++packets_out;
  };
  // 2.1's traffic and people streams (unreliable): to the clients they still
  // go to (2.2: not to one that shows the world stream), all at once when
  // that is everyone.
  const uint16_t legacy=host ? legacy_streams.load() : 0;
  uint16_t present=0;
  for(int s=1;s<kSlots;++s) if(sess.Present(s)) present|=uint16_t(1u<<s);
  auto send_legacy=[&](uint8_t type,const void* data,size_t n) {
    if(!(legacy&present)) return;
    if((legacy&present)==present) { send(type,data,n,false); return; }
    for(int s=1;s<kSlots;++s) if((legacy&present)>>s&1) send(type,data,n,false,uint8_t(s));
  };
  // The world stream (2.2): what the owner's tick queued, in its order (a
  // tick's creates and removes before the events of the same pass, F7).
  {
    std::vector<WorldSend> out;
    { std::lock_guard lock(world_mutex); out.swap(world_out); }
    for(const WorldSend& w:out) send(w.type,w.bytes.data(),w.bytes.size(),w.reliable,w.target);
  }
  // Events and projectiles (reliable, in order).
  {
    std::vector<Event> out;
    std::vector<ProjOut> projs;
    { std::lock_guard lock(event_mutex); out.swap(events_out); projs.swap(proj_out); }
    for(const ProjOut& pp:projs) send(kMsgProj,&pp.p,sizeof(pp.p),true,pp.target);
    for(const Event& e:out) {
      EventPacket p{{'W','E','V','1'},0,e.kind,{},e.f};
      std::memcpy(p.a,e.a,sizeof(p.a));
      send(kMsgEvent,&p,sizeof(p),true,e.target);
    }
  }
  // Our player's notoriety (2.1): reliable when a level, min or max changes,
  // and once a second unreliable (the points; players who join meanwhile).
  if(share_police) {
    Notoriety n; uint32_t changes;
    { std::lock_guard lock(pose_mutex); n=local_notoriety; changes=local_notoriety_changes; }
    const bool changed=changes!=st.sent_notoriety;
    if(changes && (changed || now>=st.next_notoriety)) {
      st.sent_notoriety=changes; st.next_notoriety=now+1000;
      NotorietyPacket p{{'W','N','T','1'},changes,{},{},{},0,{}};
      std::memcpy(p.level,n.level,5); std::memcpy(p.min,n.min,5); std::memcpy(p.max,n.max,5); std::memcpy(p.points,n.points,sizeof(p.points));
      send(kMsgNotoriety,&p,sizeof(p),changed); ++notoriety_sent;
    }
  }
  // Our pose, 30 a second.
  if(now>=st.next_pose) {
    st.next_pose=now+33;
    PlayerPose pose;
    { std::lock_guard lock(pose_mutex); pose=local_pose; }
    if(pose.sequence && pose.sequence!=st.sent_pose) {
      st.sent_pose=pose.sequence;
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
      std::memcpy(packet.eye,pose.eye,sizeof(packet.eye)); std::memcpy(packet.cam,pose.cam,sizeof(packet.cam));
      send(kMsgPose,&packet,sizeof(packet),false);
    }
  }
  // Chat lines typed here (reliable).
  {
    std::deque<std::string> lines;
    { std::lock_guard lock(chat_mutex); lines.swap(chat_out); }
    if(!lines.empty()) {
      const std::string me=EosDisplayName();
      for(const auto& line:lines) {
        ChatPacket c{}; std::memcpy(c.magic,"WCH1",4); c.sequence=++st.chat_seq;
        std::snprintf(c.name,sizeof(c.name),"%s",me.c_str());
        std::snprintf(c.text,sizeof(c.text),"%s",line.c_str());
        send(kMsgChat,&c,sizeof(c),true);
      }
    }
  }
  if(host) {
    // The host's HUD (reliable: each once, in order).
    std::vector<HudEvent> fresh_hud;
    { std::lock_guard lock(hud_mutex); fresh_hud.swap(hud_out); }
    for(const HudEvent& e:fresh_hud) {
      HudPacket hp{}; std::memcpy(hp.magic,"WHU1",4);
      hp.sequence=++hud_out_sequence; hp.kind=e.kind; std::memcpy(hp.a,e.a,sizeof(e.a));
      hp.length=e.length; std::memcpy(hp.text,e.text,sizeof(e.text));
      send(kMsgHud,&hp,sizeof(hp),true);
    }
    // A cutscene start (reliable, once).
    {
      CutscenePacket cp{{'W','C','S','1'},0,0,0,0,0,{}};
      bool fresh=false;
      {
        std::lock_guard lock(cutscene_mutex);
        if(cutscene_out.sequence && cutscene_out.sequence!=st.sent_cutscene) {
          st.sent_cutscene=cutscene_out.sequence; fresh=true;
          cp.sequence=cutscene_out.sequence; cp.r4=cutscene_out.r4; cp.r5=cutscene_out.r5;
          cp.r6=cutscene_out.r6; cp.r7=cutscene_out.r7; std::memcpy(cp.name,cutscene_out.name,64);
        }
      }
      if(fresh) send(kMsgCutscene,&cp,sizeof(cp),true);
    }
    // People's positions as soon as a new capture is ready (each game
    // update, up to 20 a second).
    if(share_peds && (legacy&present)) {
      PedPacket parts[kPedParts]{};
      uint32_t sequence=0;
      {
        std::lock_guard lock(traffic_mutex);
        sequence=peds_out_sequence;
        if(sequence!=st.sent_peds)
          for(unsigned part=0;part<kPedParts;++part) {
            std::memcpy(parts[part].magic,"WPD1",4); parts[part].sequence=sequence; parts[part].time=peds_out_time; parts[part].pad=uint16_t(part);
            for(size_t i=part*kPedsPerPacket;i<peds_out.size() && parts[part].count<kPedsPerPacket;++i) parts[part].peds[parts[part].count++]=peds_out[i];
          }
      }
      if(sequence && sequence!=st.sent_peds) {
        for(unsigned part=0;part<kPedParts;++part) {
          if(part>0 && !parts[part].count) break;
          send_legacy(kMsgPeds,&parts[part],sizeof(parts[part]));
        }
        st.sent_peds=sequence;
      }
    }
    // The NPC replica test.
    {
      coop::NativePacket p;
      { std::lock_guard lock(probe_mutex); p=native_out; }
      if(p.sequence && p.sequence!=st.sent_native) { st.sent_native=p.sequence; send(kMsgNative,&p,sizeof(p),false); }
    }
  }
  // Ten times a second.
  if(now>=st.next_tenth) {
    st.next_tenth=now+100;
    if(host) {
      // Paused = the pause menu screen is the active screen (as in kbm.cpp);
      // read here because the game's update may not run while paused.
      const bool menu=api->read_u32(0x839E0DF8)==0x82FFB84Cu;
      const bool paused=menu || int32_t(api->read_u32(0x8370DAE4))>0; // pause menu, or a prompt that pauses the world
      // pad (1.63): bit 0 = this byte is set, bit 1 = the pause menu itself.
      // Clients only pause for the menu: a mission end screen or a prompt
      // pausing the host's world paused the client under its own end screen.
      MissionStatePacket ms{{'W','M','S','1'},uint8_t(local_mission.load()),uint8_t(local_cutscene.load()),uint8_t(paused),uint8_t(1|(menu?2:0)),{}};
      { std::lock_guard lock(time_mutex); std::memcpy(ms.time,local_time,16); }
      send(kMsgMissionState,&ms,sizeof(ms),false);
      // The running mission: the slot's table entry starts with its name hash.
      const uint32_t slot=api->read_u32(0x82B3106C);
      MissionIdPacket mi{{'W','M','N','1'},local_mission.load() && Readable(slot,4) ? api->read_u32(slot) : 0u};
      send(kMsgMissionId,&mi,sizeof(mi),false);
      // Traffic (all parts of the latest capture), driver names, people's looks.
      std::vector<TrafficCar> cars; uint32_t time=0, seq=0;
      { std::lock_guard lock(traffic_mutex); cars=traffic_out; time=traffic_out_time; seq=traffic_out_sequence; }
      if(seq) {
        const uint16_t parts=uint16_t(std::max<size_t>(1,(cars.size()+kTrafficPerPacket-1)/kTrafficPerPacket));
        for(uint16_t part=0;part<parts && part<kTrafficParts;++part) {
          TrafficPacket t{{'W','T','R','1'},seq,time,0,part,{}};
          for(size_t i=part*kTrafficPerPacket;i<cars.size() && t.count<kTrafficPerPacket;++i) t.cars[t.count++]=cars[i];
          send_legacy(kMsgTraffic,&t,sizeof(t));
        }
      }
      TrafficNamesPacket n{{'W','T','N','1'},0,0,{}};
      {
        std::lock_guard lock(traffic_mutex);
        if(st.name_cursor>=traffic_names.size()) st.name_cursor=0;
        for(;st.name_cursor<traffic_names.size() && n.count<16;++st.name_cursor,++n.count) {
          n.names[n.count].index=uint16_t(st.name_cursor+1);
          std::strncpy(n.names[n.count].model,traffic_names[st.name_cursor].c_str(),45);
        }
      }
      if(n.count) send_legacy(kMsgTrafficNames,&n,sizeof(n));
      if(share_peds && (legacy&present)) {
        // Appearances: 6 people per round, so each is resent about once a
        // second (lost packets heal on their own).
        std::vector<PedLookPacket> looks;
        {
          std::lock_guard lock(traffic_mutex);
          for(unsigned k=0;k<6 && !peds_out.empty();++k) {
            const PedState& ps=peds_out[st.ped_look_cursor++%peds_out.size()];
            auto l=ped_looks_out.find(ps.id);
            if(l==ped_looks_out.end() || l->second.empty()) continue;
            PedLookPacket lp{{'W','P','L','1'},ps.id,uint8_t(l->second.size()),{}};
            std::memcpy(lp.look,l->second.data(),l->second.size());
            looks.push_back(lp);
          }
        }
        for(auto& lp:looks) send_legacy(kMsgPedLook,&lp,sizeof(lp));
      }
    }
    // Car looks: 3 per round, cycling, so each is resent regularly.
    std::vector<CarLookPacket> out;
    {
      std::lock_guard lock(car_look_mutex);
      const size_t n=car_looks_out.size();
      for(size_t k=0;k<3 && k<n;++k) {
        auto it=car_looks_out.begin(); std::advance(it,(st.car_look_cursor++)%n);
        CarLookPacket lp{{'W','V','L','1'},it->first,uint16_t(it->second.size()),0,{}};
        std::memcpy(lp.look,it->second.data(),it->second.size());
        out.push_back(lp);
      }
    }
    for(auto& lp:out) send(kMsgCarLook,&lp,sizeof(lp),false);
  }
  // Every second: what this player wears, body sliders, colours.
  if(now>=st.next_second) {
    st.next_second=now+1000;
    WornPacket w{{'W','W','I','1'},0,{}};
    { std::lock_guard lock(pose_mutex); w.length=uint16_t(local_worn.size()); std::memcpy(w.bytes,local_worn.data(),local_worn.size()); }
    if(w.length) send(kMsgWorn,&w,sizeof(w),false);
    MorphPacket mp{{'W','M','O','1'},{}};
    bool have=false;
    { std::lock_guard lock(pose_mutex); if(local_morphs.size()==128) { std::memcpy(mp.values,local_morphs.data(),sizeof(mp.values)); have=true; } }
    if(have) send(kMsgMorph,&mp,sizeof(mp),false);
    ExtrasPacket x{{'W','A','X','1'},0,{}};
    { std::lock_guard lock(pose_mutex); x.length=uint8_t(local_extras.size()); std::memcpy(x.bytes,local_extras.data(),local_extras.size()); }
    if(x.length) send(kMsgExtras,&x,sizeof(x),false);
  }
}

void NetworkLoop(sockaddr_in host_address, bool host) {
  // A v1 co-op knocking (it can't join): told once in the log and on screen.
  auto receive=[](char* buffer,int capacity,sockaddr_in& from) {
    const int n=NetRecv(buffer,capacity,from);
    if(n==int(sizeof(kV1Hello)-1) && !std::memcmp(buffer,kV1Hello,size_t(n))) {
      static bool told=false;
      if(!told) { told=true; ProbeLog("A player with an older co-op (1.71 or earlier) tried to join"); Notify("Co-op: a player with an older co-op version tried to join. Both players need co-op 2.2."); }
      return 0;
    }
    if(n>0) ++packets_in;
    return n;
  };
  coopnet::Session sess(NetSend,receive,[](const std::string& line) { ProbeLog(("Session: "+line).c_str()); });
  const std::string me=EosDisplayName();
  if(host) sess.StartHost(me); else sess.StartClient(host_address,me);
  for(int s=0;s<kSlots;++s) ResetRemoteIn(s,std::string());
  if(host) { std::lock_guard lock(pose_mutex); remote_in[0].name=me; }
  world_eos_backlog=0;   // (a last session's Epic queue: review A2-6)
  for(int s=0;s<kSlots;++s) { world_link_backlog[s]=0; world_link_dropped[s]=0; world_link_inflight[s]=0; world_link_rtt[s]=0; }
  SendState st;
#ifdef WHOMPAYS_EOS
  std::string eos_shown;
  if (use_eos.load()) {
    eos::log_fn = [](const char* line) { ProbeLog(line); };
    eos::Begin(std::string(self->folder), host, eos_join_code, me, uint32_t(kSlots));
  }
#endif
  uint64_t next_stats=MonoMs()+10000, next_links=0;
  const uint64_t started=MonoMs();
  bool told_slow_join=false;
  std::string status_shown;
  while (!stop_thread.load()) {
#ifdef WHOMPAYS_EOS
    if (use_eos.load() && !connected.load()) {
      const std::string t = eos::Text();
      if (t != eos_shown) {
        eos_shown = t;
        if (!t.empty()) SetStatus(t.c_str());
        if (t.rfind("Online. Join code: ", 0) == 0)
          Notify("Co-op is online. Join code: " + eos::Code() + " (also under Pause > Options). Friends pick Join Co-op and type it (up to 12 players).");
        else if (t.rfind("Online failed: ", 0) == 0)
          Notify("Co-op couldn't go online: " + t.substr(15));
      }
    }
    if (use_eos.load()) {
      static auto next_queue=std::chrono::steady_clock::now();
      const auto t=std::chrono::steady_clock::now();
      if (t>=next_queue) {
        next_queue=t+std::chrono::milliseconds(200);
        EOS_P2P_PacketQueueInfo q{};
        if (eos::QueueInfo(q)) {
          auto raise=[](std::atomic<uint32_t>& a,uint64_t v) { const uint32_t c=uint32_t(std::min<uint64_t>(v,UINT32_MAX)); uint32_t o=a.load(); while(c>o && !a.compare_exchange_weak(o,c)) {} };
          raise(link_out_bytes,q.OutgoingPacketQueueCurrentSizeBytes); raise(link_out_packets,q.OutgoingPacketQueueCurrentPacketCount);
          raise(link_in_bytes,q.IncomingPacketQueueCurrentSizeBytes); raise(link_in_packets,q.IncomingPacketQueueCurrentPacketCount);
          world_eos_backlog=uint32_t(std::min<uint64_t>(q.OutgoingPacketQueueCurrentSizeBytes,UINT32_MAX));   // (the world stream's backpressure, 2.2)
        }
      }
    }
#endif
    const uint64_t now=MonoMs();
    sess.Pump(now);
    coopnet::Received m;
    while (sess.Poll(m)) {
      if (m.type < coopnet::kFirstUserType) OnSessionMessage(sess,m,host);
      else OnGameMessage(m,host);
    }
    // Who is in the session now (for the game thread).
    {
      uint16_t mask=0; int count=0;
      for (int s=0;s<kSlots;++s) if (sess.Present(s)) { mask|=uint16_t(1u<<s); ++count; }
      present_mask=mask; player_count=count; my_slot=sess.MySlot();
      const bool now_connected=host ? count>=2 : sess.Joined();
      if (now_connected!=connected.load()) {
        connected=now_connected;
        if (now_connected) ProbeLog(host ? "V2 a client is connected" : "V2 connected to the host");
        else ProbeLog(host ? "V2 no clients left" : "V2 not connected to the host");
      }
      char text[96];
      if (now_connected) std::snprintf(text,sizeof(text),"Connected: %d players (you are player %d).",count,int(sess.MySlot())+1);
      else std::snprintf(text,sizeof(text),host ? "Hosting. Waiting for players..." : "Joining the host...");
      bool show=text!=status_shown;
#ifdef WHOMPAYS_EOS
      if (use_eos.load() && !now_connected) show=false;   // Epic's own progress text is shown then
#endif
      if (show) { status_shown=text; SetStatus(text); }
      // A host that never answers: most likely an older co-op (1.71 or
      // earlier) on the other side, or the wrong address.
      if (sess.Joined()) told_slow_join=true;   // (only for a first join that never got an answer)
      if (!host && !sess.Joined() && !told_slow_join && now-started>20000 && sess.DenyReason().empty()) {
        told_slow_join=true;
        ProbeLog("V2 no answer from the host after 20 s");
        Notify("Co-op: no answer from the host yet. Both players need co-op 2.2; still trying...");
      }
    }
    if (connected.load()) SendDue(sess,st,host,now);
    sess.Pump(now);   // what was queued goes out now
    // Each client link's backlog after the flush and its drops (2.2: the
    // world stream halves a member's budget on them), 20 times a second.
    if (host && now>=next_links) {
      next_links=now+50;
      for (int s=1;s<kSlots;++s) {
        const coopnet::LinkStat l=sess.LinkStats(s);
        world_link_backlog[s]=l.backlog; world_link_dropped[s]=l.dropped; world_link_inflight[s]=l.in_flight_bytes; world_link_rtt[s]=uint32_t(std::max(0.0f,l.rtt));
      }
    }
    if (now>=next_stats) {
      next_stats=now+10000;
      const coopnet::Stats s=sess.TakeStats();
      net_resent+=s.resent; net_dropped+=s.dropped_unreliable;
    }
    Sleep(3);
  }
  sess.Stop(MonoMs());   // a client says it's leaving
#ifdef WHOMPAYS_EOS
  if (use_eos.load()) eos::End();
#endif
  CloseSocket();
  connected=false;
  present_mask=0; player_count=0; my_slot=coopnet::kNoSlot;
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
  // Co-op Car Guard's fixes are built in since 2.0; the test mod isn't needed.
  if (GetModuleHandleA("CoopCarGuard.dll")) {
    ProbeLog("Co-op Car Guard is installed: not needed with co-op 2.0 (its fixes are built in)");
    Notify("Co-op: the Co-op Car Guard test mod isn't needed any more (built in). Remove the dist/mods/CoopCarGuard folder.");
  }
  StopSession();
  ProbeLog("Previous session stopped");
  std::lock_guard game_lock(game_mutex);
  native_encoded=0; native_received=0; native_available=0;
  replica_updates=0; tracked_host=0;
  { std::lock_guard lock(probe_mutex); native_out={}; native_in={}; native_pending=false; }
  avatar_updates=0;
  for(int s=0;s<kSlots;++s) ResetRemoteIn(s,std::string());
  ResetHostWorldIn();
  { std::lock_guard lock(event_mutex); events_in.clear(); events_out.clear(); proj_in.clear(); proj_out.clear(); }
  { std::lock_guard lock(world_mutex); world_out.clear(); world_admits_in.clear(); world_in.clear(); }   // (the last session's world stream, 2.2)
  world_eos_backlog=0; world_map_seen=0;
  npc_follow.Reset(); npc_pending_look.clear();
  actor_models.clear();   // (the last session's copies went with their references; a stale release would take one off a live request)
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
  SetStatus(host ? "Hosting on port 27015. Waiting for players..." : "Joining host...");
  Notify(host ? "Co-op: hosting. Friends join with your IP " + address + " (up to 12 players)."
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
      text += "Actions sent / played: "+std::to_string(actions_sent.load())+" / "+std::to_string(actions_played.load())+
              "   Hits sent / taken: "+std::to_string(hits_sent.load())+" / "+std::to_string(hits_taken.load())+"\n";
      text += "Players: "+std::to_string(player_count.load())+" (you are "+std::to_string(int(my_slot.load())+1)+"), "+
              std::to_string(avatar_count.load())+" shown here, "+std::to_string(avatar_updates.load())+" pose updates\n";
      for (int s=0;s<kSlots;++s) {
        if (s==my_slot.load() || !Present(s)) continue;
        std::string name, model; float pos[3]{};
        { std::lock_guard lock(pose_mutex); name=remote_in[s].name; model=remote_in[s].model; std::memcpy(pos,remote_in[s].pose.pos,sizeof(pos)); }
        char line[200];
        std::snprintf(line, sizeof(line), "  %d %s%s: %.1f, %.1f, %.1f%s%s\n", s+1, name.c_str(), s==0 ? " (host)" : "",
                      pos[0], pos[1], pos[2], model.empty() ? "" : "  ", model.c_str());
        text += line;
      }
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
    share_shooting=GetPrivateProfileIntA("settings","shooting",1,(std::string(self->folder)+"\\mod.ini").c_str())!=0;
    walk_as_player=GetPrivateProfileIntA("settings","walk",1,(std::string(self->folder)+"\\mod.ini").c_str())!=0;
    share_police=GetPrivateProfileIntA("settings","police",1,(std::string(self->folder)+"\\mod.ini").c_str())!=0;
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
    memory_speedup=GetPrivateProfileIntA("settings","memory_speedup",1,ini.c_str())!=0;
    sync_idle=GetPrivateProfileIntA("settings","sync_idle",0,ini.c_str())!=0;
    debug_cars=GetPrivateProfileIntA("settings","debug_cars",0,ini.c_str())!=0;
    if(debug_cars) ProbeLog("Car list checks on (mod.ini debug_cars = 1)");
    keep_copies=GetPrivateProfileIntA("settings","keep_copies",1,ini.c_str())!=0;
    copy_kind=std::min<unsigned>(GetPrivateProfileIntA("settings","copy_kind",0,ini.c_str()),6);
    people_memory_scale=std::clamp<unsigned>(GetPrivateProfileIntA("settings","people_memory",1,ini.c_str()),1,4);
    world_upload_kbps=std::clamp<unsigned>(GetPrivateProfileIntA("settings","world_upload_kbps",1000,ini.c_str()),48,100000);
    world_stream=GetPrivateProfileIntA("settings","world",1,ini.c_str())!=0;
    world_actions=GetPrivateProfileIntA("settings","world_actions",1,ini.c_str())!=0;
    world_join_m=std::clamp<unsigned>(GetPrivateProfileIntA("settings","member_join_m",150,ini.c_str()),40,400);
    world_leave_m=std::clamp<unsigned>(GetPrivateProfileIntA("settings","member_leave_m",200,ini.c_str()),40,400);
    if(world_leave_m<world_join_m+20) world_leave_m=world_join_m+20;
    world_member_spawns=std::min<unsigned>(GetPrivateProfileIntA("settings","member_spawns",7,ini.c_str()),7);
    { char v[32]{}; GetPrivateProfileStringA("settings","member_density","1.0",v,sizeof(v),ini.c_str()); const float d=float(std::atof(v)); world_member_density=std::isfinite(d) ? std::clamp(d,0.0f,4.0f) : 1.0f; }
  }
  api->hook(0x8250EEF8,StreamSetupHook,&original_stream_setup);
  ProbeLog("Whompays Coop 2.2 in progress (co-op v2, docs/COOP_V2.md): 2.2: a client asks for people models at level 0 with every people-model slot there (0x827ACFFC; the top level had only the slots last moved to it: 8 loaded, then 4-6, \"People-model slot quota\" lines), and car copies take their model reference the game's own way (82532360) so the game gives it back when they go (\"Car models:\" line); 2.2 Package C (protocol 6): what the host's world does shows on the players in its area - its people's gunfire played on their copies (the rounds counted with their hit points, automatic guns' triggers held and let go, guns raised), its cars' brake and reverse lights, horns and steering, burning people, exploding props and cars' second explosions, its people's rockets and grenades (mod.ini world_actions) - and a member's bullets on the host's cars count there (kEventCarDamage); \"World (owner|member) gunfire / driving / burning / explosions\" lines; 2.2 Package B (protocol 6): the host's game makes the area's content around every member too - its parked-car pass for a member's position (a stand-in for the player 8241D930 / 8241DC78 read, the real one back around the spawn and the state changes), its traffic spots around a member's camera on that member's turn (82412298's arguments), its people around a member's camera (8240F148 with the camera's pose swapped, the constructor and set-up with the real one) - and keeps alive what is near a member (8240FA90, 82412878, the make-room test 8240E338, the slot allocator's evict functions 823FFA48 / 823FFC48 but for a priority-0 creation), the wanted counts raised per member (mod.ini member_spawns, member_density; members join at 150 m and leave past 200 m), \"World (owner) B\" and \"World (member) B\" lines; step 4: the host decides who is in its area from the poses (mod.ini member_join_m / member_leave_m, 2 s in / 5 s out; only players that show the world stream), 2.1's traffic and people streams go only to players with world = 0 (and nothing is captured for them when there are none), a member's own police are off while it is in the area and the host's character is shown there (kept to member_leave_m + 30 m), a member runs no hood or special spawns (824290D0) and no new trains start while the area is shared (0x827ACF87, on the host too), the games compare their vehicle tables (\"Vehicle table:\" lines); step 3: a client in the host's area shows the host's world stream - the cars and people the host's game makes, created once here (4 an update; this game decides what it holds: 26 cars with two players, 56 people, the people models it can load; a nearer one takes a farther one's place), moved 20 times a second within a budget (the cars the way the game's multiplayer moves a remote car: woken with 826CD960 and steered by their body's velocities, placed only when far off), faded and removed when they go there; the 2.1 traffic and people streams aren't shown then (mod.ini world = 0: 2.1's as before; the host's world = 0 sends no stream); \"World (member\", \"World set match\", \"World update age\", \"World match\" and \"World models\" lines; step 2: every game keeps a registry of the cars and people its own game makes (82569700 and 8247A6D0 after the call, 824789A8 and 825324D0 as they go; checked against the object table every 10 s) and the host streams it to the players in its area, 20 times a second within a budget per player (mod.ini world_upload_kbps); 2.1.4: other players as arrows on the minimap and the pause map, in a colour per player, where they are or were last seen (their character's purple dot is gone; the pause map showed nobody); other players' characters are moved the way the game moves its own player and multiplayer's remote players - a stick input every update (8245FC08, the point to walk to 5 m ahead in +464, state 1) instead of move orders along their path (82441670; state 2 is \"walking a path\": with the gun up the character slid with its legs still while its player walked and fired, the 2.1 tests' \"floats\" lines) - so they walk, run, strafe, back-pedal and aim-walk with their legs (mod.ini walk = 0: move orders as 2.1; \"walks here\" lines show the animation layer at the first steps); every character made here (copies, stand-ins, traffic crews) gives its model's streaming reference back when it goes - nothing in the game did (BF 1 -> 139 in 12 min: dead models' requests evicted the slots live copies wore), a 10 s guard logs any excess and the slot dump shows each slot's level and the per-level quota; a client's hits on the host's traffic crews land (the seat by the crew's own +2496/+2500: the car's seat slots are empty for most AI crews); the host's mission-end message goes out only with a mission running; helicopters over the target are counted and called off; only the sync knocks another player's character down - a replay here (the host replaying that player's own rocket's explosion) knocked it down while its player stood and fired there (55 of their shots refused in the second test); the Shots line counts the refused shots by reason (lying, not set up, other gun, no ammo, fire refused, gun raising, in a car); a wreck replayed on a copy without its physics set up (far off, parked) waits quietly for it (4 tries, then the moment it has physics; before: 4 tries a second for as long as it lasted); 2.1.3: traffic crew copies' own exit requests go through again (2.1.2 refused them; the first asker seen was the game's own character removal 824789A8, which takes a human out of its car before it goes - refused, a deleted character stayed in the seat); 2.1.2 (protocol 5): a client's crimes are judged by the host's cops - the game credits an unseen crime a quarter of its points (its detection test 821D1F98, run as the record is applied: a living cop within 40 m in front of it, 8 m around it, or 40 m of its car, through walls; the first 2.1 test's client got no stars that way), so the host runs that test on another player's character when it forwards a crime and sends the record's witnessed flag set when it holds, tells each client whether a cop sees its character (kEventSeen, on change and once a second), and the client queues the crimes it credits itself (hits, carjacks) witnessed while that holds; a police car taken by another player's character counts in their own game, as a carjack does (82482400's record here is dropped: forwarded, one act counted twice); a copy carries no team, so the thief's own game never counts it) and so is a cop's police or gang alert at their character (820DB9D0, flag set); \"crime detection\" lines (seen or not, the nearest of the team) and the Police line's witnessed counts; 2.1.1 (from the first 2.1 test, protocol 4): a car a cheat, mod menu or script makes for a client stays (the removal of this game's own cars while the host's world is shown took it away at once: 82569700 with source 7 or 3 is this player's own, source 2/4/5/6/9 the game's spawners); a client's hits on the host's traffic crews go to the host (kEventCrewDamage: car id and seat) instead of killing the copy here only (the corpse was removed again and again) - cops in cars can be hurt and their kills credited; crew copies no longer get out of their cars on their own here (their exit requests are refused; a pull-out by this player goes through); another player's character gets up where that player stands when its ragdoll here ended elsewhere (a car hit sent the copy 42 m away: the get-up spots +792/+808 are set to their position); the Police line shows the police points; 2.1: notoriety per player - each player's stars are their own game's, every game's police and gangs go after the wanted player's character (its level is written on it there), a player's crimes seen in another game are credited to their own game (kills, government cars, a cop seeing their gun; not their hits and carjacks, which their own game counts), only the player the cops arrest is busted, and the host's police response - its cars, helicopter and the gangs' cars - comes for the wanted player in the host's world (the highest level within 200 m of the host; its units and cars go to the clients with the people and traffic) instead of the host (mod.ini police = 0 turns it all off: as 2.0, the host's stars rise for the crimes seen in its game and a client's own police run near the host too); car damage comes from the game that drives the car - hit points, smoke, fire and wrecks (2.0 took the fade-out before a car is removed for its wreck: intact cars blew up on the other games, real wrecks only lost their smoke there); copies are wrecked with the game's multiplayer wreck replay and take no hit point losses, explosion pushes or chain wrecks of their own; the host's people and traffic crews shown on a client wear the host's teams (minimap marks), don't shoot or hurt that player on their own and play only the host's actions (crews also their car moves), and the client's own police response and roadblocks stay off while its character stands in the host's game (made within 60 m of the host, removed past 130 m; what the response had out then is removed; beyond, its own come back); other players' knockdowns and get-ups show where they happen (2.0 replayed their get-up states on a character still lying here, which stood it up at the map's origin and back - the ~700 m jump - or left it lying for good): nothing of theirs is replayed while they lie, their character's get-up spots are set before each knockdown, one left ragdolled is knocked again so it can get up; the map menu closes on a client again (the pause menu's context 823494D0 gets the multiplayer answer over a running world); the police lights and siren of a car another game drives show on its copy; protocol 3 (2.0 can't join); 2.0: up to 12 players in one session (the host relays; messages carry the sender's slot, events are acked and resent instead of sent three times), one character, clothes, gun and car record per other player, cars named by their owner's slot so any player can drive or ride in any other player's car, the host shares its traffic and people around every client; built in from Co-op Car Guard 0.5: memory checks skip QueryWorkingSetEx under Wine and keep VirtualQuery answers per region (mod.ini memory_speedup); fixed: car smoke and fire were applied the wrong way round; the local player is put back when the game takes another player's character for it (the 1.34 check never matched); 1.71: online link - the outgoing Epic queue is capped at 64 KB (it was unlimited, so what the link couldn't carry waited in line and arrived later and later: shots 20-30 s late in the 1.69 test), and each Perf block has a \"Link:\" line (direct or through Epic's relay servers, the largest queue sizes, packets refused by the full queue); 1.70:no crash when the other player's character is made again (after a death or out of range) - its guns are holstered and removed before it goes back to the player pool, and a pool object starts with no equipped item (1.69: a C++ exception in 8247E248 right after the PC player died, the Mac's character held a grenade); thrown weapons aren't handed over; 1.69:replayed shots of single-shot guns (Magnum, shotguns) are no longer refused after the first - the trigger latch (ammo +12 bit 0x80, set by each shot, cleared by the shooter's trigger release) is cleared before each replay; 1.68:the other player's gun is given the way the game's multiplayer arms a remote player (823AA218): with the 76-byte item table entry the weapon info's +32 points to, after the item in that weapon's slot (823F79F0) is reused or removed (1.66/1.67 gave the weapon info itself: a broken item that never went into the hand, and the host froze in the world update soon after); 1.67:no freeze from the other player's gun - the pose's gun is read the way the fire function reads it (item +16 ammo, +8 info: 1.66's pose said no gun while the shots said TEC-9), and the character's gun changes only once the poses agree for 400 ms (1.5 s to holster), not while a shot holds it, using a gun it already carries (1.66 gave and holstered every frame and the host froze in the projectile/item update); \"Our gun in hand\" lines; 1.66:shooting shows in the other game - the gun in each player's hand goes with the pose and each shot is replayed on their character there the way the game's multiplayer replays a remote player's shot (muzzle flash, sound, recoil, tracers; bullet guns only), hurting no one there since the hits already come across (\"Shots:\" line); 1.65:scans of the object table check each object's type on one page before its full size, and readable pages are remembered 4096 at a time instead of 32 (the host's car scan grew from 40 to 150 ms/s over a 6 minute session: lag); 1.64: a car is no longer removed while its list of people getting in or out names someone who belongs to another car or none (the joiner froze for good while shared traffic removed a car copy; logged as \"Car removal:\"; mod.ini debug_cars = 1 checks those lists all the time, \"Car list check:\"); 1.63: the other player's character never removes someone sitting in the seat it wants (takes a free seat; Troy was removed), the host's strongholds / missions the joiner's save hasn't opened start on the joiner too, only timed mission help reaches the joiner (no \"Kick the door open\"), the joiner pauses only for the host's pause menu, a mission end that wasn't running here shows as a message, shared cars and people removed a few at a time (freeze on disconnect), no seat retries far from the player; 1.62: parked cars of the other game placed only when they move (were placed every update), the other player\'s walk re-aimed sooner on quick turns and no stop-glide on turns; 1.61: the host\'s pause pauses the joiner\'s game for real (world stopped, the joiner\'s own pause menu can always be closed), the host\'s car scan no longer runs every frame (most of the host\'s mod time: desync); 1.60: section timing in the Perf lines; 1.59: chat (T in the game, WCH1 packets); 1.58: reserved names (Whompay) only with the owner key; 1.57: co-op messages (hosting, join code, joined, left, ended, errors) in the game's own help box and the join code under Pause > Options instead of the F6 box (F6 is a debug view now); 1.56: no crash on hosting / joining online (the game's System Link over Epic and co-op online no longer run two Epic sessions in one game), Host / Join / End Co-op in the game's menus; 1.49 (1.45 + one change): story characters are never sent as pedestrians (Johnny Gat, Dex, Julius and Troy came back at the church stairs on the joiner after the end prompt); 1.45: the host sends only people within 100 m of itself, pedestrians too (a finished mission\'s gang members, turned into pedestrians, T-posed on the joiner after the host was moved to the next mission), every cutscene character of this game\'s own left standing is removed (Julius stayed on the stairs), list of who stands near the joiner after a mission in its log; 1.44: hit reactions of the host\'s people are played the same on both screens, and the joiner\'s punches make them flinch on the host too (combat reactions only on one side); this game\'s own story characters left standing after a mission cutscene (Johnny Gat, Dex, Julius, gang members, T-posing) are removed at once; 1.43: people models the host\'s people wear here keep their top priority (in fights one pushed the other out: people blinking; mod.ini keep_worn_models); 1.42: copies of the host\'s people wearing their real model are no longer removed to make room (people going invisible and back again and again); 1.41: the other player\'s character is made again when it stays stuck off their path (lay flat behind them for a whole mission after being knocked down just before a cutscene), the host sends only mission characters within 100 m of itself (the finished mission\'s people at the old spot showed on the joiner for ~10 s after the host was moved to the next mission); 1.40: the host no longer sends mission characters its own game has hidden (a finished mission\'s people, and the next mission\'s made ahead, showed up only on the joiner after the mission-passed prompts); 1.39 taken back; 1.38: this game\'s own cutscene characters still standing after a cutscene are hidden (Johnny Gat and others on the stairs at the next mission), and removed when their models keep the host\'s people from loading; all generic gang members and police count as this game\'s own extras (xx_X_*); 1.37: this game\'s own generic mission characters are removed after 1 s when their model isn\'t the host\'s, and whenever they keep the host\'s models from loading (people invisible in missions), everything the joiner\'s mission left behind is removed for 8 s after it ends (mission people staying after the end); 1.36: after a cutscene the host\'s people are shown only once the host sends them again (a finished mission\'s enemies came back for a moment); 1.35: this game\'s own mission characters are removed when the host\'s mission is over here (the finished mission\'s enemies came back), the other player\'s character is put back at once when something here throws it far off (vanished after a hit); 1.34: the local player is put back when the game takes the other player\'s character for it after a cutscene (both saw only the other one and couldn\'t move), player diagnostics after cutscenes; 1.33: this game's pedestrian spawning is turned off again when a mission turns it back on (people popping in and out, their models kept the host's from loading), the host's people are kept through cutscenes and made up to 8 at a time (one by one after a cutscene), no falling over again and again when standing on a hidden character; 1.32: the arrows over the people a mission wants attacked are shown on the joiner too (the host's in-game effects on its shared people); 1.31: this game's own hidden mission characters are removed when their models keep the host's people's models from loading (people invisible in fights); 1.30: stand-ins use models the host has loaded (models only the joiner had kept the host's people from loading: invisible people), a single far-off position of the other player is ignored (vanishing for a moment after being hit), colours set before the clothes go on and clothes put on again when colours arrive later (hair colour); 1.29: the host's people are hidden during cutscenes instead of removed and made again afterwards (people flashing in, T-poses, wrong models after cutscenes); 1.28: a mission the host starts right after passing one brings the joiner over again (1.27 left them behind), the joiner is no longer moved away from the host every time they come close in a fight (only when standing inside each other for 3 s); 1.27: the host's mission no longer counts as ended during the moment between its stages (the joiner restarted it: mission people flickered, the last cutscene wasn't played); 1.26: no freeze when the host skips the last cutscene of a mission (the joiner's cutscene is ended before the mission end, not after); 1.25: body slider check (what arrives from the other player, whether the game baked it into their model, re-applied if something resets it); 1.24: body sliders applied as the game does (only the wanted value; the game bakes the change into the character's own body model), 1.23: memory for the other player's body model (the game's player-body stream group had none in single player, so it never loaded), key 5 spawns the copy again at once; 1.22: the other player's own body model is loaded the way the game loads the player's (1.21 never got it: shared body, host turned muscular); with the shared body nothing of theirs is applied; 1.21: the other player gets their own body model (multiplayer's StyleTest_PC_MP slots, mod.ini own_body) and their body sliders (mod.ini body) - no more both players turning skinny or muscular; 1.20: the other player's skin tone and hair colour (the game's colour choices) are sent and applied, body-shape diagnostic on the player copy (mod.ini body_test); 1.19: the joiner's game can no longer remove copies after 2 s of asking (they were remade: people changing model and clothes, vanishing and coming back); 1.18: new copies hidden until the host's clothes are on (a hat came and went), people standing still get no walk orders (stood up and sat down on a bench again and again); 1.17: models kept in memory that nothing uses are unloaded with the game's own unload while the host's models wait (1.15/1.16 freed their slots by hand: 5 fps), models asked for at the top level again, freeze reports say what the mod was doing; 1.16: the host's models are no longer asked for ahead on the joiner (the joiner's freezes started with that in 1.11; mod.ini preload_host_models); 1.15: people-model slots left full after a model was let go are freed (the joiner held 8 models, the host 11: people missing); 1.14: a person's walk/idle style is set once per change of the host's (no switching between two, e.g. cane and wheelchair), model memory of both games in the logs; 1.13: models loaded here that the host doesn't have make room for the host's people's models (people missing on the joiner); starting the host's people's ongoing actions is off (it started a newspaper only the joiner saw; mod.ini sync_idle), people-model lists of both games in the joiner's log; 1.12: what the host's people are doing (newspaper, tying shoes, cane walks) is started on their copies, people in cars are removed with their car again (the joiner's game froze); 1.11: the joiner loads the people models the host has loaded (same people sooner, fewer lookalikes), no more wrong position when thrown about (other player invisible after jumping out of a car); 1.10: this game no longer removes copies of the host's people, drivers and passengers by itself (people popping in and out; mod.ini keep_copies); 1.09: copies of people are made as scripted characters, which this game doesn't remove on its own (mod.ini copy_kind, 3 = as before); 1.08: getting in as a passenger walks to the door and climbs in (was put straight in the seat); copies waiting for their model (behind a stand-in) and traffic drivers and passengers are no longer removed as this game's own people (popping in and out); 1.07: people models no character uses any more are let go (removed copies kept them: models filled up, people popped in and out on the joiner); 1.06: people-model memory back to normal (twice the memory crashed in the player creator), models no longer needed let go after 4 s instead of 30 s so the host's current people get the room (NPCs in cars), loaded/waiting people models in the log; 1.05: twice the memory for people models (mod.ini people_memory); 1.04: more people models is off by default (people_models = 3 froze the game when entering the world; freeze report written when on); 1.03: room for more people models (mod.ini people_models); 1.02: the pulled-out driver carries on here (gets up) instead of a new standing copy, crash and freeze reports next to the log; 1.01: no endless driver re-making when a seat is refused (the joiner\'s game froze), a pulled-out driver is thrown out with the same exit as in the other game; 1.00: the other player\'s character gets out of its car before it is removed (crash when they time out or leave range while seated), a car stays put while the other player pulls its driver out; 0.99: getting in where the other player drives goes straight to the passenger seat (same seats on both screens), a pulled-out driver\'s copy is removed once out (no duplicate); 0.98: passers-by wait for their real model again (no switching or wrong people), the other player is never taken out of a car their game still has them in (no gliding without a car), car models of the other player\'s car asked for at top priority; 0.97: getting in as the other player\'s passenger finishes (seated directly if it stalls), a pulled-out driver plays the being-pulled-out move on both screens before stepping out, stand-ins are always the same sex; 0.96: no car removed when a driver is pulled out, drivers, passengers and passers-by whose model won't load here get a lookalike (no invisible people), getting in where the other player drives falls back to the usual way when the passenger seat is refused; 0.95: a driver the other player pulls out is pulled out here with the game's own move, whoever got in first keeps the wheel (no two drivers in one seat); 0.94: a driver the other player pulls out is found and gets out here too (no player sitting on top of it); 0.93: players never pull each other out of a seat, no jump to the passenger seat while still getting in, traffic drivers and passengers found by where people sit (NPCs in cars shown), car doors open and close on both screens; 0.92: getting in where the other player drives asks for the passenger seat (no pulling them out), drivers' and passengers' models asked for at top priority, animations played with their blend values, their car found when the host gets in one the joiner took; 0.91: the other player's getting in shows their own moves (door, pulling the driver out, climbing in) and seats them when they sit; 0.90: taking a car with a driver: the driver gets out on both screens, then the player gets in; getting in with the other player driving walks to the passenger door; up to 104 cars shared, parked ones up to 200 m; 0.89: the other player's jumps, climbs and punches start where their character is shown (held back by the follow delay); 0.88: the other player's getting in carries on at the door (door and climb-in play), a car of this game the other player drives up in is shown again; 0.87: passengers of shared cars shown, F9 trace logs getting-in progress; 0.86: the other player is seated at once if their car drives off before getting in finishes here; 0.85: the other player's getting in is no longer cancelled right after it starts; 0.84: getting in and out of cars plays the walk, door and seat on both screens (no exit on entering), the other player no longer climbs twice, wrecked cars blow up once; 0.83: F9 trace of climbing and getting in and out of cars; 0.82: car damage (smoke, fire, wrecks) from the game driving the car, no local explosions of the other game's cars, no create-remove loop for cars; 0.81: car hits no longer applied twice (cars flying off), getting in with the car state not interrupted; 0.80: getting in and out of cars (and pulling drivers out) with the player's own moves, no stuck prompts; 0.79: car hits happen on both screens, climbs keep going through their middle step and retry refused moves, pause menu no longer stuck after the host pauses, F6 always closes the overlay; 0.78: parked cars shared too (the joiner's own go), up to 52 cars, car models asked for at top priority, animated getting in; 0.77: a car a player takes stays the same car on both screens (no vanishing, no second car), climbs play out and hand back on landing; 0.76: no drivers left sitting in thin air, people fight the other player too, the other player gets in cars (and pulls drivers out) with the animation, passers-by take the joiner's punches; 0.75: own pedestrian spawner off and its model memory freed for the host's models, climbs and jumps not cut short; 0.74: people models asked for at top priority, own people removed before they show, climbing and other movement states shared, gang stand-ins for gang members; 0.73: knockdowns and hit reactions of people shared, no random models for passers-by, other player visible further away, movement recovers after jumps; 0.72: other player and copies never fight, crouch or run off on their own AI; crouching shared; 0.71: copies whose model never loads here are remade with a loaded one (no invisible people, no full pool); 0.70: people copy the host's animation sets and only do what the host's do, stand-ins while a model loads, steadier player following, joiner held while the host's game is paused by a prompt; 0.69: joiner's mission intro confirmed at once (no reset of synced characters), up to 96 people shared; 0.68: punched pedestrians stay shared, stuck copies named in the log; 0.67: the host's mission end reaches the joiner (rewards), no walking on the spot, wider takeover; 0.66: the host's mission drives the joiner's (cutscenes, character groups, teleports); 0.65: joiner's own mission characters hidden at once and harmless; 0.64: copies still loading take over the joiner's own mission characters when they appear; 0.63: the joiner's own mission characters follow the host's (no empty cutscenes, attackers shown); 0.62: the joiner's game doesn't pause the world, mission cutscenes from its own mission; 0.61: joiner runs the host's mission with the host's HUD, characters and ending; 0.60: other player's clothing in its own memory (torso/legs, garbled textures), hidden (not removed) during cutscenes; 0.58: other player dressed again when re-created (missing torso/legs), no falling over on top of other characters (breakdancing), joiner starts next to the host; 0.57: F9 rebuilds the other player's look (no crash), F8 compares with own player; players never stand inside each other (host got stuck), only the host skips cutscenes; host keeps working after the mission cutscene, no doubled people in cutscenes, overlay closes when connected; shared cutscenes (host skip skips both), joiner placed next to the host after them, other player keeps clothes on mission start; host pause freezes the joiner, no crash after mission cutscenes; smoother people (20 Hz, buffered, per-frame), no freeze after cutscenes, no false deaths; joiner hits and host enemies count across, deaths shared, mission markers hidden for the joiner, clothing kept after cutscenes; mission detection, F7 people dump, time of day from the host; missions follow the host; car colours; shared pedestrians with appearance and actions; per-frame player smoothing; shared traffic, shared cars (driver in charge, passengers), fresh character after leaving range");
  if (api->hook(0x82209E30, UpdateHook, &original_update) != 0) return 2;
  api->hook(0x82212728, MissionStartHook, &original_mission_start);
  api->hook(0x821F9098, CutsceneStartHook, &original_cutscene_start);
  api->hook(0x827166D0, InputHook, &original_input);
  api->hook(0x822EF440, ObjectiveHook, &original_objective);
  api->hook(0x822E52C8, HelpHook, &original_help);
  api->hook(0x82215360, MissionEndHook, &original_mission_end);
  api->hook(0x8220C778, PauseHook, &original_pause);
  api->hook(0x823494D0, MenuContextHook, &original_menu_context);
  api->hook(0x822E8C78, Team0DotHook, &original_team0_dot);     // (other players on the maps, 2.1.4)
  api->hook(0x822E8FE8, FollowersHook, &original_followers);
  api->hook(0x822B3F70, PauseMapHook, &original_pause_map);
  api->hook(0x824C43B0, GroupCreateHook, &original_group_create);
  api->hook(0x824C4B58, GroupDestroyHook, &original_group_destroy);
  InstallTraces();
  InstallEventHooks();
  api->on_frame(OnFrame, nullptr);
  api->on_game_frame(GameFrame, nullptr);
  api->log(self, "Loaded Whompays Co-op prototype. F6 opens the session overlay.");
  return 0;
}

