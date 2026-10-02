-- Multiplayer (built in): runs every frame.
-- solo_start: one player can start a System Link match alone (to try out
-- maps). The lobby's "needs at least 2 player(s)" check (sub_82392758) is
-- skipped while byte 0x8370F28A is set.
-- (Players needed before a Ranked / Player Match starts, the game's
-- "mp_auto_mm_conn_needed" at 0x827ADF04, is set in MULTIPLAYER > OPTIONS >
-- "Players to Start" now; the exe keeps it in mp_min_players.txt.)
local solo = wml.setting("solo_start", true)
wml.on_frame(function()
  if solo and wml.read_u8(0x8370F28A) == 0 then wml.write_u8(0x8370F28A, 1) end
end)
