// Epic Online Services link for the co-op session: anonymous device-ID login,
// a lobby whose ID is the join code, and P2P packets (NAT traversal, Epic's
// relays as fallback). Used by NetSend / NetRecv in coop.cpp when a session
// is started online. Everything runs on the session's network thread.
//
// The EOS SDK DLL (EOSSDK-Win64-Shipping.dll) is loaded at run time from the
// mod folder; credentials come from eos.ini next to it ([eos] product,
// sandbox, deployment, client_id, client_secret). Without them the online
// options just report that EOS isn't set up.
#pragma once
#ifdef WHOMPAYS_EOS
#include "eos_sdk.h"
#include "eos_logging.h"
#include "eos_connect.h"
#include "eos_lobby.h"
#include "eos_p2p.h"

namespace eos {

// ---- SDK functions, loaded from the DLL ----
#define EOS_FUNCS(X) \
  X(EOS_Initialize) X(EOS_Platform_Create) X(EOS_Platform_Tick) X(EOS_Platform_GetConnectInterface) \
  X(EOS_Platform_GetLobbyInterface) X(EOS_Platform_GetP2PInterface) X(EOS_Logging_SetCallback) \
  X(EOS_Logging_SetLogLevel) X(EOS_EResult_ToString) X(EOS_ProductUserId_ToString) \
  X(EOS_Connect_CreateDeviceId) X(EOS_Connect_Login) X(EOS_Connect_CreateUser) \
  X(EOS_Lobby_CreateLobby) X(EOS_Lobby_JoinLobbyById) X(EOS_Lobby_LeaveLobby) X(EOS_Lobby_DestroyLobby) \
  X(EOS_Lobby_CopyLobbyDetailsHandle) X(EOS_LobbyDetails_GetLobbyOwner) X(EOS_LobbyDetails_Release) \
  X(EOS_P2P_SendPacket) X(EOS_P2P_ReceivePacket) X(EOS_P2P_GetNextReceivedPacketSize) \
  X(EOS_P2P_AcceptConnection) X(EOS_P2P_AddNotifyPeerConnectionRequest) X(EOS_P2P_RemoveNotifyPeerConnectionRequest) \
  X(EOS_Platform_Release)
#define EOS_PTR(name) inline decltype(&::name) p_##name = nullptr;
EOS_FUNCS(EOS_PTR)
#undef EOS_PTR

enum class State { Off, Starting, LoggingIn, MakingLobby, JoiningLobby, Ready, Failed };
inline State state = State::Off;
inline bool loaded = false, platform_ok = false, is_host = false;
inline HMODULE dll = nullptr;
inline EOS_HPlatform platform = nullptr;
inline EOS_HConnect connect = nullptr;
inline EOS_HLobby lobby = nullptr;
inline EOS_HP2P p2p = nullptr;
inline EOS_ProductUserId me = nullptr;
inline EOS_NotificationId request_note = EOS_INVALID_NOTIFICATIONID;
inline EOS_P2P_SocketId socket_id{};
inline std::string lobby_id, code, error;
inline std::mutex text_mutex;           // code / error / state text are read by the overlay
inline std::string status_text;
// Remote players: product user ID <-> stand-in address 10.255.0.N (N = index + 1).
inline std::vector<EOS_ProductUserId> peers;
inline EOS_ProductUserId host_id = nullptr;
inline void (*log_fn)(const char*) = nullptr;

inline void Log(const std::string& s) { if (log_fn) log_fn(("EOS: " + s).c_str()); }
inline void SetText(const std::string& s) { std::lock_guard l(text_mutex); status_text = s; Log(s); }
inline std::string Text() { std::lock_guard l(text_mutex); return status_text; }
inline std::string Code() { std::lock_guard l(text_mutex); return code; }
inline const char* R(EOS_EResult r) { return p_EOS_EResult_ToString ? p_EOS_EResult_ToString(r) : "?"; }
inline void Fail(const std::string& why) { state = State::Failed; SetText("Online failed: " + why); }

inline sockaddr_in AddressOf(size_t index) {
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(1);
  a.sin_addr.s_addr = htonl((10u << 24) | (255u << 16) | uint32_t(index + 1));
  return a;
}
inline sockaddr_in AddressOf(EOS_ProductUserId id) {
  for (size_t i = 0; i < peers.size(); ++i) if (peers[i] == id) return AddressOf(i);
  peers.push_back(id);
  char s[EOS_PRODUCTUSERID_MAX_LENGTH + 1]{}; int32_t n = sizeof(s);
  if (p_EOS_ProductUserId_ToString) p_EOS_ProductUserId_ToString(id, s, &n);
  Log(std::string("remote player ") + s + " = 10.255.0." + std::to_string(peers.size()));
  return AddressOf(peers.size() - 1);
}
inline EOS_ProductUserId IdOf(const sockaddr_in& a) {
  const uint32_t ip = ntohl(a.sin_addr.s_addr);
  if ((ip >> 16) != ((10u << 8) | 255u)) return nullptr;
  const uint32_t n = ip & 0xFFFF;
  return n >= 1 && n <= peers.size() ? peers[n - 1] : nullptr;
}

inline std::string Ini(const std::string& file, const char* key) {
  char v[256]{};
  GetPrivateProfileStringA("eos", key, "", v, sizeof(v), file.c_str());
  std::string s(v);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
  return s;
}

// The game (rexruntime: System Link over the internet, MULTIPLAYER > PLAYERS)
// runs its own EOS platform on its own thread; a second one next to it
// corrupts the heap. So co-op online borrows Epic from the game for the
// session (SrEosLend: the game leaves its lobby and releases its platform)
// and gives it back at the end (SrEosReturn).
inline bool lent = false;
inline FARPROC RuntimeFunction(const char* name) {
  HMODULE m = GetModuleHandleA("rexruntime.dll");
  return m ? GetProcAddress(m, name) : nullptr;
}
inline bool Borrow() {
  if (lent) return true;
  if (auto lend = reinterpret_cast<int (*)(int)>(RuntimeFunction("SrEosLend"))) {
    lent = true;
    if (!lend(5000)) {
      error = "the game is still using Epic - try again in a few seconds";
      return false;
    }
    Log("the game handed Epic over for co-op");
    return true;
  }
  // An older game without SrEosLend.
  if (!loaded && GetModuleHandleA("EOSSDK-Win64-Shipping.dll")) {
    error = "System Link online already uses Epic in this game run - restart the game to play co-op online";
    return false;
  }
  return true;
}
inline void GiveBack() {
  if (!lent) return;
  lent = false;
  if (auto give = reinterpret_cast<void (*)()>(RuntimeFunction("SrEosReturn"))) give();
  Log("Epic handed back to the game");
}

// Load the DLL (once per game run) and create the platform (per session).
inline bool Platform(const std::string& folder) {
  if (platform_ok) return true;
  if (!Borrow()) return false;
  if (!loaded) {
    dll = LoadLibraryA((folder + "\\eos\\EOSSDK-Win64-Shipping.dll").c_str());
    if (!dll) { error = "eos\\EOSSDK-Win64-Shipping.dll missing in the mod folder"; return false; }
#define EOS_LOAD(name) p_##name = reinterpret_cast<decltype(p_##name)>(GetProcAddress(dll, #name)); \
    if (!p_##name) { error = "EOS DLL has no " #name; return false; }
    EOS_FUNCS(EOS_LOAD)
#undef EOS_LOAD
    loaded = true;
    EOS_InitializeOptions init{};
    init.ApiVersion = EOS_INITIALIZE_API_LATEST;
    init.ProductName = "Saints Reborn";
    init.ProductVersion = "1.0";
    const EOS_EResult r = p_EOS_Initialize(&init);
    if (r != EOS_EResult::EOS_Success && r != EOS_EResult::EOS_AlreadyConfigured) { error = std::string("EOS_Initialize ") + R(r); return false; }
    p_EOS_Logging_SetCallback([](const EOS_LogMessage* m) {
      if (m && m->Message) Log(std::string("[") + (m->Category ? m->Category : "") + "] " + m->Message);
    });
    p_EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Warning);
  }
  const std::string ini = folder + "\\eos.ini";
  const std::string product = Ini(ini, "product"), sandbox = Ini(ini, "sandbox"), deployment = Ini(ini, "deployment"),
                    client = Ini(ini, "client_id"), secret = Ini(ini, "client_secret");
  if (product.empty() || sandbox.empty() || deployment.empty() || client.empty() || secret.empty()) {
    error = "eos.ini (product, sandbox, deployment, client_id, client_secret) missing or incomplete";
    return false;
  }
  EOS_Platform_Options o{};
  o.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
  o.ProductId = product.c_str(); o.SandboxId = sandbox.c_str(); o.DeploymentId = deployment.c_str();
  o.ClientCredentials.ClientId = client.c_str(); o.ClientCredentials.ClientSecret = secret.c_str();
  o.bIsServer = EOS_FALSE;
  o.Flags = EOS_PF_DISABLE_OVERLAY;
  platform = p_EOS_Platform_Create(&o);
  if (!platform) { error = "EOS_Platform_Create failed (check the IDs in eos.ini)"; return false; }
  connect = p_EOS_Platform_GetConnectInterface(platform);
  lobby = p_EOS_Platform_GetLobbyInterface(platform);
  p2p = p_EOS_Platform_GetP2PInterface(platform);
  std::snprintf(socket_id.SocketName, sizeof(socket_id.SocketName), "WHOMPAYSCOOP");
  socket_id.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
  platform_ok = true;
  return true;
}

inline std::string NewCode() {
  static const char k[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O, 1/I
  std::string c;
  LARGE_INTEGER t; QueryPerformanceCounter(&t);
  uint64_t x = uint64_t(t.QuadPart) ^ (uint64_t(GetCurrentProcessId()) << 32) ^ GetTickCount64() * 0x9E3779B97F4A7C15ull;
  for (int i = 0; i < 6; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; c += k[x % 32]; }
  return c;
}
inline std::string LobbyIdFor(const std::string& c) { return "saintsreborn-" + c; }

inline void AcceptFrom(EOS_ProductUserId remote) {
  EOS_P2P_AcceptConnectionOptions a{};
  a.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
  a.LocalUserId = me; a.RemoteUserId = remote; a.SocketId = &socket_id;
  p_EOS_P2P_AcceptConnection(p2p, &a);
}

inline void Online();  // after login: make or join the lobby

inline void OnLogin(const EOS_Connect_LoginCallbackInfo* d) {
  if (d->ResultCode == EOS_EResult::EOS_Success) { me = d->LocalUserId; Online(); return; }
  if (d->ResultCode == EOS_EResult::EOS_InvalidUser && d->ContinuanceToken) {
    EOS_Connect_CreateUserOptions c{};
    c.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST; c.ContinuanceToken = d->ContinuanceToken;
    p_EOS_Connect_CreateUser(connect, &c, nullptr, [](const EOS_Connect_CreateUserCallbackInfo* u) {
      if (u->ResultCode != EOS_EResult::EOS_Success) { Fail(std::string("creating the player ID: ") + R(u->ResultCode)); return; }
      me = u->LocalUserId; Online();
    });
    return;
  }
  Fail(std::string("login: ") + R(d->ResultCode));
}

inline std::string display_name;
inline void Login() {
  state = State::LoggingIn; SetText("Logging in to Epic Online Services...");
  EOS_Connect_CreateDeviceIdOptions c{};
  c.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST; c.DeviceModel = "PC";
  p_EOS_Connect_CreateDeviceId(connect, &c, nullptr, [](const EOS_Connect_CreateDeviceIdCallbackInfo* d) {
    if (d->ResultCode != EOS_EResult::EOS_Success && d->ResultCode != EOS_EResult::EOS_DuplicateNotAllowed) {
      Fail(std::string("device ID: ") + R(d->ResultCode)); return;
    }
    static EOS_Connect_Credentials cred{};
    cred.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST; cred.Token = nullptr; cred.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
    static EOS_Connect_UserLoginInfo info{};
    info.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST; info.DisplayName = display_name.c_str();
    EOS_Connect_LoginOptions l{};
    l.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST; l.Credentials = &cred; l.UserLoginInfo = &info;
    p_EOS_Connect_Login(connect, &l, nullptr, [](const EOS_Connect_LoginCallbackInfo* r) { OnLogin(r); });
  });
}

inline std::string wanted_code;
inline uint32_t max_players = 2;

inline void FindHost() {
  EOS_Lobby_CopyLobbyDetailsHandleOptions c{};
  c.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST; c.LobbyId = lobby_id.c_str(); c.LocalUserId = me;
  EOS_HLobbyDetails details = nullptr;
  if (p_EOS_Lobby_CopyLobbyDetailsHandle(lobby, &c, &details) != EOS_EResult::EOS_Success || !details) { Fail("could not read the lobby"); return; }
  EOS_LobbyDetails_GetLobbyOwnerOptions o{}; o.ApiVersion = EOS_LOBBYDETAILS_GETLOBBYOWNER_API_LATEST;
  host_id = p_EOS_LobbyDetails_GetLobbyOwner(details, &o);
  p_EOS_LobbyDetails_Release(details);
  if (!host_id) { Fail("lobby has no host"); return; }
  AddressOf(host_id);  // the host is always 10.255.0.1 on a joiner
  AcceptFrom(host_id);
  state = State::Ready; SetText("Joined " + Code() + ", connecting to the host...");
}

inline void Online() {
  // Incoming P2P connections are accepted from anyone in our lobby (for now: anyone with the code).
  EOS_P2P_AddNotifyPeerConnectionRequestOptions n{};
  n.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST; n.LocalUserId = me; n.SocketId = &socket_id;
  request_note = p_EOS_P2P_AddNotifyPeerConnectionRequest(p2p, &n, nullptr, [](const EOS_P2P_OnIncomingConnectionRequestInfo* i) {
    AcceptFrom(i->RemoteUserId);
    Log("accepted a connection");
  });
  if (is_host) {
    state = State::MakingLobby; SetText("Creating the lobby...");
    { std::lock_guard l(text_mutex); code = NewCode(); }
    lobby_id = LobbyIdFor(Code());
    EOS_Lobby_CreateLobbyOptions c{};
    c.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
    c.LocalUserId = me; c.MaxLobbyMembers = max_players;
    c.PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_JOINVIAPRESENCE;
    c.bPresenceEnabled = EOS_FALSE; c.bAllowInvites = EOS_TRUE;
    c.BucketId = "saintsreborn:coop"; c.bDisableHostMigration = EOS_TRUE;
    c.LobbyId = lobby_id.c_str(); c.bEnableJoinById = EOS_TRUE;
    p_EOS_Lobby_CreateLobby(lobby, &c, nullptr, [](const EOS_Lobby_CreateLobbyCallbackInfo* d) {
      if (d->ResultCode != EOS_EResult::EOS_Success) { Fail(std::string("creating the lobby: ") + R(d->ResultCode)); return; }
      state = State::Ready; SetText("Online. Join code: " + Code() + "  (waiting for a player)");
    });
  } else {
    state = State::JoiningLobby; SetText("Joining " + Code() + "...");
    lobby_id = LobbyIdFor(Code());
    EOS_Lobby_JoinLobbyByIdOptions j{};
    j.ApiVersion = EOS_LOBBY_JOINLOBBYBYID_API_LATEST;
    j.LobbyId = lobby_id.c_str(); j.LocalUserId = me; j.bPresenceEnabled = EOS_FALSE;
    p_EOS_Lobby_JoinLobbyById(lobby, &j, nullptr, [](const EOS_Lobby_JoinLobbyByIdCallbackInfo* d) {
      if (d->ResultCode != EOS_EResult::EOS_Success) {
        Fail(d->ResultCode == EOS_EResult::EOS_NotFound ? "no game with that code" : std::string("joining: ") + R(d->ResultCode));
        return;
      }
      FindHost();
    });
  }
}

// Start an online session (network thread). Returns false with `error` set.
inline bool Begin(const std::string& folder, bool host, const std::string& join_code, const std::string& name, uint32_t players) {
  peers.clear(); host_id = nullptr; lobby_id.clear(); error.clear();
  is_host = host; max_players = players; display_name = name.substr(0, EOS_CONNECT_USERLOGININFO_DISPLAYNAME_MAX_LENGTH);
  if (display_name.empty()) display_name = "Player";
  { std::lock_guard l(text_mutex); code = host ? "" : join_code; }
  state = State::Starting;
  if (!Platform(folder)) { GiveBack(); Fail(error); return false; }
  if (me) Online(); else Login();
  return true;
}

inline void Tick() { if (platform_ok) p_EOS_Platform_Tick(platform); }

inline int Send(const void* data, int length, const sockaddr_in& to) {
  if (state != State::Ready || !me) return -1;
  EOS_ProductUserId remote = IdOf(to);
  if (!remote) return -1;
  EOS_P2P_SendPacketOptions s{};
  s.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
  s.LocalUserId = me; s.RemoteUserId = remote; s.SocketId = &socket_id; s.Channel = 0;
  s.DataLengthBytes = uint32_t(length); s.Data = data;
  s.bAllowDelayedDelivery = EOS_TRUE;
  s.Reliability = EOS_EPacketReliability::EOS_PR_UnreliableUnordered;
  s.bDisableAutoAcceptConnection = EOS_FALSE;
  return p_EOS_P2P_SendPacket(p2p, &s) == EOS_EResult::EOS_Success ? length : -1;
}

inline int Receive(char* buffer, int capacity, sockaddr_in& from) {
  Tick();
  if (!me || !p2p) return -1;
  EOS_P2P_ReceivePacketOptions r{};
  r.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST; r.LocalUserId = me; r.MaxDataSizeBytes = uint32_t(capacity); r.RequestedChannel = nullptr;
  EOS_ProductUserId peer = nullptr; EOS_P2P_SocketId sid{}; uint8_t channel = 0; uint32_t written = 0;
  if (p_EOS_P2P_ReceivePacket(p2p, &r, &peer, &sid, &channel, buffer, &written) != EOS_EResult::EOS_Success || !peer) return -1;
  from = AddressOf(peer);
  return int(written);
}

// Leave / close the lobby (network thread, at the end of the session).
inline void End() {
  if (!platform_ok) { GiveBack(); state = State::Off; return; }
  if (request_note != EOS_INVALID_NOTIFICATIONID) { p_EOS_P2P_RemoveNotifyPeerConnectionRequest(p2p, request_note); request_note = EOS_INVALID_NOTIFICATIONID; }
  if (me && !lobby_id.empty()) {
    if (is_host) {
      EOS_Lobby_DestroyLobbyOptions d{}; d.ApiVersion = EOS_LOBBY_DESTROYLOBBY_API_LATEST; d.LocalUserId = me; d.LobbyId = lobby_id.c_str();
      p_EOS_Lobby_DestroyLobby(lobby, &d, nullptr, [](const EOS_Lobby_DestroyLobbyCallbackInfo*) {});
    } else {
      EOS_Lobby_LeaveLobbyOptions d{}; d.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST; d.LocalUserId = me; d.LobbyId = lobby_id.c_str();
      p_EOS_Lobby_LeaveLobby(lobby, &d, nullptr, [](const EOS_Lobby_LeaveLobbyCallbackInfo*) {});
    }
    for (int i = 0; i < 60; ++i) { Tick(); Sleep(5); }
  }
  lobby_id.clear(); peers.clear(); host_id = nullptr;
  // Release the platform so the game can use Epic again (PLAYERS list,
  // System Link) without a restart; the next co-op session logs in again.
  p_EOS_Platform_Release(platform);
  platform = nullptr; connect = nullptr; lobby = nullptr; p2p = nullptr; me = nullptr;
  platform_ok = false;
  GiveBack();
  state = State::Off;
  { std::lock_guard l(text_mutex); code.clear(); status_text.clear(); }
}

}  // namespace eos
#endif
