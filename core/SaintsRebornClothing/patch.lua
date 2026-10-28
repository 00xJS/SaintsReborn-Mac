-- Saints Reborn clothing (built in): cut and multiplayer-only clothing sold in
-- the single player stores. From SR Tweak Menu 2.6.6 (00xJS, itsMJQ), its
-- clothing part, which uses:
--   - "SR1 - Playa Choice" by CommyGT (saintsrowmods.com), clothing only:
--     necklace fixes (Sleeveless Henley, Hensley, Roundneck Pullover),
--     cropping and untucked undershirts, the Light Silver Braided Belt, the
--     Cross Pendant and the multiplayer chains, and every unlockable in the
--     stores. Prices and respect stay the game's own.
--   - "Timberland Boots" for Saints Row 1 (saintsrowmods.com).
--
-- What it does:
--   - Parted Afro: SR1's own cut hair (htop_partedafro, player_cust variant
--     36, mesh 119; hat version mesh 132) in the place of Frohawk, Striped at
--     Do It Up.
--   - Cropped Football Jersey in the Azzure Shirt's place (the name too).
--   - Multiplayer chains and the Cross Pendant in the jewelry stores.
--   - Timberland boots: in place of the Hiking Boots (boots = replace, the
--     default), or as their own item, Ultor Unlimited Work Boots, in the game's
--     unused cshoe_timberland slot (boots = duplicate).
-- Everything is written into the player's own copies of misc.vpp_xbox2,
-- misc2.vpp_xbox2 (tables, names) and cmesh_stream.vpp_xbox2 (boots).

local MISC = { "misc.vpp_xbox2", "misc2.vpp_xbox2" }

local function file(name)
  local f = io.open(wml.mod_folder .. "/" .. name, "rb")
  local d = f and f:read("a")
  if f then f:close() end
  return d
end
local function utf16(s) return (s:gsub(".", "%0\0")) end
local function plain(s) return (s:gsub("%p", "%%%0")) end

-- Names shown in the game (US_Strings.txt: UTF-16, KEY=Text per line, then the
-- number of entries). Only existing keys get new text, so the count stays right.
local function rename(names)
  for _, pack in ipairs(MISC) do
    local text = wml.packfile_read(pack, "US_Strings.txt")
    if text then
      local changed = 0
      for key, value in pairs(names) do
        local s = text:find(utf16("\r\n" .. key .. "="), 1, true)
        local e = s and text:find(utf16("\r\n"), s + 4, true)
        if e then
          text = text:sub(1, s - 1) .. utf16("\r\n" .. key .. "=" .. value) .. text:sub(e)
          changed = changed + 1
        end
      end
      if changed > 0 then
        wml.packfile_write(pack, "US_Strings.txt", text)
        wml.log(pack .. ": " .. changed .. " clothing names changed")
      end
    end
  end
end

