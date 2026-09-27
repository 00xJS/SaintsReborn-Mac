/*
 * Cheap "is this host memory readable" checks for native mods (and the exe).
 *
 * VirtualQuery also reports how far the same protection reaches (RegionSize),
 * and Windows finds that out by walking the page tables. Guest memory is a few
 * huge, uniformly committed views, so one VirtualQuery there can walk hundreds
 * of thousands of pages - it showed up as ~13% of the game's main thread.
 * QueryWorkingSetEx looks at exactly one page: for a page that is in memory
 * (almost always the case for game data) it gives the protection directly.
 * Pages that are not in memory fall back to VirtualQuery.
 *
 * Part of Saints Reborn (MIT License).
 */
#ifndef WML_PAGEQUERY_H_
#define WML_PAGEQUERY_H_

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <stddef.h>
#include <stdint.h>

/* Protection (PAGE_*) and committed state of the host page holding `p`. */
static inline bool WmlQueryPage(const void* p, DWORD* protect, bool* committed) {
  PSAPI_WORKING_SET_EX_INFORMATION ws;
  ws.VirtualAddress = const_cast<void*>(p);
  ws.VirtualAttributes.Flags = 0;
  if (K32QueryWorkingSetEx(GetCurrentProcess(), &ws, sizeof(ws)) && ws.VirtualAttributes.Valid) {
    *protect = DWORD(ws.VirtualAttributes.Win32Protection);
    *committed = true;
    return true;
  }
  MEMORY_BASIC_INFORMATION m;
  if (!VirtualQuery(p, &m, sizeof(m))) return false;
  *protect = m.Protect;
  *committed = m.State == MEM_COMMIT;
  return true;
}

/* True when every byte of [p, p + length) is committed and readable. */
static inline bool WmlHostRangeReadable(uintptr_t p, size_t length) {
  if (!length) return true;
  const uintptr_t first = p & ~uintptr_t(0xFFF);
  const uintptr_t last = (p + length - 1) & ~uintptr_t(0xFFF);
  if (last - first > (uintptr_t(64) << 12)) {
    // Long ranges: region walk (rare).
    uintptr_t q = p;
    const uintptr_t end = p + length;
    while (q < end) {
      MEMORY_BASIC_INFORMATION m;
      if (!VirtualQuery(reinterpret_cast<void*>(q), &m, sizeof(m)) || m.State != MEM_COMMIT ||
          (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !m.Protect)
        return false;
      const uintptr_t next = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
      if (next <= q) return false;
      q = next;
    }
    return true;
  }
  for (uintptr_t page = first;; page += 0x1000) {
    DWORD protect = 0;
    bool committed = false;
    if (!WmlQueryPage(reinterpret_cast<void*>(page), &protect, &committed) || !committed || !protect ||
        (protect & (PAGE_NOACCESS | PAGE_GUARD)))
      return false;
    if (page == last) break;
  }
  return true;
}
#endif

#endif /* WML_PAGEQUERY_H_ */
