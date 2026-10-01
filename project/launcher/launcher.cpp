// Whompay's Mod Loader - launcher window.
//
// Lists the mods in the "mods" folder next to this exe, lets the player turn
// them on and off and change their load order, saves mods/modlist.ini and
// starts saintsrow.exe.
//
// Part of Saints Reborn (MIT License).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "wml/mod_list.h"

namespace {

namespace fs = std::filesystem;

constexpr wchar_t kTitle[] = L"Whompay's Mod Loader";
constexpr wchar_t kGameExe[] = L"saintsrow.exe";

enum ControlId : int {
  kIdList = 100,
  kIdDescription,
  kIdUp,
  kIdDown,
  kIdRefresh,
  kIdOpenFolder,
  kIdPlay,
};

struct App {
  HINSTANCE instance = nullptr;
  HWND window = nullptr;
  HWND title = nullptr;
  HWND subtitle = nullptr;
  HWND list = nullptr;
  HWND mod_name = nullptr;
  HWND mod_meta = nullptr;
  HWND description = nullptr;
  HWND status = nullptr;
  HWND up = nullptr;
  HWND down = nullptr;
  HWND refresh = nullptr;
  HWND open_folder = nullptr;
  HWND play = nullptr;
  HFONT font = nullptr;
  HFONT bold_font = nullptr;
  HFONT title_font = nullptr;
  float scale = 1.0f;
  fs::path exe_dir;
  fs::path mods_dir;
  std::vector<wml::ModInfo> mods;
  bool updating_list = false;
};

App g_app;

int S(int value) { return static_cast<int>(value * g_app.scale + 0.5f); }

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
  std::wstring wide(length, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), wide.data(), length);
  return wide;
}

// Edit controls need \r\n line breaks.
std::wstring ToEditText(const std::string& utf8) {
  std::wstring text = Widen(utf8);
  std::wstring out;
  for (wchar_t c : text) {
    if (c == L'\n') out += L'\r';
    out += c;
  }
  return out;
}

std::wstring ModKinds(const wml::ModInfo& mod) {
  std::wstring kinds;
  auto add = [&kinds](const wchar_t* kind) {
    if (!kinds.empty()) kinds += L", ";
    kinds += kind;
  };
  if (mod.has_files()) add(L"Files");
  if (mod.has_code()) add(L"Code");
  if (mod.has_script()) add(L"Lua");
  if (mod.has_patch()) add(L"Patch");
  return kinds.empty() ? L"(empty)" : kinds;
}

void SaveList() {
  if (!wml::SaveModList(g_app.mods_dir, g_app.mods)) {
    MessageBoxW(g_app.window, L"Could not save mods\\modlist.ini.", kTitle, MB_ICONERROR);
  }
}

int SelectedIndex() {
  return ListView_GetNextItem(g_app.list, -1, LVNI_SELECTED);
}

void UpdateStatus() {
  size_t enabled = std::count_if(g_app.mods.begin(), g_app.mods.end(),
                                 [](const wml::ModInfo& m) { return m.enabled; });
  wchar_t text[128];
  if (g_app.mods.empty()) {
    std::swprintf(text, 128, L"No mods found. Put mods in the \"mods\" folder.");
  } else {
    std::swprintf(text, 128, L"%zu of %zu mods enabled", enabled, g_app.mods.size());
  }
  SetWindowTextW(g_app.status, text);
}

void UpdateDetails() {
  int index = SelectedIndex();
  bool valid = index >= 0 && index < (int)g_app.mods.size();
  EnableWindow(g_app.up, valid && index > 0);
  EnableWindow(g_app.down, valid && index + 1 < (int)g_app.mods.size());
  if (!valid) {
    SetWindowTextW(g_app.mod_name, L"");
    SetWindowTextW(g_app.mod_meta, L"");
    SetWindowTextW(g_app.description,
                   g_app.mods.empty()
                       ? L"Each mod is a folder inside \"mods\" with a mod.ini, and any of:\r\n\r\n"
                         L"  files\\    replacement game files\r\n"
                         L"  *.dll     code mod\r\n"
                         L"  main.lua  script mod"
                       : L"Select a mod to see its details.");
    return;
  }
  const auto& mod = g_app.mods[index];
  SetWindowTextW(g_app.mod_name, Widen(mod.name).c_str());
  std::wstring meta;
  if (!mod.author.empty()) meta += L"by " + Widen(mod.author);
  if (!mod.version.empty()) meta += (meta.empty() ? L"version " : L"  ·  version ") + Widen(mod.version);
  if (!meta.empty()) meta += L"  ·  ";
  meta += ModKinds(mod);
  SetWindowTextW(g_app.mod_meta, meta.c_str());
  std::wstring description =
      mod.description.empty() ? L"No description." : ToEditText(mod.description);
  description += L"\r\n\r\nFolder: mods\\" + Widen(mod.id);
  SetWindowTextW(g_app.description, description.c_str());
}

