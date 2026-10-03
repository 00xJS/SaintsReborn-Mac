// Multiplayer GANGS: bridge the game's DemonWare 1.7 lobby connection to our
// own server (research/gangs-restoration.md).
//
// SR1's gangs (form / list / invites / gang stats / Gang Match) live on
// DemonWare's "MatchMaking Service", reached through the Live title server
// list (service 0x545107EE, port 1007) that no longer exists. The DemonWare
// lobby runs over a bdStreamConnection whose socket is a plain TCP stream of
// frames: u16 little-endian length (bytes that follow), u8 encrypted flag (0),
// u8 message type, typed bdBitBuffer payload. A length of 0 is a keep-alive.
//
// Here:
//  * sub_823563E0 (title-server lookup) hands the game a fixed address
//    (kFakeIp:1007) instead of enumerating Live servers;
//  * the DemonWare stream socket calls (connect 8275A9F8, send 8275AC78,
//    recv 8275ADC8, close 8275AAE0) to that address never touch the network:
//    complete frames are POSTed to the gangs server (kDefaultServer, or the URL in gangs_server.txt
//    next to the exe) and the frames it answers with are fed back to recv.
//    gangs_server.txt containing "off" only logs the frames (capture mode).
//  * gangs_off (file next to the exe) turns the whole bridge off.
// Every frame is logged to gangs_frames.log (hex).
#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

