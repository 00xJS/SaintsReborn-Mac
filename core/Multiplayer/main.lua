-- Multiplayer (built in): runs every frame.
-- solo_start: one player can start a System Link match alone (to try out
-- maps). The lobby's "needs at least 2 player(s)" check (sub_82392758) is
-- skipped while byte 0x8370F28A is set.
-- That byte is the game's debug switch mp_allow_single_player, and it also
-- turns off round rules in a match: with it on, players could run around
-- before the round countdown ended and after a round ended (Protect tha Pimp
-- and the other round modes: sub_823DFE28, sub_823E5088, ...), and a match
-- doesn't end when everyone else leaves. So it is only on while this player
-- is alone: in the lobby and menus, and in a match with nobody else in it.
-- Players in the match: the match player table count [0x830784EC] (host
-- builds it at the start, everyone else gets it with message 83) and the
-- match records list (head [0x8307F25C], next +52).
-- (Players needed before a Ranked / Player Match starts, the game's
-- "mp_auto_mm_conn_needed" at 0x827ADF04, is set in MULTIPLAYER > OPTIONS >
-- "Players to Start" now; the exe keeps it in mp_min_players.txt.)
local solo = wml.setting("solo_start", true)
local ALLOW_SINGLE = 0x8370F28A

local function guest_ptr(p)
  return p >= 0x82000000 and p < 0xA0000000
end

local function players_in_match()
  local n = wml.read_u32(0x830784EC)
  if n > 64 then n = 0 end
  local head = wml.read_u32(0x8307F25C)
  if guest_ptr(head) then
    local nxt = wml.read_u32(head + 52)
    if nxt ~= 0 and nxt ~= head and n < 2 then n = 2 end
  end
  return n
end

wml.on_frame(function()
  if not solo then return end
  local top = wml.read_u32(0x827D578C)  -- 6 lobby / menus, 7..31 a match
  local in_match = top > 6 and top <= 31
  local want = 1
  if in_match and players_in_match() >= 2 then want = 0 end
  if wml.read_u8(ALLOW_SINGLE) ~= want then wml.write_u8(ALLOW_SINGLE, want) end
end)
