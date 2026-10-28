-- The Body set's sliders (player_creation.xtbl, patch.lua), as the game
-- loaded them: morph sets are 44 bytes each at 0x83C593E8 (Body is set 0:
-- +4 how many sliders, +8 the first), sliders (morph infos) 36 bytes each
-- (+4 shown name, +8 set, +24 index in a record's slider array).
--
-- Rows: 0 Muscle, 1 Fat, 2 Body Type, 3 Stance, 4 Walk, 5 Run, 6 Sprint,
-- 7 Melee Run, 8 Combat, 9 Melee Combat, 10 Voice, 11 Expression, 12 Taunts.
local G = require("game")
local r32, rf = wml.read_u32, wml.read_f32

local R = {}
R.SETS = 0x83C593E8
R.INFO_COUNT = 0x83117BB8
R.BODY_TYPE, R.FIRST_MOVE, R.FIRST_PERSONALITY, R.ROWS = 2, 3, 10, 13
R.STEP = 30   -- a slider step is 1/30 (0x827AA61C)

-- How many rows the Body set has (3 with Body Type only, 10 with movement,
-- 13 with personality).
function R.count()
  local n = r32(R.SETS + 4)
  return (n >= 2 and n <= 40) and n or 0
end
-- The morph info of a row and its index in the record's slider array.
function R.info(row)
  if row >= R.count() then return nil end
  local infos = r32(R.SETS + 8)
  if not G.pointer_ok(infos) then return nil end
  local info = infos + row * 36
  if r32(info + 8) ~= 0 then return nil end
  local index = r32(info + 24)
  if index >= 128 or index >= r32(R.INFO_COUNT) then return nil end
  return info, index
end
-- The row of a morph info, if it's one of ours (row 2 on).
function R.row_of(info)
  local infos = r32(R.SETS + 8)
  if not G.pointer_ok(infos) or info < infos then return nil end
  local d = info - infos
  if d % 36 ~= 0 then return nil end
  local row = d // 36
  if row >= R.BODY_TYPE and row < R.count() then return row end
end
-- A row's value on the player, 0..1: the record's slider entry (12 bytes:
-- ?, wanted +4, baked +8). The wanted value follows the slider at once.
function R.value(p, row)
  local _, index = R.info(row)
  if not index or p == 0 then return 0 end
  local record = r32(p + G.CUSTOM)
  if not G.pointer_ok(record) then return 0 end
  local sliders = r32(record + 4)
  if not G.pointer_ok(sliders) then return 0 end
  local v = rf(sliders + index * 12 + 4)
  if v ~= v then return 0 end
  return math.max(0, math.min(1, v))
end
-- A movement row's choice: 0 = the game's own, n = n slider steps.
function R.choice(p, row)
  return math.floor(R.value(p, row) * R.STEP + 0.5)
end
return R
