// Co-op v2 world stream (2.2): the records, the owner's registry and the
// per-member scheduler, modelled on the game's own multiplayer (report Q 2.1:
// remote_object_create / destroy reliable, update unreliable, one priority
// list per connection cut to a byte budget by 82353DB0 under 8235AE50).
//
// The owner (the host in 2.2) keeps a Registry of everything its game
// simulates, fed by the create / destroy hooks in coop.cpp. Every 50 ms per
// member, Scheduler::Plan turns it into creates (WWC1, reliable, at most 4 a
// tick), updates (WWS1, unreliable, within the member's byte budget) and
// removes (WWR1, reliable, only to members the id was created on), plus a
// set digest (WWD1) every 10 s. Members answer with WWA1 (what they hold,
// what they declined); the owner never removes for pool reasons.
//
// No game code here: time, positions and camera eyes are inputs; the same
// file runs in the mod and in tests/world_test.cpp. Plan is deterministic
// for equal inputs (map order, full tie-breaks).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace coopworld {

constexpr int kMaxSlots = 12;
constexpr int kMaxMessage = 1170 - 16 - 8;   // coopnet::kMaxMessage (one packet's payload)

// Create kinds (WWC1): cars below 16, people from 16.
enum : uint8_t {
  kKindTraffic = 1, kKindParked = 2, kKindResponseCar = 3, kKindSpecialCar = 4, kKindScriptCar = 5,
  kKindAmbient = 16, kKindCrew = 17, kKindResponseUnit = 18, kKindHood = 19, kKindSpecialPerson = 20, kKindMissionPerson = 21,
};
inline bool IsCar(uint8_t kind) { return kind < 16; }

// PedRec / SeatedRec flags (byte 11 of both records: a reader peeks it to tell them apart).
enum : uint8_t {
  kPedSign = 1, kPedMission = 2, kPedDead = 4, kPedCrouch = 8, kPedRagdoll = 16, kPedBurning = 32, kPedHidden = 64, kPedSeated = 128,
};
// WWR1 reasons (1 "out of interest" does not exist in 2.2; 2 "trimmed" never did).
enum : uint8_t { kRemoveGone = 0, kRemoveInterest = 1, kRemoveAreaLeft = 3 };
// Registry entry flags.
enum : uint8_t {
  kEntryRest = 1,      // parked / resting car or a seated person: 2 Hz unless something changed
  kEntryInactive = 2,  // not streamed this frame (chunk unloaded, +120 0x04000000 clear); counted
  kEntryExcluded = 4,  // never streamed (train car, named story character); counted in the owner line
  kEntryPlayerCar = 8, // a car a player sits in or gets into (authority: that player, the owner's own included): its pose carries it, no record
};

constexpr uint8_t kSolo = 0xFF;             // OwnerMapRec.area: the slot runs its own world
constexpr uint8_t kAuthorityPlayer = 0x80;  // CreateRec.authority: a player's pose carries the car (that player's slot in the low bits; the owner's own player too)

#pragma pack(push, 1)
// WWS1: unreliable, unicast per member; then n_cars CarRec and n_people PedRec / SeatedRec.
struct UpdateHeader {
  char magic[4];      // WWS1
  uint8_t owner;      // owner slot (the records' namespace)
  uint16_t epoch;     // OwnerMap epoch
  uint32_t time;      // owner clock, ms
  float ref[3];       // positions are offsets from here (the owner's stream point 82485DB0)
  uint8_t n_cars, n_people;
};
struct CarRec {
  uint32_t id;        // the owner's handle
  uint16_t model;     // vehicle info index
  int16_t pos[3];     // 2 cm from ref (+-655 m)
  int16_t rot[3];     // smallest-three quaternion, 1/32767; rot[0] low 2 bits = the dropped component
  int16_t vel[3];     // 1/128 m/s
  int8_t ang[3];      // 1/16 rad/s
  uint8_t damage;     // CarDamage (bits 0 fire, 1 smoke, 2 wrecked, 5 heavy smoke) | CarSiren (3 lights, 4 siren); bits 6-7 spare (Package C)
  uint8_t hp;         // CarHealth 0-255
};
struct PedRec {         // a person on foot
  uint32_t id;
  int16_t pos[3];     // 2 cm from ref
  uint8_t yaw;        // heading, 256 steps per turn (1.4 degrees)
  uint8_t flags;      // kPed* (kPedSeated clear)
  uint8_t state;      // +516 << 4 | +520
  uint8_t anim;       // +952
  uint16_t action;    // +756
  int8_t vel[2];      // x / z, 1/16 m/s
  uint8_t team;       // +232
  uint8_t weapon;     // gun in hand: index + 1, 0 none
};
struct SeatedRec {      // a person in a car: one id with car + seat
  uint32_t id, car;
  uint8_t seat, team, weapon;
  uint8_t flags;      // kPed* with kPedSeated set (byte 11, as PedRec)
};
// WWC1: reliable, unicast, one per object per member. Tail: name[name_len],
// look[look_len] (8238B0B8 block), paint[paint_len] (CarLookPacket content),
// u32 crew[n_crew], then the first full record (CarRec / PedRec / SeatedRec).
struct CreateRec {
  char magic[4];      // WWC1
  uint32_t id, time;
  float ref[3];       // the first record's positions are offsets from here (the owner's stream point at the create)
  uint8_t kind, authority;
  uint16_t model;     // vehicle info index or people model id
  uint8_t team;
  uint8_t name_len, look_len;   // <= kMaxName / kMaxLook
  uint16_t paint_len;           // <= kMaxPaint
  uint32_t car;       // seated people: the car id (0 on foot)
  uint8_t seat, n_crew;         // cars: the ids of the crew that follow
};
// WWR1: reliable, only to members the id was created on.
struct RemoveRec {
  char magic[4];      // WWR1
  uint32_t id, time;
  uint16_t fade_ms;   // fade then destroy (0: at once)
  uint8_t reason;     // kRemove*
};
// WWA1: unreliable, member -> owner, once a second and on change. Tail: u16 models[n_models], u32 declined[n_declined].
struct AdmitRec {
  char magic[4];      // WWA1
  uint16_t cars_held, cars_cap, people_held, people_cap;
  uint64_t chunks;    // bit i: chunk list index i loaded here
  uint8_t dropped_model, dropped_chunk;   // 10 s counts
  uint8_t near_cars, near_people;         // copies shown within 50 m of the member's camera (the owner's World match line)
  uint8_t n_models;   // people models resident here
  uint8_t n_declined; // declined or evicted since the last report (<= kMaxDeclined)
};
// WWD1: reliable, every 10 s per member: the ids the member was offered, sorted. Tail: u32 ids[n].
struct DigestRec {
  char magic[4];      // WWD1
  uint8_t owner, part, parts, n;
  uint32_t time;      // owner clock
  uint32_t hash;      // DigestHash of the whole set
};
// WOM1: host -> all, reliable on change and once a second unreliable.
struct OwnerMapRec {
  char magic[4];      // WOM1
  uint16_t epoch;     // changes only when a slot's owner changes
  struct { uint8_t area, owner; } slots[kMaxSlots];   // area kSolo: its own world
};
#pragma pack(pop)
static_assert(sizeof(UpdateHeader) == 25 && sizeof(CarRec) == 29 && sizeof(PedRec) == 20 && sizeof(SeatedRec) == 12, "wire format");
static_assert(sizeof(CreateRec) == 39 && sizeof(RemoveRec) == 15 && sizeof(AdmitRec) == 26 && sizeof(DigestRec) == 16 && sizeof(OwnerMapRec) == 30, "wire format");
static_assert(offsetof(PedRec, flags) == 11 && offsetof(SeatedRec, flags) == 11, "a reader peeks byte 11 for kPedSeated");

