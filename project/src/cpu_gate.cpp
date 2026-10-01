// CPU gate: the process entry point of saintsrow.exe (linker /ENTRY).
//
// The recompiled game code is built for x86-64-v3 (Haswell / Zen and newer:
// AVX2, BMI1/2, FMA, F16C, LZCNT, MOVBE). On an older CPU (Sandy / Ivy Bridge,
// Core 2 / Nehalem, AMD before Excavator: most PCs and laptops from 2012 and
// earlier) the first such instruction would kill the process without a word.
// This file is compiled for plain x86-64 (see CMakeLists.txt) and runs before
// the C runtime: if the CPU lacks any v3 feature it starts
// saintsrow_compat.exe (the same game built for x86-64-v2) next to it with the
// same command line and passes its exit code on; without that file it says
// what's missing. Otherwise it continues into the normal C runtime startup.
//
// Test aid: a file named "force_compat_exe" next to the exe takes the compat
// path on any CPU.
//
// No C runtime here (it isn't initialized yet): Win32 calls and CPUID only.

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>

extern "C" unsigned long wWinMainCRTStartup(void*);

namespace {

unsigned long long ReadXcr0() {
  unsigned int lo = 0, hi = 0;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return (static_cast<unsigned long long>(hi) << 32) | lo;
}

// Names of the missing x86-64-v3 features, e.g. L"AVX2 BMI2 MOVBE"; empty = all there.
void MissingFeatures(wchar_t* out, int cap) {
  int n = 0;
  auto add = [&](const wchar_t* name) {
    if (n && n < cap - 1) out[n++] = L' ';
    for (; *name && n < cap - 1; ++name) out[n++] = *name;
  };
  int r[4] = {};
  __cpuid(r, 0);
  const int max_leaf = r[0];
  __cpuid(r, 1);
  const int ecx1 = r[2];
  const bool osxsave = (ecx1 >> 27) & 1;
  const bool avx = ((ecx1 >> 28) & 1) && osxsave && (ReadXcr0() & 6) == 6;  // OS saves XMM+YMM
  if (!avx) add(L"AVX");
  if (!((ecx1 >> 12) & 1)) add(L"FMA");
  if (!((ecx1 >> 29) & 1)) add(L"F16C");
  if (!((ecx1 >> 22) & 1)) add(L"MOVBE");
  int ebx7 = 0;
  if (max_leaf >= 7) {
    __cpuidex(r, 7, 0);
    ebx7 = r[1];
  }
  if (!((ebx7 >> 5) & 1)) add(L"AVX2");
  if (!((ebx7 >> 3) & 1)) add(L"BMI1");
  if (!((ebx7 >> 8) & 1)) add(L"BMI2");
  __cpuid(r, 0x80000000);
  int ecx_ext = 0;
  if (static_cast<unsigned>(r[0]) >= 0x80000001u) {
    __cpuid(r, 0x80000001);
    ecx_ext = r[2];
  }
  if (!((ecx_ext >> 5) & 1)) add(L"LZCNT");
  out[n] = 0;
}

// Folder of this exe with a trailing backslash (0 on failure).
int ExeFolder(wchar_t* out, int cap) {
  const DWORD len = GetModuleFileNameW(nullptr, out, DWORD(cap));
  if (len == 0 || len >= DWORD(cap)) return 0;
  int n = int(len);
  while (n > 0 && out[n - 1] != L'\\' && out[n - 1] != L'/') --n;
  out[n] = 0;
  return n;
}

bool Append(wchar_t* buf, int cap, int at, const wchar_t* tail) {
  for (; *tail; ++tail) {
    if (at >= cap - 1) return false;
    buf[at++] = *tail;
  }
  buf[at] = 0;
  return true;
}

bool Exists(const wchar_t* path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

// Command line without the program name (the part after argv[0]).
const wchar_t* ArgumentsOf(const wchar_t* cmd) {
  if (!cmd) return L"";
  if (*cmd == L'"') {
    ++cmd;
    while (*cmd && *cmd != L'"') ++cmd;
    if (*cmd) ++cmd;
  } else {
    while (*cmd && *cmd != L' ' && *cmd != L'\t') ++cmd;
  }
  return cmd;  // keeps the leading space, if any
}

}  // namespace

extern "C" unsigned long sr_cpu_gate_entry(void* peb) {
  wchar_t missing[128];
  MissingFeatures(missing, 128);
  wchar_t dir[1024];
  const int dn = ExeFolder(dir, 1024);
  bool forced = false;
  if (dn) {
    wchar_t probe[1100];
    Append(probe, 1100, 0, dir);
    forced = Append(probe, 1100, dn, L"force_compat_exe") && Exists(probe);
  }
  if (!missing[0] && !forced) return wWinMainCRTStartup(peb);

  wchar_t compat[1100];
  const bool have_compat = dn && Append(compat, 1100, 0, dir) &&
                           Append(compat, 1100, dn, L"saintsrow_compat.exe") && Exists(compat);
  if (have_compat) {
    // "<compat path>" + the original arguments.
    static wchar_t cmd[32768];
    int n = 0;
    cmd[n++] = L'"';
    for (const wchar_t* p = compat; *p && n < 32000; ++p) cmd[n++] = *p;
    cmd[n++] = L'"';
    for (const wchar_t* p = ArgumentsOf(GetCommandLineW()); *p && n < 32766; ++p) cmd[n++] = *p;
    cmd[n] = 0;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(compat, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
      CloseHandle(pi.hThread);
      WaitForSingleObject(pi.hProcess, INFINITE);
      DWORD code = 0;
      GetExitCodeProcess(pi.hProcess, &code);
      CloseHandle(pi.hProcess);
      ExitProcess(code);
    }
  }
  wchar_t text[1024];
  int n = 0;
  Append(text, 1024, 0,
         L"This processor is missing instructions the fast build of Saints Reborn needs:\n\n    ");
  for (n = 0; text[n]; ++n) {}
  Append(text, 1024, n, missing[0] ? missing : L"(test: force_compat_exe)");
  for (n = 0; text[n]; ++n) {}
  Append(text, 1024, n,
         have_compat ? L"\n\nsaintsrow_compat.exe could not be started."
                     : L"\n\nsaintsrow_compat.exe (the build for older processors) was not found next to "
                       L"saintsrow.exe. Reinstall or update Saints Reborn.");
  MessageBoxW(nullptr, text, L"Saints Reborn", MB_OK | MB_ICONERROR);
  ExitProcess(1);
  return 1;
}
#endif
