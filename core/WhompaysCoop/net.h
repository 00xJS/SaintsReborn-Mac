// Co-op v2 network layer: a session of up to 12 players over one datagram
// transport (Epic P2P or UDP), modelled on the game's own multiplayer (report Q):
// a star, where the host (slot 0) talks to every client and relays what a
// client sends to everyone else.
//
// A link (host <-> one client) carries packets of at most kMaxPacket bytes: a
// header with acks for the other side's last 33 packets, then messages.
// Reliable messages are kept until a packet carrying them is acked, resent
// after a round trip, and delivered in order. Unreliable ones go once (state
// snapshots, where the newest wins). Every message names its origin slot and
// its target (one slot or everyone), so the host can relay it.
//
// No game code here: the same file runs in the mod and in the stand-alone test
// (tests/net_test.cpp). Pump() runs on the network thread; Send*/Poll on the
// game thread; a mutex covers the queues.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace coopnet {

constexpr int kMaxSlots = 12;            // host + 11 clients (the game's player pool)
constexpr uint8_t kVersion = 7;          // protocol version: a mismatch is refused at join (5: co-op 2.1.2, 6: 2.2's world stream, 7: 2.2 with Package C's records and events)
constexpr uint32_t kMagic = 0x32435253;  // "SRC2"
constexpr int kMaxPacket = 1170;         // Epic's P2P limit (EOS_P2P_MAX_PACKET_SIZE)
constexpr int kMaxMessage = kMaxPacket - 16 - 8;  // payload of one message (headers below)
constexpr uint8_t kEveryone = 0xFF;
constexpr uint8_t kNoSlot = 0xFE;
constexpr uint64_t kTimeoutMs = 6000;    // no packet for this long: the peer is gone
constexpr uint64_t kKeepAliveMs = 200;   // a packet at least this often (carries the acks)
constexpr uint64_t kJoinRetryMs = 500;
constexpr int kMaxPacketsPerFlush = 8;   // per link and Pump; the rest waits for the next Pump
constexpr size_t kMaxUnreliableBacklog = 48 * 1024;  // per link: older unreliable messages beyond this are dropped
constexpr uint16_t kRelWindow = 1024;    // reliable messages in flight per link (the receiver holds up to twice that)
constexpr size_t kMaxRelBacklog = 16384; // reliable messages waiting on one link: past this the link is dropped
constexpr size_t kMaxName = 32;          // bytes of a player name

// Session messages (types below kFirstUserType are the session's own).
enum : uint8_t {
  kJoin = 1,        // client -> host, unreliable, repeated: u8 version, name
  kJoinOk = 2,      // host -> client, reliable: u8 your slot, then u8 count, {u8 slot, u8 len, name}
  kJoinDeny = 3,    // host -> client, unreliable: reason text
  kPeerJoined = 4,  // host -> clients, reliable: u8 slot, name
  kPeerLeft = 5,    // host -> clients, reliable: u8 slot
  kLeave = 6,       // client -> host, unreliable, sent a few times
  kFirstUserType = 16,
};

#pragma pack(push, 1)
struct PacketHeader {
  uint32_t magic;
  uint8_t version, slot;   // sender's slot (kNoSlot before a client has one)
  uint8_t flags;           // bit 0: ack / ack_bits are valid (the sender has received something)
  uint8_t epoch;           // the client's join attempt (both ways): a client that starts over gets a new one
  uint16_t seq;            // this packet
  uint16_t ack;            // the newest packet received from the other side
  uint32_t ack_bits;       // bit i: packet ack-1-i received too
};
struct MessageHeader {
  uint8_t type, origin, target, flags;  // flags bit 0: reliable
  uint16_t id;                          // reliable: per-link message id
  uint16_t len;
};
#pragma pack(pop)
static_assert(sizeof(PacketHeader) == 16 && sizeof(MessageHeader) == 8, "wire format");

inline bool SeqNewer(uint16_t a, uint16_t b) { return int16_t(uint16_t(a - b)) > 0; }