constexpr size_t kMaxName = 46, kMaxLook = 127, kMaxPaint = 320, kMaxDeclined = 32, kMaxModels = 32;
constexpr size_t kDigestMaxIds = 80;    // per WWD1 message (more go in parts)

inline bool HasMagic(const void* p, const char* m) { return std::memcmp(p, m, 4) == 0; }
inline void SetMagic(char* dst, const char* m) { std::memcpy(dst, m, 4); }

// Quantisation. Positions 2 cm, velocities 1/128 m/s, angular 1/16 rad/s,
// people 1/16 m/s and 1.4 degrees; a record whose offset saturates is skipped.
constexpr float kPosUnit = 0.02f, kPosMax = 32767 * kPosUnit;
constexpr float kVelUnit = 1.0f / 128, kAngUnit = 1.0f / 16, kPedVelUnit = 1.0f / 16;
constexpr float kPi = 3.14159265358979f;
inline int32_t Round(float v) { return int32_t(std::lround(v)); }
inline int16_t Clamp16(int32_t v) { return int16_t(std::max(-32767, std::min(32767, v))); }
inline int8_t Clamp8(int32_t v) { return int8_t(std::max(-127, std::min(127, v))); }
inline bool QPos(const float ref[3], const float pos[3], int16_t out[3]) {
  for (int i = 0; i < 3; ++i) {
    const int32_t v = Round((pos[i] - ref[i]) / kPosUnit);
    if (v < -32767 || v > 32767) return false;
    out[i] = int16_t(v);
  }
  return true;
}
inline void UnqPos(const float ref[3], const int16_t q[3], float out[3]) { for (int i = 0; i < 3; ++i) out[i] = ref[i] + q[i] * kPosUnit; }
inline int16_t QVel(float v) { return Clamp16(Round(v / kVelUnit)); }
inline float UnqVel(int16_t q) { return q * kVelUnit; }
inline int8_t QAng(float v) { return Clamp8(Round(v / kAngUnit)); }
inline float UnqAng(int8_t q) { return q * kAngUnit; }
inline int8_t QPedVel(float v) { return Clamp8(Round(v / kPedVelUnit)); }
inline float UnqPedVel(int8_t q) { return q * kPedVelUnit; }
inline uint8_t QYaw(float rad) { return uint8_t(Round(rad * (128 / kPi)) & 255); }
inline float UnqYaw(uint8_t q) { return q * (kPi / 128); }
inline float YawOf(const float fwd[3]) { return std::atan2(fwd[0], fwd[2]); }   // y up: heading as coop.cpp's atan2(fx, fz)
inline uint8_t QHp(float hp, float max) { return uint8_t(std::max(0, std::min(255, max > 0 ? Round(255 * hp / max) : 0))); }

// Orientation rows (+32 right, +44 up, +56 forward, as PosePacket.rows) ->
// quaternion (x, y, z, w), smallest three: the largest component is dropped,
// its index goes in the low two bits of rot[0] (that one keeps 1/8192).
inline void QRot(const float rows[9], int16_t out[3]) {
  const float* m = rows;   // m[3 * row + col]
  float q[4];
  const float t = m[0] + m[4] + m[8];
  if (t > 0) { const float s = std::sqrt(t + 1) * 2; q[3] = 0.25f * s; q[0] = (m[7] - m[5]) / s; q[1] = (m[2] - m[6]) / s; q[2] = (m[3] - m[1]) / s; }
  else if (m[0] > m[4] && m[0] > m[8]) { const float s = std::sqrt(1 + m[0] - m[4] - m[8]) * 2; q[3] = (m[7] - m[5]) / s; q[0] = 0.25f * s; q[1] = (m[1] + m[3]) / s; q[2] = (m[2] + m[6]) / s; }
  else if (m[4] > m[8]) { const float s = std::sqrt(1 + m[4] - m[0] - m[8]) * 2; q[3] = (m[2] - m[6]) / s; q[0] = (m[1] + m[3]) / s; q[1] = 0.25f * s; q[2] = (m[5] + m[7]) / s; }
  else { const float s = std::sqrt(1 + m[8] - m[0] - m[4]) * 2; q[3] = (m[3] - m[1]) / s; q[0] = (m[2] + m[6]) / s; q[1] = (m[5] + m[7]) / s; q[2] = 0.25f * s; }
  int big = 0;
  for (int i = 1; i < 4; ++i) if (std::fabs(q[i]) > std::fabs(q[big])) big = i;
  if (q[big] < 0) for (float& v : q) v = -v;
  float a[3]; int n = 0;
  for (int i = 0; i < 4; ++i) if (i != big) a[n++] = std::max(-1.0f, std::min(1.0f, q[i]));
  out[0] = int16_t((std::max(-8191, std::min(8191, Round(a[0] * 8191.75f))) * 4) | big);
  out[1] = Clamp16(Round(a[1] * 32767));
  out[2] = Clamp16(Round(a[2] * 32767));
}
inline void UnqRot(const int16_t in[3], float rows[9]) {
  const int big = in[0] & 3;
  const float a[3] = {int16_t(in[0] & ~3) / 32767.0f, in[1] / 32767.0f, in[2] / 32767.0f};
  float q[4]; int n = 0;
  for (int i = 0; i < 4; ++i) if (i != big) q[i] = a[n++];
  q[big] = std::sqrt(std::max(0.0f, 1 - a[0] * a[0] - a[1] * a[1] - a[2] * a[2]));
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  rows[0] = 1 - 2 * (y * y + z * z); rows[1] = 2 * (x * y - z * w);     rows[2] = 2 * (x * z + y * w);
  rows[3] = 2 * (x * y + z * w);     rows[4] = 1 - 2 * (x * x + z * z); rows[5] = 2 * (y * z - x * w);
  rows[6] = 2 * (x * z - y * w);     rows[7] = 2 * (y * z + x * w);     rows[8] = 1 - 2 * (x * x + y * y);
}