-- ------------------------------------------------------------ Playa Choice
do
  local items, stores = file("playa_choice/items.xml"), file("playa_choice/customization_stores.xtbl")
  if wml.setting("playa_choice", true) and items and stores then
    for _, pack in ipairs(MISC) do
      local xml = wml.packfile_read(pack, "customization_items.xtbl")
      if xml and not xml:find("<!-- playa choice -->", 1, true) then
        local n = 0
        for block in items:gmatch("<Customization_Item>.-</Customization_Item>") do
          local item = block:match("<Name>([^<]*)</Name>")
          local s = xml:find("<Customization_Item>%s*<Name>" .. plain(item) .. "</Name>")
          local e = s and xml:find("</Customization_Item>", s, true)
          if e then
            xml = xml:sub(1, s - 1) .. block .. xml:sub(e + #"</Customization_Item>")
            n = n + 1
          end
        end
        -- The Parted Afro (cut, never sold) takes Frohawk, Striped's place: its
        -- meshes and name on that item. The pair the game wears by must be one
        -- player_cust.xtbl lists - (variant 36, mesh 119) for the afro and
        -- (36, 132) for its hat version - and the Frohawk's variant is 36.
        local fs = xml:find("<Customization_Item>%s*<Name>curley mohawk, striped sides</Name>")
        local fe = fs and xml:find("</Customization_Item>", fs, true)
        if fe then
          local row = xml:sub(fs, fe - 1)
          row = row:gsub("CUST_ITEM_CURLY_MOHAWK_STRIPED_SIDES", "CUST_ITEM_PARTED_AFRO", 1)
          row = row:gsub("hTop_fuzzyMowhawkC%.cmeshx", "htop_partedafro.cmeshx", 1)
                   :gsub("<MeshID>123</MeshID>", "<MeshID>119</MeshID>", 1)
          row = row:gsub("hTop_flatmoC%.cmeshx", "hTop_flatAfro.cmeshx", 1)
                   :gsub("<MeshID>142</MeshID>", "<MeshID>132</MeshID>", 1)
          xml = xml:sub(1, fs - 1) .. row .. xml:sub(fe)
          n = n + 1
        end
        wml.packfile_write(pack, "customization_items.xtbl", (xml:gsub("</Table>", "<!-- playa choice -->\n</Table>", 1)))
        wml.packfile_write(pack, "customization_stores.xtbl", stores)
        wml.log(pack .. ": clothing - " .. n .. " items updated (Parted Afro, chains, crops), stores")
      end
    end
    rename({ CUST_ITEM_SHIRT_AZZURE = "Cropped Football Jersey" })
  end
end

-- ------------------------------------------------------------ Timberland boots
do
  local mode = tostring(wml.setting("boots", "replace")):lower()
  local STREAM = "cmesh_stream.vpp_xbox2"
  local parts = { { ".cmesh_xbox2" }, { ".peg_xbox2" }, { "_pc.morph_xbox2" } }
  for _, p in ipairs(parts) do p[2] = file("work_boots/cshoe_timberland" .. p[1]) end
  local ready = parts[1][2] and parts[2][2] and parts[3][2]
  if mode == "off" then
    -- nothing
  elseif not ready then
    wml.log("Boots: files missing in work_boots/")
  elseif mode == "replace" then
    for _, p in ipairs(parts) do wml.packfile_write(STREAM, "cshoe_hikingboots" .. p[1], p[2]) end
    wml.log("Boots: the Hiking Boots are the Timberland boots")
  else
    local ok, err = pcall(function()
      for _, p in ipairs(parts) do wml.packfile_write(STREAM, "cshoe_timberland" .. p[1], p[2]) end
    end)
    if not ok then
      wml.log("Boots: the Timberland slot couldn't be written (" .. tostring(err) .. ")")
    else
      for _, pack in ipairs(MISC) do
        local xml = wml.packfile_read(pack, "customization_items.xtbl")
        if xml and not xml:find("<Name>ultor work boots</Name>", 1, true) then
          local s = xml:find("<Customization_Item>%s*<Name>hiking boots</Name>")
          local e = s and xml:find("</Customization_Item>", s, true)
          local ending = xml:find("</Table>", 1, true)
          if e and ending then
            -- ids the game's items leave unused, inside their range
            local function free(tag)
              local used, top = {}, 0
              for v in xml:gmatch("<" .. tag .. ">%s*(%d+)%s*</" .. tag .. ">") do
                local n = tonumber(v)
                used[n] = true
                top = math.max(top, n)
              end
              for v = top - 1, 1, -1 do if not used[v] then return tostring(v) end end
            end
            local item, mesh, variant = free("ID"), free("MeshID"), free("VariantID")
            if item and mesh and variant then
              local row = xml:sub(s, e + #"</Customization_Item>" - 1)
              row = row:gsub("<Name>hiking boots</Name>", "<Name>ultor work boots</Name>", 1)
                :gsub("CUST_ITEM_HIKING_BOOTS", "CUST_ITEM_ULTOR_WORK_BOOTS", 1)
                :gsub("cShoe_hikingBoots%.cmeshx", "cShoe_timberland.cmeshx")
                :gsub("<MeshID>%d+</MeshID>", "<MeshID>" .. mesh .. "</MeshID>", 1)
                :gsub("<ID>%d+</ID>", "<ID>" .. item .. "</ID>")
              local vs, ve = row:find("<Variants>.-</Variants>")
              row = row:sub(1, vs - 1) .. "<Variants>\n\t\t\t<Variant>\n\t\t\t\t<Name>CUST_VARIANT_DEFAULT</Name>\n" ..
                "\t\t\t\t<Variant_Name>default</Variant_Name>\n\t\t\t\t<VariantID>" .. variant .. "</VariantID>\n" ..
                "\t\t\t\t<Price>250</Price>\n\t\t\t\t<Respect_Bonus>30</Respect_Bonus>\n\t\t\t\t<MPPrice>0</MPPrice>\n" ..
                "\t\t\t\t</Variant>\n\t\t\t</Variants>" .. row:sub(ve + 1)
              wml.packfile_write(pack, "customization_items.xtbl", xml:sub(1, ending - 1) .. "\t" .. row .. "\n" .. xml:sub(ending))
              wml.log(pack .. ": Ultor Unlimited Work Boots added (item " .. item .. ")")
            end
          end
        end
      end
    end
  end
end