extern "C" void __imp__sub_823563E0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8275A9F8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8275AC78(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8275ADC8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8275AAE0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8274C4E0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826BF8C0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826BF628(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826BA690(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826B8A18(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826BB368(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826B8F48(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82356998(PPCContext& ctx, uint8_t* base);

namespace {

constexpr uint32_t kFakeIp = 0xC6336401u;  // 198.51.100.1 (TEST-NET-2, never a real host)
constexpr uint16_t kDwPort = 1007;

uint8_t* Host(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
uint32_t R32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Host(base, a), 4);
  return __builtin_bswap32(v);
}
void W32(uint8_t* base, uint32_t a, uint32_t v) {
  const uint32_t be = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &be, 4);
}
void W16(uint8_t* base, uint32_t a, uint16_t v) {
  uint8_t* p = Host(base, a);
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}

bool FileExists(const char* name) {
  FILE* f = std::fopen(name, "rb");
  if (f) std::fclose(f);
  return f != nullptr;
}
std::string ReadLine(const char* name);
// The Saints Reborn gangs server (server/gangs, Cloudflare Worker).
constexpr const char* kDefaultServer = "https://saints-reborn-gangs.saintsreborn.workers.dev";
// gangs_server.txt next to the exe overrides it (another server, or "off").
std::string ServerUrl() {
  std::string url = ReadLine("gangs_server.txt");
  if (url.empty()) url = kDefaultServer;
  if (url == "off") url.clear();
  return url;
}
std::string ReadLine(const char* name) {
  std::string s;
  if (FILE* f = std::fopen(name, "rb")) {
    char buf[1024];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    s.assign(buf, n);
  }
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
  while (!s.empty() && (s.front() == ' ' || s.front() == '\xEF' || s.front() == '\xBB' || s.front() == '\xBF')) s.erase(0, 1);
  return s;
}

bool Enabled() {
  static const bool on = [] {
    const bool off = FileExists("gangs_off");
    REXLOG_INFO("GANGS: bridge {} (server: {})", off ? "OFF (gangs_off)" : "on",
                ServerUrl().empty() ? "none, capture only" : ServerUrl());
    return !off;
  }();
  return on;
}

// Random per-install key: the server ties a player's XUID to it the first time
// it sees that XUID, so nobody else can act as that player afterwards.
std::string InstallKey() {
  static const std::string key = [] {
    std::string k = ReadLine("gangs_key.txt");
    if (k.size() >= 32) return k;
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    k.clear();
    for (int i = 0; i < 40; ++i) k.push_back(hex[rd() & 15]);
    if (FILE* f = std::fopen("gangs_key.txt", "wb")) {
      std::fwrite(k.data(), 1, k.size(), f);
      std::fclose(f);
    }
    return k;
  }();
  return key;
}

std::string Hex(const uint8_t* p, size_t n, size_t max = 4096) {
  static const char* h = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n && i < max; ++i) {
    s.push_back(h[p[i] >> 4]);
    s.push_back(h[p[i] & 15]);
  }
  if (n > max) s += "...";
  return s;
}

std::mutex g_log_mutex;
void LogFrame(const char* dir, const std::vector<uint8_t>& f) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (FILE* out = std::fopen("gangs_frames.log", "ab")) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(out, "%s %s %zu %s\n", ts, dir, f.size(), Hex(f.data(), f.size(), 1 << 20).c_str());
    std::fclose(out);
  }
  const uint8_t type = f.size() >= 4 ? f[3] : 0;
  REXLOG_INFO("GANGS {} frame {} bytes, type {}: {}", dir, f.size(), type, Hex(f.data(), f.size(), 96));
}

// ---------------------------------------------------------------- session ---
struct Session {
  uint32_t handle = 0;          // the game's socket handle (guest value)
  std::string id;               // random id sent with every request
  std::vector<uint8_t> out;     // bytes the game sent, not yet a whole frame
  std::deque<std::vector<uint8_t>> frames;  // whole frames waiting for the server
  std::vector<uint8_t> in;      // server bytes the game has not read yet
  bool closed = false;
  bool failed = false;          // server unreachable -> recv reports a reset
};

std::mutex g_mutex;
std::condition_variable g_cv;
Session* g_session = nullptr;  // one DemonWare lobby connection at a time
std::atomic<bool> g_worker_started{false};

#ifdef _WIN32
// POST body to url; returns false when the server could not be reached.
bool Post(const std::string& url, const std::string& session, const std::vector<uint8_t>& body,
          std::vector<uint8_t>& reply, int& status) {
  status = 0;
  reply.clear();
  std::wstring wurl(url.begin(), url.end());
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t hostname[256] = {}, path[1024] = {};
  uc.lpszHostName = hostname;
  uc.dwHostNameLength = 255;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 1023;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
  static HINTERNET s = WinHttpOpen(L"SaintsReborn-Gangs/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) return false;
  WinHttpSetTimeouts(s, 5000, 5000, 10000, 15000);
  HINTERNET c = WinHttpConnect(s, hostname, uc.nPort, 0);
  if (!c) return false;
  std::wstring p = path;
  p += L"/dw";
  HINTERNET r = WinHttpOpenRequest(c, L"POST", p.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  bool ok = false;
  if (r) {
    std::string h = "Content-Type: application/octet-stream\r\nX-SR-Session: " + session +
                    "\r\nX-SR-Key: " + InstallKey() + "\r\nX-SR-Version: 2\r\n";
    // Our Epic id: friends see this player under XuidFromPuid(puid) (the runtime's
    // friends list), the server maps that to the gang account.
    {
      using Fn = int (*)(char*, int);
      HMODULE m = GetModuleHandleW(L"rexruntime.dll");
      const Fn fn = m ? reinterpret_cast<Fn>(GetProcAddress(m, "SrMyPuid")) : nullptr;
      char puid[128] = {};
      if (fn && fn(puid, sizeof(puid)) > 0) h += std::string("X-SR-Puid: ") + puid + "\r\n";
    }
    std::wstring wh(h.begin(), h.end());
    if (WinHttpSendRequest(r, wh.c_str(), DWORD(-1), body.empty() ? nullptr : (void*)body.data(),
                           DWORD(body.size()), DWORD(body.size()), 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
      DWORD code = 0, len = sizeof(code);
      WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len,
                          nullptr);
      status = int(code);
      for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
        const size_t at = reply.size();
        reply.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(r, reply.data() + at, avail, &got)) break;
        reply.resize(at + got);
      }
      ok = true;
    }
    WinHttpCloseHandle(r);
  }
  WinHttpCloseHandle(c);
  return ok;
}
#else
bool Post(const std::string&, const std::string&, const std::vector<uint8_t>&, std::vector<uint8_t>&, int& s) {
  s = 0;
  return false;
}
#endif

// Splits server bytes into frames for the log; all of it goes to the game.
void Deliver(Session* ss, const std::vector<uint8_t>& reply) {
  size_t at = 0;
  while (at + 2 <= reply.size()) {
    const size_t len = size_t(reply[at]) | size_t(reply[at + 1]) << 8;
    if (at + 2 + len > reply.size()) break;
    std::vector<uint8_t> f(reply.begin() + at, reply.begin() + at + 2 + len);
    LogFrame("<-", f);
    at += 2 + len;
  }
  ss->in.insert(ss->in.end(), reply.begin(), reply.begin() + at);
}

void Worker() {
  const std::string url = ServerUrl();
  auto last = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lock(g_mutex);
  for (;;) {  // runs for the whole game; idle while there is no lobby connection
    g_cv.wait_for(lock, std::chrono::milliseconds(500));
    Session* ss = g_session;
    if (!ss) continue;
    const bool poll = std::chrono::steady_clock::now() - last > std::chrono::seconds(4);
    if (ss->frames.empty() && !poll) continue;
    std::vector<uint8_t> body;
    for (auto& f : ss->frames) body.insert(body.end(), f.begin(), f.end());
    ss->frames.clear();
    const std::string id = ss->id;
    last = std::chrono::steady_clock::now();
    if (url.empty()) continue;  // capture only
    lock.unlock();
    std::vector<uint8_t> reply;
    int status = 0;
    const bool ok = Post(url, id, body, reply, status);
    lock.lock();
    if (g_session != ss) continue;
    if (!ok || status != 200) {
      REXLOG_WARN("GANGS: server {} (http {}, {} bytes sent)", ok ? "refused" : "unreachable", status,
                  body.size());
      if (!ok || status >= 500 || status == 403) ss->failed = true;
      continue;
    }
    Deliver(ss, reply);
  }
}

}  // namespace

bool SrGangsActive() { return g_session != nullptr; }

// Title-server lookup: one server, our fixed address.
PPC_FUNC(sub_823563E0) {
  if (!Enabled()) {
    __imp__sub_823563E0(ctx, base);
    return;
  }
  const uint32_t addr = ctx.r3.u32;  // bdAddr out: +0 IPv4, +4 port
  W32(base, addr, kFakeIp);
  W16(base, addr + 4, kDwPort);
  W16(base, addr + 6, 0);
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("GANGS: DemonWare server lookup -> bridge");
  }
  ctx.r3.u64 = 1;
}

