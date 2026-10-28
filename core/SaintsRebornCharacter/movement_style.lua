-- Saints Reborn character (built in): movement styles, chosen on the Body
-- page of character creation and the Plastic Surgeon.
--
-- From SR Tweak Menu 2.6.6's Movement page (itsMJQ, 00xJS; "SaintsReborn New
-- Base Assets" zip 08). How your character moves, in parts - stance, walk,
-- run, sprint, melee/throw run, combat, melee combat - each the game's own or
-- any animation set that has that move (movement.lua lists them, built by
-- build_movement.py, unchanged).
--
-- The choices are Body page sliders (patch.lua, rows 3 to 9; body_rows.lua):
-- 0 = the game's own, each slider step (1/30) the next style. The row's name
-- shows the style ("Walk: Pimp swagger"): each row's shown name (morph info
-- +4) points at a text of ours, rewritten when the choice changes. Being
-- sliders, the choices are saved with the character like the face.
--
-- While any part is chosen, the player's body instance (human+568, its set at
-- owner +4) is given the empty SRB_Player set (patch.lua), and every
-- animation lookup for SRB_Player (0x82226DA8: set, group, state) is answered
-- here from chains of sets - in each weapon group from the one in hand up to
-- Default, the chosen part's set first, then the player's own - so a gun out
-- keeps the game's aim, fire and reload unless the style has its own, and
-- nothing is ever missing. Heavy weapons keep the game's own sprint (and
-- walk/run for some styles).
--
-- In the creator, the character walks, runs or sprints on the spot while a
-- walking row is selected, to show the choice.
local G = require("game")
local ROWS = require("body_rows")
local PARTS = require("movement")
local r8, r16, r32, w16, w32 = wml.read_u8, wml.read_u16, wml.read_u32, wml.write_u16, wml.write_u32
local MASK = 0xFFFFFFFF

local M = {}
local m = { chain = {}, srt = 0, parts = {}, over = {}, dirty = true }
M.state = m

