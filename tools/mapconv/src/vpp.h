// Read-only access to Saints Row packfiles (.vpp_xbox2, version 3). Reads the directory once and
// seeks to single files, so the big stream packs are never loaded whole.
#pragma once
#include <filesystem>
#include <fstream>
#include "be.h"

namespace mapconv {

class Packfile {
 public:
  explicit Packfile(const std::filesystem::path& path);
  bool Has(const std::string& name) const;
  Bytes Read(const std::string& name);

 private:
  struct Entry {
    std::string name;
    uint64_t at;
    uint32_t size, stored;
  };
  const Entry* Find(const std::string& name) const;
  std::filesystem::path path_;
  std::ifstream in_;
  bool compressed_ = false;
  std::vector<Entry> entries_;
};

}  // namespace mapconv