// Join attempts of this game, each newer than the last (1..255, wrapping; the
// host compares them as signed differences). A restarted game starts at a
// random one; the host then waits for the old link to time out at worst.
inline uint8_t NextEpoch() {
  static std::atomic<uint32_t> counter{uint32_t(std::chrono::steady_clock::now().time_since_epoch().count())};
  return uint8_t(1 + counter.fetch_add(1) % 255);
}

struct Received {
  uint8_t origin = 0, type = 0;
  std::vector<uint8_t> data;
};

struct Stats {
  uint32_t packets_out = 0, packets_in = 0, bytes_out = 0, bytes_in = 0;
  uint32_t resent = 0, dropped_unreliable = 0, bad_packets = 0;
};
// One link's state (the host: one per client; a client: its link to the
// host), for a sender that paces itself by it (2.2's world stream halves a
// member's budget while its backlog grows or its snapshots are dropped).
struct LinkStat {
  float rtt = 0;                  // ms, smoothed
  uint32_t backlog = 0;             // bytes of unreliable messages waiting for the next flush
  uint32_t in_flight = 0;           // reliable messages queued or sent and not acked yet
  uint32_t in_flight_bytes = 0;
  uint32_t dropped = 0;             // unreliable messages dropped past kMaxUnreliableBacklog, since the link began
};

class Session {
 public:
  using SendFn = std::function<int(const void*, int, const sockaddr_in&)>;
  using RecvFn = std::function<int(char*, int, sockaddr_in&)>;
  using LogFn = std::function<void(const std::string&)>;

  Session(SendFn send, RecvFn recv, LogFn log = nullptr) : send_(std::move(send)), recv_(std::move(recv)), log_(std::move(log)) {}

  // Host: slot 0, accepts joins. Client: joins the host at `host`.
  void StartHost(const std::string& name) {
    std::lock_guard l(m_);
    Reset();
    host_ = true; my_slot_ = 0; names_[0] = name.substr(0, kMaxName); present_[0] = true;
  }
  void StartClient(const sockaddr_in& host, const std::string& name) {
    std::lock_guard l(m_);
    Reset();
    host_ = false; my_slot_ = kNoSlot; names_[0].clear(); my_name_ = name.substr(0, kMaxName);
    // A new join attempt: the same UDP socket or Epic address can come back
    // while the host still has the last one.
    epoch_ = NextEpoch();
    Link& k = links_[0]; k.used = true; k.addr = host; k.slot = 0;
  }
  // Leave: a client tells the host (three packets, no acks wait); then everything is cleared.
  void Stop(uint64_t now) {
    std::lock_guard l(m_);
    if (!host_ && links_[0].used && my_slot_ != kNoSlot)
      for (int i = 0; i < 3; ++i) { SendControl(links_[0], kLeave, nullptr, 0, false); Flush(links_[0], now); }
    Reset();
  }

  bool IsHost() const { std::lock_guard l(m_); return host_; }
  uint8_t MySlot() const { std::lock_guard l(m_); return my_slot_; }
  bool Joined() const { std::lock_guard l(m_); return my_slot_ != kNoSlot; }
  bool Present(int slot) const { std::lock_guard l(m_); return slot >= 0 && slot < kMaxSlots && present_[slot]; }
  std::string Name(int slot) const { std::lock_guard l(m_); return slot >= 0 && slot < kMaxSlots ? names_[slot] : std::string(); }
  std::string DenyReason() const { std::lock_guard l(m_); return deny_reason_; }
  int PlayerCount() const { std::lock_guard l(m_); int n = 0; for (bool p : present_) n += p; return n; }
  Stats TakeStats() { std::lock_guard l(m_); Stats s = stats_; stats_ = {}; return s; }
  // The link to `slot` (a client: 0, its host), all zero when there is none.
  LinkStat LinkStats(int slot) const {
    std::lock_guard l(m_);
    LinkStat s;
    if (slot < 0 || slot >= kMaxSlots || !links_[slot].used) return s;
    const auto& k = links_[slot];
    s.rtt = k.rtt; s.backlog = uint32_t(k.unrel.size()); s.in_flight = uint32_t(k.rel_out.size()); s.dropped = k.dropped;
    s.in_flight_bytes = k.rel_bytes;
    return s;
  }

