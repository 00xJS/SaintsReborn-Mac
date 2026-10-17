// Stand-alone test of the co-op 2.2 world stream core (world.h): records,
// quantisers, the owner's registry, the per-member scheduler and the set
// digest, on a simulated clock with no game and no network. Build (from the
// repo root, with the toolchain that Setup installs):
//   clang++ -std=c++20 -O1 core/WhompaysCoop/tests/world_test.cpp -o build/world_test.exe
// Exit code 0 = all checks passed. An argument picks another random seed.
#include "../world.h"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <set>
#include <sstream>

using namespace coopworld;

namespace {

int failures = 0;
void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string Fmt(const char* f, double a, double b = 0, double c = 0) { char b2[256]; std::snprintf(b2, sizeof b2, f, a, b, c); return b2; }

// An owner with members and a registry on a 50 ms tick; checks the per-tick
// invariants (budget, creates before updates) as it goes.
struct World {
  std::mt19937 rng;
  Registry reg;
  std::array<Member, kMaxSlots> members{};
  Scheduler sched;
  float ref[3] = {0, 0, 0};
  uint64_t now = 1000;
  int ticks = 0;
  const uint8_t owner = 0;
  // Per member: ids created (tick) and per id the plans (ticks), for rates and ordering.
  std::map<Key, int> created[kMaxSlots];
  std::map<Key, std::vector<int>> planned[kMaxSlots];
  std::vector<std::array<MemberPlan, kMaxSlots>> history;
  bool keep_history = false;
  uint32_t over_budget = 0, update_before_create = 0, max_bytes = 0;