// Variable messages: write into a buffer (returns the length, 0 when it would
// not fit), read back (false when malformed). Tails follow the fixed head.
struct CreateMsg {
  CreateRec head{};
  std::string name;
  std::vector<uint8_t> look, paint;
  std::vector<uint32_t> crew;
  std::vector<uint8_t> first;   // the first full record (CarRec / PedRec / SeatedRec bytes)
};
inline size_t WriteCreate(const CreateMsg& c, uint8_t* out, size_t cap) {
  if (c.name.size() > kMaxName || c.look.size() > kMaxLook || c.paint.size() > kMaxPaint || c.crew.size() > 255) return 0;
  CreateRec h = c.head;
  SetMagic(h.magic, "WWC1");
  h.name_len = uint8_t(c.name.size()); h.look_len = uint8_t(c.look.size()); h.paint_len = uint16_t(c.paint.size()); h.n_crew = uint8_t(c.crew.size());
  const size_t n = sizeof(h) + c.name.size() + c.look.size() + c.paint.size() + 4 * c.crew.size() + c.first.size();
  if (n > cap) return 0;
  uint8_t* p = out;
  std::memcpy(p, &h, sizeof(h)); p += sizeof(h);
  std::memcpy(p, c.name.data(), c.name.size()); p += c.name.size();
  std::memcpy(p, c.look.data(), c.look.size()); p += c.look.size();
  std::memcpy(p, c.paint.data(), c.paint.size()); p += c.paint.size();
  for (uint32_t id : c.crew) { std::memcpy(p, &id, 4); p += 4; }
  std::memcpy(p, c.first.data(), c.first.size());
  return n;
}
inline bool ReadCreate(const uint8_t* d, size_t n, CreateMsg& c) {
  if (n < sizeof(CreateRec) || !HasMagic(d, "WWC1")) return false;
  std::memcpy(&c.head, d, sizeof(c.head));
  const CreateRec& h = c.head;
  const size_t fixed = sizeof(h) + h.name_len + h.look_len + h.paint_len + 4 * size_t(h.n_crew);
  if (h.name_len > kMaxName || h.look_len > kMaxLook || h.paint_len > kMaxPaint || fixed > n) return false;
  const uint8_t* p = d + sizeof(h);
  c.name.assign(reinterpret_cast<const char*>(p), h.name_len); p += h.name_len;
  c.look.assign(p, p + h.look_len); p += h.look_len;
  c.paint.assign(p, p + h.paint_len); p += h.paint_len;
  c.crew.resize(h.n_crew);
  for (uint32_t& id : c.crew) { std::memcpy(&id, p, 4); p += 4; }
  c.first.assign(p, d + n);
  const size_t want = IsCar(h.kind) ? sizeof(CarRec) : h.car ? sizeof(SeatedRec) : sizeof(PedRec);
  return c.first.size() == want;
}

struct AdmitMsg {
  AdmitRec head{};
  std::vector<uint16_t> models;
  std::vector<uint32_t> declined;
};
inline size_t WriteAdmit(const AdmitMsg& a, uint8_t* out, size_t cap) {
  if (a.models.size() > kMaxModels || a.declined.size() > kMaxDeclined) return 0;
  AdmitRec h = a.head;
  SetMagic(h.magic, "WWA1");
  h.n_models = uint8_t(a.models.size()); h.n_declined = uint8_t(a.declined.size());
  const size_t n = sizeof(h) + 2 * a.models.size() + 4 * a.declined.size();
  if (n > cap) return 0;
  uint8_t* p = out;
  std::memcpy(p, &h, sizeof(h)); p += sizeof(h);
  for (uint16_t m : a.models) { std::memcpy(p, &m, 2); p += 2; }
  for (uint32_t id : a.declined) { std::memcpy(p, &id, 4); p += 4; }
  return n;
}
inline bool ReadAdmit(const uint8_t* d, size_t n, AdmitMsg& a) {
  if (n < sizeof(AdmitRec) || !HasMagic(d, "WWA1")) return false;
  std::memcpy(&a.head, d, sizeof(a.head));
  const AdmitRec& h = a.head;
  if (h.n_models > kMaxModels || h.n_declined > kMaxDeclined || sizeof(h) + 2 * size_t(h.n_models) + 4 * size_t(h.n_declined) != n) return false;
  const uint8_t* p = d + sizeof(h);
  a.models.resize(h.n_models); for (uint16_t& m : a.models) { std::memcpy(&m, p, 2); p += 2; }
  a.declined.resize(h.n_declined); for (uint32_t& id : a.declined) { std::memcpy(&id, p, 4); p += 4; }
  return true;
}

