// Self-test: chunk read/write round trip, geometry round trip, collision traversal, packfile read.
#include <cstdio>
#include <fstream>
#include "../src/chunk.h"
#include "../src/collision.h"
#include "../src/vpp.h"
using namespace mapconv;
static Bytes Load(const char* p) {
  std::ifstream f(p, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(f), {});
}
int main(int argc, char** argv) {
  int bad = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.size() > 10 && a.substr(a.size() - 10) == ".vpp_xbox2") {
      Packfile p(a);
      Bytes b = p.Read(argv[++i]);
      std::ofstream(std::string("/tmp/vpp_out_") + argv[i], std::ios::binary).write((char*)b.data(), b.size());
      std::printf("%s: read %s (%zu bytes)\n", a.c_str(), argv[i], b.size());
      continue;
    }
    Bytes d = Load(argv[i]);
    try {
      Chunk ch = ReadChunk(d);
      Bytes w = WriteChunk(ch);
      Geometry g = ChunkGeometry(ch);
      Bytes gw = WriteGeometry(g, g.start);
      bool geo_ok = gw == ch.Get("geometry").data || Bytes(ch.Get("geometry").data.begin(), ch.Get("geometry").data.begin() + gw.size()) == gw;
      size_t keys = 0;
      for (auto& blk : ParseMoppBlocks(g.coll)) keys += MoppKeys(blk.code).size();
      std::printf("%s: chunk %s, geometry %s, sections %zu, meshes %zu, collision keys %zu\n", argv[i],
                  w == d ? "IDENTICAL" : "DIFFERENT", geo_ok ? "identical" : "DIFFERENT", ch.secs.size(), g.meshes.size(), keys);
      if (w != d || !geo_ok) ++bad;
    } catch (const std::exception& e) {
      std::printf("%s: ERROR %s\n", argv[i], e.what());
      ++bad;
    }
  }
  return bad;
}
