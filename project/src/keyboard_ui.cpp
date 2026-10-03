// The game's on-screen keyboard (XShowKeyboardUI) on PC.
//
// On the 360, text fields (Form Gang: name / tag / URL / motto, ...) open the
// console's keyboard. sub_8235E528 calls the XamShowKeyboardUI import through
// sub_82717300 (user, flags, default text, title, description, buffer,
// buffer length in characters, XOVERLAPPED) and polls the XOVERLAPPED until
// it completes. The runtime has no keyboard window here, so this hook opens
// the chat input line (chat.cpp) with the field's title as the prompt: type
// on the keyboard, Enter keeps the text, Esc cancels.
#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include "chat.h"

#include <cstring>
#include <string>

#include <rex/logging.h>
#include <rex/ppc/function.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

uint8_t* Host(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
void W32(uint8_t* base, uint32_t a, uint32_t v) {
  const uint32_t be = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &be, 4);
}
uint32_t R32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Host(base, a), 4);
  return __builtin_bswap32(v);
}

// Guest UTF-16 (big-endian) -> UTF-8.
std::string ReadWide(uint8_t* base, uint32_t a, size_t max = 512) {
  if (!a) return {};
  std::wstring w;
  for (size_t i = 0; i < max; ++i) {
    const uint8_t* p = Host(base, a + uint32_t(i * 2));
    const wchar_t c = wchar_t(p[0] << 8 | p[1]);
    if (!c) break;
    w.push_back(c);
  }
#ifdef _WIN32
  if (w.empty()) return {};
  char out[2048];
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out, sizeof(out), nullptr, nullptr);
  return n > 0 ? std::string(out, size_t(n)) : std::string();
#else
  std::string s;
  for (wchar_t c : w) s.push_back(c < 128 ? char(c) : '?');
  return s;
#endif
}

// UTF-8 -> guest UTF-16 (big-endian), at most max_chars characters + NUL.
void WriteWide(uint8_t* base, uint32_t a, const std::string& s, size_t max_chars) {
  std::wstring w;
#ifdef _WIN32
  if (!s.empty()) {
    w.resize(s.size());
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    w.resize(n > 0 ? size_t(n) : 0);
  }
#else
  for (char c : s) w.push_back(wchar_t(uint8_t(c)));
#endif
  if (w.size() > max_chars) w.resize(max_chars);
  for (size_t i = 0; i < w.size(); ++i) {
    uint8_t* p = Host(base, a + uint32_t(i * 2));
    p[0] = uint8_t(w[i] >> 8);
    p[1] = uint8_t(w[i]);
  }
  uint8_t* end = Host(base, a + uint32_t(w.size() * 2));
  end[0] = end[1] = 0;
}

constexpr uint32_t kIoPending = 997, kCancelled = 1223;

}  // namespace

// XShowKeyboardUI(user, flags, default, title, description, buffer, length, overlapped)
PPC_FUNC(sub_82717300) {
  const uint32_t def = ctx.r5.u32, title = ctx.r6.u32, desc = ctx.r7.u32;
  const uint32_t buffer = ctx.r8.u32, length = ctx.r9.u32, ov = ctx.r10.u32;
  if (!buffer || !length || !ov) {
    ctx.r3.u64 = 87;  // ERROR_INVALID_PARAMETER
    return;
  }
  std::string prompt = ReadWide(base, title);
  const std::string description = ReadWide(base, desc);
  if (prompt.empty()) prompt = description;
  const std::string text = ReadWide(base, def);
  const size_t max_chars = length > 1 ? length - 1 : 1;
  if (R32(base, ov + 12)) REXLOG_WARN("Keyboard: overlapped has an event ({:08X}), not signalled", R32(base, ov + 12));
  W32(base, ov + 24, 0);
  W32(base, ov + 4, 0);
  W32(base, ov + 0, kIoPending);
  REXLOG_INFO("Keyboard: \"{}\" ({}) default \"{}\", {} chars", prompt, description, text, max_chars);
  sr::KeyboardBegin(prompt, text, max_chars, [base, buffer, ov, max_chars](bool ok, const std::string& typed) {
    if (ok) WriteWide(base, buffer, typed, max_chars);
    W32(base, ov + 24, ok ? 0 : kCancelled);  // extended error
    W32(base, ov + 4, 0);
    W32(base, ov + 0, 0);                     // ERROR_SUCCESS: done
    REXLOG_INFO("Keyboard: {} \"{}\"", ok ? "entered" : "cancelled", ok ? typed : std::string());
  });
  ctx.r3.u64 = kIoPending;
}
