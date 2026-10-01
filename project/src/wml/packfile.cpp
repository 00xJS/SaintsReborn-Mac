// Whompay's Mod Loader - reading and rebuilding Saints Row packfiles.
//
// Layout (all values big-endian, sections aligned to 2048 bytes):
//   0x000  header: magic 0x51890ACE, version 3, ..., at 0x14C flags (bit 0 =
//          compressed, bit 1 = condensed: all files in one zlib stream, each
//          at its 64-byte aligned uncompressed offset), 0x154 file count, 0x158 packfile size, 0x15C
//          directory size, 0x160 names size, 0x164 total aligned uncompressed
//          size, 0x168 end of the last compressed file
//   0x800  directory: 28 bytes per file: name offset, 0, uncompressed offset
//          (aligned), name hash, uncompressed size, compressed size, 0
//          names: null-terminated strings
//          data: each file's zlib stream (or raw data), each aligned

#include "packfile.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#include <zlib.h>

namespace wml {
namespace {

constexpr uint32_t kMagic = 0x51890ACE;
constexpr uint32_t kAlign = 2048;

uint32_t Align(uint64_t x) { return static_cast<uint32_t>((x + kAlign - 1) & ~uint64_t(kAlign - 1)); }

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

bool Packfile::Load(const std::filesystem::path& path, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open";
    return false;
  }
  data_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (data_.size() < 0x800 || Be32(&data_[0]) != kMagic || Be32(&data_[4]) != 3) {
    if (error) *error = "not a version 3 packfile";
    return false;
  }
  compressed_ = (Be32(&data_[0x14C]) & 1) != 0;
  condensed_ = (Be32(&data_[0x14C]) & 2) != 0;
  uint32_t count = Be32(&data_[0x154]);
  uint32_t directory_size = Be32(&data_[0x15C]);
  names_size_ = Be32(&data_[0x160]);
  names_offset_ = Align(0x800 + uint64_t(directory_size));
  data_offset_ = Align(uint64_t(names_offset_) + names_size_);
  if (uint64_t(count) * 28 > directory_size || data_offset_ > data_.size()) {
    if (error) *error = "corrupt directory";
    return false;
  }

  entries_.clear();
  entries_.reserve(count);
  uint64_t stored = data_offset_;
  for (uint32_t i = 0; i < count; ++i) {
    Entry e;
    for (int f = 0; f < 7; ++f) e.fields[f] = Be32(&data_[0x800 + i * 28 + f * 4]);
    uint64_t name_at = uint64_t(names_offset_) + e.fields[0];
    if (name_at >= data_.size()) {
      if (error) *error = "corrupt name table";
      return false;
    }
    const char* name = reinterpret_cast<const char*>(&data_[name_at]);
    e.name.assign(name, strnlen(name, data_.size() - name_at));
    if (compressed_) {
      e.stored_offset = stored;
      stored = Align(stored + e.fields[5]);
    } else {
      e.stored_offset = uint64_t(data_offset_) + e.fields[2];
    }
    entries_.push_back(std::move(e));
  }
  if (condensed_) {
    uint32_t blob_size = Be32(&data_[0x164]);
    uint32_t stored_size = compressed_ ? Be32(&data_[0x168]) : blob_size;
    if (uint64_t(data_offset_) + stored_size > data_.size()) {
      if (error) *error = "truncated data";
      return false;
    }
    if (!compressed_) {
      blob_.assign(data_.begin() + data_offset_, data_.begin() + data_offset_ + blob_size);
    } else {
      blob_.assign(blob_size, 0);
      z_stream z = {};
      if (inflateInit(&z) != Z_OK) return false;
      z.next_in = &data_[data_offset_];
      z.avail_in = stored_size;
      z.next_out = blob_.data();
      z.avail_out = blob_size;
      inflate(&z, Z_FINISH);
      uLong produced = z.total_out;
      inflateEnd(&z);
      if (produced < blob_size) {
        if (error) *error = "cannot decompress data";
        return false;
      }
    }
  }
  return true;
}

std::vector<std::string> Packfile::Names() const {
  std::vector<std::string> names;
  names.reserve(entries_.size());
  for (const auto& e : entries_) names.push_back(e.name);
  return names;
}

int Packfile::Find(const std::string& name) const {
  std::string lower = Lower(name);
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (Lower(entries_[i].name) == lower) return static_cast<int>(i);
  }
  return -1;
}

bool Packfile::Contains(const std::string& name) const { return Find(name) >= 0; }

bool Packfile::Read(const std::string& name, std::string& out) const {
  int index = Find(name);
  if (index < 0) return false;
  const Entry& e = entries_[index];
  uint32_t size = e.fields[4];
  if (condensed_) {
    if (uint64_t(e.fields[2]) + size > blob_.size()) return false;
    out.assign(reinterpret_cast<const char*>(&blob_[e.fields[2]]), size);
    return true;
  }
  if (!compressed_) {
    if (e.stored_offset + size > data_.size()) return false;
    out.assign(reinterpret_cast<const char*>(&data_[e.stored_offset]), size);
    return true;
  }
  if (e.stored_offset + e.fields[5] > data_.size()) return false;
  out.assign(size, '\0');
  z_stream z = {};
  if (inflateInit(&z) != Z_OK) return false;
  z.next_in = const_cast<Bytef*>(&data_[e.stored_offset]);
  z.avail_in = e.fields[5];
  z.next_out = reinterpret_cast<Bytef*>(out.data());
  z.avail_out = size;
  int status = inflate(&z, Z_FINISH);
  uLong produced = z.total_out;
  inflateEnd(&z);
  // Some streams are stored without their end marker; the size is what counts.
  return (status == Z_STREAM_END || status == Z_OK || status == Z_BUF_ERROR) && produced == size;
}

