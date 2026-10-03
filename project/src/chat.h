// In-game chat (chat.cpp): T opens a "Say:" line, Enter sends, Esc cancels.
// Lines go to everyone in the lobby / match over Epic (runtime SrChat*) and
// to the other co-op player (WhompaysCoop.dll WhompaysCoopChat*).
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace sr {
struct ChatLine {
  std::string name, text;
  float alpha = 1.0f;
  bool system = false;  // a note from the game ("nobody to chat with"), no name
};
// Input thread, every input poll: T / typing / Enter / Esc, and new lines.
void ChatPoll(bool in_multiplayer = false);  // in_multiplayer: a System Link lobby or a match
// True while the player types (the game and the mods get no keys).
bool ChatTyping();
// For drawing (UI thread): the lines to show now; true + input while typing.
bool ChatSnapshot(std::vector<ChatLine>& lines, std::string& input);
// The label in front of the input line ("Say: ", or the keyboard prompt's).
std::string ChatPrompt();
// The game's on-screen keyboard (XShowKeyboardUI, keyboard_ui.cpp) on PC: the
// chat input line opens with prompt and text; Enter calls done(true, text),
// Esc done(false, ""). done runs on the input thread.
void KeyboardBegin(const std::string& prompt, const std::string& text, size_t max_chars,
                   std::function<void(bool, const std::string&)> done);
// A list to pick from (the friends list for "Invite New Gang Member",
// friends_ui.cpp): Up/Down choose, Enter calls done(index), Esc done(-1).
// note = a line under the title. done runs on the input thread.
struct PickerItem {
  std::string text, detail;
  bool dim = false;
};
void PickerBegin(const std::string& title, const std::string& note, std::vector<PickerItem> items,
                 std::function<void(int)> done);
bool PickerSnapshot(std::string& title, std::string& note, std::vector<PickerItem>& items, int& selected);
}  // namespace sr