  explicit World(unsigned seed) : rng(seed) {}
  float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
  Entry& AddCar(uint32_t id, const float pos[3], uint8_t kind = kKindTraffic, uint8_t flags = 0) {
    Entry& e = reg.Add(owner, id, kind, 2, now); std::memcpy(e.pos, pos, sizeof e.pos); e.flags = flags; e.model = uint16_t(id % 40); return e;
  }
  Entry& AddPed(uint32_t id, const float pos[3], uint32_t car = 0, uint8_t seat = 0, uint8_t kind = kKindAmbient) {
    Entry& e = reg.Add(owner, id, kind, 1, now); std::memcpy(e.pos, pos, sizeof e.pos); e.car = car; e.seat = seat; e.model = uint16_t(1 + id % 12);
    if (car) e.flags |= kEntryRest;
    return e;
  }
  void Join(int slot, const float eye[3], const float fwd[3], uint32_t budget) {
    Member& m = members[slot]; m.present = m.in_area = true; std::memcpy(m.eye, eye, sizeof m.eye); std::memcpy(m.fwd, fwd, sizeof m.fwd); m.budget = budget;
  }
  std::array<MemberPlan, kMaxSlots> Tick() {
    now += kTickMs; ++ticks;
    auto plans = sched.Plan(now, reg, members, ref);
    for (int s = 0; s < kMaxSlots; ++s) {
      const MemberPlan& p = plans[s];
      if (!members[s].present || !members[s].in_area) continue;
      if (p.bytes > p.budget) ++over_budget;
      max_bytes = std::max(max_bytes, p.bytes);
      for (auto& c : p.creates) { created[s][c.key] = ticks; planned[s][c.key].push_back(ticks); }
      for (auto& u : p.updates) {
        auto it = created[s].find(u.key);
        if (it == created[s].end() || it->second >= ticks) ++update_before_create;
        planned[s][u.key].push_back(ticks);
      }
      for (auto& r : p.removes) created[s].erase(r.key);
    }
    if (keep_history) history.push_back(plans);
    return plans;
  }
  void Run(int n) { for (int i = 0; i < n; ++i) Tick(); }
  // Plans per second of one id for one member within [from, to] ticks.
  double Hz(int s, Key k, int from, int to) const {
    auto it = planned[s].find(k);
    if (it == planned[s].end()) return 0;
    int n = 0; for (int t : it->second) n += t >= from && t <= to;
    return n / ((to - from + 1) * kTickMs / 1000.0);
  }
  std::string Serialise() const {
    std::ostringstream o;
    for (auto& plans : history) for (int s = 0; s < kMaxSlots; ++s) { auto& p = plans[s]; o << s << ':' << p.bytes << ';'; for (auto& c : p.creates) o << 'c' << c.key << ','; for (auto& u : p.updates) o << 'u' << u.key << ','; for (auto& r : p.removes) o << 'r' << r.key << ','; o << '|'; }
    return o.str();
  }
};

// Rotation matrix rows from an axis and an angle (Rodrigues), independent of UnqRot's formula.
void AxisAngle(const float k[3], float th, float rows[9]) {
  const float c = std::cos(th), s = std::sin(th), t = 1 - c;
  rows[0] = c + k[0] * k[0] * t;        rows[1] = k[0] * k[1] * t - k[2] * s; rows[2] = k[0] * k[2] * t + k[1] * s;
  rows[3] = k[1] * k[0] * t + k[2] * s; rows[4] = c + k[1] * k[1] * t;        rows[5] = k[1] * k[2] * t - k[0] * s;
  rows[6] = k[2] * k[0] * t - k[1] * s; rows[7] = k[2] * k[1] * t + k[0] * s; rows[8] = c + k[2] * k[2] * t;
}

// The scheduler's layout of tests 11 / 13: n_members eyes and n objects
// placed by `place`, cars (one in six parked) and people (some seated).
void Populate(World& w, int n_objects, const std::function<void(float*)>& place) {
  std::vector<uint32_t> cars;
  for (int i = 0; i < n_objects; ++i) {
    float pos[3]; place(pos);
    const uint32_t id = 0x10000u + uint32_t(i) * 7;
    if (i % 10 < 3) { w.AddCar(id, pos, i % 6 == 0 ? kKindParked : kKindTraffic, i % 6 == 0 ? kEntryRest : 0); cars.push_back(id); }
    else if (i % 10 == 3 && !cars.empty()) { const uint32_t car = cars[w.rng() % cars.size()]; w.AddPed(id, w.reg.Find(0, car)->pos, car, uint8_t(i % 4), kKindCrew); }
    else w.AddPed(id, pos);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const unsigned seed = argc > 1 ? unsigned(std::strtoul(argv[1], nullptr, 10)) : 12345u;
  std::mt19937 rng(seed);
  auto U = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
  std::printf("world_test seed %u: CarRec %zu PedRec %zu SeatedRec %zu UpdateHeader %zu CreateRec %zu RemoveRec %zu AdmitRec %zu DigestRec %zu OwnerMapRec %zu\n",
              seed, sizeof(CarRec), sizeof(PedRec), sizeof(SeatedRec), sizeof(UpdateHeader), sizeof(CreateRec), sizeof(RemoveRec), sizeof(AdmitRec), sizeof(DigestRec), sizeof(OwnerMapRec));

  // 1. Quantiser round trips and bounds.
  {
    float worst_pos = 0, worst_rot = 0, worst_orth = 0, worst_vel = 0, worst_ang = 0, worst_yaw = 0, worst_pv = 0;
    int saturated = 0, in_range_false = 0;
    for (int i = 0; i < 20000; ++i) {
      const float ref[3] = {U(-3000, 3000), U(-100, 100), U(-3000, 3000)};
      const float pos[3] = {ref[0] + U(-700, 700), ref[1] + U(-700, 700), ref[2] + U(-700, 700)};
      int16_t q[3]; float back[3];
      const bool in = std::fabs(pos[0] - ref[0]) <= kPosMax - 0.011f && std::fabs(pos[1] - ref[1]) <= kPosMax - 0.011f && std::fabs(pos[2] - ref[2]) <= kPosMax - 0.011f;
      if (QPos(ref, pos, q)) { UnqPos(ref, q, back); for (int k = 0; k < 3; ++k) worst_pos = std::max(worst_pos, std::fabs(back[k] - pos[k])); }
      else { ++saturated; if (in) ++in_range_false; }
      float k[3] = {U(-1, 1), U(-1, 1), U(-1, 1)};
      const float kn = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]) + 1e-6f; for (float& v : k) v /= kn;
      float rows[9], rows2[9]; int16_t r[3];
      AxisAngle(k, i % 7 == 0 ? kPi - U(0, 0.01f) : U(-kPi, kPi), rows);
      QRot(rows, r); UnqRot(r, rows2);
      for (int j = 0; j < 9; ++j) worst_rot = std::max(worst_rot, std::fabs(rows[j] - rows2[j]));
      for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) { float d = 0; for (int j = 0; j < 3; ++j) d += rows2[3 * a + j] * rows2[3 * b + j]; worst_orth = std::max(worst_orth, std::fabs(d - (a == b ? 1 : 0))); }
      const float v = U(-250, 250); worst_vel = std::max(worst_vel, std::fabs(UnqVel(QVel(v)) - v));
      const float w = U(-7.9f, 7.9f); worst_ang = std::max(worst_ang, std::fabs(UnqAng(QAng(w)) - w));
      const float y = U(0, 2 * kPi); float dy = std::fabs(UnqYaw(QYaw(y)) - y); dy = std::min(dy, 2 * kPi - dy); worst_yaw = std::max(worst_yaw, dy);
      const float pv = U(-7.9f, 7.9f); worst_pv = std::max(worst_pv, std::fabs(UnqPedVel(QPedVel(pv)) - pv));
    }
    Check(worst_pos <= kPosUnit / 2 + 1e-3f && in_range_false == 0 && saturated > 0, Fmt("positions: round trip within 1 cm (worst %.4f m), offsets past 655 m saturate (%g of 20000)", worst_pos, saturated));
    Check(worst_rot < 1e-3f && worst_orth < 2e-3f, Fmt("rotations: smallest-three round trip within 1e-3 per element (worst %.2e), orthonormal (%.2e)", worst_rot, worst_orth));
    Check(worst_vel <= kVelUnit / 2 + 1e-4f && worst_ang <= kAngUnit / 2 + 1e-4f, Fmt("velocities within 1/256 m/s (%.5f), angular within 1/32 rad/s (%.4f)", worst_vel, worst_ang));
    Check(worst_yaw <= kPi / 256 + 1e-4f && worst_pv <= kPedVelUnit / 2 + 1e-4f, Fmt("yaw within 0.7 degrees (%.3f deg), people velocity within 1/32 m/s (%.4f)", worst_yaw * 180 / kPi, worst_pv));
    Check(QVel(1000) == 32767 && QVel(-1000) == -32767 && QAng(100) == 127 && QPedVel(-100) == -127 && QHp(50, 100) == 128 && QHp(-5, 100) == 0 && QHp(200, 100) == 255, "velocities and hp clamp");
    const float fwd[3] = {1, 0, 0};
    Check(std::fabs(YawOf(fwd) - kPi / 2) < 1e-5f, "YawOf: +x forward is a quarter turn (atan2(x, z), y up)");
  }

  // 2. Message round trips: create, admit, update packing, digest parts.
  {
    CreateMsg c; c.head.id = 0x12345678; c.head.time = 777; c.head.kind = kKindTraffic; c.head.authority = 0; c.head.model = 42; c.head.team = 3;
    c.paint.assign(300, 0x5A); c.crew = {1, 2, 3};
    CarRec car{}; car.id = c.head.id; car.model = 42; car.hp = 200; car.damage = 0x21;
    c.first.assign(reinterpret_cast<uint8_t*>(&car), reinterpret_cast<uint8_t*>(&car) + sizeof car);
    uint8_t buf[kMaxMessage];
    const size_t n = WriteCreate(c, buf, sizeof buf);
    CreateMsg back;
    const bool car_ok = n == sizeof(CreateRec) + 300 + 12 + sizeof(CarRec) && ReadCreate(buf, n, back) && back.head.id == c.head.id && back.paint == c.paint && back.crew == c.crew && back.first == c.first && HasMagic(buf, "WWC1");
    CreateMsg p; p.head.id = 9; p.head.kind = kKindCrew; p.head.car = 0x12345678; p.head.seat = 2; p.name = "PD_cop_a"; p.look.assign(127, 7);
    SeatedRec sr{}; sr.id = 9; sr.car = 0x12345678; sr.seat = 2; sr.flags = kPedSeated;
    p.first.assign(reinterpret_cast<uint8_t*>(&sr), reinterpret_cast<uint8_t*>(&sr) + sizeof sr);
    const size_t n2 = WriteCreate(p, buf, sizeof buf);
    CreateMsg back2;
    const bool ped_ok = n2 && ReadCreate(buf, n2, back2) && back2.name == "PD_cop_a" && back2.look == p.look && back2.head.car == p.head.car && back2.first.size() == sizeof(SeatedRec);
    CreateMsg bad = p; bad.name.assign(47, 'x');
    const bool bounds = WriteCreate(bad, buf, sizeof buf) == 0 && !ReadCreate(buf, n2 - 1, back2);
    Check(car_ok && ped_ok && bounds, "WWC1: car with paint and crew, seated person with name and look, round trip; oversized name refused, a cut message refused");

    AdmitMsg a; a.head.cars_held = 16; a.head.cars_cap = 16; a.head.people_held = 40; a.head.people_cap = 56; a.head.chunks = 0x8000000000000005ull; a.head.dropped_chunk = 2;
    a.models = {5, 1, 9}; for (uint32_t i = 0; i < kMaxDeclined; ++i) a.declined.push_back(0x20000 + i);
    const size_t na = WriteAdmit(a, buf, sizeof buf);
    AdmitMsg ab;
    Check(na == sizeof(AdmitRec) + 6 + 4 * kMaxDeclined && ReadAdmit(buf, na, ab) && ab.models == a.models && ab.declined == a.declined && ab.head.chunks == a.head.chunks && ab.head.cars_cap == 16 && !ReadAdmit(buf, na - 1, ab), "WWA1: counts, 3 models, 32 declined ids round trip; a cut message refused");

    std::vector<CarRec> cars(30); std::vector<PedRec> peds(40); std::vector<SeatedRec> seated(15);
    for (size_t i = 0; i < cars.size(); ++i) { cars[i].id = uint32_t(100 + i); cars[i].hp = uint8_t(i); }
    for (size_t i = 0; i < peds.size(); ++i) { peds[i].id = uint32_t(200 + i); peds[i].flags = uint8_t(i % 2 ? kPedCrouch : 0); peds[i].action = uint16_t(i); }
    for (size_t i = 0; i < seated.size(); ++i) { seated[i].id = uint32_t(300 + i); seated[i].car = 100; seated[i].seat = uint8_t(i % 4); seated[i].flags = kPedSeated | kPedSign; }
    UpdateHeader h{}; h.owner = 0; h.epoch = 3; h.time = 4321; h.ref[0] = 100; h.ref[2] = -50;
    auto msgs = PackUpdates(h, cars, peds, seated);
    std::vector<CarRec> gc; std::vector<PedRec> gp; std::vector<SeatedRec> gs;
    bool sizes = true, parse = true;
    for (auto& m : msgs) { sizes &= m.size() <= size_t(kMaxMessage); UpdateMsg u; parse &= ReadUpdate(m.data(), m.size(), u) && u.head.time == 4321 && u.head.epoch == 3; gc.insert(gc.end(), u.cars.begin(), u.cars.end()); gp.insert(gp.end(), u.peds.begin(), u.peds.end()); gs.insert(gs.end(), u.seated.begin(), u.seated.end()); }
    const size_t total = 30 * 29 + 40 * 20 + 15 * 12;
    const bool same = gc.size() == 30 && gp.size() == 40 && gs.size() == 15 && std::memcmp(gc.data(), cars.data(), 30 * sizeof(CarRec)) == 0 && std::memcmp(gp.data(), peds.data(), 40 * sizeof(PedRec)) == 0 && std::memcmp(gs.data(), seated.data(), 15 * sizeof(SeatedRec)) == 0;
    Check(sizes && parse && same && msgs.size() == 2, Fmt("WWS1: 30 cars + 40 people + 15 seated (%g B) pack into %g messages <= kMaxMessage and read back identical", double(total), double(msgs.size())));
    UpdateMsg u; auto m0 = msgs[0]; m0.pop_back();
    Check(!ReadUpdate(m0.data(), m0.size(), u) && !ReadUpdate(buf, 10, u), "WWS1: a cut or short message is refused");

    std::vector<uint32_t> ids; for (int i = 0; i < 190; ++i) ids.push_back(uint32_t(1000 + i * 3));
    auto parts = WriteDigest(0, 5555, ids);
    DigestParts dp; bool complete = false; int completes = 0;
    for (size_t i = parts.size(); i-- > 0;) { complete = dp.Add(parts[i].data(), parts[i].size()); completes += complete; }
    Check(parts.size() == 3 && completes == 1 && complete && dp.Ids() == ids && dp.hash == DigestHash(ids) && dp.time == 5555, "WWD1: 190 ids go as 3 parts, complete once all are in (any order), hash matches");
    std::vector<uint32_t> mine = ids; mine.erase(mine.begin() + 5); mine.erase(mine.begin() + 100); mine.push_back(7); mine.push_back(999999); std::sort(mine.begin(), mine.end());
    std::vector<uint32_t> missing, extra;
    DigestDiff(ids, mine, missing, extra);
    Check(missing.size() == 2 && missing[0] == ids[5] && missing[1] == ids[101] && extra.size() == 2 && extra[0] == 7 && extra[1] == 999999, "digest diff: two planted missing and two planted extra ids found");
    DigestDiff(ids, ids, missing, extra);
    Check(missing.empty() && extra.empty() && DigestHash(ids) != DigestHash(mine), "digest diff of equal sets is empty; different sets hash differently");
    auto empty = WriteDigest(0, 1, {});
    Check(empty.size() == 1 && dp.Add(empty[0].data(), empty[0].size()) && dp.Ids().empty(), "an empty digest is one part and complete");
  }

  // 3. Budget formula.
  Check(TickBudget(1000, 1) == 1200 && TickBudget(1000, 11) == 568 && TickBudget(1000, 12) == 520 && TickBudget(100, 11) == 300 && TickBudget(100000, 2) == 1200, "TickBudget: 1000 kbps gives 1200 B/tick at 1 member, 568 at 11; clamped to 6-24 KB/s");

  // 4. Scheduler: 12 members in a 400 m square, 200 objects, 400 ticks, at 1200 B/tick
  //    (one member's clamp) the floors 12 / 5 / 1.5 Hz; at 570 B/tick (11 members)
  //    proportional slowdown, no band starved; the budget never exceeded; no
  //    update before its create; identical runs plan identically.
  auto square = [&](World& w, uint32_t budget, int n_objects) {
    for (int s = 0; s < kMaxSlots; ++s) {
      const float eye[3] = {w.U(0, 400), 1.5f, w.U(0, 400)};
      const float a = w.U(0, 2 * kPi); const float fwd[3] = {std::sin(a), 0, std::cos(a)};
      w.Join(s, eye, fwd, budget);
    }
    Populate(w, n_objects, [&](float* p) { p[0] = w.U(0, 400); p[1] = w.U(0, 3); p[2] = w.U(0, 400); });
  };
  struct Rates { double min[3] = {1e9, 1e9, 1e9}, sum[3] = {}, rest_min = 1e9; int n[3] = {}, n_rest = 0; };
  auto rates = [&](World& w, int from, int to) {
    Rates r;
    for (int s = 0; s < kMaxSlots; ++s) {
      if (!w.members[s].present) continue;
      for (auto& [k, e] : w.reg.Entries()) {
        const Entry& o = e;
        const float* pos = o.Seated() ? w.reg.Find(0, o.car)->pos : o.pos;
        const int band = BandOf(Dist(pos, w.members[s].eye));
        const double hz = w.Hz(s, k, from, to);
        if (o.flags & kEntryRest) { r.rest_min = std::min(r.rest_min, hz); ++r.n_rest; continue; }
        r.min[band] = std::min(r.min[band], hz); r.sum[band] += hz; ++r.n[band];
      }
    }
    return r;
  };
  std::string plan_a, plan_b;
  for (int run = 0; run < 2; ++run) {
    World w(seed);
    w.keep_history = true;
    square(w, 1200, 200);
    w.Run(400);
    if (run == 1) { plan_b = w.Serialise(); break; }
    plan_a = w.Serialise();
    int catch_up = 0;
    for (int s = 0; s < kMaxSlots; ++s) for (auto& [k, t] : w.created[s]) catch_up = std::max(catch_up, t);
    bool all_created = true;
    for (int s = 0; s < kMaxSlots; ++s) all_created &= w.created[s].size() == w.reg.Size();
    Check(all_created && catch_up <= 60, Fmt("12 members x 200 objects at 1200 B/tick: every object created on every member within %g ticks (4 a tick)", catch_up));
    const Rates r = rates(w, 100, 400);
    std::printf("      near %.1f Hz (min %.1f, %d), mid %.1f Hz (min %.1f, %d), far %.2f Hz (min %.2f, %d), resting min %.2f Hz (%d); max %u B of 1200\n",
                r.sum[0] / std::max(1, r.n[0]), r.min[0], r.n[0], r.sum[1] / std::max(1, r.n[1]), r.min[1], r.n[1], r.sum[2] / std::max(1, r.n[2]), r.min[2], r.n[2], r.rest_min, r.n_rest, w.max_bytes);
    Check(r.min[0] >= 12 && r.min[1] >= 5 && r.min[2] >= 1.5, "rate floors at 1200 B/tick: every object >= 12 Hz within 50 m, >= 5 Hz within 150 m, >= 1.5 Hz beyond");
    Check(r.rest_min >= 1.5, "resting objects (seated, parked) >= 1.5 Hz");
    Check(w.over_budget == 0, Fmt("the budget is never exceeded (max %g B of 1200 in a tick)", w.max_bytes));
    Check(w.update_before_create == 0, "no update is planned before (or in the same tick as) the object's create");
    uint32_t over = 0; for (int s = 0; s < kMaxSlots; ++s) over += w.members[s].over_create_budget;
    std::printf("      creates held back by the budget: %u\n", over);
  }
  Check(plan_a == plan_b && plan_a.size() > 10000, Fmt("the plan is deterministic: two runs on equal inputs give identical plans (%g bytes of plan)", double(plan_a.size())));
  {
    World w(seed);
    square(w, 570, 200);
    w.Run(400);
    const Rates r = rates(w, 100, 400);
    const double near = r.sum[0] / std::max(1, r.n[0]), mid = r.sum[1] / std::max(1, r.n[1]), far = r.sum[2] / std::max(1, r.n[2]);
    std::printf("      at 570 B/tick: near %.1f Hz (min %.1f), mid %.1f Hz (min %.1f), far %.2f Hz (min %.2f), resting min %.2f; ratios of target %.2f / %.2f / %.2f; max %u B\n",
                near, r.min[0], mid, r.min[1], far, r.min[2], r.rest_min, near / 20, mid / 10, far / 2, w.max_bytes);
    const double ratios[3] = {near / 20, mid / 10, far / 2};
    const double lo = std::min({ratios[0], ratios[1], ratios[2]}), hi = std::max({ratios[0], ratios[1], ratios[2]});
    Check(w.over_budget == 0, Fmt("570 B/tick: the budget is never exceeded (max %g B)", w.max_bytes));
    Check(lo > 0.25 && hi / lo < 1.8, Fmt("570 B/tick: every band slows in proportion (achieved / target %.2f .. %.2f), none starved", lo, hi));
    Check(r.min[0] >= 6 && r.min[1] >= 3 && r.min[2] >= 0.6 && r.rest_min >= 0.6, "570 B/tick: no object below 6 / 3 / 0.6 Hz in its band");
    Check(w.update_before_create == 0, "570 B/tick: no update before its create");
  }

  // 5. No starvation: 400 people at one distance, room for 20 a tick: each is
  //    planned within every 20 ticks once the creates are through.
  {
    World w(seed);
    const float eye[3] = {0, 1.5f, 0}, fwd[3] = {0, 0, 1};
    w.Join(1, eye, fwd, 20 * 20 + 25);
    for (int i = 0; i < 400; ++i) { const float a = float(i) * 2 * kPi / 400; const float pos[3] = {100 * std::sin(a), 1, 100 * std::cos(a)}; w.AddPed(1000 + i, pos); }
    w.Run(400);   // creates: 2-3 a tick within 425 B
    int worst_gap = 0;
    for (auto& [k, ticks] : w.planned[1]) { int last = -1; for (int t : ticks) { if (t > 200) { if (last > 0) worst_gap = std::max(worst_gap, t - last); last = t; } } if (last < 0) worst_gap = 1000; }
    Check(w.created[1].size() == 400 && worst_gap <= 20, Fmt("400 objects at one distance with room for 20 a tick: each planned within %g ticks (no starvation)", worst_gap));
    Check(w.over_budget == 0, "no starvation run: budget kept");
  }

  // 6. Declines: declined ids get nothing for 3 s, then only when the member reports room, nearest first.
  {
    World w(seed);
    const float eye[3] = {0, 1.5f, 0}, fwd[3] = {0, 0, 1};
    w.Join(2, eye, fwd, 1200);
    for (int i = 0; i < 30; ++i) { const float pos[3] = {float(5 + i * 4), 0, 0}; w.AddCar(500 + i, pos); }
    w.Run(20);
    AdmitMsg a; a.head.cars_held = 20; a.head.cars_cap = 20; a.head.people_cap = 56;
    for (int i = 20; i < 30; ++i) a.declined.push_back(500 + i);   // the 10 farthest
    w.sched.OnAdmit(w.now, w.reg, w.members[2], 2, 0, a);
    int offered_while_declined = 0, created_full = 0;
    const uint64_t declined_at = w.now;
    for (int t = 0; t < 80; ++t) {   // 4 s, still full
      auto plans = w.Tick();
      for (auto& u : plans[2].updates) if (IdOf(u.key) >= 520) ++offered_while_declined;
      for (auto& c : plans[2].creates) if (IdOf(c.key) >= 520) ++created_full;
    }
    Check(offered_while_declined == 0 && created_full == 0, "declined ids get no update and no new create for 4 s while the member is full");
    a.head.cars_held = 18; a.declined.clear();   // room for two
    w.sched.OnAdmit(w.now, w.reg, w.members[2], 2, 0, a);
    std::vector<uint32_t> reoffered;
    for (int t = 0; t < 2; ++t) { auto plans = w.Tick(); for (auto& c : plans[2].creates) reoffered.push_back(IdOf(c.key)); }
    Check(reoffered.size() >= 2 && reoffered[0] == 520 && reoffered[1] == 521 && w.now - declined_at > kDeclineMs, Fmt("with room reported after 3 s the declined ids are re-offered, nearest first (%g in two ticks, first 520 then 521)", double(reoffered.size())));
    Check(w.members[2].reoffers == reoffered.size() && w.members[2].declines == 10, "the member's counters: 10 declines, the re-offers counted");
    // Declined early, room reported early: still nothing before 3 s.
    World w2(seed);
    w2.Join(3, eye, fwd, 1200);
    for (int i = 0; i < 5; ++i) { const float pos[3] = {float(5 + i * 4), 0, 0}; w2.AddCar(600 + i, pos); }
    w2.Run(5);
    AdmitMsg d; d.head.cars_held = 4; d.head.cars_cap = 20; d.declined = {604};
    w2.sched.OnAdmit(w2.now, w2.reg, w2.members[3], 3, 0, d);
    int early = 0, late = 0;
    for (int t = 0; t < 59; ++t) { auto plans = w2.Tick(); for (auto& c : plans[3].creates) if (IdOf(c.key) == 604) ++early; }
    for (int t = 0; t < 3; ++t) { auto plans = w2.Tick(); for (auto& c : plans[3].creates) if (IdOf(c.key) == 604) ++late; }
    Check(early == 0 && late == 1, "a declined id with room is still not re-offered before 3 s, and is right after");
  }

  // 7. Removes: gone -> reason 0 with the fade to members holding it, nothing
  //    to a member that declined it; leaving the area -> reason 3 for everything
  //    held and nothing for declined ids; a car driven by a player gets no
  //    updates (its crew does); a removed id is dropped from the digest.
  {
    World w(seed);
    const float eye[3] = {0, 1.5f, 0}, fwd[3] = {0, 0, 1};
    for (int s = 1; s <= 3; ++s) w.Join(s, eye, fwd, 1200);
    for (int i = 0; i < 10; ++i) { const float pos[3] = {float(5 + i * 4), 0, 0}; w.AddCar(700 + i, pos); }
    const float ppos[3] = {9, 0, 0};
    w.AddPed(800, ppos, 701, 0, kKindCrew);
    w.Run(10);
    AdmitMsg a; a.head.cars_held = 5; a.head.cars_cap = 5; a.declined = {705, 706};
    w.sched.OnAdmit(w.now, w.reg, w.members[2], 2, 0, a);
    w.Tick();
    w.reg.Remove(0, 705, 400);
    w.reg.Remove(0, 701, 1000);
    auto plans = w.Tick();
    auto has = [](const std::vector<PlannedRemove>& v, uint32_t id, uint8_t reason, uint16_t fade) { for (auto& r : v) if (IdOf(r.key) == id && r.reason == reason && r.fade_ms == fade) return true; return false; };
    Check(has(plans[1].removes, 705, kRemoveGone, 400) && has(plans[3].removes, 705, kRemoveGone, 400) && !has(plans[2].removes, 705, kRemoveGone, 400) && plans[1].removes.size() == 2 && plans[2].removes.size() == 1,
          "a removed id goes as reason 0 with its fade to the members holding it, not to the one that declined it");
    Check(has(plans[2].removes, 701, kRemoveGone, 1000), "the member that declined another id still gets the remove of the car it held");
    // The crew of the removed car stays (its own destroy hook removes it) and streams on foot now.
    w.reg.Find(0, 703)->authority = 2;   // and a player drives car 703: no record for it
    int car_updates = 0, other_updates = 0, crew_on_foot = 0, crew_seated = 0;
    for (int t = 0; t < 20; ++t) { auto p = w.Tick(); for (auto& u : p[1].updates) { if (IdOf(u.key) == 703) ++car_updates; else ++other_updates; if (IdOf(u.key) == 800) (u.bytes == sizeof(PedRec) ? crew_on_foot : crew_seated)++; } }
    Check(w.reg.Find(0, 800) && w.reg.Find(0, 800)->offer[1].created && crew_on_foot > 0 && crew_seated == 0, "the crew of a removed car stays in the registry and streams as a person on foot");
    Check(car_updates == 0 && other_updates > 0, "a car with a player's authority gets no WWS1 record; the others do");
    // The owner's own player gets into car 704: the authority stays the owner's slot, the flag says a pose carries it.
    w.reg.Find(0, 704)->flags |= kEntryPlayerCar;
    int own_driven = 0;
    for (int t = 0; t < 20; ++t) { auto p = w.Tick(); for (auto& u : p[1].updates) own_driven += IdOf(u.key) == 704; }
    w.reg.Find(0, 704)->flags &= uint8_t(~kEntryPlayerCar);
    int own_back = 0;
    for (int t = 0; t < 20; ++t) { auto p = w.Tick(); for (auto& u : p[1].updates) own_back += IdOf(u.key) == 704; }
    Check(own_driven == 0 && own_back > 0, "a car the owner's own player drives (kEntryPlayerCar) gets no record, and gets them again once it is back");
    // Member 3 leaves the area: 8 cars (10 - 705 - 701) and the crew member.
    w.members[3].in_area = false;
    plans = w.Tick();
    bool all3 = plans[3].removes.size() == 9;
    for (auto& r : plans[3].removes) all3 &= r.reason == kRemoveAreaLeft;
    Check(all3 && plans[1].removes.empty(), Fmt("a member that leaves the area gets reason 3 removes for everything it held (%g), the others nothing", double(plans[3].removes.size())));
    w.members[3].in_area = true;
    plans = w.Tick();
    Check(plans[3].creates.size() == 4 && plans[3].updates.empty(), "back in the area it is caught up again from the start, 4 creates a tick");
    // Digest after 10 s lists what member 1 was offered and not what was removed.
    bool due = false; std::vector<uint32_t> digest;
    for (int t = 0; t < 210 && !due; ++t) { auto p = w.Tick(); if (p[1].digest_due) { due = true; digest = p[1].digest; } }
    Check(due && std::is_sorted(digest.begin(), digest.end()) && !std::binary_search(digest.begin(), digest.end(), 705u) && std::binary_search(digest.begin(), digest.end(), 703u) && digest.size() == 9,
          Fmt("a digest comes within 10 s, sorted, without the removed id and with the player-driven car (%g ids)", double(digest.size())));
    // Member 2 declined 706: its digest lists it (declined ids are in the offered set) so its diff counts it as declined, not missing.
    due = false;
    for (int t = 0; t < 210 && !due; ++t) { auto p = w.Tick(); if (p[2].digest_due) { due = true; digest = p[2].digest; } }
    Check(due && std::binary_search(digest.begin(), digest.end(), 706u), "the digest to a member includes the ids it declined (the proof line counts them apart)");
  }

  // 8. Crowd: 12 members within 30 m of 55 objects at 570 B/tick: the near band
  //    >= 8 Hz for every object; nothing below 1.5 Hz.
  {
    World w(seed);
    for (int s = 0; s < kMaxSlots; ++s) { const float eye[3] = {w.U(-10, 10), 1.5f, w.U(-10, 10)}; const float a = w.U(0, 2 * kPi); const float fwd[3] = {std::sin(a), 0, std::cos(a)}; w.Join(s, eye, fwd, 570); }
    for (int i = 0; i < 20; ++i) { const float pos[3] = {w.U(-20, 20), 0, w.U(-20, 20)}; w.AddCar(2000 + i, pos); }
    for (int i = 0; i < 35; ++i) { const float pos[3] = {w.U(-20, 20), 0, w.U(-20, 20)}; w.AddPed(3000 + i, pos); }
    w.Run(400);
    const Rates r = rates(w, 100, 400);
    std::printf("      crowd: near %.1f Hz (min %.1f over %d member-objects); max %u B of 570\n", r.sum[0] / std::max(1, r.n[0]), r.min[0], r.n[0], w.max_bytes);
    Check(r.n[0] == 12 * 55 && r.min[0] >= 8 && r.n[1] == 0, "crowd of 12 at 570 B/tick: every member's near band >= 8 Hz for every one of the 55 objects");
    Check(r.min[0] >= 1.5 && w.over_budget == 0, "crowd: nothing below 1.5 Hz, budget kept");
  }

  // 9. Long run: 60 s at 20 Hz, 12 members and 300 objects all moving, at
  //    570 B/tick: budget kept every tick; update age (time since the last
  //    plan, sampled each tick) p50 <= 120 ms and p95 <= 250 ms in the near
  //    band; objects come and go (churn) without an update before a create.
  {
    World w(seed);
    square(w, 570, 300);
    std::vector<uint32_t> ids; for (auto& [k, e] : w.reg.Entries()) ids.push_back(e.id);
    std::vector<uint32_t> ages_near, ages_mid;
    uint32_t next_id = 0x90000;
    for (int t = 0; t < 1200; ++t) {
      // Everyone drifts; every second a few objects go and a few come.
      for (auto& [k, e] : w.reg.Entries()) if (!e.Seated()) { e.pos[0] += w.U(-0.5f, 0.5f); e.pos[2] += w.U(-0.5f, 0.5f); }
      for (int s = 0; s < kMaxSlots; ++s) { w.members[s].eye[0] += w.U(-0.3f, 0.3f); w.members[s].eye[2] += w.U(-0.3f, 0.3f); }
      if (t % 20 == 19) {
        for (int i = 0; i < 3 && !ids.empty(); ++i) { const size_t j = w.rng() % ids.size(); if (w.reg.Find(0, ids[j]) && !w.reg.Find(0, ids[j])->Seated()) { w.reg.Remove(0, ids[j]); ids[j] = ids.back(); ids.pop_back(); } }
        for (int i = 0; i < 3; ++i) { const float pos[3] = {w.U(0, 400), 1, w.U(0, 400)}; w.AddPed(next_id, pos); ids.push_back(next_id++); }
      }
      w.Tick();
      if (t >= 200)
        for (int s = 0; s < kMaxSlots; ++s)
          for (auto& [k, e] : w.reg.Entries()) {
            const Offer& o = e.offer[s];
            if (!o.created || (e.flags & kEntryRest) || e.Seated()) continue;
            const int band = BandOf(Dist(e.pos, w.members[s].eye));
            const uint32_t age = uint32_t(w.now - o.last_sent);
            if (band == 0) ages_near.push_back(age); else if (band == 1) ages_mid.push_back(age);
          }
    }
    auto pct = [](std::vector<uint32_t>& v, double p) { if (v.empty()) return 0u; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, size_t(p * v.size()))]; };
    const uint32_t n50 = pct(ages_near, 0.5), n95 = pct(ages_near, 0.95), m50 = pct(ages_mid, 0.5), m95 = pct(ages_mid, 0.95);
    std::printf("      60 s: near age p50 %u ms p95 %u ms (%zu samples), mid p50 %u p95 %u; max %u B of 570; registry %zu\n", n50, n95, ages_near.size(), m50, m95, w.max_bytes, w.reg.Size());
    Check(w.over_budget == 0, "60 s run: the budget is never exceeded");
    Check(n50 <= 120 && n95 <= 250, "60 s run: near-band age p50 <= 120 ms, p95 <= 250 ms (the plan-side part of the update age)");
    Check(w.update_before_create == 0, "60 s run with churn: no update before a create");
    uint32_t added, removed, re; w.reg.TakeCounts(added, removed, re);
    Check(added == 300 + 3 * 60 && removed > 100 && re == 0, Fmt("registry counts: %g added, %g removed, %g re-activated", added, removed, re));
    Entry& again = w.reg.Add(0, ids[0], kKindAmbient, 1, w.now);
    w.reg.TakeCounts(added, removed, re);
    Check(again.activations == 2 && re == 1 && added == 0, "adding a known id again is idempotent: one activation more, no new entry");
  }

  std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