// WWS1 messages from one tick's records: cars, then people on foot, then
// seated; a new message (with the header again) whenever the next record
// would not fit. A reader tells PedRec from SeatedRec by byte 11 (flags).
struct UpdateMsg {
  UpdateHeader head{};
  std::vector<CarRec> cars;
  std::vector<PedRec> peds;
  std::vector<SeatedRec> seated;
};
inline std::vector<std::vector<uint8_t>> PackUpdates(UpdateHeader head, const std::vector<CarRec>& cars, const std::vector<PedRec>& peds, const std::vector<SeatedRec>& seated) {
  std::vector<std::vector<uint8_t>> out;
  SetMagic(head.magic, "WWS1");
  size_t ci = 0, pi = 0, si = 0;
  auto fits = [](const std::vector<uint8_t>& m, size_t n) { return m.size() + n <= size_t(kMaxMessage); };
  while (ci < cars.size() || pi < peds.size() || si < seated.size()) {
    std::vector<uint8_t> m(sizeof(head));
    uint8_t nc = 0, np = 0;
    for (; ci < cars.size() && nc < 255 && fits(m, sizeof(CarRec)); ++ci, ++nc) { const auto* b = reinterpret_cast<const uint8_t*>(&cars[ci]); m.insert(m.end(), b, b + sizeof(CarRec)); }
    for (; pi < peds.size() && np < 255 && fits(m, sizeof(PedRec)); ++pi, ++np) { const auto* b = reinterpret_cast<const uint8_t*>(&peds[pi]); m.insert(m.end(), b, b + sizeof(PedRec)); }
    for (; si < seated.size() && np < 255 && fits(m, sizeof(SeatedRec)); ++si, ++np) { const auto* b = reinterpret_cast<const uint8_t*>(&seated[si]); m.insert(m.end(), b, b + sizeof(SeatedRec)); }
    head.n_cars = nc; head.n_people = np;
    std::memcpy(m.data(), &head, sizeof(head));
    out.push_back(std::move(m));
  }
  return out;
}
inline bool ReadUpdate(const uint8_t* d, size_t n, UpdateMsg& u) {
  if (n < sizeof(UpdateHeader) || !HasMagic(d, "WWS1")) return false;
  std::memcpy(&u.head, d, sizeof(u.head));
  u.cars.clear(); u.peds.clear(); u.seated.clear();
  size_t o = sizeof(u.head);
  for (int i = 0; i < u.head.n_cars; ++i) {
    if (o + sizeof(CarRec) > n) return false;
    CarRec c; std::memcpy(&c, d + o, sizeof(c)); o += sizeof(c); u.cars.push_back(c);
  }
  for (int i = 0; i < u.head.n_people; ++i) {
    if (o + sizeof(SeatedRec) > n) return false;
    if (d[o + 11] & kPedSeated) { SeatedRec s; std::memcpy(&s, d + o, sizeof(s)); o += sizeof(s); u.seated.push_back(s); }
    else { if (o + sizeof(PedRec) > n) return false; PedRec p; std::memcpy(&p, d + o, sizeof(p)); o += sizeof(p); u.peds.push_back(p); }
  }
  return o == n;
}

// Set digest: FNV-1a over the sorted ids; WWD1 messages of at most
// kDigestMaxIds each; the member collects the parts and diffs.
inline uint32_t DigestHash(const std::vector<uint32_t>& sorted_ids) {
  uint32_t h = 2166136261u;
  for (uint32_t id : sorted_ids) for (int i = 0; i < 4; ++i) { h ^= (id >> (8 * i)) & 255; h *= 16777619u; }
  return h;
}
inline std::vector<std::vector<uint8_t>> WriteDigest(uint8_t owner, uint32_t time, const std::vector<uint32_t>& sorted_ids) {
  std::vector<std::vector<uint8_t>> out;
  const size_t parts = std::max<size_t>(1, (sorted_ids.size() + kDigestMaxIds - 1) / kDigestMaxIds);
  if (parts > 255) return out;
  const uint32_t hash = DigestHash(sorted_ids);
  for (size_t p = 0; p < parts; ++p) {
    const size_t from = p * kDigestMaxIds, to = std::min(sorted_ids.size(), from + kDigestMaxIds);
    DigestRec h{}; SetMagic(h.magic, "WWD1"); h.owner = owner; h.part = uint8_t(p); h.parts = uint8_t(parts); h.n = uint8_t(to - from); h.time = time; h.hash = hash;
    std::vector<uint8_t> m(sizeof(h) + 4 * (to - from));
    std::memcpy(m.data(), &h, sizeof(h));
    if (to > from) std::memcpy(m.data() + sizeof(h), sorted_ids.data() + from, 4 * (to - from));
    out.push_back(std::move(m));
  }
  return out;
}
// Member side: the parts of one digest (same owner time). Add returns true once all are in.
struct DigestParts {
  uint32_t time = 0, hash = 0; uint8_t parts = 0;
  std::vector<std::vector<uint32_t>> got;
  std::vector<bool> have;
  bool Add(const uint8_t* d, size_t n) {
    if (n < sizeof(DigestRec) || !HasMagic(d, "WWD1")) return false;
    DigestRec h; std::memcpy(&h, d, sizeof(h));
    if (!h.parts || h.part >= h.parts || h.n > kDigestMaxIds || sizeof(h) + 4 * size_t(h.n) != n) return false;
    if (h.time != time || h.parts != parts || have.size() != h.parts) { time = h.time; parts = h.parts; hash = h.hash; got.assign(h.parts, {}); have.assign(h.parts, false); }
    got[h.part].resize(h.n);
    if (h.n) std::memcpy(got[h.part].data(), d + sizeof(h), 4 * h.n);
    have[h.part] = true;
    return std::all_of(have.begin(), have.end(), [](bool b) { return b; });
  }
  std::vector<uint32_t> Ids() const { std::vector<uint32_t> all; for (auto& g : got) all.insert(all.end(), g.begin(), g.end()); return all; }
};
// ids the owner lists that I don't hold (missing: a lost create) and ids I
// hold that it no longer lists (extra: a lost remove). Both inputs sorted.
inline void DigestDiff(const std::vector<uint32_t>& owner_ids, const std::vector<uint32_t>& my_ids, std::vector<uint32_t>& missing, std::vector<uint32_t>& extra) {
  missing.clear(); extra.clear();
  std::set_difference(owner_ids.begin(), owner_ids.end(), my_ids.begin(), my_ids.end(), std::back_inserter(missing));
  std::set_difference(my_ids.begin(), my_ids.end(), owner_ids.begin(), owner_ids.end(), std::back_inserter(extra));
}

