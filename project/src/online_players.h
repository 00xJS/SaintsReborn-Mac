// MULTIPLAYER > PLAYERS: everyone online in Saints Reborn, as a tab of the
// game's own MULTIPLAYER screen (online_players.cpp). Hooks are called from
// the menu overrides in coop_menu.cpp.
#pragma once
#include <cstdint>

struct PPCContext;

namespace sr {
// After the game added a MULTIPLAYER tab (sub_822903E8): adds PLAYERS after
// LEADERBOARDS and asks the runtime for the online list.
void PlayersAfterTabAdded(PPCContext& ctx, uint8_t* base, uint32_t lr, uint32_t id);
// True while the PLAYERS tab (menu id 47, built with the OPTIONS tab's
// functions) is being built / is the current menu.
bool PlayersBuilding(uint8_t* base);
bool PlayersCurrent(uint8_t* base);
// At the OPTIONS list finisher while PLAYERS is built: the rows become the
// player list.
void PlayersFillList(PPCContext& ctx, uint8_t* base);
// Every frame of the PLAYERS tab, before the OPTIONS update runs: rebuilds
// the list when it changed.
void PlayersUpdate(PPCContext& ctx, uint8_t* base);
// After the list finisher: puts the cursor back where it was.
void PlayersAfterFinish(PPCContext& ctx, uint8_t* base);
// Often (input hook): tells the runtime what this player is doing.
void PlayersActivityPoll(uint8_t* base);
}  // namespace sr
