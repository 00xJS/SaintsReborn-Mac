-- Saints Reborn character (built in): the movement and personality rows of
-- the creator's BODY page as menus, like the hair list: the row shows the
-- choice ("Walk: Pimp swagger", "Voice: Hispanic Male 2"), A opens a list of
-- every choice for that row (row_lists.lua), the cursor previews each one on
-- your character, A keeps it, B goes back.
--
-- Two screens have a Body page: the character creation / Plastic Surgeon
-- screen (tabs PRESETS .. HAIR, BODY, SKIN; its Body builder 0x8231FCE0,
-- input 0x8231C3D0 - the one new games use in Saints Reborn) and the older
-- creator screen (builder 0x822FFCA8, input 0x822FDF40). Menu.SCREENS
-- describes where each keeps its page fields.
--
-- The rows are still the Body set's sliders underneath (saved with the
-- character, body_rows.lua); only how they're shown and chosen changes:
--   - on the Body page they're plain menu rows (Menu.add_rows, from the
--     builder hooks in main.lua); left / right still step through the styles;
--   - the list is a page of our own in the same menu: the game's page start
--     (0x8228B9D0, as the Body builders call it) and rows (0x8228BAB0), the
--     title written into the Body page's title text; while it's open the
--     screen's input is answered here: up / down move the game's own cursor
--     (0x8228CBA0 / 0x8228CC90, with scrolling and sound), A (action 55) and
--     B (56) as the HEAD tab uses them;
--   - leaving rebuilds the Body page through the screen's own tab builder,
--     the cursor back on the row.
local G = require("game")
local ROWS = require("body_rows")
local LISTS = require("row_lists")
local r16, r32, w16, w32 = wml.read_u16, wml.read_u32, wml.write_u16, wml.write_u32
local MASK = 0xFFFFFFFF

local A = {
  PRESSED = 0x8216EE80,   -- (action) -> pressed this frame (and taken)
  HELD = 0x8216ED48,      -- (action) -> pressed or repeating while held
  LIST = 0x8370DDC4,      -- the menu list being shown: +0 first row, +58 rows, +60 top row shown, +62 cursor
  LIST_START = 0x8228B9D0,
  ROW_ADD = 0x8228BAB0,   -- (text, widget or 0, cursor here, -1, 1, 0)
  UP = 0x8228CBA0, DOWN = 0x8228CC90,
  SOUND = 0x82287FC0, SOUND_ACCEPT = 0x827B05B8, SOUND_BACK = 0x827B05BC,
  SLIDER_SET = 0x823227E8,
  -- (0, 0, -, 1, f1 = -280): shows the list just built - swaps it in and lays
  -- it out, as the screens' tab builders (0x8231D828 / 0x822FEDB8) do last
  SHOW = 0x8228CAD8, SHOW_FROM = -280.0,
}
local ACT_A, ACT_B, ACT_UP, ACT_UP2, ACT_DOWN, ACT_DOWN2 = 55, 56, 41, 45, 42, 46
local VISIBLE = 8         -- rows the creator's panel shows at once

local Menu = { open = nil }
-- Page fields: tab (+8) of the Body page, its "inside a list" field that must
-- be 0, selected row, row count, rows {morph info, value}, row widgets (32
-- bytes, value at +16), the title object (points at the title text) and the
-- tab builder (page).
Menu.SCREENS = {
  surgeon = { tab = 4, sub = 20, row = 124, count = 288, slots = 128, widgets = 456,
    title_object = 0x8283A41C, title = 0x8304E080, rebuild = 0x8231D828 },
  creator = { tab = 3, sub = 16, row = 188, count = 2440, slots = 2120, widgets = 2444,
    title_object = 0x8283A414, title = 0x830331E0, rebuild = 0x822FEDB8 },
}


local function pressed(ctx, action) return (G.call(ctx, A.PRESSED, { action }) & 0xFF) ~= 0 end
local function held(ctx, action) return (G.call(ctx, A.HELD, { action }) & 0xFF) ~= 0 end
local function sound(ctx, which)
  local s = r32(which)
  if s ~= MASK then G.call(ctx, A.SOUND, { s, 1, 0 }) end
