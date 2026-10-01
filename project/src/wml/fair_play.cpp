// Whompay's Mod Loader - which enabled mods may be used in public online play.
//
// Public matches and System Link are for unmodded games, but mods that only
// change how the game looks are fine there. A mod counts as "looks only" when
// everything it changes is on the list below; that is decided from what the
// mod actually changes, never from what it says about itself:
//
//  - files/ folder: replaced packfiles are compared with the player's own
//    copies file by file; loose files must be textures or fonts.
//  - patch.lua: every file it replaced inside a packfile (patch_phase.cpp).
//  - main.lua / DLLs: code can do anything, so only approved versions
//    (kApprovedCode: SHA-256 of the mod's .lua and .dll files) count.
//
// Looks-only files: textures (.peg_xbox2), fonts (.vf3_*), and the
// time-of-day lighting (time_of_day.xtbl) as long as it has at least the
// game's fog and no brighter nights than the game's.
//
// Packfiles are large, so the check runs on a background thread and its
// results are cached in mods/.cache/keys/_fairplay.txt.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

#include "mod_loader.h"
#include "packfile.h"
#include "wml_internal.h"

namespace wml {
namespace {

namespace fs = std::filesystem;

constexpr const char* kCheckVersion = "fairplay-1";

// Code mods approved for public play: mod folder name and the SHA-256 of
// its code (CodeHash). A changed mod needs a new review.
struct Approved {
  const char* id;
  const char* sha256;
};
const Approved kApprovedCode[] = {
    // Modern Look 1.x: picture grading / sharpening / bloom at swap time only.
    {"ModernLook", "af1b6bc4493b21f9e5885fa99e9160ed36eb1c7215e18a52d9709032449cb9e5"},
};

std::mutex g_mutex;
bool g_done = false;
std::vector<std::pair<std::string, std::string>> g_results;  // mod id -> reason ("" = fine)

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
uint64_t Fnv(uint64_t h, const std::string& s) {
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return (h ^ 0xFF) * 1099511628211ull;
}
std::string Rel(const fs::path& p, const fs::path& base) {
  std::error_code ec;
  std::string r = PathToUtf8(fs::relative(p, base, ec));
  for (char& c : r)
    if (c == '\\') c = '/';
  return r;
}
bool EndsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Files that only change how the game looks (lower-case name).
bool LooksOnly(const std::string& lower_name) {
  return EndsWith(lower_name, ".peg_xbox2") || EndsWith(lower_name, ".vf3_xbox2") ||
         EndsWith(lower_name, ".vf3_pc");
}
std::string BaseName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// --- time_of_day.xtbl -----------------------------------------------------

double Number(const std::string& s, const char* tag, size_t from = 0, size_t to = std::string::npos) {
  const std::string open = std::string("<") + tag + ">";
  const size_t at = s.find(open, from);
  if (at == std::string::npos || at >= to) return -1.0;
  return std::atof(s.c_str() + at + open.size());
}
// Sum of every <R>/<G>/<B> inside each <tag>...</tag> block of the segment.
double ColourSum(const std::string& s, const char* tag) {
  const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
  const size_t a = s.find(open);
  if (a == std::string::npos) return 0.0;
  const size_t b = s.find(close, a);
  if (b == std::string::npos) return 0.0;
  double sum = 0.0;
  for (const char* c : {"<R>", "<G>", "<B>"}) {
    for (size_t p = s.find(c, a); p != std::string::npos && p < b; p = s.find(c, p + 3)) sum += std::atof(s.c_str() + p + 3);
  }
  return sum;
}
std::vector<std::string> Segments(const std::string& xml) {
  std::vector<std::string> out;
  for (size_t p = xml.find("<Time_Segment>"); p != std::string::npos;) {
    const size_t e = xml.find("</Time_Segment>", p);
    if (e == std::string::npos) break;
    out.push_back(xml.substr(p, e - p));
    p = xml.find("<Time_Segment>", e);
  }
  return out;
}
// "" when the modded lighting keeps at least the game's fog and no brighter
// nights (light from the sky, sun/moon, fill and ambient summed per night
// segment); otherwise why not.
std::string LightingFair(const std::string& original, const std::string& modded) {
  const auto o = Segments(original), m = Segments(modded);
  if (o.empty() || o.size() != m.size()) return "changes the time-of-day table's layout";
  for (size_t i = 0; i < o.size(); ++i) {
    if (Number(m[i], "Fog_Strength") + 1e-3 < Number(o[i], "Fog_Strength") ||
        Number(m[i], "Fog_End_Distance") > Number(o[i], "Fog_End_Distance") + 1e-3 ||
        Number(m[i], "Fog_Start_Distance") > Number(o[i], "Fog_Start_Distance") + 1e-3)
      return "less fog than the game";
    // <Time> (not <Time_Segment>): hhmm.
    const double time = Number(o[i], "Time");
    if (time >= 2100 || (time >= 0 && time < 500)) {
      double so = 0, sm = 0;
      for (const char* tag : {"Level_Ambient_Light_Color", "Tod_Light_Color", "Fill_Light_Color",
                              "Tree_Ambient_Light_Color", "Char_Ambient"}) {
        so += ColourSum(o[i], tag);
        sm += ColourSum(m[i], tag);
      }
      if (sm > so * 1.02 + 1.0) return "brighter nights than the game";
    }
  }
  return {};
}

// Why a changed file inside a packfile isn't looks-only ("" = it is).
std::string ChangedFile(const std::string& pack, const std::string& name, const std::string* original,
                        const std::string* modded) {
  const std::string lower = Lower(name);
  if (LooksOnly(lower)) return {};
  if (lower == "time_of_day.xtbl" && original && modded) {
    const std::string why = LightingFair(*original, *modded);
    return why.empty() ? std::string() : why + " (time_of_day.xtbl)";
  }
  return "changes " + name + " in " + BaseName(pack);
}

// A replacement packfile against the player's own copy.
std::string ComparePack(const fs::path& mod_pack, const fs::path& game_pack, const std::string& rel) {
  Packfile a, b;
  std::string error;
  if (!b.Load(game_pack, &error)) return "replaces " + rel + " (no such packfile in the game)";
  if (!a.Load(mod_pack, &error)) return "replaces " + rel + " (unreadable: " + error + ")";
  std::map<std::string, size_t> by_name;
  for (size_t i = 0; i < b.Count(); ++i) by_name[Lower(b.NameAt(i))] = i;
  for (size_t i = 0; i < a.Count(); ++i) {
    const std::string lower = Lower(a.NameAt(i));
    if (LooksOnly(lower)) continue;
    auto it = by_name.find(lower);
    if (it == by_name.end()) return "adds " + a.NameAt(i) + " to " + BaseName(rel);
    const uint8_t *pa = nullptr, *pb = nullptr;
    size_t na = 0, nb = 0;
    if (a.StoredAt(i, pa, na) && b.StoredAt(it->second, pb, nb) && na == nb && std::memcmp(pa, pb, na) == 0) continue;
    std::string da, db;
    if (!a.ReadAt(i, da) || !b.ReadAt(it->second, db)) return "changes " + a.NameAt(i) + " in " + BaseName(rel);
    if (da == db) continue;
    const std::string why = ChangedFile(rel, a.NameAt(i), &db, &da);
    if (!why.empty()) return why;
  }
  return {};
}

#ifdef _WIN32
std::string Sha256(const std::string& data) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  unsigned char out[32] = {};
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
  if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
    BCryptHashData(h, (PUCHAR)data.data(), ULONG(data.size()), 0);
    BCryptFinishHash(h, out, sizeof(out), 0);
    BCryptDestroyHash(h);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  std::string hex;
  char buf[3];
  for (unsigned char c : out) {
    std::snprintf(buf, sizeof(buf), "%02x", c);
    hex += buf;
  }
  return hex;
}
#else
std::string Sha256(const std::string&) { return {}; }
#endif

// SHA-256 over the mod's .lua and .dll files, sorted by lower-case relative
// path: for each "path\0size\0contents" (scripts without CR).
std::string CodeHash(const ModInfo& mod) {
  std::error_code ec;
  std::vector<std::pair<std::string, fs::path>> files;
  for (auto it = fs::recursive_directory_iterator(mod.folder, ec); it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string ext = Lower(PathToUtf8(it->path().extension()));
    if (ext == ".lua" || ext == ".dll") files.emplace_back(Lower(Rel(it->path(), mod.folder)), it->path());
  }
  std::sort(files.begin(), files.end());
  std::string all;
  for (const auto& [rel, path] : files) {
    std::ifstream in(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Scripts: line endings don't count (Git may check them out either way).
    if (EndsWith(rel, ".lua")) data.erase(std::remove(data.begin(), data.end(), '\r'), data.end());
    all += rel;
    all += '\0';
    all += std::to_string(data.size());
    all += '\0';
    all += data;
  }
  return Sha256(all);
}

std::string CheckMod(const ModInfo& mod, const fs::path& game_dir, const fs::path& cache_dir) {
  if (mod.has_code() || mod.has_script()) {
    const std::string hash = CodeHash(mod);
    bool approved = false;
    for (const auto& a : kApprovedCode) approved |= mod.id == a.id && hash == a.sha256;
    if (!approved) {
      Log("WML", "Fair play: " + mod.id + " code " + hash + " is not an approved version");
      return mod.has_code() ? "native code mod" : "script mod";
    }
  }
  std::error_code ec;
  if (mod.has_files()) {
    for (auto it = fs::recursive_directory_iterator(mod.files_folder, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const std::string rel = Rel(it->path(), mod.files_folder);
      const std::string lower = Lower(rel);
      if (LooksOnly(lower)) continue;
      if (EndsWith(lower, ".vpp_xbox2")) {
        const std::string why = ComparePack(it->path(), game_dir / fs::u8path(rel), rel);
        if (!why.empty()) return why;
        continue;
      }
      return "replaces " + rel;
    }
  }
  for (const auto& w : PatchWrites()) {
    if (w.mod_id != mod.id || LooksOnly(Lower(w.name))) continue;
    std::string original, modded;
    Packfile o, m;
    const bool have = o.Load(game_dir / fs::u8path(w.pack), nullptr) && o.Read(w.name, original) &&
                      m.Load(cache_dir / "files" / fs::u8path(w.pack), nullptr) && m.Read(w.name, modded);
    const std::string why = ChangedFile(w.pack, w.name, have ? &original : nullptr, have ? &modded : nullptr);
    if (!why.empty()) return why;
  }
  return {};
}

// Cache key of one mod's result: its files (size, time), its patch writes,
// the game's packfiles and the check itself.
std::string ModKey(const ModInfo& mod, const fs::path& game_dir, const fs::path& cache_dir) {
  std::error_code ec;
  uint64_t h = Fnv(14695981039346656037ull, kCheckVersion);
  for (const auto& a : kApprovedCode) h = Fnv(h, std::string(a.id) + a.sha256);
  auto add = [&](const fs::path& p, const std::string& name) {
    h = Fnv(h, Lower(name));
    h = Fnv(h, std::to_string(fs::file_size(p, ec)));
    h = Fnv(h, std::to_string(fs::last_write_time(p, ec).time_since_epoch().count()));
  };
  std::vector<std::pair<std::string, fs::path>> files;
  for (auto it = fs::recursive_directory_iterator(mod.folder, ec); it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec) || Lower(PathToUtf8(it->path().extension())) == ".log") continue;
    files.emplace_back(Rel(it->path(), mod.folder), it->path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) add(f.second, f.first);
  for (const auto& w : PatchWrites()) {
    if (w.mod_id != mod.id) continue;
    h = Fnv(h, w.pack + "|" + w.name);
    add(cache_dir / "files" / fs::u8path(w.pack), "out:" + w.pack);
  }
  std::vector<fs::path> packs;
  for (auto it = fs::directory_iterator(game_dir / "packfiles", ec); it != fs::directory_iterator(); it.increment(ec))
    if (it->is_regular_file(ec)) packs.push_back(it->path());
  std::sort(packs.begin(), packs.end());
  for (const auto& p : packs) add(p, "game:" + PathToUtf8(p.filename()));
  char text[32];
  std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)h);
  return text;
}

}  // namespace