  // Game thread: queue a message for one slot or everyone (kEveryone). Clients
  // send through the host. Returns false when it can't be queued (too big, not joined).
  bool Send(uint8_t target, uint8_t type, const void* data, uint16_t len, bool reliable) {
    if (len > kMaxMessage) return false;
    std::lock_guard l(m_);
    if (my_slot_ == kNoSlot) return false;
    MessageHeader h{type, my_slot_, target, uint8_t(reliable ? 1 : 0), 0, len};
    if (host_) {
      for (int s = 1; s < kMaxSlots; ++s)
        if (links_[s].used && present_[s] && (target == kEveryone || target == s)) Queue(links_[s], h, data);
    } else if (links_[0].used) {
      Queue(links_[0], h, data);
    }
    return true;
  }

  // Game thread: the next received message (session and user types).
  bool Poll(Received& out) {
    std::lock_guard l(m_);
    if (inbox_.empty()) return false;
    out = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
  }

  // Network thread: read everything waiting, then send what is due.
  void Pump(uint64_t now) {
    std::lock_guard l(m_);
    char buf[2048];
    for (int n = 0; n < 256; ++n) {
      sockaddr_in from{};
      const int got = recv_(buf, sizeof(buf), from);
      if (got <= 0) break;
      OnDatagram(reinterpret_cast<const uint8_t*>(buf), got, from, now);
    }
    // (A different link first each time: with a full outgoing queue the
    // last ones would never get a turn.)
    flush_start_ = (flush_start_ + 1) % kMaxSlots;
    for (int i = 0; i < kMaxSlots; ++i) {
      const int s = (flush_start_ + i) % kMaxSlots;
      Link& k = links_[s];
      if (!k.used) continue;
      if (k.last_recv && now - k.last_recv > kTimeoutMs) {
        // A join attempt that has heard nothing yet keeps asking on the same
        // attempt (same epoch, same packet numbers): a host that still hears
        // us keeps its link and resends kJoinOk, instead of taking us for a
        // new player every 6 s (everyone saw "left" / "joined" over and over).
        if (!host_ && my_slot_ == kNoSlot && !k.recv_any) k.last_recv = now;
        else { Drop(s, "timed out"); continue; }
      }
      if (k.rel_out.size() > kMaxRelBacklog) { Drop(s, "fell too far behind"); continue; }
      if (!host_ && s == 0 && my_slot_ == kNoSlot && (!k.join_sent || now - k.join_sent >= kJoinRetryMs)) {
        k.join_sent = now;
        std::vector<uint8_t> j{kVersion};
        j.insert(j.end(), my_name_.begin(), my_name_.end());
        SendControl(k, kJoin, j.data(), uint16_t(j.size()), false);
        if (!k.last_recv) k.last_recv = now;  // start the join timeout
      }
      Flush(k, now);
    }
  }

 private:
  struct Out { uint16_t id; std::vector<uint8_t> bytes; uint64_t sent_at = 0; };
  struct Sent { bool used = false; uint16_t seq = 0; uint64_t at = 0; std::vector<uint16_t> ids; };
  struct Link {
    bool used = false;
    sockaddr_in addr{};
    uint8_t slot = kNoSlot;
    uint64_t last_recv = 0, last_send = 0, join_sent = 0;
    uint8_t epoch = 0;                                   // host: the client's join attempt this link belongs to
    uint16_t send_seq = 0;
    bool recv_any = false; uint16_t recv_latest = 0; uint32_t recv_bits = 0;
    uint16_t rel_next = 0, rel_expect = 0;
    std::deque<Out> rel_out;
    uint32_t rel_bytes = 0;                              // what rel_out holds (LinkStat, kept as it changes)
    std::map<uint16_t, std::vector<uint8_t>> rel_hold;  // reliable messages that came early
    std::vector<Sent> sent = std::vector<Sent>(4096);   // by packet seq & 4095 (outlives a full send window)
    std::vector<uint8_t> unrel;                          // whole messages for the next flush
    float rtt = 150.0f;
    uint32_t dropped = 0;                                // unreliable messages dropped (LinkStat)
  };

