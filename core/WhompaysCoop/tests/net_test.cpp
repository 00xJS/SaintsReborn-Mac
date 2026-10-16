// Stand-alone test of the co-op v2 network layer (net.h): a host and up to 12
// clients over a simulated network that drops, duplicates, delays and reorders
// packets. No game, no sockets. Build (from the repo root, with the toolchain
// that Setup installs):
//   clang++ -std=c++20 -O1 core/WhompaysCoop/tests/net_test.cpp -o build/net_test.exe -lws2_32
// Exit code 0 = all checks passed.
#include "../net.h"
#include <cstdio>
#include <memory>
#include <random>
#include <set>

namespace {

struct Datagram { uint64_t at; sockaddr_in from, to; std::vector<uint8_t> bytes; };

// A lossy network shared by every endpoint, on a simulated clock.
struct Network {
  std::mt19937 rng{12345};
  double loss = 0.20, dup = 0.03;
  int min_delay = 10, max_delay = 90;
  std::vector<Datagram> air;
  std::map<uint64_t, std::deque<Datagram>> inbox;  // key = address
  uint64_t now = 0;
  bool cut[64] = {};                               // endpoint silenced (by index in the address)
  std::map<std::pair<int, int>, int> drop_next;    // (from, to) index: drop that many packets

  static uint64_t Key(const sockaddr_in& a) { return (uint64_t(a.sin_addr.s_addr) << 16) | a.sin_port; }
  void Send(const sockaddr_in& from, const sockaddr_in& to, const void* d, int n) {
    if (cut[ntohs(from.sin_port) & 63]) return;
    if (auto it = drop_next.find({ntohs(from.sin_port) & 63, ntohs(to.sin_port) & 63}); it != drop_next.end() && it->second > 0) { --it->second; return; }
    std::uniform_real_distribution<double> u(0, 1);
    if (u(rng) < loss) return;
    const int copies = u(rng) < dup ? 2 : 1;
    for (int i = 0; i < copies; ++i) {
      std::uniform_int_distribution<int> delay(min_delay, max_delay);
      air.push_back({now + uint64_t(delay(rng)), from, to, std::vector<uint8_t>((const uint8_t*)d, (const uint8_t*)d + n)});
    }
  }
  void Deliver() {
    for (size_t i = 0; i < air.size();) {
      if (air[i].at <= now) {
        if (!cut[ntohs(air[i].to.sin_port) & 63]) inbox[Key(air[i].to)].push_back(std::move(air[i]));
        air[i] = std::move(air.back()); air.pop_back();
      } else ++i;
    }
  }
  int Recv(const sockaddr_in& me, char* buf, int cap, sockaddr_in& from) {
    auto& q = inbox[Key(me)];
    if (q.empty()) return -1;
    Datagram d = std::move(q.front()); q.pop_front();
    if (int(d.bytes.size()) > cap) return -1;
    std::memcpy(buf, d.bytes.data(), d.bytes.size());
    from = d.from;
    return int(d.bytes.size());
  }
};

sockaddr_in Addr(int i) {
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(0x0A000001); a.sin_port = htons(uint16_t(i));
  return a;
}

struct Endpoint {
  int index;
  sockaddr_in addr;
  std::unique_ptr<coopnet::Session> s;
  std::vector<coopnet::Received> got;
  std::vector<std::string> log;
};

int failures = 0;
void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

}  // namespace

