-- Saints Reborn character (built in): what each list row of the Body page
-- offers. movement_menu.lua opens these lists (A on the row), main.lua stops
-- a row's slider at its last choice.
--
-- A list is { title = "WALK STYLE", names = { [0] = "Default", "Gang: Saints", ... } }:
-- choice 0 is the game's own (or none / off), choice n is n slider steps
-- (1/30, body_rows.lua), so a list holds at most 30 choices after 0.
--   rows 3-9  (movement): movement.lua's parts, as movement_style.lua uses them
--   rows 10+  (personality): registered by personality.lua (L.providers[row])
local ROWS = require("body_rows")
local PARTS = require("movement")

local L = { providers = {} }

function L.get(row)
  if row >= ROWS.FIRST_MOVE and row < ROWS.FIRST_PERSONALITY then
    local part = PARTS[row - ROWS.FIRST_MOVE + 1]
    if not part then return nil end
    local names = { [0] = "Default" }
    for c = 1, math.min(#part[5], ROWS.STEP) do names[c] = part[5][c][1] end
    return { title = part[2], names = names }
  end
  local provider = L.providers[row]
  return provider and provider() or nil
end

-- How many choices a row has after 0.
function L.choices(row)
  local list = L.get(row)
  return list and math.min(#list.names, ROWS.STEP) or 0
end

return L
