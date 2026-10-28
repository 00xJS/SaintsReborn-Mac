-- Saints Reborn character (built in): Body Type, movement styles and personality in the
-- game's own character creation, Plastic Surgeon and multiplayer export.
--
-- The creator's slider pages are data: player_creation.xtbl lists 11 morph
-- sets (Body, Brow, Ears, Eyes, Nose, Cheeks, Mouth, Jaws, Face_scale, Hair,
-- Skin - the names are a fixed list in the game) and the sliders in each. The
-- BODY tab (sub_822FFCA8) and the surgeon's Body page (sub_8231FCE0) add one
-- row for every slider of the Body set, so sliders added there are drawn,
-- changed, saved with the character and exported to multiplayer by the game
-- itself. Their morph keys name no target in any mesh, so the game's own
-- morphing ignores them (as it does the Skin sliders' "none" keys); main.lua
-- and movement_style.lua act on their values.
--
-- Body set rows after Muscle and Fat:
--   Body Type        0 (left, the default) = male .. 1 = fully female
--   Stance, Walk, Run, Sprint, Melee Run, Combat, Melee Combat
--                    a movement style each: 0 = the game's own, then one
--                    style per slider step (1/30); the row's name shows it
--   Voice, Expression, Taunts
--                    personality (personality.lua), the same way: 0 = the
--                    game's own / none / off, then one choice per step
--
-- Sliders are numbered in file order (the global index at morph info +24) and
-- saves keep (index, value) pairs, so the new rows move Hair and Skin up:
-- main.lua shifts them when an older save is loaded.
--
-- GML_CUST.xtbl also gets an empty animation set, SRB_Player (parent GML1),
-- which only the player is given while a movement style is chosen
-- (movement_style.lua answers its lookups).

local MISC = { "misc.vpp_xbox2", "misc2.vpp_xbox2" }

-- { morph info name, string key, shown name } in row order. main.lua and
-- movement_style.lua find them by these positions (Body set row 2 on).
local SLIDERS = {
  { "body type",          "PLAYER_MORPH_BODY_TYPE_TEXT",    "Body Type" },
  { "move stance",        "PLAYER_MORPH_MOVE_STANCE_TEXT",  "Stance" },
  { "move walk",          "PLAYER_MORPH_MOVE_WALK_TEXT",    "Walk" },
  { "move run",           "PLAYER_MORPH_MOVE_RUN_TEXT",     "Run" },
  { "move sprint",        "PLAYER_MORPH_MOVE_SPRINT_TEXT",  "Sprint" },
  { "move melee run",     "PLAYER_MORPH_MOVE_MELEE_RUN_TEXT", "Melee Run" },
  { "move combat",        "PLAYER_MORPH_MOVE_COMBAT_TEXT",  "Combat" },
  { "move melee combat",  "PLAYER_MORPH_MOVE_MELEE_COMBAT_TEXT", "Melee Combat" },
  { "personality voice",  "PLAYER_MORPH_VOICE_TEXT",        "Voice" },
  { "personality face",   "PLAYER_MORPH_FACE_TEXT",         "Expression" },
  { "personality taunts", "PLAYER_MORPH_TAUNTS_TEXT",       "Taunts" },
}

local function slider(name, key)
  return table.concat({
    "\t\t\t<Morph_Info>",
    "\t\t\t\t<Name>" .. name .. "</Name>",
    "\t\t\t\t<DisplayName>" .. key .. "</DisplayName>",
    "\t\t\t\t<Initial_Value>0</Initial_Value>",
    "\t\t\t\t<Morph_Keys>",
    "\t\t\t\t\t<Morph_Key>",
    "\t\t\t\t\t\t<Name>" .. name .. "</Name>",
    "\t\t\t\t\t\t<Full_Value>1</Full_Value>",
    "\t\t\t\t\t\t<Nill_Value>0</Nill_Value>",
    "\t\t\t\t\t\t<Full_Shader_Value>",
    "\t\t\t\t\t\t\t<X>0.0</X>",
    "\t\t\t\t\t\t\t<Y>0.0</Y>",
    "\t\t\t\t\t\t\t<Z>0.0</Z>",
    "\t\t\t\t\t\t\t</Full_Shader_Value>",
    "\t\t\t\t\t\t<Nill_Shader_Value>",
    "\t\t\t\t\t\t\t<X>0.0</X>",
    "\t\t\t\t\t\t\t<Y>0.0</Y>",
    "\t\t\t\t\t\t\t<Z>0.0</Z>",
    "\t\t\t\t\t\t\t</Nill_Shader_Value>",
    "\t\t\t\t\t\t</Morph_Key>",
    "\t\t\t\t\t</Morph_Keys>",
    "\t\t\t\t<Base_Value>0</Base_Value>",
    "\t\t\t\t<Type>basic</Type>",
    "\t\t\t\t</Morph_Info>",
    "",
  }, "\r\n")
end

-- ------------------------------------------------------------ the sliders
for _, pack in ipairs(MISC) do
  local xml = wml.packfile_read(pack, "player_creation.xtbl")
  if xml then
    local set = xml:find("<Morph_Set>%s*<Name>Body</Name>")
    local close = set and xml:find("</Morph_Infos>", set, true)
    local next_set = set and xml:find("<Morph_Set>", set + 1, true)
    if close and (not next_set or close < next_set) then
      -- Each slider missing from the Body set goes at the end of its list,
      -- in order (a copy patched before the movement rows gets just those).
      local added = {}
      local text = ""
      for _, s in ipairs(SLIDERS) do
        if not xml:find("<Name>" .. s[1] .. "</Name>", 1, true) then
          text = text .. slider(s[1], s[2])
          added[#added + 1] = s[3]
        end
      end
      if #added > 0 then
        -- (the closing tag sits on its own line after three tabs)
        local line = xml:sub(1, close - 1):match(".*()\n") or close
        xml = xml:sub(1, line) .. text .. xml:sub(line + 1)
        wml.packfile_write(pack, "player_creation.xtbl", xml)
        wml.log(pack .. ": Body page sliders added: " .. table.concat(added, ", "))
      end
    else
      wml.log(pack .. ": player_creation.xtbl has no Body set - Body Type and movement not added")
    end
  end
end

-- ------------------------------------------------------------ their names
-- US_Strings.txt: UTF-16, "KEY=Text" lines, then the number of entries on the
-- last line (right-aligned). New keys go before that line and the count goes
-- up by as many.
local function utf16(s) return (s:gsub(".", "%0\0")) end
local function ascii(s) return (s:gsub("(.)%z", "%1")) end
for _, pack in ipairs(MISC) do
  local text = wml.packfile_read(pack, "US_Strings.txt")
  if text then
    local lines, n = "", 0
    for _, s in ipairs(SLIDERS) do
      if not text:find(utf16("\r\n" .. s[2] .. "="), 1, true) then
        lines = lines .. "\r\n" .. s[2] .. "=" .. s[3]
        n = n + 1
      end
    end
    if n > 0 then
      local nl = utf16("\r\n")
      -- the line break before the count line (the text ends with one too)
      local tail = text:sub(-#nl) == nl and #text - #nl or #text
      local last, at = nil, 1
      while true do
        local s = text:find(nl, at, true)
        if not s or s > tail then break end
        if (s % 2) == 1 and s <= tail - #nl then last = s end
        at = s + 2
      end
      local count_line = last and ascii(text:sub(last + #nl, tail)) or ""
      local count = tonumber(count_line:match("^%s*(%d+)%s*$"))
      if count then
        text = text:sub(1, last - 1) .. utf16(lines) ..
          utf16("\r\n" .. string.format("%" .. #count_line .. "d", count + n)) .. nl
        wml.packfile_write(pack, "US_Strings.txt", text)
        wml.log(pack .. ": " .. n .. " Body page names added to the strings (" .. (count + n) .. " entries)")
      else
        wml.log(pack .. ": US_Strings.txt doesn't end with its entry count - the new Body rows have no names")
      end
    end
  end
end

-- ------------------------------------------------------------ SRB_Player
local PLAYER_SET = table.concat({
  "\t<Skeleton_Set>",
  "\t\t<Parent_Skeleton>GML1</Parent_Skeleton>",
  "\t\t<Name>SRB_Player</Name>",
  "\t\t<Groups>",
  "\t\t\t<Group>",
  "\t\t\t\t<group>Default</group>",
  "\t\t\t\t<States></States>",
  "\t\t\t\t<Actions></Actions>",
  "\t\t\t\t</Group>",
  "\t\t\t</Groups>",
  "\t\t<_Editor>",
  "\t\t\t<Category>Entries</Category>",
  "\t\t\t</_Editor>",
  "\t\t</Skeleton_Set>",
  "",
}, "\r\n")
for _, pack in ipairs(MISC) do
  local xml = wml.packfile_read(pack, "GML_CUST.xtbl")
  if xml and not xml:find("<Name>SRB_Player</Name>", 1, true) then
    local at = xml:find("</Table>", 1, true)
    if at then
      local line = at
      while line > 1 and xml:sub(line - 1, line - 1) ~= "\n" do line = line - 1 end
      wml.packfile_write(pack, "GML_CUST.xtbl", xml:sub(1, line - 1) .. PLAYER_SET .. xml:sub(line))
      wml.log(pack .. ": GML_CUST.xtbl - SRB_Player animation set added (movement styles)")
    end
  end
end
