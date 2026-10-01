// Saints Reborn Map Converter: turns a 3D scene (.glb / .gltf) into a multiplayer map pack for the
// game's maps folder. Drag a file onto the exe, or run:
//   SaintsRebornMapConverter <map.glb> [--name "Display Name"] [--id mpx_name] [--out <maps folder>]
//                            [--game <game folder>] [--texture-size 256] [--no-pause]
#include <cstdio>
#include <filesystem>
#include <iostream>

#include "build.h"
#include "gltf_import.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace mapconv;

namespace {

fs::path ExeFolder() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return fs::path(std::wstring(buf, n)).parent_path();
#else
  return fs::canonical("/proc/self/exe").parent_path();
#endif
}

// The game folder is the one with packfiles\mp_city_stream.vpp_xbox2; look next to the exe and upwards
// (dist\tools\MapConverter -> dist\game).
fs::path FindGame(const fs::path& start) {
  for (fs::path p = start; !p.empty(); p = p.parent_path()) {
    for (const fs::path& c : {p / "game", p, p / "dist" / "game"})
      if (fs::exists(c / "packfiles" / "mp_city_stream.vpp_xbox2")) return c;
    if (p == p.parent_path()) break;
  }
  return {};
}

std::string MakeId(const std::string& name) {
  std::string s = "mpx_";
  for (unsigned char c : name) {
    if (std::isalnum(c)) s += char(std::tolower(c));
    else if ((c == ' ' || c == '_' || c == '-') && s.back() != '_') s += '_';
    if (s.size() >= 24) break;
  }
  while (s.back() == '_') s.pop_back();
  if (s == "mpx") s = "mpx_map";
  return s;
}

std::string MakeFolder(const std::string& name) {
  std::string s;
  bool up = true;
  for (unsigned char c : name) {
    if (std::isalnum(c)) {
      s += up ? char(std::toupper(c)) : char(c);
      up = false;
    } else {
      up = true;
    }
  }
  return s.empty() ? "CustomMap" : s;
}

bool StartedFromExplorer() {
#ifdef _WIN32
  DWORD ids[2];
  return GetConsoleProcessList(ids, 2) <= 1;
#else
  return false;
#endif
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  int wargc;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  std::vector<std::string> args;
  for (int i = 1; i < wargc; ++i) args.push_back(fs::path(wargv[i]).u8string());
#else
  std::vector<std::string> args(argv + 1, argv + argc);
#endif
  bool pause = StartedFromExplorer();
  fs::path input, out_dir, game_dir;
  MapSettings st;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= args.size()) throw Error(a + " needs a value");
      return args[++i];
    };
    try {
      if (a == "--name") st.display_name = next();
      else if (a == "--id") st.map_id = next();
      else if (a == "--out") out_dir = fs::u8path(next());
      else if (a == "--game") game_dir = fs::u8path(next());
      else if (a == "--folder") st.folder = next();
      else if (a == "--texture-size") st.texture_size = std::stoi(next());
      else if (a == "--no-pause") pause = false;
      else if (a == "--pause") pause = true;
      else if (!a.empty() && a[0] == '-') throw Error("unknown option " + a);
      else input = fs::u8path(a);
    } catch (const std::exception& e) {
      std::printf("Error: %s\n", e.what());
      return 2;
    }
  }
  std::printf("Saints Reborn Map Converter\n\n");
  int code = 0;
  try {
    if (input.empty())
      throw Error("drag a .glb or .gltf file onto SaintsRebornMapConverter.exe (or pass it on the command line)");
    if (!fs::exists(input)) throw Error("file not found: " + input.u8string());
    std::string ext = input.extension().u8string();
    for (auto& c : ext) c = char(std::tolower((unsigned char)c));
    if (ext != ".glb" && ext != ".gltf")
      throw Error("only .glb and .gltf files are supported (export glTF from Blender, Unity or your editor)");
    if (st.display_name.empty()) st.display_name = input.stem().u8string();
    if (st.map_id.empty()) st.map_id = MakeId(st.display_name);
    if (st.folder.empty()) st.folder = MakeFolder(st.display_name);
    int ts = st.texture_size;
    if (ts != 256 && ts != 512 && ts != 1024) throw Error("--texture-size must be 256, 512 or 1024");
    if (game_dir.empty()) game_dir = FindGame(ExeFolder());
    if (game_dir.empty()) game_dir = FindGame(fs::current_path());
    if (game_dir.empty() || !fs::exists(game_dir / "packfiles" / "mp_city_stream.vpp_xbox2"))
      throw Error("could not find the game files (game\\packfiles). Run the converter from the game's tools folder or pass --game <folder>");
    if (out_dir.empty()) out_dir = game_dir.parent_path() / "maps";

    std::printf("Map:     %s\nInput:   %s\n", st.display_name.c_str(), input.u8string().c_str());
    MapInput in = ImportGltf(input);
    std::printf("Read %zu triangles, %zu materials, %zu markers\n", in.tris.size(), in.materials.size(), in.markers.size());
    auto log = [](const std::string& s) { std::printf("%s\n", s.c_str()); };
    BuildReport rep = BuildPack(in, st, game_dir / "packfiles", out_dir, log);
    std::printf("\nDone: %zu triangles, %zu vertices, %zu meshes, %zu textures\n", rep.triangles, rep.vertices, rep.meshes, rep.textures);
    std::printf("      %zu spawns, %zu weapons, %zu vehicles, %zu chain drop-offs\n", rep.spawns, rep.weapons, rep.vehicles, rep.dropoffs);
    std::printf("Modes: ");
    for (size_t i = 0; i < rep.modes.size(); ++i) std::printf("%s%s", i ? ", " : "", rep.modes[i].c_str());
    std::printf("\nMap pack written to %s\n", rep.folder.u8string().c_str());
    if (!rep.warnings.empty()) {
      std::printf("\nNotes:\n");
      for (auto& w : rep.warnings) std::printf("  - %s\n", w.c_str());
    }
    std::printf("\nStart the game: the map is in the System Link lobby's Level list as \"%s\".\n", st.display_name.c_str());
  } catch (const std::exception& e) {
    std::printf("\nError: %s\n", e.what());
    code = 1;
  }
  if (pause) {
    std::printf("\nPress Enter to close.");
    std::getchar();
  }
  return code;
}
