-- SR Tweak Menu - breast physics for the player's female body (Body Type past 25 % female), as Saints Row 1
-- moves Lin's and Samantha's: their breasts are skinned to L-Lapel / R-Lapel and the game's "Breasts"
-- animation simulation (anim_simulation.xtbl: mass 1, drag 0.45, force Y -2) swings those bones. The player
-- shares that skeleton with every NPC, so nothing on the rig or the simulation table changes: each worn mesh
-- that covers the chest has 8 bounce targets in its Body Type bank (fit/build_jiggle.py: L/R up, down,
-- forward, back, 3 cm, weighted like Lin's Lapel skin) and this spring picks their weights each update.
-- Only the local player, only while Body Type is at least 25 % female.

local G = require("game")
local P = { clock = os.clock, MOVE = 0.03, ON = 0.25, K = 260, DRAG = 0.45 * 12 }
P.side = { L = { y = 0, z = 0, vy = 0, vz = 0 }, R = { y = 0, z = 0, vy = 0, vz = 0 } }

-- amount 0..1 female (Body Type: -50 is full female)
function P.amount(value) return math.max(0, -(value or 0) / 50) end
function P.active(value) return P.amount(value) >= P.ON end
function P.reset() for _, s in pairs(P.side) do s.y, s.z, s.vy, s.vz = 0, 0, 0, 0 end end

-- Every update: the chest's motion from the body's (position, velocity changes, a step bob on foot).
function P.update(p, value, on_foot)
  local now = P.clock()
  if p == 0 or not P.active(value) then P.last = nil; P.reset() return end
  local x, y, z = G.vec(p + G.P.POS)
  local L = P.last
  local dt = L and math.min(0.05, math.max(1e-3, now - L.t)) or 0
  local N = { t = now, x = x, y = y, z = z, phase = L and L.phase or 0,
    vx = L and (x - L.x) / dt or 0, vy = L and (y - L.y) / dt or 0, vz = L and (z - L.z) / dt or 0 }
  P.last = N
  if not L then return end
  -- teleports and respawns are not motion
  if (x - L.x) ^ 2 + (y - L.y) ^ 2 + (z - L.z) ^ 2 > 4 then P.reset() return end
  local ay = (N.vy - L.vy) / dt
  local sp, spl = math.sqrt(N.vx ^ 2 + N.vz ^ 2), math.sqrt(L.vx ^ 2 + L.vz ^ 2)
  local af = (sp - spl) / dt
  -- running: each step lifts the chest (about 2.6 steps a second at a sprint)
  if on_foot and sp > 1.2 then
    N.phase = L.phase + dt * math.pi * 2 * (1.1 + 0.25 * sp)
    ay = ay - math.cos(N.phase) * (0.035 * (math.pi * 2.6) ^ 2) * math.min(1, sp / 5)
  end
  ay, af = math.max(-60, math.min(60, ay)), math.max(-40, math.min(40, af))
  local scale = (0.4 + 0.6 * (P.amount(value) - P.ON) / (1 - P.ON)) * 0.75   -- (2.5.14: a tad softer)
  for _, s in pairs(P.side) do
    for _ = 1, 4 do                         -- (spring to the Lapel anchor; the chest's motion pushes)
      local h = dt / 4
      s.vy = s.vy + (-P.K * s.y - P.DRAG * s.vy - ay) * h; s.y = s.y + s.vy * h
      s.vz = s.vz + (-P.K * s.z - P.DRAG * s.vz - af) * h; s.z = s.z + s.vz * h
    end
    s.y = math.max(-P.MOVE, math.min(P.MOVE, s.y)); s.z = math.max(-P.MOVE, math.min(P.MOVE, s.z))
    s.scale = math.min(1, scale)
  end
end

-- Commands for one mesh: its bank id and the first of its 8 bounce targets.
function P.commands(id, first, value)
  if not first or not P.active(value) then return nil end
  local out = {}
  for i, k in ipairs({ "L", "R" }) do
    local s = P.side[k]; local base = first + (i - 1) * 4
    local sc = s.scale or 1
    local function add(t, v)
      local w = math.floor(math.min(1, math.abs(v) * sc / P.MOVE) * 255 + .5)
      if w > 2 then out[#out + 1] = { id = id, target = t, weight = w } end
    end
    if s.y >= 0 then add(base, s.y) else add(base + 1, s.y) end
    if s.z >= 0 then add(base + 2, s.z) else add(base + 3, s.z) end
  end
  return #out > 0 and out or nil
end

return P
