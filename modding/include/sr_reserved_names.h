// Saints Reborn: player names only one person may use.
//
// A reserved name ("Whompay" and look-alikes such as "wh0mpay", "Whom Pay",
// "vvhompay", "WhompayTTV") is only accepted from the name's owner. The owner
// proves it with a signature made by owner.key (next to the exe, never
// shared); other players' games check that signature against the public key
// built into the runtime (online_identity.cpp). This header only answers
// "is this name reserved?", so the game, the runtime and mods agree on it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sr_names {

// Lower-case letters and digits only, look-alike characters folded
// (0->o, 4/@->a, 3->e, 1/!/|->i, 5/$->s, vv/uu->w, rn->m), repeated letters
// collapsed ("whommpay" -> "whompay"). Anything that is not printable ASCII
// is dropped (the game's font has no other characters).
inline std::string Normalize(std::string_view in) {
  std::string a;
  for (unsigned char c : in) {
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
    switch (c) {
      case '0': c = 'o'; break;
      case '4': case '@': case '^': c = 'a'; break;
      case '3': c = 'e'; break;
      case '1': case '!': case '|': case 'l': c = 'i'; break;
      case '5': case '$': c = 's'; break;
      case '7': case '+': c = 't'; break;
      default: break;
    }
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) a.push_back(static_cast<char>(c));
  }
  std::string b;
  for (size_t i = 0; i < a.size(); ++i) {
    const char n = i + 1 < a.size() ? a[i + 1] : 0;
    if ((a[i] == 'v' && n == 'v') || (a[i] == 'u' && n == 'u')) { b.push_back('w'); ++i; continue; }
    if (a[i] == 'r' && n == 'n') { b.push_back('m'); ++i; continue; }
    b.push_back(a[i]);
  }
  std::string c;
  for (char ch : b)
    if (c.empty() || c.back() != ch) c.push_back(ch);
  return c;
}

inline bool IsReserved(std::string_view name) {
  const std::string n = Normalize(name);
  static const char* const kReserved[] = {"whompay", "wompay", "whompai", "whompey", "whompei", "hwompay",
                                          "whonpay"};
  for (const char* r : kReserved)
    if (n.find(r) != std::string::npos) return true;
  return false;
}

// True when a network packet carries a reserved name as text: printable
// ASCII runs and UTF-16 runs (either byte order) of 5+ characters.
inline bool PacketHasReservedName(const uint8_t* data, size_t len) {
  auto printable = [](uint8_t c) { return c >= 0x20 && c < 0x7F; };
  std::string run;
  auto check = [&]() {
    const bool hit = run.size() >= 5 && IsReserved(run);
    run.clear();
    return hit;
  };
  for (size_t i = 0; i < len; ++i) {
    if (printable(data[i])) run.push_back(static_cast<char>(data[i]));
    else if (check()) return true;
  }
  if (check()) return true;
  for (size_t start = 0; start < 2; ++start) {
    for (size_t i = start; i + 1 < len; i += 2) {
      const uint8_t hi = data[i], lo = data[i + 1];
      if (hi == 0 && printable(lo)) run.push_back(static_cast<char>(lo));
      else if (lo == 0 && printable(hi)) run.push_back(static_cast<char>(hi));
      else if (check()) return true;
    }
    if (check()) return true;
  }
  return false;
}

}  // namespace sr_names
