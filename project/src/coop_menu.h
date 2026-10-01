// Co-op in the game's own menus (coop_menu.cpp): Options > Host / Join / End
// Co-op in the story pause menu and Join Co-op on the main menu. The rows only
// appear when the Whompays Co-op mod is loaded; they call its exported
// WhompaysCoop* functions.
#pragma once
#include <cstdint>

struct PPCContext;

namespace sr {

// Called often from the input hook: joins once the save picked after
// Join Co-op (main menu) is loaded.
void CoopMenuPoll(uint8_t* base);

// Main menu update (sub_822861F0, wrapped in kbm.cpp): before and after the
// game's own update. Pre returns true when it handled the frame itself.
bool CoopMainMenuPre(PPCContext& ctx, uint8_t* base);
void CoopMainMenuPost(PPCContext& ctx, uint8_t* base);

// True while the Join Co-op code box (the game's message dialog) has the
// keyboard: kbm.cpp sends the game no keys then.
bool CoopDialogOpen();

// True for a moment after HOST / JOIN CO-OP in the pause menu: kbm.cpp sends
// a Start press then, which closes the pause menu.
bool CoopResumePulse();

}  // namespace sr