void StartFairCheck(const std::vector<ModInfo>& mods, const fs::path& game_dir, const fs::path& cache_dir) {
  std::thread([mods, game_dir, cache_dir]() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    const fs::path cache_file = cache_dir / "keys" / "_fairplay.txt";
    std::map<std::string, std::pair<std::string, std::string>> cached;  // id -> key, reason
    {
      std::ifstream in(cache_file);
      for (std::string line; std::getline(in, line);) {
        const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b != std::string::npos) cached[line.substr(0, a)] = {line.substr(a + 1, b - a - 1), line.substr(b + 1)};
      }
    }
    std::vector<std::pair<std::string, std::string>> results;
    std::string lines;
    for (const auto& mod : mods) {
      const std::string key = ModKey(mod, game_dir, cache_dir);
      std::string reason;
      auto it = cached.find(mod.id);
      if (it != cached.end() && it->second.first == key) {
        reason = it->second.second;
      } else {
        const auto start = std::chrono::steady_clock::now();
        reason = CheckMod(mod, game_dir, cache_dir);
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start).count();
        Log("WML", "Fair play: checked " + mod.id + " in " + std::to_string(ms) + " ms");
      }
      Log("WML", "Fair play: " + mod.id + (reason.empty() ? " only changes how the game looks - allowed in public play"
                                                           : " - private play only (" + reason + ")"));
      for (char& c : reason)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
      lines += mod.id + "\t" + key + "\t" + reason + "\n";
      results.emplace_back(mod.id, reason);
    }
    std::error_code ec;
    fs::create_directories(cache_file.parent_path(), ec);
    std::ofstream(cache_file, std::ios::trunc) << lines;
    std::lock_guard<std::mutex> l(g_mutex);
    g_results = std::move(results);
    g_done = true;
  }).detach();
}

bool FairCheckResults(std::vector<std::pair<std::string, std::string>>& results) {
  std::lock_guard<std::mutex> l(g_mutex);
  if (!g_done) return false;
  results = g_results;
  return true;
}

}  // namespace wml
