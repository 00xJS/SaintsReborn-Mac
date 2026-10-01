#include "assetlink.h"

#include <shlobj.h>

#include <algorithm>
#include <chrono>

namespace editor {
namespace fs = std::filesystem;

namespace {

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), 0);
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::vector<std::string> Split(const std::string& s) {
  std::vector<std::string> out;
  size_t a = 0;
  for (;;) {
    size_t b = s.find('\t', a);
    out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

// File-name safe version of a texture name ("a23_bbrick_co.tga" stays as it is).
std::string SafeName(const std::string& s) {
  std::string out;
  for (unsigned char c : s) out += (std::isalnum(c) || c == '.' || c == '_' || c == '-') ? char(std::tolower(c)) : '_';
  return out;
}

std::string Stem(const std::string& s) {
  size_t dot = s.find('.');
  return SafeName(dot == std::string::npos ? s : s.substr(0, dot));
}

fs::path FindGlb(const fs::path& dir) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) return {};
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    if (it->path().extension() == ".glb") return it->path();
  return {};
}

}  // namespace

fs::path ResolveGameDir(const fs::path& picked) {
  if (picked.empty()) return {};
  std::error_code ec;
  for (const fs::path& c : {picked, picked / "game", picked / "dist" / "game", picked.parent_path()})
    if (fs::exists(c / "packfiles" / "mp_city_stream.vpp_xbox2", ec) && fs::exists(c / "packfiles" / "smesh.vpp_xbox2", ec))
      return c;
  return {};
}

fs::path AssetCacheDir(const fs::path& game_dir) {
  wchar_t* local = nullptr;
  fs::path base;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) base = fs::path(local);
  CoTaskMemFree(local);
  if (base.empty()) base = fs::temp_directory_path();
  uint32_t h = 2166136261u;
  for (wchar_t c : fs::weakly_canonical(game_dir).wstring()) h = (h ^ uint32_t(towlower(c))) * 16777619u;
  char id[16];
  snprintf(id, sizeof id, "%08x", h);
  return base / "SaintsReborn" / "MapEditor" / "cache" / id;
}

std::string AssetLink::error() const {
  std::lock_guard<std::mutex> l(mu_);
  return error_;
}

void AssetLink::Start(const fs::path& exe, const fs::path& game) {
  Stop();
  meshes_.clear();
  textures_.clear();
  requested_.clear();
  results_.clear();
  queue_.clear();
  in_flight_ = 0;
  error_.clear();
  stop_ = false;
  cache_ = AssetCacheDir(game);
  std::error_code ec;
  if (!fs::exists(exe, ec)) {
    state_ = State::Failed;
    error_ = "SaintsRowAssetViewer.exe was not found next to the editor (" + exe.u8string() + ")";
    return;
  }
  state_ = State::Starting;
  worker_ = std::thread([this, exe, game] { Run(exe, game); });
}