void FillList(int select) {
  g_app.updating_list = true;
  ListView_DeleteAllItems(g_app.list);
  for (size_t i = 0; i < g_app.mods.size(); ++i) {
    const auto& mod = g_app.mods[i];
    std::wstring name = Widen(mod.name);
    std::wstring kinds = ModKinds(mod);
    std::wstring version = Widen(mod.version);
    std::wstring author = Widen(mod.author);
    LVITEMW item = {};
    item.mask = LVIF_TEXT;
    item.iItem = (int)i;
    item.pszText = name.data();
    ListView_InsertItem(g_app.list, &item);
    ListView_SetItemText(g_app.list, (int)i, 1, kinds.data());
    ListView_SetItemText(g_app.list, (int)i, 2, version.data());
    ListView_SetItemText(g_app.list, (int)i, 3, author.data());
    ListView_SetCheckState(g_app.list, (int)i, mod.enabled);
  }
  if (select >= 0 && select < (int)g_app.mods.size()) {
    ListView_SetItemState(g_app.list, select, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_app.list, select, FALSE);
  }
  g_app.updating_list = false;
  UpdateDetails();
  UpdateStatus();
}

void Reload() {
  std::error_code ec;
  fs::create_directories(g_app.mods_dir, ec);
  std::string selected_id;
  int index = SelectedIndex();
  if (index >= 0 && index < (int)g_app.mods.size()) selected_id = g_app.mods[index].id;
  g_app.mods = wml::LoadMods(g_app.mods_dir);
  int select = g_app.mods.empty() ? -1 : 0;
  for (size_t i = 0; i < g_app.mods.size(); ++i) {
    if (g_app.mods[i].id == selected_id) select = (int)i;
  }
  FillList(select);
}

void Move(int delta) {
  int index = SelectedIndex();
  int target = index + delta;
  if (index < 0 || target < 0 || target >= (int)g_app.mods.size()) return;
  std::swap(g_app.mods[index], g_app.mods[target]);
  SaveList();
  FillList(target);
  SetFocus(g_app.list);
}

// Online play (Epic) comes as a separate online pack that setup downloads
// (scripts\online_pack.ps1). When that download failed, or the files were
// replaced, the pack is fetched again here before the game starts: a few
// seconds, at most once every 30 minutes, nothing at all when it's installed.
std::string ReadSmallFile(const fs::path& path) {
  std::string text;
  if (FILE* f = _wfopen(path.c_str(), L"rb")) {
    char buffer[256];
    size_t n = fread(buffer, 1, sizeof(buffer), f);
    fclose(f);
    text.assign(buffer, n);
  }
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
  return text;
}

void RepairOnlinePack() {
  const fs::path dist = g_app.exe_dir;
  const std::string wanted = ReadSmallFile(dist / L"online_pack_wanted.txt");
  if (wanted.empty()) return;  // not set up by setup.ps1 (own Epic files or a developer build)
  std::error_code ec;
  if (ReadSmallFile(dist / L"online_pack.txt") == wanted &&
      fs::exists(dist / L"core" / L"WhompaysCoop" / L"eos" / L"EOSSDK-Win64-Shipping.dll", ec))
    return;
  const fs::path script = dist.parent_path() / L"scripts" / L"online_pack.ps1";
  if (!fs::exists(script, ec)) return;
  const fs::path tried = dist / L"online_pack_try.txt";
  const auto last = fs::last_write_time(tried, ec);
  if (!ec && fs::file_time_type::clock::now() - last < std::chrono::minutes(30)) return;
  if (FILE* f = _wfopen(tried.c_str(), L"wb")) fclose(f);

  wchar_t system_dir[MAX_PATH] = {};
  GetSystemDirectoryW(system_dir, MAX_PATH);
  const std::wstring powershell = std::wstring(system_dir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
  std::wstring command = L"\"" + powershell + L"\" -NoProfile -ExecutionPolicy Bypass -File \"" +
                         script.wstring() + L"\" -Root \"" + dist.parent_path().wstring() + L"\" -Dist \"" +
                         dist.wstring() + L"\"";
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      dist.c_str(), &startup, &process))
    return;
  CloseHandle(process.hThread);
  SetWindowTextW(g_app.window, L"Whompay's Mod Loader - getting the online play files...");
  HCURSOR old_cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(120)) {
    if (MsgWaitForMultipleObjects(1, &process.hProcess, FALSE, 100, QS_ALLINPUT) == WAIT_OBJECT_0) break;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_LBUTTONDOWN || msg.message == WM_KEYDOWN || msg.message == WM_LBUTTONUP) continue;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  CloseHandle(process.hProcess);
  SetCursor(old_cursor);
  SetWindowTextW(g_app.window, kTitle);
}