// bdPlatformSocket::connect(handle*, inaddr*, port)
PPC_FUNC(sub_8275A9F8) {
  const uint32_t ip = R32(base, ctx.r4.u32);
  if (!Enabled() || ip != kFakeIp) {
    __imp__sub_8275A9F8(ctx, base);
    return;
  }
  const uint32_t handle = R32(base, ctx.r3.u32);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_session) g_session->closed = true;
    delete g_session;
    g_session = new Session();
    g_session->handle = handle;
    std::random_device rd;
    char id[33];
    std::snprintf(id, sizeof(id), "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
    g_session->id = id;
    if (!g_worker_started.exchange(true)) std::thread(Worker).detach();
    REXLOG_INFO("GANGS: lobby connect (socket {:08X}, session {})", handle, g_session->id);
  }
  ctx.r3.u64 = 1;  // connected
}

// bdPlatformSocket::send(handle*, buf, len) -> bytes or negative error
PPC_FUNC(sub_8275AC78) {
  const uint32_t handle = R32(base, ctx.r3.u32);
  std::unique_lock<std::mutex> lock(g_mutex);
  Session* ss = g_session;
  if (!ss || ss->handle != handle) {
    lock.unlock();
    __imp__sub_8275AC78(ctx, base);
    return;
  }
  const uint32_t buf = ctx.r4.u32, len = ctx.r5.u32;
  if (ss->failed) {
    ctx.r3.s64 = -5;  // connection reset
    return;
  }
  const uint8_t* p = Host(base, buf);
  ss->out.insert(ss->out.end(), p, p + len);
  while (ss->out.size() >= 2) {
    const size_t flen = size_t(ss->out[0]) | size_t(ss->out[1]) << 8;
    if (ss->out.size() < 2 + flen) break;
    std::vector<uint8_t> f(ss->out.begin(), ss->out.begin() + 2 + flen);
    ss->out.erase(ss->out.begin(), ss->out.begin() + 2 + flen);
    if (flen == 0) continue;  // keep-alive
    LogFrame("->", f);
    ss->frames.push_back(std::move(f));
  }
  g_cv.notify_all();
  ctx.r3.u64 = len;
}