void AssetLink::Stop() {
  {
    std::lock_guard<std::mutex> l(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  for (int i = 0; i < 100 && worker_.joinable() && !process_ && state_ == State::Starting; ++i) Sleep(20);
  if (in_w_) Send("quit");
  // the viewer ending closes its end of the pipe, which ends a read the worker is waiting in
  if (process_ && WaitForSingleObject(process_, 1500) == WAIT_TIMEOUT) TerminateProcess(process_, 0);
  if (worker_.joinable()) worker_.join();
  if (in_w_) CloseHandle(in_w_);
  if (out_r_) CloseHandle(out_r_);
  if (process_) CloseHandle(process_);
  in_w_ = out_r_ = process_ = nullptr;
  buffer_.clear();
  if (state_ != State::Failed) state_ = State::Off;
}

void AssetLink::Fail(const std::string& msg) {
  std::lock_guard<std::mutex> l(mu_);
  if (!stop_) {
    error_ = msg;
    state_ = State::Failed;
  }
  // everything still waiting fails
  for (auto& r : queue_) results_.push_back({r.model, r.name, r.size, {}, msg});
  queue_.clear();
  in_flight_ = 0;
}

bool AssetLink::Send(const std::string& line) {
  std::string s = line + "\n";
  DWORD w = 0;
  return in_w_ && WriteFile(in_w_, s.data(), DWORD(s.size()), &w, nullptr) && w == s.size();
}

bool AssetLink::ReadLine(std::string& line) {
  for (;;) {
    size_t nl = buffer_.find('\n');
    if (nl != std::string::npos) {
      line = buffer_.substr(0, nl);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      buffer_.erase(0, nl + 1);
      return true;
    }
    char buf[65536];
    DWORD n = 0;
    if (!out_r_ || !ReadFile(out_r_, buf, sizeof buf, &n, nullptr) || n == 0) return false;
    buffer_.append(buf, n);
  }
}

void AssetLink::Run(fs::path exe, fs::path game) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE in_r = nullptr, out_w = nullptr;
  if (!CreatePipe(&in_r, &in_w_, &sa, 0) || !CreatePipe(&out_r_, &out_w, &sa, 0)) return Fail("could not create pipes");
  SetHandleInformation(in_w_, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_r_, HANDLE_FLAG_INHERIT, 0);
  // the viewer finds the packfiles through SR1_PACKFILES (inherited environment)
  SetEnvironmentVariableW(L"SR1_PACKFILES", game.wstring().c_str());
  STARTUPINFOW si{sizeof si};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_r;
  si.hStdOutput = out_w;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + exe.wstring() + L"\" --serve";
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, exe.parent_path().wstring().c_str(), &si, &pi);
  CloseHandle(in_r);
  CloseHandle(out_w);
  if (!ok) return Fail("could not start " + exe.filename().u8string());
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;

  std::string line;
  if (!ReadLine(line)) return Fail("the asset viewer stopped while starting");
  if (line.rfind("ready", 0) != 0) {
    auto f = Split(line);
    return Fail("the asset viewer could not read the game files: " + (f.size() > 1 ? f[1] : line));
  }
  std::vector<GameMeshInfo> meshes;
  std::vector<GameTextureInfo> textures;
  Send("meshes");
  while (ReadLine(line) && line.rfind("ok", 0) != 0 && line.rfind("err", 0) != 0) {
    auto f = Split(line);
    if (f.size() >= 4 && f[0] == "mesh") meshes.push_back({f[1], f[2], f[3]});
  }
  Send("textures");
  while (ReadLine(line) && line.rfind("ok", 0) != 0 && line.rfind("err", 0) != 0) {
    auto f = Split(line);
    if (f.size() >= 5 && f[0] == "tex") textures.push_back({f[1], std::atoi(f[2].c_str()), std::atoi(f[3].c_str()), f[4]});
  }
  if (meshes.empty() && textures.empty()) return Fail("the asset viewer found no props or textures in the game files");
  std::sort(textures.begin(), textures.end(), [](const GameTextureInfo& a, const GameTextureInfo& b) {
    return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  meshes_ = std::move(meshes);
  textures_ = std::move(textures);
  state_ = State::Ready;

  for (;;) {
    Request r;
    {
      std::unique_lock<std::mutex> l(mu_);
      cv_.wait(l, [&] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      r = queue_.front();
      queue_.pop_front();
    }
    Result res{r.model, r.name, r.size, {}, {}};
    std::error_code ec;
    if (r.model) {
      fs::path dir = cache_ / "models" / Stem(r.name);
      res.path = FindGlb(dir);
      if (res.path.empty()) {
        fs::create_directories(dir, ec);
        if (!Send("mesh\t" + dir.u8string() + "\t" + r.name) || !ReadLine(line)) return Fail("the asset viewer stopped");
        auto f = Split(line);
        if (f[0] == "ok" && f.size() > 1) res.path = fs::u8path(f[1]);
        else res.error = f.size() > 1 ? f[1] : line;
      }
    } else {
      fs::path file = cache_ / ("textures" + std::to_string(r.size)) / (SafeName(r.name) + ".png");
      if (fs::exists(file, ec)) {
        res.path = file;
      } else {
        fs::create_directories(file.parent_path(), ec);
        if (!Send("texture\t" + file.u8string() + "\t" + std::to_string(r.size) + "\t" + r.name) || !ReadLine(line))
          return Fail("the asset viewer stopped");
        auto f = Split(line);
        if (f[0] == "ok") res.path = file;
        else res.error = f.size() > 1 ? f[1] : line;
      }
    }
    std::lock_guard<std::mutex> l(mu_);
    results_.push_back(std::move(res));
    if (in_flight_) --in_flight_;
    cv_.notify_all();
  }
}

void AssetLink::RequestModel(const std::string& name) {
  std::lock_guard<std::mutex> l(mu_);
  if (!requested_.insert({name, -1}).second) return;
  if (state_ == State::Failed) {
    results_.push_back({true, name, -1, {}, error_});
    return;
  }
  queue_.push_front({true, name, -1});  // models are placed right away: first in line
  ++in_flight_;
  cv_.notify_all();
}

void AssetLink::RequestTexture(const std::string& name, int size, bool urgent) {
  std::lock_guard<std::mutex> l(mu_);
  if (!requested_.insert({name, size}).second) return;
  if (state_ == State::Failed) {
    results_.push_back({false, name, size, {}, error_});
    return;
  }
  if (urgent) queue_.push_front({false, name, size});
  else queue_.push_back({false, name, size});
  ++in_flight_;
  cv_.notify_all();
}

bool AssetLink::Requested(const std::string& name, int size) const {
  std::lock_guard<std::mutex> l(mu_);
  return requested_.count({name, size}) != 0;
}

std::vector<AssetLink::Result> AssetLink::Poll() {
  std::lock_guard<std::mutex> l(mu_);
  std::vector<Result> r;
  r.swap(results_);
  return r;
}

size_t AssetLink::Busy() const {
  std::lock_guard<std::mutex> l(mu_);
  return in_flight_;
}

bool AssetLink::WaitIdle(int timeout_ms) {
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    State s = state_;
    if (s == State::Failed || s == State::Off) return false;
    if (s == State::Ready && Busy() == 0) return true;
    if (std::chrono::steady_clock::now() > until) return false;
    Sleep(20);
  }
}

}  // namespace editor