  void Reset() {
    for (auto& k : links_) k = Link{};
    for (auto& n : names_) n.clear();
    present_.fill(false);
    inbox_.clear();
    my_slot_ = kNoSlot; deny_reason_.clear();
  }
  void Log(const std::string& s) { if (log_) log_(s); }

  void Queue(Link& k, MessageHeader h, const void* data) {
    if (h.len > kMaxMessage) return;   // could never be sent (and would block the reliable channel)
    std::vector<uint8_t> m(sizeof(h) + h.len);
    if (h.flags & 1) h.id = k.rel_next++;
    std::memcpy(m.data(), &h, sizeof(h));
    if (h.len) std::memcpy(m.data() + sizeof(h), data, h.len);
    if (h.flags & 1) { k.rel_bytes += uint32_t(m.size()); k.rel_out.push_back({h.id, std::move(m)}); }
    else k.unrel.insert(k.unrel.end(), m.begin(), m.end());
  }
  void SendControl(Link& k, uint8_t type, const void* data, uint16_t len, bool reliable) {
    MessageHeader h{type, my_slot_, k.slot, uint8_t(reliable ? 1 : 0), 0, len};
    Queue(k, h, data);
  }

  // Build and send this link's packets: the reliable messages that are due,
  // then the queued unreliable ones; an empty packet as a keep-alive.
  void Flush(Link& k, uint64_t now) {
    const uint64_t resend_after = uint64_t(std::max(100.0f, k.rtt * 1.5f));
    size_t ui = 0;  // position in k.unrel
    int packets = 0;
    for (;;) {
      std::vector<uint8_t> p(sizeof(PacketHeader));
      std::vector<uint16_t> ids;
      // At most kRelWindow ids past the oldest unacked one: the receiver holds
      // only so many early ones (more were lost for good and stalled the link).
      const uint16_t base = k.rel_out.empty() ? 0 : k.rel_out.front().id;
      for (auto& o : k.rel_out) {
        if (uint16_t(o.id - base) >= kRelWindow) break;
        if (o.sent_at && now - o.sent_at < resend_after) continue;
        if (p.size() + o.bytes.size() > size_t(kMaxPacket)) break;
        if (o.sent_at) ++stats_.resent;
        p.insert(p.end(), o.bytes.begin(), o.bytes.end());
        o.sent_at = now;
        ids.push_back(o.id);
      }
      while (ui < k.unrel.size()) {
        MessageHeader h; std::memcpy(&h, &k.unrel[ui], sizeof(h));
        const size_t n = sizeof(h) + h.len;
        if (p.size() + n > size_t(kMaxPacket)) break;
        p.insert(p.end(), k.unrel.begin() + ui, k.unrel.begin() + ui + n);
        ui += n;
      }
      const bool content = p.size() > sizeof(PacketHeader);
      if (!content && (packets || now - k.last_send < kKeepAliveMs)) break;
      // No acks before anything arrived: an ack of "packet 0" then dropped a
      // reliable message (the join reply) the other side never got.
      PacketHeader ph{kMagic, kVersion, my_slot_, uint8_t(k.recv_any ? 1 : 0), host_ ? k.epoch : epoch_, k.send_seq, k.recv_latest, k.recv_bits};
      std::memcpy(p.data(), &ph, sizeof(ph));
      Sent& s = k.sent[k.send_seq & 4095];
      s.used = true; s.seq = k.send_seq; s.at = now; s.ids = std::move(ids);
      ++k.send_seq;
      send_(p.data(), int(p.size()), k.addr);
      k.last_send = now;
      ++stats_.packets_out; stats_.bytes_out += uint32_t(p.size());
      if (++packets >= kMaxPacketsPerFlush || ui >= k.unrel.size()) break;
    }
    // What didn't fit waits for the next Pump; past the backlog limit the
    // oldest go (a late snapshot is worth less than a new one).
    k.unrel.erase(k.unrel.begin(), k.unrel.begin() + ui);
    size_t drop = 0;
    while (k.unrel.size() - drop > kMaxUnreliableBacklog) {
      MessageHeader h; std::memcpy(&h, &k.unrel[drop], sizeof(h));
      drop += sizeof(h) + h.len;
      ++stats_.dropped_unreliable; ++k.dropped;
    }
    if (drop) k.unrel.erase(k.unrel.begin(), k.unrel.begin() + drop);
  }

