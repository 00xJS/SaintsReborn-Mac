#include "vpp.h"

#include <algorithm>

extern "C" {
#include "puff.h"
}

namespace mapconv {
namespace {
std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}
}  // namespace

Packfile::Packfile(const std::filesystem::path& path) : path_(path), in_(path, std::ios::binary) {
  if (!in_) throw Error("cannot open " + path.string());
  Bytes h(0x800);
  in_.read(reinterpret_cast<char*>(h.data()), h.size());
  if (!in_ || U32(h, 0) != 0x51890ACE || U32(h, 4) != 3) throw Error(path.string() + " is not a Saints Row packfile");
  uint32_t flags = U32(h, 0x14C), count = U32(h, 0x154), dir_size = U32(h, 0x15C), names_size = U32(h, 0x160);
  if (flags & 2) throw Error(path.string() + ": condensed packfiles are not supported here");
  compressed_ = (flags & 1) != 0;
  Bytes dir(dir_size);
  in_.read(reinterpret_cast<char*>(dir.data()), dir.size());
  uint64_t names_at = Al(0x800 + uint64_t(dir_size), 2048);
  Bytes names(names_size);
  in_.seekg(std::streamoff(names_at));
  in_.read(reinterpret_cast<char*>(names.data()), names.size());
  if (!in_) throw Error(path.string() + ": truncated directory");
  uint64_t data_at = Al(names_at + names_size, 2048), cur = data_at;
  for (uint32_t i = 0; i < count; ++i) {
    Entry e;
    uint32_t no = U32(dir, 28 * i), off = U32(dir, 28 * i + 8);
    e.size = U32(dir, 28 * i + 16);
    e.stored = U32(dir, 28 * i + 20);
    size_t end = no;
    while (end < names.size() && names[end]) ++end;
    e.name.assign(reinterpret_cast<const char*>(&names[no]), end - no);
    if (compressed_) {
      e.at = cur;
      cur = Al(cur + e.stored, 2048);
    } else {
      e.at = data_at + off;
    }
    entries_.push_back(std::move(e));
  }
}

const Packfile::Entry* Packfile::Find(const std::string& name) const {
  std::string l = Lower(name);
  for (const auto& e : entries_)
    if (Lower(e.name) == l) return &e;
  return nullptr;
}

bool Packfile::Has(const std::string& name) const { return Find(name) != nullptr; }

Bytes Packfile::Read(const std::string& name) {
  const Entry* e = Find(name);
  if (!e) throw Error(name + " is not in " + path_.filename().string());
  Bytes raw(compressed_ ? e->stored : e->size);
  in_.clear();
  in_.seekg(std::streamoff(e->at));
  in_.read(reinterpret_cast<char*>(raw.data()), raw.size());
  if (!in_) throw Error("cannot read " + name);
  if (!compressed_) return raw;
  // zlib stream: 2-byte header, raw deflate. Some stock files end without the final block marker,
  // so the output counts once it has the expected size.
  if (raw.size() < 2) throw Error("cannot decompress " + name);
  Bytes result(e->size);
  unsigned long dlen = e->size, slen = (unsigned long)(raw.size() - 2);
  int r = puff(result.data(), &dlen, raw.data() + 2, &slen);
  if (dlen != e->size || (r != 0 && r != 2 && r != 1)) throw Error("cannot decompress " + name);
  return result;
}

}  // namespace mapconv
