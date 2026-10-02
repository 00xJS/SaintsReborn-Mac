-- Multiplayer (built in): runs every frame.
-- 1. solo_start: one player can start a System Link match alone (to try out
--    maps). The lobby's "needs at least 2 player(s)" check (sub_82392758) is
--    skipped while byte 0x8370F28A is set.
-- 2. min_players: Ranked / Player Match (matchmaking) start their countdown
--    only when the host has the game's "mp_auto_mm_conn_needed" players
--    connected (int at 0x827ADF04, default 4; sub_82362108). Co-op needs 2.
--    With 2 or 3 players a matchmade game never started.
local solo = wml.setting("solo_start", true)
local min_players = math.max(1, math.floor(wml.setting("min_players", 2)))
local logged = false
wml.on_frame(function()
  if solo and wml.read_u8(0x8370F28A) == 0 then wml.write_u8(0x8370F28A, 1) end
  if wml.read_u32(0x827ADF04) ~= min_players then
    wml.write_u32(0x827ADF04, min_players)
    if not logged then
      logged = true
      wml.log("[Multiplayer] matchmade games start with " .. min_players .. " player(s) (game default 4)")
    end
  end
end)