  // The other side acked packet `seq`: the reliable messages it carried arrived.
  void Acked(Link& k, uint16_t seq, uint64_t now) {
    Sent& s = k.sent[seq & 4095];
    if (!s.used || s.seq != seq) return;
    k.rtt += (float(now - s.at) - k.rtt) * 0.1f;
    for (uint16_t id : s.ids)
      for (auto it = k.rel_out.begin(); it != k.rel_out.end(); ++it)
        if (it->id == id) { k.rel_bytes -= uint32_t(it->bytes.size()); k.rel_out.erase(it); break; }
    s = Sent{};
  }

  // Track which of the other side's packets arrived (for our acks). Returns
  // false for a packet already seen.
  bool MarkReceived(Link& k, uint16_t seq) {
    if (!k.recv_any) { k.recv_any = true; k.recv_latest = seq; k.recv_bits = 0; return true; }
    if (SeqNewer(seq, k.recv_latest)) {
      const uint16_t shift = uint16_t(seq - k.recv_latest);
      k.recv_bits = shift > 32 ? 0 : ((shift == 32 ? 0 : k.recv_bits << shift) | (1u << (shift - 1)));
      k.recv_latest = seq;
      return true;
    }
    const uint16_t back = uint16_t(k.recv_latest - seq);
    if (back == 0 || back > 32) return false;
    const uint32_t bit = 1u << (back - 1);
    if (k.recv_bits & bit) return false;
    k.recv_bits |= bit;
    return true;
  }

  int LinkFor(const sockaddr_in& from) const {
    for (int s = 0; s < kMaxSlots; ++s)
      if (links_[s].used && links_[s].addr.sin_addr.s_addr == from.sin_addr.s_addr && links_[s].addr.sin_port == from.sin_port) return s;
    return -1;
  }

  void OnDatagram(const uint8_t* d, int n, const sockaddr_in& from, uint64_t now) {
    if (n < int(sizeof(PacketHeader))) { ++stats_.bad_packets; return; }
    PacketHeader ph; std::memcpy(&ph, d, sizeof(ph));
    if (ph.magic != kMagic) { ++stats_.bad_packets; return; }
    int s = LinkFor(from);
    if (s > 0 && host_ && ph.epoch != links_[s].epoch) {
      // A known address on another join attempt: the client started over (it
      // lost us first, or left and came back before we noticed). A newer
      // attempt with a join replaces the old link; a delayed packet of an
      // older attempt is ignored.
      if (ph.slot == kNoSlot && IsJoin(d, n) && int8_t(uint8_t(ph.epoch - links_[s].epoch)) > 0) {
        Drop(s, "joined again");
        s = AcceptJoin(d, n, ph, from, now);
        if (s < 0) return;
      } else {
        return;
      }
    }
    if (s < 0) {
      if (!host_) return;  // a client only listens to its host
      s = AcceptJoin(d, n, ph, from, now);
      if (s < 0) return;
    }
    if (!host_ && ph.epoch != epoch_) return;  // meant for an earlier join attempt
    Link& k = links_[s];
    ++stats_.packets_in; stats_.bytes_in += uint32_t(n);
    if (!MarkReceived(k, ph.seq)) return;
    k.last_recv = now;   // (only new packets: a dead link's old sequence numbers can't keep it alive)
    if (ph.flags & 1) {
      Acked(k, ph.ack, now);
      for (int i = 0; i < 32; ++i) if (ph.ack_bits & (1u << i)) Acked(k, uint16_t(ph.ack - 1 - i), now);
    }
    for (int o = sizeof(PacketHeader); o + int(sizeof(MessageHeader)) <= n;) {
      MessageHeader h; std::memcpy(&h, d + o, sizeof(h));
      const int total = int(sizeof(h)) + h.len;
      if (o + total > n || h.len > kMaxMessage) { ++stats_.bad_packets; break; }
      std::vector<uint8_t> msg(d + o, d + o + total);
      o += total;
      if (!(h.flags & 1)) { OnMessage(s, h, std::move(msg)); continue; }
      if (h.id == k.rel_expect) {
        ++k.rel_expect;
        OnMessage(s, h, std::move(msg));
        for (auto it = k.rel_hold.find(k.rel_expect); it != k.rel_hold.end(); it = k.rel_hold.find(k.rel_expect)) {
          MessageHeader hh; std::memcpy(&hh, it->second.data(), sizeof(hh));
          std::vector<uint8_t> held = std::move(it->second);
          k.rel_hold.erase(it);
          ++k.rel_expect;
          OnMessage(s, hh, std::move(held));
        }
      } else if (SeqNewer(h.id, k.rel_expect) && uint16_t(h.id - k.rel_expect) < 2 * kRelWindow) {
        k.rel_hold.emplace(h.id, std::move(msg));
      }  // else: an older copy, already delivered
    }
  }