// Scheduler constants: the MP's priority (-10 a metre, +500 in the cone or
// within 10 m, cone dot > 0.97: 0x820899BC / 0x820899C0 / 0x82038164) plus
// the age boost (ms since the last send / 10) and +200 for people whose
// model the member has resident (creates only); it orders the creates and
// breaks ties among updates (a fair queue per band keeps those from
// starving, see Scheduler). Bands by distance to the member's eye: 20 / 10 /
// 2 Hz targets, proportional shares when short.
constexpr float kPrioDist = -10.0f, kPrioCone = 500.0f, kPrioModel = 200.0f, kConeDot = 0.97f, kConeNear = 10.0f;
constexpr uint32_t kAgeDiv = 10;
constexpr uint32_t kTickMs = 50;
constexpr float kBandNear = 50.0f, kBandMid = 150.0f;
constexpr uint32_t kBandMs[3] = {50, 100, 500};   // near 20 Hz, mid 10 Hz, far 2 Hz
constexpr uint32_t kRestMs = 500;                // kEntryRest objects unless changed since the last send
constexpr float kTagMax = 60000.0f;              // a start tag's range (ms of the band's virtual time; a guard, not a limit)
constexpr int kCreatesPerTick = 4;
constexpr uint32_t kDeclineMs = 3000, kDigestMs = 10000, kBackpressureMs = 2000;
// A declined id waits kDeclineMs, doubled on each repeat decline up to
// kDeclineMaxMs (the count is forgotten once a create has held that long);
// a person is re-offered only while the member can load its model (it has it,
// or fewer than kModelSlots people models are loaded there).
constexpr uint32_t kDeclineMaxMs = 30000;
constexpr size_t kModelSlots = 11;
constexpr uint32_t kBudgetMinBps = 6000, kBudgetMaxBps = 24000;
constexpr uint32_t kBackpressureBacklog = 16 * 1024;
constexpr uint16_t kCreateBytesCar = 120, kCreateBytesPed = 160;   // charged per create until the game thread knows the look sizes
constexpr uint16_t kDefaultFadeMs = 1000;
constexpr uint32_t kInactiveMs = 5000;   // an entry not streaming this long is removed where it was created

// Bytes per 50 ms tick per member: world_upload_kbps shared by the members, clamped to 6-24 KB/s each.
inline uint32_t TickBudget(uint32_t kbps, int members) {
  const uint32_t bps = std::max(kBudgetMinBps, std::min(kBudgetMaxBps, kbps * 1000 / 8 / uint32_t(std::max(1, members))));
  return bps * kTickMs / 1000;
}

using Key = uint64_t;   // (owner slot, id)
inline Key MakeKey(uint8_t owner, uint32_t id) { return (Key(owner) << 32) | id; }
inline uint8_t OwnerOf(Key k) { return uint8_t(k >> 32); }
inline uint32_t IdOf(Key k) { return uint32_t(k); }

// Per-member offer state of one entry.
struct Offer {
  uint64_t last_sent = 0;       // create or update time (the age boost)
  uint64_t offered_at = 0;      // the last create's time
  uint64_t declined_until = 0;  // declined by the member: no updates, re-offered after this when it has room
  float tag = 0;                // start tag (start-time fair queuing): ms of its band's virtual time until its turn; +interval a send
  uint8_t band = 255;           // the band it was last planned in (a change starts the tag over at the band's virtual now)
  uint8_t declines = 0;         // declines in a row (the hold doubles each time)
  bool created = false;         // a create went to this member and was not declined / removed since
};

struct Entry {
  uint8_t owner = 0, kind = 0, source = 0, authority = 0, flags = 0, seat = 0;
  int16_t chunk = -1;           // chunk list index when known (parked records' +56), else -1
  uint16_t model = 0;
  uint16_t create_bytes = 0;    // the member's create size (0: the default by kind)
  uint32_t id = 0, car = 0;     // car: a seated person's car id
  uint32_t activations = 1;     // the same human coming through 8247A6D0 again (model reload, queued mission NPC)
  uint64_t created_at = 0, changed_at = 0;   // changed_at: hp / flags / occupant changed (a resting object sends at band rate once)
  uint64_t inactive_since = 0;  // not streaming since (Plan keeps it)
  float pos[3] = {0, 0, 0};
  std::string name;             // people: the model name for the create
  Offer offer[kMaxSlots];
  bool Seated() const { return !IsCar(kind) && car != 0; }
  size_t RecordBytes() const { return IsCar(kind) ? sizeof(CarRec) : Seated() ? sizeof(SeatedRec) : sizeof(PedRec); }
  uint16_t CreateBytes() const { return create_bytes ? create_bytes : IsCar(kind) ? kCreateBytesCar : kCreateBytesPed; }
  bool Streams() const { return !(flags & (kEntryInactive | kEntryExcluded)); }
};

struct Removal { Key key = 0; uint8_t kind = 0, reason = 0; uint16_t fade_ms = 0; bool created[kMaxSlots] = {}; };

// Every game runs one; the owner streams from it. Keyed (owner, id): map
// order makes Plan deterministic.
class Registry {
 public:
  // Idempotent: the same object again keeps created_at and counts an activation.
  Entry& Add(uint8_t owner, uint32_t id, uint8_t kind, uint8_t source, uint64_t now) {
    auto [it, fresh] = entries_.try_emplace(MakeKey(owner, id));
    Entry& e = it->second;
    if (fresh) { e.owner = owner; e.id = id; e.kind = kind; e.source = source; e.authority = owner; e.created_at = now; e.changed_at = now; ++added_; }
    else { ++e.activations; ++reactivated_; }
    return e;
  }
  // Gone in the owner's game (or fading there with fade_ms left): members it was created on get a WWR1.
  bool Remove(uint8_t owner, uint32_t id, uint16_t fade_ms = kDefaultFadeMs, uint8_t reason = kRemoveGone) {
    auto it = entries_.find(MakeKey(owner, id));
    if (it == entries_.end()) return false;
    Removal r; r.key = it->first; r.kind = it->second.kind; r.reason = reason; r.fade_ms = fade_ms;
    for (int s = 0; s < kMaxSlots; ++s) r.created[s] = it->second.offer[s].created;
    pending_.push_back(r);
    entries_.erase(it);
    ++removed_;
    return true;
  }
  Entry* Find(uint8_t owner, uint32_t id) { auto it = entries_.find(MakeKey(owner, id)); return it == entries_.end() ? nullptr : &it->second; }
  const Entry* Find(uint8_t owner, uint32_t id) const { auto it = entries_.find(MakeKey(owner, id)); return it == entries_.end() ? nullptr : &it->second; }
  std::map<Key, Entry>& Entries() { return entries_; }
  const std::map<Key, Entry>& Entries() const { return entries_; }
  size_t Size() const { return entries_.size(); }
  void Clear() { entries_.clear(); pending_.clear(); }
  std::vector<Removal> TakeRemovals() { std::vector<Removal> r; r.swap(pending_); return r; }
  // The owner line's churn baseline: the game's own creations and removals since the last take.
  void TakeCounts(uint32_t& added, uint32_t& removed, uint32_t& reactivated) { added = added_; removed = removed_; reactivated = reactivated_; added_ = removed_ = reactivated_ = 0; }
  int CountKind(uint8_t kind) const { int n = 0; for (auto& [k, e] : entries_) n += e.kind == kind; return n; }