void Play() {
  SaveList();
  RepairOnlinePack();
  fs::path game = g_app.exe_dir / kGameExe;
  std::error_code ec;
  if (!fs::exists(game, ec)) {
    MessageBoxW(g_app.window, L"saintsrow.exe was not found next to the mod loader.", kTitle,
                MB_ICONERROR);
    return;
  }
  std::wstring command = L"\"" + game.wstring() + L"\"";
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      g_app.exe_dir.c_str(), &startup, &process)) {
    MessageBoxW(g_app.window, L"Could not start saintsrow.exe.", kTitle, MB_ICONERROR);
    return;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  DestroyWindow(g_app.window);
}

void Layout() {
  RECT rc;
  GetClientRect(g_app.window, &rc);
  int w = rc.right, h = rc.bottom;
  int m = S(16);
  int list_w = std::max(S(300), (w - 3 * m) * 58 / 100);
  int right_x = m + list_w + m;
  int right_w = std::max(S(120), w - right_x - m);
  int top = S(74);
  int button_h = S(30);
  int bottom_y = h - m - button_h;

  MoveWindow(g_app.title, m, S(12), w - 2 * m, S(34), TRUE);
  MoveWindow(g_app.subtitle, m, S(46), w - 2 * m, S(20), TRUE);

  int list_h = bottom_y - m - top;
  MoveWindow(g_app.list, m, top, list_w, list_h, TRUE);

  MoveWindow(g_app.mod_name, right_x, top, right_w, S(24), TRUE);
  MoveWindow(g_app.mod_meta, right_x, top + S(26), right_w, S(20), TRUE);
  MoveWindow(g_app.description, right_x, top + S(52), right_w, list_h - S(52), TRUE);

  int bx = m;
  int bw = S(92);
  MoveWindow(g_app.up, bx, bottom_y, bw, button_h, TRUE);
  bx += bw + S(8);
  MoveWindow(g_app.down, bx, bottom_y, bw, button_h, TRUE);
  bx += bw + S(8);
  MoveWindow(g_app.refresh, bx, bottom_y, bw, button_h, TRUE);
  bx += bw + S(8);
  MoveWindow(g_app.open_folder, bx, bottom_y, S(130), button_h, TRUE);

  int play_w = S(140);
  MoveWindow(g_app.play, w - m - play_w, bottom_y, play_w, button_h, TRUE);
  int status_x = right_x;
  MoveWindow(g_app.status, status_x, bottom_y + S(7), std::max(0, w - m - play_w - S(12) - status_x),
             S(20), TRUE);
}

HWND MakeControl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, HFONT font,
                 DWORD ex_style = 0) {
  HWND hwnd = CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                              g_app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                              g_app.instance, nullptr);
  SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  return hwnd;
}