  // The packet's first message is a well-formed join.
  static bool IsJoin(const uint8_t* d, int n) {
    if (n < int(sizeof(PacketHeader) + sizeof(MessageHeader))) return false;
    MessageHeader h; std::memcpy(&h, d + sizeof(PacketHeader), sizeof(h));
    return h.type == kJoin && h.len >= 1 && h.len <= kMaxMessage && int(sizeof(PacketHeader) + sizeof(h) + h.len) <= n;
  }
  // Host: a packet from an unknown address must carry a join.
  int AcceptJoin(const uint8_t* d, int n, const PacketHeader& ph, const sockaddr_in& from, uint64_t now) {
    if (ph.slot != kNoSlot || !IsJoin(d, n)) return -1;
    MessageHeader h; std::memcpy(&h, d + sizeof(PacketHeader), sizeof(h));
    const uint8_t* p = d + sizeof(PacketHeader) + sizeof(h);
    const std::string name(reinterpret_cast<const char*>(p + 1), std::min<size_t>(h.len - 1, kMaxName));
    std::string deny;
    if (p[0] != kVersion) deny = "different co-op version (host " + std::to_string(kVersion) + ", you " + std::to_string(p[0]) + ")";
    int slot = -1;
    for (int s = 1; s < kMaxSlots && slot < 0 && deny.empty(); ++s) if (!links_[s].used) slot = s;
    if (deny.empty() && slot < 0) deny = "the game is full (12 players)";
    if (!deny.empty()) {
      Link tmp; tmp.used = true; tmp.addr = from; tmp.slot = kNoSlot; tmp.epoch = ph.epoch;
      SendControl(tmp, kJoinDeny, deny.data(), uint16_t(deny.size()), false);
      Flush(tmp, now);
      Log("refused a join from " + name + ": " + deny);
      return -1;
    }
    Link& k = links_[slot];
    k = Link{}; k.used = true; k.addr = from; k.slot = uint8_t(slot); k.last_recv = now; k.epoch = ph.epoch;
    names_[slot] = name; present_[slot] = true;
    std::vector<uint8_t> ok{uint8_t(slot), 0};
    for (int s = 0; s < kMaxSlots; ++s)
      if (present_[s]) {
        ++ok[1];
        ok.push_back(uint8_t(s)); ok.push_back(uint8_t(std::min<size_t>(names_[s].size(), 255)));
        ok.insert(ok.end(), names_[s].begin(), names_[s].begin() + ok.back());
      }
    SendControl(k, kJoinOk, ok.data(), uint16_t(ok.size()), true);
    std::vector<uint8_t> pj{uint8_t(slot)};
    pj.insert(pj.end(), name.begin(), name.end());
    for (int s = 1; s < kMaxSlots; ++s) if (s != slot && links_[s].used) SendControl(links_[s], kPeerJoined, pj.data(), uint16_t(pj.size()), true);
    inbox_.push_back({uint8_t(slot), kPeerJoined, std::vector<uint8_t>(pj.begin() + 1, pj.end())});
    Log(name + " joined as slot " + std::to_string(slot));
    return slot;
  }