 private:
  std::map<Key, Entry> entries_;
  std::vector<Removal> pending_;
  uint32_t added_ = 0, removed_ = 0, reactivated_ = 0;
};

// The owner's view of a member. eye / fwd from its poses (PosePacket.eye,
// cam row 3); caps, held, chunks and models from its WWA1.
struct BandStat { uint32_t objects = 0, planned = 0, bytes = 0, rest_objects = 0, rest_planned = 0; };   // summed over ticks; the 10 s line divides (rest_*: the 2 Hz resting ones among them)
struct Member {
  bool present = false, in_area = false;
  float eye[3] = {0, 0, 0}, fwd[3] = {0, 0, 1};
  uint64_t chunks = ~0ull;      // all loaded until it reports
  uint16_t cars_held = 0, cars_cap = 0xFFFF, people_held = 0, people_cap = 0xFFFF;
  std::vector<uint16_t> models; // resident people models, sorted
  uint32_t budget = 570;        // bytes per tick (TickBudget)
  uint64_t halved_until = 0;    // backpressure: half budget until then
  uint64_t last_digest = 0, last_admit = 0;
  // Scheduler state and the 10 s line's counters.
  bool was_in_area = false;
  uint32_t ticks = 0;
  BandStat band[3];
  uint32_t creates = 0, reoffers = 0, declines = 0, removes = 0, bytes = 0, over_create_budget = 0, skipped_chunk = 0, skipped_far = 0, skipped_crew = 0, halvings = 0;
  bool HasRoom(uint8_t kind) const { return IsCar(kind) ? cars_held < cars_cap : people_held < people_cap; }
  bool HasModel(uint16_t model) const { return std::binary_search(models.begin(), models.end(), model); }
  void ResetStats() { ticks = 0; for (auto& b : band) b = {}; creates = reoffers = declines = removes = bytes = over_create_budget = skipped_chunk = skipped_far = skipped_crew = halvings = 0; }
};

inline float Dist(const float a[3], const float b[3]) { const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2]; return std::sqrt(dx * dx + dy * dy + dz * dz); }
inline bool InCone(const float eye[3], const float fwd[3], const float pos[3], float d) {
  if (d < 1e-3f) return true;
  return ((pos[0] - eye[0]) * fwd[0] + (pos[1] - eye[1]) * fwd[1] + (pos[2] - eye[2]) * fwd[2]) / d > kConeDot;
}
inline int BandOf(float d) { return d <= kBandNear ? 0 : d <= kBandMid ? 1 : 2; }

struct Planned { Key key = 0; uint8_t kind = 0, band = 0; uint16_t bytes = 0; float priority = 0; };
struct PlannedRemove { Key key = 0; uint8_t reason = 0; uint16_t fade_ms = 0; };
struct MemberPlan {
  std::vector<Planned> creates, updates;   // in send order (crew right after its car)
  std::vector<PlannedRemove> removes;
  std::vector<uint32_t> digest;            // sorted ids, when digest_due
  uint32_t digest_hash = 0;
  bool digest_due = false;
  uint32_t budget = 0, bytes = 0, messages = 0;   // this tick: the budget, what the plan costs, WWS1 messages needed
};

class Scheduler {
 public:
  // One tick for every member: creates (<= 4, charged against the budget;
  // the first always goes), then updates by band share and priority, then
  // the registry's removals and the digest. Plans index by slot.
  std::array<MemberPlan, kMaxSlots> Plan(uint64_t now, Registry& reg, std::array<Member, kMaxSlots>& members, const float ref[3]) {
    std::array<MemberPlan, kMaxSlots> plans;
    const std::vector<Removal> removals = reg.TakeRemovals();
    for (auto& [k, e] : reg.Entries()) { if (e.Streams()) e.inactive_since = 0; else if (!e.inactive_since) e.inactive_since = now; }
    for (int s = 0; s < kMaxSlots; ++s) {
      Member& m = members[s];
      MemberPlan& p = plans[s];
      for (const Removal& r : removals) if (r.created[s]) { p.removes.push_back({r.key, r.reason, r.fade_ms}); ++m.removes; }
      if (!(m.present && m.in_area)) {
        // Left the area (reason 3 for what it held) or the session (nothing): the offers start over.
        for (auto& [k, e] : reg.Entries()) {
          Offer& o = e.offer[s];
          if (o.created && m.was_in_area && m.present) { p.removes.push_back({k, kRemoveAreaLeft, kDefaultFadeMs}); ++m.removes; }
          o = Offer{};
        }
        m.was_in_area = false;
        continue;
      }
      if (!m.was_in_area) { m.was_in_area = true; m.last_digest = now; }
      ++m.ticks;
      PlanMember(now, reg, m, s, ref, p);
    }
    return plans;
  }