// bdPlatformSocket::recv(handle*, buf, len) -> bytes, -2 would block, -5 reset
PPC_FUNC(sub_8275ADC8) {
  const uint32_t handle = R32(base, ctx.r3.u32);
  std::unique_lock<std::mutex> lock(g_mutex);
  Session* ss = g_session;
  if (!ss || ss->handle != handle) {
    lock.unlock();
    __imp__sub_8275ADC8(ctx, base);
    return;
  }
  static int empty_calls = 0;
  if (ss->in.empty()) {
    if (++empty_calls % 300 == 1) REXLOG_INFO("GANGS: recv empty x{}", empty_calls);
    ctx.r3.s64 = ss->failed ? -5 : -2;
    return;
  }
  const uint32_t buf = ctx.r4.u32;
  const size_t n = std::min<size_t>(ctx.r5.u32, ss->in.size());
  std::memcpy(Host(base, buf), ss->in.data(), n);
  REXLOG_INFO("GANGS: recv gave {} of {} bytes (asked {})", n, ss->in.size(), ctx.r5.u32);
  ss->in.erase(ss->in.begin(), ss->in.begin() + n);
  ctx.r3.u64 = n;
}

// bdPlatformSocket::close(handle*)
PPC_FUNC(sub_8275AAE0) {
  const uint32_t handle = R32(base, ctx.r3.u32);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_session && g_session->handle == handle) {
      REXLOG_INFO("GANGS: lobby closed (session {})", g_session->id);
      delete g_session;
      g_session = nullptr;
      g_cv.notify_all();
    }
  }
  __imp__sub_8275AAE0(ctx, base);
}

// ---- diagnostics (DemonWare reply path) ----
namespace { std::atomic<int> g_diag{0}; bool Diag() { return g_session && g_diag++ < 400; } }
PPC_FUNC(sub_8274C4E0) {  // bdStreamConnection::getMessageToDispatch
  const uint32_t self = ctx.r3.u32;
  __imp__sub_8274C4E0(ctx, base);
  if (ctx.r3.u8 && Diag()) REXLOG_INFO("GANGS diag: message dispatched (conn {:08X}, status {})", self, R32(base, self + 96));
}
PPC_FUNC(sub_826BF8C0) {  // bdRemoteTaskManager::handleTaskReply
  if (Diag()) REXLOG_INFO("GANGS diag: handleTaskReply mgr {:08X}", ctx.r3.u32);
  __imp__sub_826BF8C0(ctx, base);
}
PPC_FUNC(sub_826BF628) {  // find task (mgr, slot, seq)
  const uint32_t slot = ctx.r4.u32, seq = ctx.r5.u32 & 0xff;
  __imp__sub_826BF628(ctx, base);
  if (Diag()) REXLOG_INFO("GANGS diag: find task slot {} seq {} -> {:08X}", slot, seq, ctx.r3.u32);
}
PPC_FUNC(sub_826BA690) {  // read task result
  const uint32_t self = ctx.r3.u32, task = ctx.r4.u32;
  __imp__sub_826BA690(ctx, base);
  if (Diag()) REXLOG_INFO("GANGS diag: task result {:08X} -> ok {} err {}", task, ctx.r3.u8, R32(base, self + 4));
}

PPC_FUNC(sub_826BB368) {
  __imp__sub_826BB368(ctx, base);
  if (Diag()) REXLOG_INFO("GANGS diag: profiles result -> {}", ctx.r3.u8);
}
PPC_FUNC(sub_826B8F48) {  // bdLobbyService::getStatus (pumps messages)
  static int n = 0;
  __imp__sub_826B8F48(ctx, base);
  if (g_session && ++n % 300 == 1) REXLOG_INFO("GANGS diag: lobby status {} (x{}) from {:08X}", ctx.r3.u32, n, uint32_t(ctx.lr));
}

PPC_FUNC(sub_82356998) {  // game DemonWare manager tick
  static int n = 0;
  if (g_session && ++n % 300 == 1) REXLOG_INFO("GANGS diag: manager tick x{}", n);
  __imp__sub_82356998(ctx, base);
}
