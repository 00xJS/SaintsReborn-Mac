// Game assets for the editor, read by the Saints Row Asset Viewer (SaintsRowAssetViewer.exe --serve):
// the lists of props, vehicles and textures in the player's own game files, and models (.glb) and
// textures (.png) on request. Nothing from the game ships with the editor; what the viewer writes is
// cached per user in %LOCALAPPDATA%\SaintsReborn\MapEditor\cache.
#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace editor {

struct GameMeshInfo {
  std::string kind;  // "prop" or "vehicle"
  std::string archive, name;
};

struct GameTextureInfo {
  std::string name;
  int w = 0, h = 0;
  std::string peg;
};

class AssetLink {
 public:
  enum class State { Off, Starting, Ready, Failed };
  struct Result {
    bool model = false;
    std::string name;
    int size = 0;                 // textures: requested size
    std::filesystem::path path;   // empty on failure
    std::string error;
  };

  ~AssetLink() { Stop(); }
  void Start(const std::filesystem::path& viewer_exe, const std::filesystem::path& game_dir);
  void Stop();
  State state() const { return state_; }
  std::string error() const;
  // Valid once state() == Ready (not changed afterwards until the next Start).
  const std::vector<GameMeshInfo>& Meshes() const { return meshes_; }
  const std::vector<GameTextureInfo>& Textures() const { return textures_; }
  // Requests are answered once each (until Start again); urgent ones go first.
  void RequestModel(const std::string& name);
  void RequestTexture(const std::string& name, int max_size, bool urgent);
  bool Requested(const std::string& name, int size) const;
  std::vector<Result> Poll();
  size_t Busy() const;
  // Blocks until the given requests are answered (or the link fails / timeout).
  bool WaitIdle(int timeout_ms);

 private:
  struct Request {
    bool model;
    std::string name;
    int size;
  };
  void Run(std::filesystem::path exe, std::filesystem::path game);
  bool Send(const std::string& line);
  bool ReadLine(std::string& line);
  void Fail(const std::string& msg);

  std::atomic<State> state_{State::Off};
  std::thread worker_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Request> queue_;
  std::set<std::pair<std::string, int>> requested_;
  std::vector<Result> results_;
  size_t in_flight_ = 0;
  bool stop_ = false;
  std::string error_;
  std::vector<GameMeshInfo> meshes_;
  std::vector<GameTextureInfo> textures_;
  std::filesystem::path cache_;
  HANDLE process_ = nullptr, in_w_ = nullptr, out_r_ = nullptr;
  std::string buffer_;
};

// Where the editor keeps what the viewer extracts (per game folder).
std::filesystem::path AssetCacheDir(const std::filesystem::path& game_dir);
// Resolves a folder the user picked (install folder, dist, game or packfiles) to the game folder that
// holds packfiles\; empty when it is not a Saints Reborn / Saints Row folder.
std::filesystem::path ResolveGameDir(const std::filesystem::path& picked);

}  // namespace editor
