// In-game chat (chat.cpp): T opens a "Say:" line, Enter sends, Esc cancels.
// Lines go to everyone in the lobby / match over Epic (runtime SrChat*) and
// to the other co-op player (WhompaysCoop.dll WhompaysCoopChat*).
#pragma once
#include <string>
#include <vector>

namespace sr {
struct ChatLine {
  std::string name, text;
  float alpha = 1.0f;
  bool system = false;  // a note from the game ("nobody to chat with"), no name
};
// Input thread, every input poll: T / typing / Enter / Esc, and new lines.
void ChatPoll();
// True while the player types (the game and the mods get no keys).
bool ChatTyping();
// For drawing (UI thread): the lines to show now; true + input while typing.
bool ChatSnapshot(std::vector<ChatLine>& lines, std::string& input);
}  // namespace sr
