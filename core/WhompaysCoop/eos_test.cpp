// Checks the EOS setup outside the game: login, create a lobby (host) or
// join one by code, then leave. Usage: eos_test <mod folder> [code]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "eos_link.h"
int main(int argc, char** argv) {
  if (argc < 2) return 2;
  eos::log_fn = [](const char* s) { std::printf("%s\n", s); std::fflush(stdout); };
  const bool host = argc < 3;
  if (!eos::Begin(argv[1], host, host ? "" : argv[2], "EosTest", 2)) { std::printf("FAILED: %s\n", eos::error.c_str()); return 1; }
  for (int i = 0; i < 3000 && eos::state != eos::State::Ready && eos::state != eos::State::Failed; ++i) { eos::Tick(); Sleep(10); }
  const bool ok = eos::state == eos::State::Ready;
  std::printf("%s: %s\n", ok ? "OK" : "FAILED", eos::Text().c_str());
  eos::End();
  return ok ? 0 : 1;
}
