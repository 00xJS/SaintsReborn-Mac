// Whompay's Mod Loader - reading and rebuilding Saints Row packfiles
// (.vpp_xbox2, "VPP" version 3), so mods can change files inside them.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace wml {

class Packfile {
 public:
  bool Load(const std::filesystem::path& path, std::string* error);

  // File names in the packfile, in stored order.
  std::vector<std::string> Names() const;
  bool Contains(const std::string& name) const;
  // Decompressed contents of a file. Names are case-insensitive.
  bool Read(const std::string& name, std::string& out) const;

  // Writes a copy of the packfile with some files replaced (keys are file
  // names as returned by Names()).
  bool Save(const std::filesystem::path& path, const std::map<std::string, std::string>& replacements,
            std::string* error) const;

 private:
  struct Entry {
    std::string name;
    uint32_t fields[7];      // directory entry as stored
    uint64_t stored_offset;  // offset of the stored data in the file
  };
  int Find(const std::string& name) const;
  bool SaveCondensed(const std::filesystem::path& path, const std::map<std::string, std::string>& replacements,
                     std::string* error) const;

  std::vector<uint8_t> data_;
  std::vector<Entry> entries_;
  uint32_t names_offset_ = 0;
  uint32_t names_size_ = 0;
  uint32_t data_offset_ = 0;
  bool compressed_ = false;
  // Condensed packfiles (flag bit 1, e.g. preload.vpp_xbox2) store all files
  // back to back (64-byte aligned) in one zlib stream; blob_ is that data.
  bool condensed_ = false;
  std::vector<uint8_t> blob_;
};

}  // namespace wml