  // A member's WWA1: caps, held, models, chunks; its declined ids are not
  // re-offered for 3 s (6, 12, ... 30 s when declined again) and then only
  // while it reports room (counting the creates planned since).
  void OnAdmit(uint64_t now, Registry& reg, Member& m, int slot, uint8_t owner, const AdmitMsg& a) {
    m.cars_held = a.head.cars_held; m.cars_cap = a.head.cars_cap; m.people_held = a.head.people_held; m.people_cap = a.head.people_cap;
    m.chunks = a.head.chunks;
    m.models = a.models; std::sort(m.models.begin(), m.models.end());
    m.last_admit = now;
    for (uint32_t id : a.declined)
      if (Entry* e = reg.Find(owner, id)) {
        Offer& o = e->offer[slot];
        o.created = false; o.declined_until = now + std::min<uint64_t>(uint64_t(kDeclineMs) << std::min<int>(o.declines, 4), kDeclineMaxMs);
        if (o.declines < 255) ++o.declines;
        ++m.declines;
      }
  }
  // Backpressure (LinkStats): half the budget for 2 s.
  static void Backpressure(uint64_t now, Member& m) { if (now >= m.halved_until) ++m.halvings; m.halved_until = now + kBackpressureMs; }

 private:
  struct Cand {
    Entry* e; Offer* o;
    float priority, d, tag;     // tag: the sort key (its own start tag, a crew member's car's)
    int band; uint16_t bytes, unit; uint32_t anchor, interval;   // unit: a car with its due crew (sent together)
    bool crew, seated, streams, due, sent, rest; size_t car;
  };
  // Plan order: start-time fair queuing per band (the smallest start tag
  // first; a send moves the tag on by the object's interval, a new object
  // starts at the band's virtual now, which is 0 after each tick's rebase):
  // nothing starves, a short band slows every object alike whatever its
  // rate, a newcomer gets its turn in the current round. Ties by the MP
  // priority, then id; crew right after its car.
  static bool Before(const Cand& a, const Cand& b) {
    if (a.tag != b.tag) return a.tag < b.tag;
    if (a.priority != b.priority) return a.priority > b.priority;
    if (a.anchor != b.anchor) return a.anchor < b.anchor;
    if (a.crew != b.crew) return !a.crew;
    return a.e->id < b.e->id;
  }

