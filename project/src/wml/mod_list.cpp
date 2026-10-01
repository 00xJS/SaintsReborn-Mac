// Whompay's Mod Loader - mod discovery and the enabled/load-order list.

#include "mod_list.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <system_error>

namespace wml {
namespace {

namespace fs = std::filesystem;

std::string Trim(const std::string& s) {
  size_t begin = 0;
  size_t end = s.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
  return s.substr(begin, end - begin);
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Reads "key = value" lines of an ini file into "section.key" entries (in file
// order; keys before any section have no prefix). "\n" in a value becomes a
// line break.
std::vector<std::pair<std::string, std::string>> ReadIni(const fs::path& path) {
  std::vector<std::pair<std::string, std::string>> values;
  std::ifstream in(path);
  std::string line;
  std::string section;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string t = Trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#') continue;
    if (t[0] == '[') {
      size_t end = t.find(']');
      section = Lower(Trim(t.substr(1, end == std::string::npos ? std::string::npos : end - 1)));
      continue;
    }
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string key = Trim(t.substr(0, eq));
    std::string value = Trim(t.substr(eq + 1));
    // Strip a trailing "; comment".
    size_t comment = value.find(" ;");
    if (comment != std::string::npos) value = Trim(value.substr(0, comment));
    std::string unescaped;
    for (size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == 'n') {
        unescaped += '\n';
        ++i;
      } else {
        unescaped += value[i];
      }
    }
    values.emplace_back(section.empty() ? Lower(key) : section + "." + key, unescaped);
  }
  return values;
}

std::string Get(const std::vector<std::pair<std::string, std::string>>& ini,
                const std::string& key, const std::string& fallback) {
  for (const auto& kv : ini) {
    if (Lower(kv.first) == key) return kv.second;
  }
  return fallback;
}

bool ReadMod(const fs::path& folder, ModInfo& mod) {
  std::error_code ec;
  mod.id = PathToUtf8(folder.filename());
  mod.folder = folder;
  auto ini = ReadIni(folder / "mod.ini");
  mod.name = Get(ini, "mod.name", Get(ini, "name", mod.id));
  mod.author = Get(ini, "mod.author", Get(ini, "author", ""));
  mod.version = Get(ini, "mod.version", Get(ini, "version", ""));
  mod.description = Get(ini, "mod.description", Get(ini, "description", ""));
  // "enabled_by_default = true": on when it isn't in modlist.ini yet (a mod
  // that came with an update); the player's own +/- choice always wins.
  {
    const std::string on = Lower(Get(ini, "mod.enabled_by_default", Get(ini, "enabled_by_default", "")));
    mod.enabled = on == "true" || on == "1" || on == "yes";
  }
  for (const auto& kv : ini) {
    if (kv.first.rfind("settings.", 0) == 0) mod.settings.emplace_back(kv.first.substr(9), kv.second);
  }

  if (fs::is_directory(folder / "files", ec)) {
    mod.files_folder = folder / "files";
  }
  std::string script = Get(ini, "mod.script", Get(ini, "script", "main.lua"));
  if (fs::is_regular_file(folder / script, ec)) {
    mod.script = folder / script;
  }
  if (fs::is_regular_file(folder / "patch.lua", ec)) {
    mod.patch_script = folder / "patch.lua";
  }
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    if (entry.is_regular_file(ec) && Lower(PathToUtf8(entry.path().extension())) == ".dll") {
      mod.dlls.push_back(entry.path());
    }
  }
  std::sort(mod.dlls.begin(), mod.dlls.end());
  return true;
}

}  // namespace

std::vector<ModInfo> LoadMods(const fs::path& mods_dir) {
  std::error_code ec;
  std::vector<ModInfo> found;
  for (const auto& entry : fs::directory_iterator(mods_dir, ec)) {
    if (!entry.is_directory(ec)) continue;
    // Folders starting with "." (such as the loader's .cache) are not mods.
    if (PathToUtf8(entry.path().filename()).rfind(".", 0) == 0) continue;
    ModInfo mod;
    if (ReadMod(entry.path(), mod)) found.push_back(std::move(mod));
  }

  // modlist.ini: one mod folder per line, "+ Name" (enabled) or "- Name".
  std::vector<ModInfo> ordered;
  std::ifstream in(mods_dir / "modlist.ini");
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string t = Trim(line);
    if (t.size() < 2 || (t[0] != '+' && t[0] != '-')) continue;
    std::string id = Trim(t.substr(1));
    auto it = std::find_if(found.begin(), found.end(),
                           [&](const ModInfo& m) { return Lower(m.id) == Lower(id); });
    if (it == found.end()) continue;
    it->enabled = t[0] == '+';
    ordered.push_back(std::move(*it));
    found.erase(it);
  }
  std::sort(found.begin(), found.end(),
            [](const ModInfo& a, const ModInfo& b) { return Lower(a.name) < Lower(b.name); });
  for (auto& mod : found) ordered.push_back(std::move(mod));
  return ordered;
}

bool SaveModList(const fs::path& mods_dir, const std::vector<ModInfo>& mods) {
  std::error_code ec;
  fs::create_directories(mods_dir, ec);
  std::ofstream out(mods_dir / "modlist.ini", std::ios::trunc);
  if (!out) return false;
  out << "; Whompay's Mod Loader - load order and on/off state.\n"
         "; + enabled, - disabled. Mods further down load later and win when two\n"
         "; mods replace the same file.\n";
  for (const auto& mod : mods) {
    out << (mod.enabled ? "+ " : "- ") << mod.id << "\n";
  }
  return static_cast<bool>(out);
}

}  // namespace wml