int main() {
  Network net;
  std::vector<std::unique_ptr<Endpoint>> eps;
  auto make = [&](int index) {
    auto e = std::make_unique<Endpoint>();
    e->index = index; e->addr = Addr(index);
    Endpoint* raw = e.get();
    e->s = std::make_unique<coopnet::Session>(
        [&net, raw](const void* d, int n, const sockaddr_in& to) { net.Send(raw->addr, to, d, n); return n; },
        [&net, raw](char* b, int cap, sockaddr_in& from) { return net.Recv(raw->addr, b, cap, from); },
        [raw, &net](const std::string& l) { raw->log.push_back(std::to_string(net.now) + " " + l); });
    eps.push_back(std::move(e));
    return raw;
  };
  auto step = [&](int ms) {
    for (int t = 0; t < ms; t += 5) {
      net.now += 5;
      net.Deliver();
      for (auto& e : eps) {
        e->s->Pump(net.now);
        coopnet::Received r;
        while (e->s->Poll(r)) e->got.push_back(std::move(r));
      }
    }
  };

  // 1. A host and 11 clients join.
  Endpoint* host = make(1);
  host->s->StartHost("host");
  std::vector<Endpoint*> clients;
  for (int i = 0; i < 11; ++i) { clients.push_back(make(2 + i)); clients.back()->s->StartClient(host->addr, "player" + std::to_string(i + 1)); }
  step(4000);
  bool all_joined = true; std::set<int> slots;
  for (auto* c : clients) { all_joined &= c->s->Joined(); slots.insert(c->s->MySlot()); }
  Check(all_joined, "11 clients joined through 20% loss");
  Check(slots.size() == 11 && !slots.count(0), "each client has its own slot 1-11");
  bool counts = host->s->PlayerCount() == 12;
  for (auto* c : clients) counts &= c->s->PlayerCount() == 12;
  Check(counts, "everyone sees 12 players");

  // 2. A 13th player is refused.
  Endpoint* extra = make(20);
  extra->s->StartClient(host->addr, "late");
  step(3000);
  Check(!extra->s->Joined() && extra->s->DenyReason().find("full") != std::string::npos, "a 13th player is refused: " + extra->s->DenyReason());

  // 3. A different protocol version is refused (a hand-made join packet).
  {
    // A raw address, not an Endpoint: nothing must read its inbox before the check.
    struct { sockaddr_in addr; } oldep{Addr(21)}; auto* old = &oldep;
    std::vector<uint8_t> p(sizeof(coopnet::PacketHeader) + sizeof(coopnet::MessageHeader) + 4);
    coopnet::PacketHeader ph{coopnet::kMagic, 0, coopnet::kNoSlot, 0, 0, 0};
    coopnet::MessageHeader mh{coopnet::kJoin, coopnet::kNoSlot, 0, 0, 0, 4};
    std::memcpy(p.data(), &ph, sizeof(ph));
    std::memcpy(p.data() + sizeof(ph), &mh, sizeof(mh));
    const uint8_t body[4] = {99, 'o', 'l', 'd'};
    std::memcpy(p.data() + sizeof(ph) + sizeof(mh), body, 4);
    const double loss = net.loss; net.loss = 0;
    net.Send(old->addr, host->addr, p.data(), int(p.size()));
    step(500);
    net.loss = loss;
    bool denied = false;
    for (auto& d : net.inbox[Network::Key(old->addr)]) {
      coopnet::MessageHeader h; std::memcpy(&h, d.bytes.data() + sizeof(coopnet::PacketHeader), sizeof(h));
      denied |= h.type == coopnet::kJoinDeny;
    }
    Check(denied, "a different co-op version is refused");
  }

  // 4. Every client sends 200 reliable messages to everyone; all arrive in order, once.
  for (auto& e : eps) e->got.clear();
  const int kCount = 200;
  for (int n = 0; n < kCount; ++n) {
    for (auto* c : clients) {
      uint8_t payload[8];
      const uint32_t v = uint32_t(n);
      std::memcpy(payload, &v, 4);
      payload[4] = c->s->MySlot();
      c->s->Send(coopnet::kEveryone, 100, payload, 5, true);
      c->s->Send(coopnet::kEveryone, 101, payload, 5, false);  // unreliable: may be lost
    }
    step(20);
  }
  step(8000);
  bool in_order = true, complete = true; int unreliable_seen = 0, unreliable_sent = kCount * 10 * 12;
  for (auto& e : eps) {
    if (e.get() == extra || !e->s->Joined()) continue;
    std::map<int, uint32_t> next;
    std::map<int, int> got;
    for (auto& r : e->got) {
      if (r.type == 101) { ++unreliable_seen; continue; }
      if (r.type != 100) continue;
      uint32_t v; std::memcpy(&v, r.data.data(), 4);
      if (v != next[r.origin]) in_order = false;
      next[r.origin] = v + 1;
      ++got[r.origin];
    }
    for (auto* c : clients)
      if (c != e.get() && got[c->s->MySlot()] != kCount) complete = false;
  }
  Check(complete, "200 reliable messages from each client reached every other player");
  Check(in_order, "reliable messages arrived in order and exactly once");
  std::printf("      unreliable messages delivered: %d of %d (losses expected)\n", unreliable_seen, unreliable_sent);
  Check(unreliable_seen > unreliable_sent / 2, "most unreliable messages arrive");

  // 5. The host's own messages reach every client.
  for (auto& e : eps) e->got.clear();
  for (int n = 0; n < 50; ++n) { uint32_t v = uint32_t(n); host->s->Send(coopnet::kEveryone, 102, &v, 4, true); step(10); }
  step(5000);
  bool host_msgs = true;
  for (auto* c : clients) { int k = 0; for (auto& r : c->got) if (r.type == 102 && r.origin == 0) ++k; host_msgs &= k == 50; }
  Check(host_msgs, "50 reliable messages from the host reached every client");

  // 5b. A burst of full-size unreliable messages (the host's traffic: 9 parts
  // of 1120 bytes at once) isn't cut short by the per-Pump packet limit.
  for (auto& e : eps) e->got.clear();
  {
    const double loss = net.loss, dup = net.dup; net.loss = 0; net.dup = 0;
    std::vector<uint8_t> big(coopnet::kMaxMessage, 0xAB);
    for (int n = 0; n < 20; ++n) { big[0] = uint8_t(n); host->s->Send(coopnet::kEveryone, 104, big.data(), uint16_t(big.size()), false); }
    step(1000);
    net.loss = loss; net.dup = dup;
    bool all = true;
    for (auto* c : clients) { int k = 0; for (auto& r : c->got) if (r.type == 104 && r.data.size() == big.size()) ++k; all &= k == 20; }
    Check(all, "a burst of 20 full-size unreliable messages reaches every client whole");
  }

  // 6. A message to one slot reaches only that slot.
  for (auto& e : eps) e->got.clear();
  { uint32_t v = 7; clients[0]->s->Send(clients[5]->s->MySlot(), 103, &v, 4, true); }
  step(3000);
  int receivers = 0; bool right = false;
  for (auto& e : eps) for (auto& r : e->got) if (r.type == 103) { ++receivers; right = e.get() == clients[5]; }
  Check(receivers == 1 && right, "a message for one slot reaches only that slot (through the host)");

  // 7. A client leaves cleanly; a client vanishes (timeout).
  for (auto& e : eps) e->got.clear();
  const uint8_t leaving = clients[2]->s->MySlot(), vanishing = clients[3]->s->MySlot();
  clients[2]->s->Stop(net.now);
  net.cut[clients[3]->index] = true;
  step(9000);
  bool left_seen = true, timeout_seen = true;
  for (auto* c : {clients[0], clients[1], clients[4]}) {
    bool l = false, t = false;
    for (auto& r : c->got) if (r.type == coopnet::kPeerLeft) { l |= r.origin == leaving; t |= r.origin == vanishing; }
    left_seen &= l && !c->s->Present(leaving);
    timeout_seen &= t && !c->s->Present(vanishing);
  }
  Check(left_seen, "a player who leaves is gone for everyone");
  Check(timeout_seen, "a player who vanishes times out for everyone");
  Check(host->s->PlayerCount() == 10, "the host counts 10 players after two left");

  // 8. A new player takes a freed slot.
  extra->s->StartClient(host->addr, "late again");
  step(4000);
  Check(extra->s->Joined() && (extra->s->MySlot() == leaving || extra->s->MySlot() == vanishing), "a new player gets a freed slot");
  if (!extra->s->Joined()) {
    const coopnet::Stats xs = extra->s->TakeStats();
    std::printf("      extra: slot %d, refused: %s; packets in %u out %u bad %u\n", extra->s->MySlot(), extra->s->DenyReason().c_str(), xs.packets_in, xs.packets_out, xs.bad_packets);
    for (auto& l : extra->log) std::printf("      extra log: %s\n", l.c_str());
    for (size_t i = host->log.size() > 12 ? host->log.size() - 12 : 0; i < host->log.size(); ++i) std::printf("      host log: %s\n", host->log[i].c_str());
  }

  // 8b. The host's first packet to a new player (the join reply) is lost: it
  // is sent again (a client that had received nothing used to ack it).
  {
    Endpoint* late = make(30);
    net.drop_next[{host->index, late->index}] = 1;
    clients[6]->s->Stop(net.now);  // a free slot
    step(500);
    late->s->StartClient(host->addr, "unlucky");
    step(3000);
    Check(late->s->Joined(), "a player whose join reply is lost still joins");
  }

  // 9. The host vanishes: clients notice and keep trying to join.
  for (auto& e : eps) e->got.clear();
  net.cut[host->index] = true;
  step(9000);
  bool lost = true;
  for (auto* c : {clients[0], clients[1]}) {
    bool l = false;
    for (auto& r : c->got) l |= r.type == coopnet::kPeerLeft && r.origin == 0;
    lost &= l && !c->s->Joined();
  }
  Check(lost, "clients notice that the host is gone");
  net.cut[host->index] = false;
  host->s->StartHost("host again");
  step(5000);
  Check(clients[0]->s->Joined() && clients[1]->s->Joined(), "clients join again when the host is back");

  const coopnet::Stats hs = host->s->TakeStats();
  std::printf("      host totals: %u packets out, %u in, %u resent, %u unreliable batches dropped\n", hs.packets_out, hs.packets_in, hs.resent, hs.dropped_unreliable);
  std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