  void PlanMember(uint64_t now, Registry& reg, Member& m, int s, const float ref[3], MemberPlan& p) {
    uint32_t budget = m.budget;
    if (now < m.halved_until) budget /= 2;
    p.budget = budget;
    // Candidates: streamed entries the member can know about, with the MP
    // priority, their band and, once created, their interval and start tag.
    std::vector<Cand> cands;
    cands.reserve(reg.Size());
    std::map<uint32_t, size_t> car_index;   // car id -> cand (for its crew)
    for (auto& [k, e] : reg.Entries()) {
      if (!e.Streams()) {
        // Not streaming (inactive: its chunk or model unloaded, a mission
        // person after the mission; excluded): no records. A member it was
        // created on keeps it (the digest still lists it: a model reload is
        // over in under 5 s); after kInactiveMs it is removed there (reason 0)
        // and created again when it streams.
        Offer& o = e.offer[s];
        if (o.created && e.inactive_since && now - e.inactive_since >= kInactiveMs) { p.removes.push_back({k, kRemoveGone, kDefaultFadeMs}); ++m.removes; o = Offer{}; }
        continue;
      }
      if (e.chunk >= 0 && e.chunk < 64 && !((m.chunks >> e.chunk) & 1)) { ++m.skipped_chunk; continue; }
      if (Dist(e.pos, ref) >= kPosMax) { ++m.skipped_far; continue; }
      Offer& o = e.offer[s];
      if (o.declined_until && (now < o.declined_until || !m.HasRoom(e.kind))) continue;
      if (o.declined_until && !IsCar(e.kind) && !m.HasModel(e.model) && m.models.size() >= kModelSlots) continue;   // (it would be declined "model" again)
      if (o.created && o.declines && now - o.offered_at >= kDeclineMaxMs) o.declines = 0;   // (held since: the next decline starts at 3 s)
      const float d = Dist(e.pos, m.eye);
      const bool cone = d < kConeNear || InCone(m.eye, m.fwd, e.pos, d);
      const uint64_t age = o.created ? now - o.last_sent : 0;
      float prio = kPrioDist * d + (cone ? kPrioCone : 0) + float(age) / kAgeDiv;
      if (!o.created && !IsCar(e.kind) && m.HasModel(e.model)) prio += kPrioModel;
      Cand c{&e, &o, prio, d, 0, BandOf(d), 0, 0, e.id, 0, false, false, false, false, false, false, size_t(-1)};
      c.seated = e.car && reg.Find(e.owner, e.car);   // (a car that is gone: on foot until its own hook says otherwise)
      c.bytes = uint16_t(IsCar(e.kind) ? sizeof(CarRec) : c.seated ? sizeof(SeatedRec) : sizeof(PedRec));
      // Streams: created before this tick (the create carries the first record) and not a car a player drives.
      c.streams = o.created && o.offered_at != now && !(IsCar(e.kind) && (e.authority != e.owner || (e.flags & kEntryPlayerCar)));
      if (c.streams) {
        c.rest = (e.flags & kEntryRest) && e.changed_at <= o.last_sent;
        c.interval = c.rest ? std::max(kRestMs, kBandMs[c.band]) : kBandMs[c.band];
        if (o.band != c.band) { o.band = uint8_t(c.band); o.tag = 0; }
        c.tag = o.tag;
        c.due = now + kTickMs / 2 >= o.last_sent + c.interval;
      }
      if (IsCar(e.kind)) car_index[e.id] = cands.size();
      cands.push_back(c);
    }
    // Crew inherits its car's priority, band and place and sorts right after
    // it; a seated person whose car the member cannot have is not offered alone.
    std::vector<size_t> idx;   // the candidates in plan order
    idx.reserve(cands.size());
    for (size_t i = 0; i < cands.size(); ++i) {
      Cand& c = cands[i];
      if (c.seated) {
        auto it = car_index.find(c.e->car);
        if (it == car_index.end()) { ++m.skipped_crew; continue; }
        const Cand& car = cands[it->second];
        c.priority = car.priority; c.band = car.band; c.tag = car.tag; c.anchor = car.e->id; c.crew = true; c.car = it->second;
        if (c.streams && c.o->band != c.band) { c.o->band = uint8_t(c.band); c.o->tag = 0; }
      }
      idx.push_back(i);
    }
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return Before(cands[a], cands[b]); });

    // Creates: priority order, at most kCreatesPerTick, charged against the
    // budget (the first always goes: catch-up must move on any budget). A crew
    // member waits for its car's create; a declined id is re-offered here.
    uint32_t spent = 0;
    int creates = 0;
    for (size_t i : idx) {
      Cand& c = cands[i];
      if (c.o->created || creates >= kCreatesPerTick) continue;
      if (c.crew && !cands[c.car].o->created) continue;
      if (c.o->declined_until && !m.HasRoom(c.e->kind)) continue;   // (the room the member reported is taken by this tick's creates)
      const uint16_t cost = c.e->CreateBytes();
      if (creates && spent + cost > budget) { ++m.over_create_budget; break; }
      spent += cost; ++creates;
      if (c.o->declined_until) ++m.reoffers;
      const uint8_t declines = c.o->declines;
      *c.o = Offer{}; c.o->created = true; c.o->offered_at = c.o->last_sent = now; c.o->declines = declines;
      // Counted as held until the member's next WWA1 says what it holds (a
      // re-offer only goes into reported room: one free slot, one re-offer).
      uint16_t& held = IsCar(c.e->kind) ? m.cars_held : m.people_held;
      if (held < 0xFFFF) ++held;
      p.creates.push_back({MakeKey(c.e->owner, c.e->id), c.e->kind, uint8_t(c.band), cost, c.priority});
      ++m.creates;
    }
    // Updates: due = the band's interval (500 ms at rest, unless changed) has
    // passed since the last send; shares per band proportional to the band's
    // target bytes, capped at its need; leftovers redistributed; within a
    // band in plan order (the smallest start tag first).
    uint32_t need[3] = {}, target[3] = {}, first[3] = {};   // first: the first due unit's bytes (a band's floor)
    for (size_t pi = 0; pi < idx.size(); ++pi) {
      Cand& c = cands[idx[pi]];
      if (!c.streams) continue;
      ++m.band[c.band].objects; m.band[c.band].rest_objects += c.rest;
      target[c.band] += c.bytes * kTickMs * 16 / c.interval;   // x16: integer shares of a 50 ms tick
      c.unit = c.bytes;
      if (IsCar(c.e->kind)) for (size_t q = pi + 1; q < idx.size(); ++q) { const Cand& k = cands[idx[q]]; if (!k.crew || k.anchor != c.e->id) break; if (k.streams && k.due) c.unit = uint16_t(c.unit + k.bytes); }
      if (c.due) { need[c.band] += c.bytes; if (!first[c.band]) first[c.band] = c.unit; }
    }
    // Record bytes: what is left after the creates, less a header per message the budget could fill.
    const uint32_t left = budget > spent ? budget - spent : 0;
    const uint32_t headers = uint32_t(sizeof(UpdateHeader)) * std::max<uint32_t>(1, (left + kMaxMessage - 1) / kMaxMessage);
    const uint32_t avail = left > headers ? left - headers : 0;
    uint32_t share[3] = {};
    if (need[0] + need[1] + need[2] <= avail) { for (int b = 0; b < 3; ++b) share[b] = need[b]; }
    else {
      // One record for every band with something due (a share by target can
      // be smaller than a record: two parked cars near a member), then the
      // rest in proportion to the bands' target bytes.
      uint32_t pool = avail;
      bool open[3] = {need[0] > 0, need[1] > 0, need[2] > 0};
      for (int b = 0; b < 3; ++b) if (open[b] && first[b] <= pool) { share[b] = first[b]; pool -= first[b]; if (share[b] >= need[b]) open[b] = false; }
      for (int pass = 0; pass < 3 && pool; ++pass) {
        uint64_t tsum = 0;
        for (int b = 0; b < 3; ++b) if (open[b]) tsum += target[b];
        if (!tsum) break;
        uint32_t given = 0;
        for (int b = 0; b < 3; ++b) {
          if (!open[b]) continue;
          uint32_t g = uint32_t(uint64_t(pool) * target[b] / tsum);
          if (share[b] + g >= need[b]) { g = need[b] - share[b]; open[b] = false; }
          share[b] += g; given += g;
        }
        pool -= given;
        if (!given) break;
      }
      // Rounding leftovers: the highest-priority due records that still fit.
      for (size_t i : idx) { const Cand& c = cands[i]; if (c.due && pool >= c.bytes && share[c.band] < need[c.band]) { share[c.band] += c.bytes; pool -= c.bytes; } }
    }
    uint32_t used[3] = {}, bytes = 0, msg = 0, messages = 0;
    float vnow[3] = {0, 0, 0};   // each band's virtual time: the start tag in service
    for (size_t i : idx) {
      Cand& c = cands[i];
      if (!c.due || used[c.band] + c.unit > share[c.band]) continue;
      if (c.crew) { const Cand& car = cands[c.car]; if (car.due && !car.sent) continue; }   // crew rides with its car
      c.sent = true;
      if (!messages || msg + c.bytes > uint32_t(kMaxMessage)) { ++messages; msg = uint32_t(sizeof(UpdateHeader)); }
      msg += c.bytes; used[c.band] += c.bytes; bytes += c.bytes;
      vnow[c.band] = std::max(vnow[c.band], c.tag);
      c.o->last_sent = now; c.o->tag = std::max(c.o->tag + float(c.interval), vnow[c.band]);
      ++m.band[c.band].planned; m.band[c.band].bytes += c.bytes; m.band[c.band].rest_planned += c.rest;
      p.updates.push_back({MakeKey(c.e->owner, c.e->id), c.e->kind, uint8_t(c.band), c.bytes, c.priority});
    }
    // Rebase every band's tags on its virtual now (0 next tick): only the differences matter.
    for (size_t i : idx) { Cand& c = cands[i]; if (c.streams) c.o->tag = std::max(-kTagMax, std::min(kTagMax, c.o->tag - vnow[c.band])); }
    p.messages = messages;
    p.bytes = spent + bytes + messages * uint32_t(sizeof(UpdateHeader));
    m.bytes += p.bytes;
    // Digest every 10 s: the ids this member was offered (held or declined), sorted.
    if (now - m.last_digest >= kDigestMs) {
      m.last_digest = now;
      p.digest_due = true;
      for (auto& [k, e] : reg.Entries()) { const Offer& o = e.offer[s]; if (o.created || (e.Streams() && o.declined_until)) p.digest.push_back(e.id); }
      std::sort(p.digest.begin(), p.digest.end());
      p.digest_hash = DigestHash(p.digest);
    }
  }
};

}  // namespace coopworld