void CreateControls() {
  g_app.title = MakeControl(L"STATIC", kTitle, SS_LEFT, 0, g_app.title_font);
  g_app.subtitle = MakeControl(
      L"STATIC", L"Saints Reborn  ·  tick the mods you want, then press Play.", SS_LEFT, 0,
      g_app.font);

  g_app.list = MakeControl(WC_LISTVIEWW, L"",
                           LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, kIdList,
                           g_app.font, WS_EX_CLIENTEDGE);
  SetWindowTheme(g_app.list, L"Explorer", nullptr);
  ListView_SetExtendedListViewStyle(
      g_app.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  struct Column {
    const wchar_t* name;
    int width;
  };
  const Column columns[] = {{L"Mod", 210}, {L"Type", 100}, {L"Version", 64}, {L"Author", 100}};
  for (int i = 0; i < 4; ++i) {
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<wchar_t*>(columns[i].name);
    column.cx = S(columns[i].width);
    ListView_InsertColumn(g_app.list, i, &column);
  }

  g_app.mod_name = MakeControl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0, g_app.bold_font);
  g_app.mod_meta = MakeControl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0, g_app.font);
  g_app.description = MakeControl(
      L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, kIdDescription,
      g_app.font, WS_EX_CLIENTEDGE);

  g_app.up = MakeControl(L"BUTTON", L"Move up", BS_PUSHBUTTON | WS_TABSTOP, kIdUp, g_app.font);
  g_app.down = MakeControl(L"BUTTON", L"Move down", BS_PUSHBUTTON | WS_TABSTOP, kIdDown, g_app.font);
  g_app.refresh = MakeControl(L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, kIdRefresh, g_app.font);
  g_app.open_folder =
      MakeControl(L"BUTTON", L"Open mods folder", BS_PUSHBUTTON | WS_TABSTOP, kIdOpenFolder, g_app.font);
  g_app.play =
      MakeControl(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, kIdPlay, g_app.bold_font);
  g_app.status = MakeControl(L"STATIC", L"", SS_RIGHT, 0, g_app.font);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      g_app.window = hwnd;
      CreateControls();
      Layout();
      Reload();
      return 0;
    case WM_SIZE:
      Layout();
      return 0;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize.x = S(640);
      info->ptMinTrackSize.y = S(400);
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      // Keep the read-only description white like a normal text box.
      if (reinterpret_cast<HWND>(lparam) == g_app.description) {
        SetBkColor(reinterpret_cast<HDC>(wparam), GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
      }
      break;
    }
    case WM_NOTIFY: {
      auto* header = reinterpret_cast<NMHDR*>(lparam);
      if (header->idFrom == kIdList && header->code == LVN_ITEMCHANGED && !g_app.updating_list) {
        auto* change = reinterpret_cast<NMLISTVIEW*>(lparam);
        if (change->uChanged & LVIF_STATE) {
          bool check_changed =
              ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK) != 0;
          if (check_changed && change->iItem >= 0 && change->iItem < (int)g_app.mods.size()) {
            g_app.mods[change->iItem].enabled = ListView_GetCheckState(g_app.list, change->iItem);
            SaveList();
            UpdateStatus();
          }
          if ((change->uNewState ^ change->uOldState) & LVIS_SELECTED) UpdateDetails();
        }
      }
      if (header->idFrom == kIdList && header->code == NM_DBLCLK) {
        int index = SelectedIndex();
        if (index >= 0) {
          ListView_SetCheckState(g_app.list, index, !ListView_GetCheckState(g_app.list, index));
        }
      }
      break;
    }
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kIdUp: Move(-1); return 0;
        case kIdDown: Move(1); return 0;
        case kIdRefresh: Reload(); return 0;
        case kIdOpenFolder:
          ShellExecuteW(hwnd, L"open", g_app.mods_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          return 0;
        case kIdPlay: Play(); return 0;
      }
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

// Common Controls 6 (themed buttons and list) without a resource file.
void EnableVisualStyles() {
  static const char kManifest[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
      "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
      "<dependency><dependentAssembly><assemblyIdentity type=\"win32\" "
      "name=\"Microsoft.Windows.Common-Controls\" version=\"6.0.0.0\" "
      "processorArchitecture=\"*\" publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>"
      "</dependentAssembly></dependency></assembly>";
  wchar_t temp_dir[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, temp_dir)) return;
  std::wstring path = std::wstring(temp_dir) + L"WhompaysModLoader.manifest";
  if (FILE* f = _wfopen(path.c_str(), L"wb")) {
    std::fwrite(kManifest, 1, sizeof(kManifest) - 1, f);
    std::fclose(f);
  } else {
    return;
  }
  ACTCTXW context = {sizeof(context)};
  context.lpSource = path.c_str();
  HANDLE handle = CreateActCtxW(&context);
  if (handle != INVALID_HANDLE_VALUE) {
    ULONG_PTR cookie = 0;
    ActivateActCtx(handle, &cookie);
  }
}

HFONT MakeFont(int point_size, int weight) {
  NONCLIENTMETRICSW metrics = {sizeof(metrics)};
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
  LOGFONTW font = metrics.lfMessageFont;
  HDC dc = GetDC(nullptr);
  font.lfHeight = -MulDiv(point_size, GetDeviceCaps(dc, LOGPIXELSY), 72);
  ReleaseDC(nullptr, dc);
  font.lfWeight = weight;
  return CreateFontIndirectW(&font);
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
  SetProcessDPIAware();
  EnableVisualStyles();
  INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);

  g_app.instance = instance;
  wchar_t exe_path[MAX_PATH];
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  g_app.exe_dir = fs::path(exe_path).parent_path();
  g_app.mods_dir = g_app.exe_dir / "mods";

  HDC dc = GetDC(nullptr);
  g_app.scale = GetDeviceCaps(dc, LOGPIXELSY) / 96.0f;
  ReleaseDC(nullptr, dc);
  g_app.font = MakeFont(9, FW_NORMAL);
  g_app.bold_font = MakeFont(10, FW_SEMIBOLD);
  g_app.title_font = MakeFont(18, FW_BOLD);

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
  wc.lpszClassName = L"WhompaysModLoader";
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));  // own icon (res/modloader.rc)
  if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  RegisterClassExW(&wc);

  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, kTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, S(900), S(560), nullptr, nullptr, instance, nullptr);
  if (!hwnd) return 1;
  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return 0;
}