bool Packfile::Save(const std::filesystem::path& path,
                    const std::map<std::string, std::string>& replacements,
                    std::string* error) const {
  if (condensed_) return SaveCondensed(path, replacements, error);
  // Stored bytes of every file, recompressing the replaced ones.
  std::vector<std::vector<uint8_t>> blobs(entries_.size());
  std::vector<Entry> entries = entries_;
  for (size_t i = 0; i < entries.size(); ++i) {
    auto it = std::find_if(replacements.begin(), replacements.end(), [&](const auto& r) {
      return Lower(r.first) == Lower(entries[i].name);
    });
    if (it == replacements.end()) {
      uint32_t stored_size = compressed_ ? entries[i].fields[5] : entries[i].fields[4];
      const uint8_t* begin = &data_[entries[i].stored_offset];
      blobs[i].assign(begin, begin + stored_size);
      continue;
    }
    const std::string& raw = it->second;
    entries[i].fields[4] = static_cast<uint32_t>(raw.size());
    if (compressed_) {
      uLongf bound = compressBound(static_cast<uLong>(raw.size()));
      blobs[i].resize(bound);
      if (compress2(blobs[i].data(), &bound, reinterpret_cast<const Bytef*>(raw.data()),
                    static_cast<uLong>(raw.size()), 9) != Z_OK) {
        if (error) *error = "compression failed for " + entries[i].name;
        return false;
      }
      blobs[i].resize(bound);
      entries[i].fields[5] = static_cast<uint32_t>(bound);
    } else {
      blobs[i].assign(raw.begin(), raw.end());
    }
  }

  // Header, directory and names are copied; offsets and sizes are rebuilt.
  std::vector<uint8_t> out(data_.begin(), data_.begin() + data_offset_);
  uint32_t uncompressed_offset = 0;
  uint32_t data_end = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    entries[i].fields[2] = uncompressed_offset;
    uncompressed_offset = Align(uint64_t(uncompressed_offset) + entries[i].fields[4]);
    if (!compressed_) {
      out.resize(uint64_t(data_offset_) + entries[i].fields[2], 0);
    }
    out.insert(out.end(), blobs[i].begin(), blobs[i].end());
    data_end = static_cast<uint32_t>(out.size() - data_offset_);
    out.resize(Align(out.size()), 0);
    for (int f = 0; f < 7; ++f) PutBe32(&out[0x800 + i * 28 + f * 4], entries[i].fields[f]);
  }
  PutBe32(&out[0x158], static_cast<uint32_t>(out.size()));
  PutBe32(&out[0x164], uncompressed_offset);
  if (compressed_) PutBe32(&out[0x168], data_end);

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    if (error) *error = "cannot write " + path.string();
    return false;
  }
  file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  if (!file) {
    if (error) *error = "write failed";
    return false;
  }
  return true;
}

bool Packfile::SaveCondensed(const std::filesystem::path& path,
                             const std::map<std::string, std::string>& replacements,
                             std::string* error) const {
  // Rebuild the single data block: every file at a 64-byte aligned offset.
  std::vector<Entry> entries = entries_;
  std::vector<uint8_t> blob;
  blob.reserve(blob_.size() + 1024 * 1024);
  for (size_t i = 0; i < entries.size(); ++i) {
    auto it = std::find_if(replacements.begin(), replacements.end(), [&](const auto& r) {
      return Lower(r.first) == Lower(entries[i].name);
    });
    const uint8_t* begin;
    size_t size;
    if (it != replacements.end()) {
      begin = reinterpret_cast<const uint8_t*>(it->second.data());
      size = it->second.size();
      // The per-file compressed size is kept meaningful for the replaced file.
      uLongf bound = compressBound(static_cast<uLong>(size));
      std::vector<uint8_t> tmp(bound);
      if (compress2(tmp.data(), &bound, begin, static_cast<uLong>(size), 9) == Z_OK) {
        entries[i].fields[5] = static_cast<uint32_t>(bound);
      }
    } else {
      begin = &blob_[entries_[i].fields[2]];
      size = entries_[i].fields[4];
    }
    blob.resize((blob.size() + 63) & ~size_t(63), 0);
    entries[i].fields[2] = static_cast<uint32_t>(blob.size());
    entries[i].fields[4] = static_cast<uint32_t>(size);
    blob.insert(blob.end(), begin, begin + size);
  }
  blob.resize((blob.size() + 63) & ~size_t(63), 0);

  std::vector<uint8_t> stored;
  if (compressed_) {
    uLongf bound = compressBound(static_cast<uLong>(blob.size()));
    stored.resize(bound);
    if (compress2(stored.data(), &bound, blob.data(), static_cast<uLong>(blob.size()), 9) != Z_OK) {
      if (error) *error = "compression failed";
      return false;
    }
    stored.resize(bound);
  } else {
    stored = blob;
  }

  std::vector<uint8_t> out(data_.begin(), data_.begin() + data_offset_);
  for (size_t i = 0; i < entries.size(); ++i) {
    for (int f = 0; f < 7; ++f) PutBe32(&out[0x800 + i * 28 + f * 4], entries[i].fields[f]);
  }
  out.insert(out.end(), stored.begin(), stored.end());
  out.resize(Align(out.size()), 0);
  PutBe32(&out[0x158], static_cast<uint32_t>(out.size()));
  PutBe32(&out[0x164], static_cast<uint32_t>(blob.size()));
  if (compressed_) PutBe32(&out[0x168], static_cast<uint32_t>(stored.size()));

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    if (error) *error = "cannot write " + path.string();
    return false;
  }
  file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  if (!file) {
    if (error) *error = "write failed";
    return false;
  }
  return true;
}

}  // namespace wml