  void OnMessage(int link_slot, const MessageHeader& h, std::vector<uint8_t> msg) {
    const uint8_t* p = msg.data() + sizeof(MessageHeader);
    if (h.type < kFirstUserType) { OnSession(link_slot, h, p); return; }
    if (host_) {
      // A client's message: the origin must be that client. Relay it on.
      if (h.origin != link_slot) return;
      if (h.target == kEveryone || (h.target != 0 && h.target < kMaxSlots)) {
        for (int s = 1; s < kMaxSlots; ++s)
          if (s != link_slot && links_[s].used && present_[s] && (h.target == kEveryone || h.target == s)) {
            Queue(links_[s], h, p);
          }
      }
      if (h.target != kEveryone && h.target != 0) return;
    }
    inbox_.push_back({h.origin, h.type, std::vector<uint8_t>(p, p + h.len)});
  }

  void OnSession(int link_slot, const MessageHeader& h, const uint8_t* p) {
    switch (h.type) {
      case kJoinOk:
        if (host_ || h.len < 2) return;
        my_slot_ = p[0];
        present_.fill(false);
        for (int i = 0, o = 2; i < p[1] && o + 2 <= h.len; ++i) {
          const uint8_t s = p[o], len = p[o + 1];
          if (s < kMaxSlots && o + 2 + len <= h.len) { names_[s].assign(reinterpret_cast<const char*>(p + o + 2), len); present_[s] = true; }
          o += 2 + len;
        }
        inbox_.push_back({0, kJoinOk, {my_slot_}});
        Log("joined as slot " + std::to_string(my_slot_));
        return;
      case kJoinDeny:
        deny_reason_.assign(reinterpret_cast<const char*>(p), h.len);
        inbox_.push_back({0, kJoinDeny, std::vector<uint8_t>(p, p + h.len)});
        Log("join refused: " + deny_reason_);
        links_[0] = Link{};
        return;
      case kPeerJoined:
        if (host_ || h.len < 1 || p[0] >= kMaxSlots) return;
        names_[p[0]].assign(reinterpret_cast<const char*>(p + 1), h.len - 1);
        present_[p[0]] = true;
        inbox_.push_back({p[0], kPeerJoined, std::vector<uint8_t>(p + 1, p + h.len)});
        return;
      case kPeerLeft:
        if (host_ || h.len < 1 || p[0] >= kMaxSlots) return;
        present_[p[0]] = false;
        inbox_.push_back({p[0], kPeerLeft, {}});
        return;
      case kLeave:
        if (host_ && link_slot > 0) Drop(link_slot, "left");
        return;
      default:
        return;
    }
  }

  void Drop(int s, const char* why) {
    Link& k = links_[s];
    if (!k.used) return;
    Log((s == 0 ? std::string("host") : names_[s] + " (slot " + std::to_string(s) + ")") + " " + why);
    const sockaddr_in addr = k.addr;
    k = Link{};
    if (host_) {
      present_[s] = false;
      const uint8_t slot = uint8_t(s);
      for (int o = 1; o < kMaxSlots; ++o) if (links_[o].used) SendControl(links_[o], kPeerLeft, &slot, 1, true);
      inbox_.push_back({slot, kPeerLeft, {}});
    } else {
      // Lost the host: everyone is gone; the client keeps joining (as v1 did).
      const bool was_joined = my_slot_ != kNoSlot;
      present_.fill(false);
      my_slot_ = kNoSlot;
      epoch_ = NextEpoch();   // a new join attempt
      k.used = true; k.addr = addr; k.slot = 0;
      if (was_joined) inbox_.push_back({0, kPeerLeft, {}});
    }
  }

  SendFn send_;
  RecvFn recv_;
  LogFn log_;
  mutable std::mutex m_;
  bool host_ = false;
  uint8_t my_slot_ = kNoSlot;
  uint8_t epoch_ = 1;      // client: this join attempt
  int flush_start_ = 0;
  std::string my_name_, deny_reason_;
  std::array<Link, kMaxSlots> links_{};  // host: index = client slot; client: index 0 = the host
  std::array<std::string, kMaxSlots> names_{};
  std::array<bool, kMaxSlots> present_{};
  std::deque<struct Received> inbox_;
  Stats stats_;
};

}  // namespace coopnet