-- How many styles a movement row has (row 3 = PARTS[1]).
function M.choices(row)
  local part = PARTS[row - ROWS.FIRST_MOVE + 1]
  return part and math.min(#part[5], ROWS.STEP) or 0
end

-- ------------------------------------------------------------ names and numbers
local set_cache, state_cache, action_cache = {}, {}, {}
local function set_entry(ctx, name)
  local e = set_cache[name]
  if e == nil then
    e = G.call(ctx, G.ANIM_SET_FIND, { G.cstr(ctx, name, 0) })
    if not G.pointer_ok(e) then return 0 end   -- not remembered: asked again later
    set_cache[name] = e
  end
  return e
end
local function state_number(ctx, name)
  local n = state_cache[name]
  if n == nil then
    n = G.call(ctx, G.ANIM_STATE_INDEX, { G.cstr(ctx, name, 0) })
    if n ~= MASK then state_cache[name] = n end
  end
  return n
end
local function action_number(ctx, name)
  local n = action_cache[name]
  if n == nil then
    n = G.call(ctx, G.ACTION_INDEX, { G.cstr(ctx, name, 0) })
    action_cache[name] = n ~= MASK and n or false
  end
  return n or nil
end

-- ------------------------------------------------------------ the lookup
-- The animation for `state` in the group at `g`, from the first set of
-- `chain` (set indices) that has one, or nil.
local function find_in(g, state, chain)
  local n = r8(g + 4 + state)
  if n == 0 then return nil end
  local first = r16(g + 872 + state * 2)
  for _, idx in ipairs(chain) do
    for k = 0, n - 1 do
      if r32(G.ANIM_POOL_SET + (first + k) * 4) == idx then
        return r32(G.ANIM_POOL_ANIM + (first + k) * 4)
      end
    end
  end
  return nil
end
-- The held group, then its parents (group +0), as the game walks them; with
-- `armed_only`, the weapon groups only (not Default, group 0).
local function resolve(group, state, chain, armed_only)
  local g = G.ANIM_GROUPS + group * G.ANIM_GROUP_SIZE
  for _ = 1, 8 do
    if armed_only and g == G.ANIM_GROUPS then return nil end
    local anim = find_in(g, state, chain)
    if anim then return anim end
    g = r32(g)
    if g == 0 then return nil end
  end
  return nil
end

-- Heavy guns keep the game's own walk, run and sprint for styles that don't
-- carry them: no sprint style with a shotgun, RPG or AK; with an AK or a
-- shotgun the street, skater, mascot and variant runs fall back too; and the
-- same walks (and every walk variant) with those and the RPG.
local HEAVY = { pump_action_shotgun = "shotgun", twelve_gauge = "shotgun", spas12 = "shotgun",
  rpg_launcher = "rpg", ak47 = "ak", ak47l = "ak" }
local HEAVY_STYLES = { ["Street: Man"] = true, ["Street: Heavyset man"] = true, ["Street: Elderly"] = true,
  ["Street: Roller skater"] = true, ["Special: Mascot"] = true, ["Run variant 1"] = true }
local function heavy_weapon()
  if m.heavy_frame ~= M.frame then
    m.heavy_frame, m.heavy = M.frame, nil
    local p = G.player()
    local item = p ~= 0 and r32(p + G.P.EQUIPPED) or 0
    local info = G.pointer_ok(item) and r32(item + 8) or 0
    local namep = G.pointer_ok(info) and r32(info) or 0
    if G.pointer_ok(namep) then
      local chars = {}
      for i = 0, 31 do
        local c = r8(namep + i)
        if c < 32 or c > 126 then break end
        chars[#chars + 1] = string.char(c)
      end
      m.heavy = HEAVY[table.concat(chars):lower()]
    end
  end
  return m.heavy
end
local function heavy_blocks(e)
  local part = e.part
  if part ~= "walk" and part ~= "run" and part ~= "sprint" then return false end
  local kind = heavy_weapon()
  if not kind then return false end
  if part == "sprint" then return true end
  if part == "run" then return kind ~= "rpg" and HEAVY_STYLES[e.label] == true end
  return HEAVY_STYLES[e.label] == true or (e.label or ""):find("^Walk variant") ~= nil
end

-- The melee family of weapon groups (Melee, Melee 1h / 2h and what's under
-- them): Melee is the child of Default whose children include one with four
-- or more of its own.
local function melee_groups()
  local parent, kids = {}, {}
  for i = 0, 21 do
    local up = r32(G.ANIM_GROUPS + i * G.ANIM_GROUP_SIZE)
    local pi = (up ~= 0) and (up - G.ANIM_GROUPS) // G.ANIM_GROUP_SIZE or nil
    if pi and pi >= 0 and pi < 22 then parent[i] = pi; kids[pi] = (kids[pi] or 0) + 1 end
  end
  local family = {}
  for i = 1, 21 do
    if parent[i] == 0 then
      for c = 0, 21 do
        if parent[c] == i and (kids[c] or 0) >= 4 then family[i] = true end
      end
    end
  end
  for _ = 1, 4 do
    for i = 0, 21 do if parent[i] and family[parent[i]] then family[i] = true end end
  end
  return family
end

G.hook(G.ANIM_LOOKUP, function(ctx)
  local set = ctx:r(3) & MASK
  if set ~= m.srt or set == 0 then return ctx:call_original() end
  local group, state = ctx:r(4) & MASK, ctx:r(5) & MASK
  local armed_stand = group ~= 0 and not (m.melee and m.melee[group]) and state == m.stand
  local anim
  if group < 22 and state < 867 then
    -- empty-handed: the stance / walk / run / sprint choice (or combat);
    -- a melee weapon out: Melee & Throw Run and Melee Combat first - the
    --   kit's own move for that weapon, else the same move it has for
    --   another melee weapon;
    -- any weapon out, nothing more specific chosen: the stance / walk / run /
    --   sprint choice's own move for that weapon, else its empty-handed one.
    local o = m.over[state]
    if o then
      local melee = m.melee and m.melee[group]
      if group == 0 then
        local e = o.default or o.any
        if e then anim = resolve(0, e.alt or state, e.chain or m.chain) end
      else
        local e = melee and o.melee
        if e then
          anim = resolve(group, e.alt or state, e.chain or m.chain)
          if not anim and e.chain then
            for g in pairs(m.melee) do
              anim = find_in(G.ANIM_GROUPS + g * G.ANIM_GROUP_SIZE, e.alt or state, e.chain)
              if anim then break end
            end
          end
        end
        if not anim and o.any then anim = resolve(group, o.any.alt or state, o.any.chain or m.chain) end
        if not anim and o.default and not heavy_blocks(o.default) then
          local d = o.default
          anim = resolve(group, d.alt or state, d.chain or m.chain, true)
            or (not armed_stand and resolve(0, d.alt or state, d.chain or m.chain))
        end
      end
    end
    anim = anim or resolve(group, state, m.chain, armed_stand)
    if not anim then anim = resolve(group, state, m.chain) end
  end
  ctx:set_r(3, anim or MASK)
end)

-- Set indices of a set and its parents (entry +8).
local function chain_into(chain, seen, entry)
  for _ = 1, 8 do
    if not G.pointer_ok(entry) then return end
    local idx = (entry - G.ANIM_SETS) // 12
    if not seen[idx] then seen[idx] = true; chain[#chain + 1] = idx end
    entry = r32(entry + 8)
  end
end

-- ------------------------------------------------------------ applying
-- Keeps the player's body on SRB_Player while a part is chosen (a respawn or
-- model change makes a new body), and gives the player's own set back when
-- every part is the game's own again.
local function update(ctx, p, own)
  local inst = r32(p + G.ANIM_OWNER)
  if not G.pointer_ok(inst) then return end
  local def = r32(p + G.HUMAN_DEF)
  local any_part = false
  for _, v in pairs(m.parts) do if v > 0 then any_part = true end end
  local want = own and any_part
  local srt = want and set_entry(ctx, "SRB_Player") or 0
  if want and srt == 0 then
    want = false
    if not m.no_set_logged then m.no_set_logged = true; wml.log("Movement: no SRB_Player animation set (patch.lua)") end
  end
  if m.dirty or def ~= m.def then
    m.def, m.dirty, m.refresh = def, false, true
    local base = G.pointer_ok(def) and G.call(ctx, G.ANIM_SET_FIND, { r32(def + G.D.ANIM_SET) }) or 0
    if not G.pointer_ok(base) or base == set_entry(ctx, "SRB_Player") then base = set_entry(ctx, "GML1") end
    m.base = base
    local chain = {}
    chain_into(chain, {}, base)
    m.chain = chain
    m.stand = state_number(ctx, "stand")
    m.over, m.melee = {}, melee_groups()
    for i, part in ipairs(PARTS) do
      local choice = m.parts[part[1]] or 0
      local opt = choice > 0 and part[5][choice]
      if opt then
        local ch
        if opt[2] then
          local e = set_entry(ctx, opt[2])
          if G.pointer_ok(e) then ch = {}; chain_into(ch, {}, e) end
        end
        local alt = opt[3] and state_number(ctx, opt[3]) or MASK
        if ch or alt ~= MASK then
          for _, sname in ipairs(part[3]) do
            local n = MASK
            if part[1] == "combat" or part[1] == "melee_combat" then
              local a = action_number(ctx, sname)
              if a then n = 227 + a end
            else
              n = state_number(ctx, sname)
            end
            if n ~= MASK then
              m.over[n] = m.over[n] or {}
              m.over[n][part[4]] = { chain = ch, alt = alt ~= MASK and alt or nil, part = part[1], label = opt[1] }
            end
          end
        end
      end
    end
    if srt ~= 0 then m.srt = srt end
  end
  local cur = r32(inst + G.OWNER_SET)
  if not want and (m.srt == 0 or cur ~= m.srt) then return end
  local target = want and srt or m.base
  if not G.pointer_ok(target) then return end
  if cur ~= target or m.refresh then
    m.refresh = false
    w32(inst + G.OWNER_SET, target)
    -- The animation cached for the state playing now, looked up again.
    local st = r32(p + G.HUMAN_STATE_NOW)
    if st ~= MASK and st < 867 then
      w32(p + G.HUMAN_ANIM_NOW, (G.call(ctx, G.ANIM_LOOKUP, { target, r32(inst + G.OWNER_GROUP), st })))
    end
  end
end

-- ------------------------------------------------------------ row names
-- "Walk: Pimp swagger" - the part's name and the style without its group
-- ("Gang: ", "Street: " ...), "Default" for the game's own.
local SHORT = { stance = "Stance", walk = "Walk", run = "Run", sprint = "Sprint",
  melee_run = "Melee Run", combat = "Combat", melee_combat = "Melee Combat" }
local LABEL_BYTES = 128   -- 63 UTF-16 characters and the end
local function label_text(part, choice)
  local opt = choice > 0 and part[5][choice]
  local style = opt and (opt[1]:gsub("^%a+: ", "")) or "Default"
  return (SHORT[part[1]] or part[2]) .. ": " .. style
end
local function write_label(at, text)
  local n = math.min(#text, LABEL_BYTES // 2 - 1)
  for i = 1, n do w16(at + (i - 1) * 2, text:byte(i)) end
  w16(at + n * 2, 0)
end
local function update_labels(ctx)
  if not m.labels then
    local block = G.call(ctx, 0x8271B570, { 0, LABEL_BYTES * #PARTS, 0x3000, 4 })
    if not G.pointer_ok(block) then return end
    m.labels, m.label_text = block, {}
  end
  for i, part in ipairs(PARTS) do
    local info = ROWS.info(ROWS.FIRST_MOVE + i - 1)
    if info then
      local at = m.labels + (i - 1) * LABEL_BYTES
      local text = label_text(part, m.parts[part[1]] or 0)
      if m.label_text[i] ~= text then write_label(at, text); m.label_text[i] = text end
      if r32(info + 4) ~= at then w32(info + 4, at) end
    end
  end
end

-- ------------------------------------------------------------ preview
-- In the creator, a walking row selected: the character does that move on the
-- spot; moving off the row puts back what it was doing.
local PREVIEW = { stance = "stand", walk = "walk", run = "run", sprint = "sprint", melee_run = "run" }
local function preview(ctx, p, shown, in_creator)
  local part = shown and in_creator and M.frame - shown.at <= 2 and PARTS[shown.row - ROWS.FIRST_MOVE + 1]
  local want = part and PREVIEW[part[1]]
  local P = m.preview
  if P and (not want or P.player ~= p or P.handle ~= r32(p + 68)) then
    if P.player == p and P.handle == r32(p + 68) then
      w32(p + G.ANIM_STATE, P.state)
      G.call(ctx, G.ANIM_APPLY, { p, MASK, MASK, 0 })
    end
    m.preview, P = nil, nil
  end
  if not want or p == 0 then return end
  if not P then
    P = { player = p, handle = r32(p + 68), state = r32(p + G.ANIM_STATE) }
    m.preview = P
  end
  local n = state_number(ctx, want)
  if n ~= MASK and (r32(p + G.ANIM_STATE) ~= n or P.part ~= part[1] or P.choice ~= m.parts[part[1]]) then
    P.part, P.choice = part[1], m.parts[part[1]]
    w32(p + G.ANIM_STATE, n)
    G.call(ctx, G.ANIM_APPLY, { p, MASK, MASK, 0 })
  end
end

-- ------------------------------------------------------------ every update
-- own: the player is on their own character model.
function M.tick(ctx, p, own, shown, in_creator, frame)
  M.frame = frame
  if ROWS.count() < ROWS.FIRST_MOVE + #PARTS then return end
  if p == 0 then return update_labels(ctx) end
  local changed = false
  for i, part in ipairs(PARTS) do
    local row = ROWS.FIRST_MOVE + i - 1
    -- (the style under a list's cursor, while it's open: shown, not kept)
    local c = (M.override_row == row and M.override_choice) or ROWS.choice(p, row)
    c = math.min(c, M.choices(row))
    if m.parts[part[1]] ~= c then m.parts[part[1]] = c; changed = true end
  end
  if changed then
    m.dirty = true
    if wml.setting("debug", false) then
      local t = {}
      for _, part in ipairs(PARTS) do t[#t + 1] = part[1] .. "=" .. m.parts[part[1]] end
      wml.log("Movement: " .. table.concat(t, " "))
    end
  end
  update_labels(ctx)
  update(ctx, p, own)
  preview(ctx, p, shown, in_creator)
end

return M
