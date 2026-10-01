-- Multiplayer (built in, dist\core\Multiplayer):
-- 1. The main menu's XBOX LIVE item is renamed MULTIPLAYER (US_Strings.txt:
--    UTF-16 LE, one KEY=Text per line).
-- 2. Quick Match waits until a map's Optimal_Players_Min players are in
--    (4 or 8 in multiplayer_levels.xtbl): lowered to min_players, so a few
--    friends can play.
-- 3. Co-op allows a party of 1 in Player Match (MaxStandardParty in
--    multiplayer_mode.xtbl, "Co-op needs at most 1 player(s)"): raised to
--    coop_party.
local min_players = math.max(1, math.floor(wml.setting("min_players", 2)))
local coop_party = math.max(1, math.floor(wml.setting("coop_party", 2)))

local function utf16(s) return (s:gsub(".", "%0\0")) end
local key = utf16("MAINMENU_XBOX_LIVE=")
local old = key .. utf16("XBOX LIVE")
local new = key .. utf16("MULTIPLAYER")

for _, pack in ipairs({ "misc2.vpp_xbox2", "misc.vpp_xbox2" }) do
  local text = wml.packfile_read(pack, "US_Strings.txt")
  if text then
    local s, e = text:find(old, 1, true)
    if s then
      wml.packfile_write(pack, "US_Strings.txt", text:sub(1, s - 1) .. new .. text:sub(e + 1))
      wml.log("[Multiplayer] " .. pack .. ": XBOX LIVE menu item renamed to MULTIPLAYER")
    end
  end

  local levels = wml.packfile_read(pack, "multiplayer_levels.xtbl")
  if levels then
    local changed = 0
    levels = levels:gsub("<Optimal_Players_Min>(%d+)</Optimal_Players_Min>", function(n)
      if tonumber(n) > min_players then
        changed = changed + 1
        return "<Optimal_Players_Min>" .. min_players .. "</Optimal_Players_Min>"
      end
    end)
    if changed > 0 then
      wml.packfile_write(pack, "multiplayer_levels.xtbl", levels)
      wml.log("[Multiplayer] " .. pack .. ": Quick Match starts with " .. min_players .. " player(s) (" .. changed .. " maps)")
    end
  end

  local modes = wml.packfile_read(pack, "multiplayer_mode.xtbl")
  if modes then
    local at = modes:find("<Name>Co-op</Name>", 1, true)
    if at then
      local s, e, n = modes:find("<MaxStandardParty>(%d+)</MaxStandardParty>", at)
      if s and tonumber(n) < coop_party then
        modes = modes:sub(1, s - 1) .. "<MaxStandardParty>" .. coop_party .. "</MaxStandardParty>" .. modes:sub(e + 1)
        wml.packfile_write(pack, "multiplayer_mode.xtbl", modes)
        wml.log("[Multiplayer] " .. pack .. ": Co-op allows a party of " .. coop_party)
      end
    end
  end
end