end
local function write_text(at, text, chars)
  local n = math.min(#text, chars - 1)
  for i = 1, n do w16(at + (i - 1) * 2, text:byte(i)) end
  w16(at + n * 2, 0)
end

-- The list's row texts (UTF-16): one block of 31 texts, made once and
-- rewritten each time a list opens (one list is open at a time).
local TEXT_BYTES = 96
local texts
local function list_texts(ctx, names)
  if not texts then
    local block = G.call(ctx, 0x8271B570, { 0, (ROWS.STEP + 1) * TEXT_BYTES, 0x3000, 4 })
    if not G.pointer_ok(block) then return nil end
    texts = {}
    for c = 0, ROWS.STEP do texts[c] = block + c * TEXT_BYTES end
  end
  for c = 0, ROWS.STEP do
    if names[c] then write_text(texts[c], names[c], TEXT_BYTES // 2) end
  end
  return texts
end

-- The menu's rows in order (circular list: node +4 = next).
local function rows_shown()
  local list = r32(A.LIST)
  if not G.pointer_ok(list) then return nil end
  local out, node = {}, r32(list)
  local n = r16(list + 58)
  for i = 0, math.min(n, 40) - 1 do
    if not G.pointer_ok(node) then return nil end
    out[i] = node
    node = r32(node + 4)
  end
  return out, list, n
end

-- Body page: the builder is run with the Body set cut to its three slider
-- rows (main.lua), then the movement rows are added here as plain menu rows
-- (no widget, like the HEAD tab's list), each with its page slot {morph
-- info, value} so the game's own up / down, left / right and Randomize still
-- see them; the page's row count includes them.
function Menu.add_rows(ctx, page, S)
  local p = G.player()
  for row = ROWS.FIRST_MOVE, ROWS.ROWS - 1 do
    local info = ROWS.info(row)
    if not info then return end
    G.call(ctx, A.ROW_ADD, { r32(info + 4), 0, 0, MASK, 1, 0 })
    w32(page + S.slots + 8 * row, info)
    wml.write_f32(page + S.slots + 4 + 8 * row, ROWS.value(p, row))
    wml.write_f32(page + S.widgets + 16 + 32 * row, ROWS.value(p, row))
  end
  w32(page + S.count, ROWS.ROWS)
end

-- The cursor on `row`, scrolled so it shows.
local function put_cursor(row)
  local list = r32(A.LIST)
  if not G.pointer_ok(list) then return end
  w16(list + 62, row)
  w16(list + 60, math.max(0, row - (VISIBLE - 1)))
end

function Menu.start(ctx, page, row, S)
  local list = LISTS.get(row)
  local info = ROWS.info(row)
  if not list or not info then return false end
  local p = G.player()
  local count = math.min(#list.names, ROWS.STEP)
  local current = math.min(ROWS.choice(p, row), count)
  local text = list_texts(ctx, list.names)
  if not text then return false end
  write_text(S.title, list.title:upper(), 64)
  G.call(ctx, A.LIST_START, { S.title_object, 1, 0, 1, 0 })
  local shown_list = r32(A.LIST)
  if G.pointer_ok(shown_list) then w32(shown_list + 392, 0) end
  for c = 0, count do
    G.call(ctx, A.ROW_ADD, { text[c], 0, c == current and 1 or 0, MASK, 1, 0 })
  end
  put_cursor(current)
  G.call(ctx, A.SHOW, { 0, 0, 0, 1 }, A.SHOW_FROM)
  put_cursor(current)
  Menu.open = { page = page, row = row, info = info, count = count, choice = current, screen = S }
  sound(ctx, A.SOUND_ACCEPT)
  return true
end

local function finish(ctx, keep)
  local o = Menu.open
  Menu.open = nil
  if keep then
    local p = G.player()
    if p ~= 0 then G.call(ctx, A.SLIDER_SET, { p, o.info }, o.choice / ROWS.STEP) end
  end
  -- the BODY page again, the cursor back on the row
  G.call(ctx, o.screen.rebuild, { o.page })
  w32(o.page + o.screen.row, o.row)
  put_cursor(o.row)
  sound(ctx, keep and A.SOUND_ACCEPT or A.SOUND_BACK)
end

-- The creator's input while our list is open (instead of the game's).
-- Returns true when it was ours.
function Menu.input(ctx, page, S)
  local o = Menu.open
  if not o then return false end
  if o.screen ~= S then return false end
  if o.page ~= page or r32(page + 8) ~= S.tab or r32(page + S.sub) ~= 0 then Menu.open = nil; return false end
  if pressed(ctx, ACT_B) then finish(ctx, false); return true end
  if pressed(ctx, ACT_A) then finish(ctx, true); return true end
  local list = r32(A.LIST)
  if held(ctx, ACT_UP) or held(ctx, ACT_UP2) then G.call(ctx, A.UP, { 1 })
  elseif held(ctx, ACT_DOWN) or held(ctx, ACT_DOWN2) then G.call(ctx, A.DOWN, { 1 }) end
  if G.pointer_ok(list) then
    local c = r16(list + 62)
    if c <= o.count then o.choice = c end
  end
  return true
end

-- A on a list row of the BODY page (movement, personality) opens its list.
function Menu.maybe_start(ctx, page, S)
  local row = r32(page + S.row)
  if row < ROWS.FIRST_MOVE or row >= ROWS.ROWS or row >= r32(page + S.count) then return false end
  if not pressed(ctx, ACT_A) then return false end
  return Menu.start(ctx, page, row, S)
end

-- What the list's cursor is on, for the preview: { row, choice } or nil.
function Menu.previewing()
  local o = Menu.open
  if o then return o.row, o.choice end
end

return Menu
