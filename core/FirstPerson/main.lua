-- First Person - press V to look through the player's eyes.
--
-- The game's camera keeps working as usual (mouse / right stick turn it);
-- only where it sits changes: while first person is on, the camera is moved
-- from behind the player to the player's eyes, taken from the animated
-- skeleton every frame. If the skeleton can't be read, fixed eye heights
-- (standing, crouching, in a vehicle) are used instead.
--
-- Camera state (0x827D9778):
--   +44  final camera position (what the game renders from)
--   +56  position the follow camera wants this frame
--   +80  final orientation, 3 rows; row 3 (+104) points where the camera looks
--   +188 field of view in degrees
-- The follow camera (0x8210D518) fills in +56; the camera manager
-- (0x82109608) then moves +44 towards it. Other cameras (cutscenes, scripted
-- shots) don't go through the follow camera and are left alone.
--
-- The player (pointer at 0x8309ABEC) has its position at +20 and its
-- orientation (three rows) at +32.

local CAMERA        = 0x827D9778
local CAMERA_MODE   = 0x827DA15C  -- follow camera preset in use (see below)
local FOLLOW_CAMERA = 0x8210D518
local CAMERA_UPDATE = 0x82109608
local PLAYER        = 0x8309ABEC
local OBJECTS       = 0x830866C8  -- handle table, 16 bytes per entry

local key            = wml.setting("key", "V")
local forward_offset = wml.setting("forward_offset", 0.15)
local eye_height     = wml.setting("eye_height", 1.62)
local vehicle_eye_height = wml.setting("vehicle_eye_height", 1.1)
local crouch_eye_height  = wml.setting("crouch_eye_height", 1.05)
local vehicle_height_offset = wml.setting("vehicle_height_offset", -0.03)
local look_down_offset   = wml.setting("look_down_offset", 0.25)
local body_follows_camera = wml.setting("body_follows_camera", true)
local height_offset  = wml.setting("height_offset", 0.0)
local enabled        = wml.setting("start_in_first_person", false)
local debug          = wml.setting("debug", false)
local fov            = wml.setting("fov", 75)
local vehicle_side_offset = wml.setting("vehicle_side_offset", 0.08)
local vehicle_look_limit  = wml.setting("vehicle_look_limit", 115)
local vehicle_forward_offset = wml.setting("vehicle_forward_offset", 0.09)
local vehicle_turn_offset    = wml.setting("vehicle_turn_offset", 0.18)
local vehicle_look_up     = wml.setting("vehicle_look_up", 35)
local vehicle_look_down   = wml.setting("vehicle_look_down", 45)

-- Follow camera presets used in normal play (chosen by 0x8210EC20 /
-- 0x8210EA20): on foot 6, 13, 15, 16, 17, 18; vehicles 7 and 21. Anything
-- else (shops, scripted scenes, the sniper scope's own view) keeps the
-- game's camera.
local GAMEPLAY_MODES = { [6] = true, [7] = true, [13] = true, [15] = true, [16] = true,
                         [17] = true, [18] = true, [21] = true }
-- The game's own is_store_interface_open (script function 0x824D0AB0 ->
-- 0x82494208): true while a shop menu is open. Checked once a frame from the
-- follow camera hook.
local IS_STORE_INTERFACE_OPEN = 0x82494208
local store_open = false
local function check_store(ctx)
  local r3 = ctx:r(3)
  ctx:call(IS_STORE_INTERFACE_OPEN)
  store_open = (ctx:r(3) & 0xFF) ~= 0
  ctx:set_r(3, r3)
end

-- Camera type 0 is the normal follow camera; other scenes switch to their
-- own camera types while the follow camera keeps running.
local function first_person_allowed()
  return not store_open and wml.read_u32(CAMERA) == 0 and GAMEPLAY_MODES[wml.read_u32(CAMERA_MODE)] == true
end

wml.take_key(key)

local function read_vec(address)
  return wml.read_f32(address), wml.read_f32(address + 4), wml.read_f32(address + 8)
end

local function write_vec(address, x, y, z)
  wml.write_f32(address, x)
  wml.write_f32(address + 4, y)
  wml.write_f32(address + 8, z)
end

-- The game's own "is the player in a vehicle" test (as in the keyboard
-- controls): the vehicle object, or nil.
local function player_vehicle(player)
  if wml.read_u32(player + 3456) ~= 0 then return nil end
  local handle = wml.read_u32(player + 2496)
  if handle == 0 or (wml.read_u8(player + 2569) & 0x10) ~= 0 then return nil end
  local state = wml.read_u32(player + 508)
  if state == 9 or state == 12 then return nil end
  local index = handle & 0xFFFF
  if index >= 4096 then return nil end
  local object = wml.read_u32(OBJECTS + 12 + index * 16)
  if object == 0 or wml.read_u32(object + 68) ~= handle or wml.read_u32(object + 72) ~= 5 then
    return nil
  end
  return object
end

-- The player's skeleton. The animation handle is at +3592 of the object the
-- player points to at +568. Its animation instance is
-- 0x8283E7D0 + (handle % 77) * 8028, with the skeleton at +7152 and each
-- bone's pose (48 bytes, position at +36) at +(bone + 64) * 48, relative to
-- the player's position (+20) and orientation (three rows at +32).
local ANIM_INSTANCES = 0x8283E7D0

local function read_string(address)
  local chars = {}
  for i = 0, 31 do
    local c = wml.read_u8(address + i)
    if c == 0 then break end
    chars[#chars + 1] = string.char(c)
  end
  return table.concat(chars):lower()
end

local bones_by_skeleton = {}
local function find_bones(skeleton)
  local found = bones_by_skeleton[skeleton]
  if found then return found end
  found = {}
  local count = wml.read_u32(skeleton + 36)
  local list = wml.read_u32(skeleton + 56)
  local stride = (wml.read_u32(skeleton + 32) & 4) ~= 0 and 36 or 32
  if list ~= 0 and count > 0 and count < 512 then
    for i = 0, count - 1 do
      local name_ptr = wml.read_u32(list + i * stride)
      if name_ptr ~= 0 then
        local name = read_string(name_ptr)
        if name == "l-eye" or name == "r-eye" or name == "head" then found[name] = i end
      end
    end
  end
  bones_by_skeleton[skeleton] = found
  return found
end

-- The eyes relative to the player (x right, y up, z forward), or nil.
local function eyes_from_skeleton(player)
  local owner = wml.read_u32(player + 568)
  if owner == 0 then return nil end
  local handle = wml.read_u32(owner + 3592)
  if handle >= 0x80000000 then return nil end
  local instance = ANIM_INSTANCES + (handle % 77) * 8028
  local skeleton = wml.read_u32(instance + 7152)
  if skeleton == 0 then return nil end
  local bones = find_bones(skeleton)
  local function bone(i) return read_vec(instance + (i + 64) * 48 + 36) end
  local x, y, z
  if bones["l-eye"] and bones["r-eye"] then
    local ax, ay, az = bone(bones["l-eye"])
    local bx, by, bz = bone(bones["r-eye"])
    x, y, z = (ax + bx) * 0.5, (ay + by) * 0.5, (az + bz) * 0.5
  elseif bones["head"] then
    x, y, z = bone(bones["head"])
    y = y + 0.08
  else
    return nil
  end
  -- Only trust something that looks like a head.
  -- (Low and far forward is fine: swimming lays the body flat.)
  if not (y > -0.3 and y < 2.2 and math.abs(x) < 1.2 and math.abs(z) < 1.5) then return nil end
  return x, y, z
end

-- Crouching: bit 0x20000000 of the player's flags at +216 (the follow
-- camera checks the same bit). The eye height eases between standing and
-- crouching.
local CROUCH_FLAG = 0x20000000
local crouch = 0.0
local last = {}

-- Where the player's eyes are in the world, or nil.
local function raw_eye_position(no_look_offset)
  local player = wml.read_u32(PLAYER)
  if player == 0 then return nil end
  local px, py, pz = read_vec(player + 20)
  -- The player's up direction (second row of its orientation).
  local ux, uy, uz = read_vec(player + 44)
  if not (uy > 0.5 and uy < 1.5) then ux, uy, uz = 0, 1, 0 end

  local vehicle = player_vehicle(player)
  last.player = { px, py, pz }
  last.vehicle = vehicle ~= nil
  -- A little in front of the eyes, along where the camera looks (level), so
  -- the inside of the head and hair stay out of view; further when looking
  -- down, so the chest stays behind the camera.
  local fx, fy, fz = read_vec(CAMERA + 104)
  local flat = math.sqrt(fx * fx + fz * fz)
  local ox, oz = 0, 0
  if flat > 0.001 and not no_look_offset then
    local amount = forward_offset + math.max(0, -fy) * look_down_offset
    ox, oz = fx / flat * amount, fz / flat * amount
  end
  local extra_height = height_offset + (vehicle and vehicle_height_offset or 0)

  -- Snap to the eyes of the animated skeleton when possible.
  local lx, ly, lz = eyes_from_skeleton(player)
  if lx then
    local r0x, r0y, r0z = read_vec(player + 32)
    local r2x, r2y, r2z = read_vec(player + 56)
    ly = ly + extra_height
    last.bone = { lx, ly, lz }
    last.height = ly
    return px + lx * r0x + ly * ux + lz * r2x + ox,
           py + lx * r0y + ly * uy + lz * r2y,
           pz + lx * r0z + ly * uz + lz * r2z + oz
  end
  last.bone = nil

  local height
  if vehicle then
    height = vehicle_eye_height
  else
    local crouching = (wml.read_u32(player + 216) & CROUCH_FLAG) ~= 0
    crouch = crouch + ((crouching and 1 or 0) - crouch) * 0.15
    height = eye_height + (crouch_eye_height - eye_height) * crouch
  end
  height = height + extra_height
  last.height = height
  return px + ux * height + ox, py + uy * height, pz + uz * height + oz
end

-- In a vehicle the player's own position can lag a frame behind the vehicle,
-- which made the view shake. There the eye is kept relative to the vehicle
-- (position +20, rows +32/+44/+56) and only eases towards where the
-- skeleton says it is.
local vehicle_eye = nil  -- { vehicle, x, y, z } in the vehicle's frame
-- Leaning out of the window (shooting from a vehicle): the head leaves its
-- seated place, and the view follows it. The seated eyes are remembered
-- relative to the player's body (x right, y up, z forward); when the eyes
-- move further than a few centimetres from there, the camera moves along.
-- Measured against the body rather than the world, so a lagging player
-- position can't shake the view.
local seat_eyes = nil    -- { vehicle, x, y, z } relative to the player
local lean_blend = 0
local seat_vehicle, seat_frames = nil, 0
-- How far the vehicle moved over the last frame.
local vehicle_moved, vehicle_last = 0, nil
wml.on_frame(function()
  local player = wml.read_u32(PLAYER)
  local vehicle = player ~= 0 and player_vehicle(player) or nil
  if not vehicle then vehicle_last = nil return end
  local x, y, z = read_vec(vehicle + 20)
  if vehicle_last and vehicle_last[1] == vehicle then
    local mx, my, mz = x - vehicle_last[2], y - vehicle_last[3], z - vehicle_last[4]
    vehicle_moved = math.sqrt(mx * mx + my * my + mz * mz)
  else
    vehicle_moved = 0
  end
  vehicle_last = { vehicle, x, y, z }
end)
-- Swimming (preset 13): the body lies flat with the head at water level, so
-- the eyes dip under the surface and show the world from below, where there
-- is nothing to see. The view is held a little above where the player floats
-- and bobs gently with the swimming, plus a share of the head's own motion.
local swim_camera_height = wml.setting("swim_camera_height", 0.25)
local swim_bob = wml.setting("swim_bob", 0.04)
local swim_blend, swim_head, swim_t = 0, nil, 0
wml.on_frame(function() swim_t = swim_t + 1 end)
local function swim_adjust(x, y, z)
  local player = wml.read_u32(PLAYER)
  local py = wml.read_f32(player + 24)
  -- Standing eyes are ~1.6 m above the player; flat in the water they are
  -- near the player's own height.
  local swimming = wml.read_u32(CAMERA_MODE) == 13 and (y - py) < 0.8
  swim_blend = swim_blend + ((swimming and 1 or 0) - swim_blend) * 0.1
  if swim_blend < 0.01 then swim_head = nil return x, y, z end
  swim_head = swim_head and swim_head + (y - swim_head) * 0.05 or y
  -- Bob only upwards from the set height, so the view never dips under.
  local bob = (math.sin(swim_t * 0.07) * 0.6 + math.sin(swim_t * 0.031) * 0.4 + 1) * 0.5 * swim_bob
  local target = py + swim_camera_height + bob
  return x, y + (target - y) * swim_blend, z
end

local function eye_position()
  local player = wml.read_u32(PLAYER)
  local vehicle = player ~= 0 and player_vehicle(player) or nil
  -- In a vehicle the eye is kept without the "in front of the eyes" offset;
  -- that is added below along where the view points, so looking sideways
  -- or back moves the camera out of the head that way too (a fixed offset
  -- towards the vehicle's front showed the player's own face when looking
  -- back).
  local x, y, z = raw_eye_position(vehicle ~= nil)
  if not x then return nil end
  if not vehicle then
    vehicle_eye = nil
    return swim_adjust(x, y, z)
  end
  local lean_x, lean_y, lean_z = 0, 0, 0
  local bx, by, bz = eyes_from_skeleton(player)
  if seat_vehicle ~= vehicle then seat_vehicle, seat_frames, seat_eyes = vehicle, 0, nil end
  seat_frames = seat_frames + 1
  -- The seated eyes are taken once the getting-in animation is over.
  if bx and not seat_eyes and seat_frames >= 45 then seat_eyes = { vehicle, bx, by, bz } end
  if bx and seat_eyes then
    local dx, dy, dz = bx - seat_eyes[2], by - seat_eyes[3], bz - seat_eyes[4]
    local d = math.sqrt(dx * dx + dy * dy + dz * dz)
    -- Steering and looking around move the head less than this; leaning
    -- out of the window to shoot moves it a lot more.
    local want = math.max(0, math.min(1, (d - 0.18) / 0.15))
    lean_blend = lean_blend + (want - lean_blend) * 0.25
    if d < 0.15 then
      -- Keep following where the head rests while seated.
      seat_eyes[2] = seat_eyes[2] + dx * 0.02
      seat_eyes[3] = seat_eyes[3] + dy * 0.02
      seat_eyes[4] = seat_eyes[4] + dz * 0.02
    end
    if lean_blend > 0.001 then
      local r0x, r0y, r0z = read_vec(player + 32)
      local r1x, r1y, r1z = read_vec(player + 44)
      local r2x, r2y, r2z = read_vec(player + 56)
      lean_x = (dx * r0x + dy * r1x + dz * r2x) * lean_blend
      lean_y = (dx * r0y + dy * r1y + dz * r2y) * lean_blend
      lean_z = (dx * r0z + dy * r1z + dz * r2z) * lean_blend
    end
  else
    lean_blend = lean_blend * 0.8
  end
  local vx, vy, vz = read_vec(vehicle + 20)
  local ax, ay, az = read_vec(vehicle + 32)
  local bx, by, bz = read_vec(vehicle + 44)
  local cx, cy, cz = read_vec(vehicle + 56)
  local dx, dy, dz = x - vx, y - vy, z - vz
  local lx = dx * ax + dy * ay + dz * az
  local ly = dx * bx + dy * by + dz * bz
  local lz = dx * cx + dy * cy + dz * cz
  if not vehicle_eye or vehicle_eye[1] ~= vehicle then
    vehicle_eye = { vehicle, lx, ly, lz }
  elseif vehicle_moved < 0.02 and lean_blend < 0.01 then
    -- Only learn while the vehicle is (nearly) standing still, where a
    -- lagging player position doesn't matter, and not while leaning out.
    vehicle_eye[2] = vehicle_eye[2] + (lx - vehicle_eye[2]) * 0.05
    vehicle_eye[3] = vehicle_eye[3] + (ly - vehicle_eye[3]) * 0.05
    vehicle_eye[4] = vehicle_eye[4] + (lz - vehicle_eye[4]) * 0.05
  end
  lx, ly, lz = vehicle_eye[2] + vehicle_side_offset, vehicle_eye[3], vehicle_eye[4]
  local ex = vx + lx * ax + ly * bx + lz * cx + lean_x
  local ey = vy + lx * ay + ly * by + lz * cy + lean_y
  local ez = vz + lx * az + ly * bz + lz * cz + lean_z
  -- Out of the head along the view: more when looking sideways or back,
  -- where the head is in the way.
  local fx, fy, fz = read_vec(CAMERA + 104)
  local flat = math.sqrt(fx * fx + fz * fz)
  if flat > 0.001 then
    local along = (fx * cx + fz * cz) / flat  -- 1 = looking ahead, -1 = back
    local amount = vehicle_forward_offset + (1 - along) * 0.5 * vehicle_turn_offset
    ex, ez = ex + fx / flat * amount, ez + fz / flat * amount
  end
  return ex, ey, ez
end

-- On foot, the body turns to where the camera looks (the game builds the
-- player's orientation, rows at +32/+44/+56, from its previous value plus the
-- animation's turning, so setting it each frame keeps the body facing the
-- camera).
local function face_camera()
  if not body_follows_camera then return end
  local player = wml.read_u32(PLAYER)
  -- Not in a vehicle, and not while getting in or out of one.
  if player == 0 or player_vehicle(player) then return end
  -- Walking sideways or back: let the game turn the body towards where it
  -- walks (it has no way to walk that way while facing forward, except
  -- while shooting).
  if wml.key_down("A") or wml.key_down("D") or wml.key_down("S") then return end
  if wml.read_u32(player + 2496) ~= 0 and (wml.read_u8(player + 2569) & 0x10) ~= 0 then return end
  local fx, _, fz = read_vec(CAMERA + 104)
  local flat = math.sqrt(fx * fx + fz * fz)
  if flat < 0.001 then return end
  fx, fz = fx / flat, fz / flat
  local r0x, r0y, r0z = read_vec(player + 32)
  local r1x, r1y, r1z = read_vec(player + 44)
  local r2x, r2y, r2z = read_vec(player + 56)
  if r1y < 0.9 then return end  -- not upright (ragdoll, climbing...)
  local cx = r1y * r2z - r1z * r2y
  local cy = r1z * r2x - r1x * r2z
  local cz = r1x * r2y - r1y * r2x
  local sign = (cx * r0x + cy * r0y + cz * r0z) >= 0 and 1 or -1
  write_vec(player + 32, sign * fz, 0, -sign * fx)
  write_vec(player + 44, 0, 1, 0)
  write_vec(player + 56, fx, 0, fz)
end

-- The follow camera doesn't run on every frame in vehicles, and the camera
-- manager can run more than once per frame, so "the follow camera is in
-- charge" is judged by game frames: it ran this frame or one of the last few.
local game_frame = 0
local follow_camera_frame = -100
wml.on_frame(function() game_frame = game_frame + 1 end)
local function follow_camera_recent() return game_frame - follow_camera_frame <= 3 end

-- In a vehicle the view may turn only so far from where the vehicle points
-- (far enough to look over a shoulder), and tilt only so far up and down.
-- The mouse camera code drops turning past these limits (wml.limit_camera
-- is called every frame while they apply).
local function angle_diff(a, b)
  local d = a - b
  while d > math.pi do d = d - 2 * math.pi end
  while d < -math.pi do d = d + 2 * math.pi end
  return d
end
local function limit_vehicle_look(active)
  local player = wml.read_u32(PLAYER)
  local vehicle = active and player ~= 0 and player_vehicle(player) or nil
  if not vehicle then wml.limit_camera() return end
  local fx, fy, fz = read_vec(CAMERA + 104)
  local vx, _, vz = read_vec(vehicle + 56)
  local yaw = angle_diff(math.atan(fx, fz), math.atan(vx, vz))
  local pitch = math.asin(math.max(-1, math.min(1, fy)))
  wml.limit_camera(yaw, pitch, math.rad(vehicle_look_limit), math.rad(vehicle_look_up), math.rad(vehicle_look_down))
end

wml.hook(FOLLOW_CAMERA, function(ctx)
  ctx:call_original()
  check_store(ctx)
  follow_camera_frame = game_frame
  if not enabled or not first_person_allowed() then return end
  -- Only the body is turned here; the camera itself is placed after the
  -- camera manager (below). Writing the follow camera's own position (+56)
  -- made the vehicle cameras, which aim from it, spin.
  face_camera()
end)

-- The camera manager fades out people close to the camera (it looks for
-- them within a radius of the value at 0x827D93C0, and they fade back in
-- once they're out of it). In first person that hid the player and anyone
-- standing near, so the radius is 0 while first person is on.
local CAMERA_FADE_RADIUS = 0x827D93C0
local saved_fade_radius = nil

-- The people picked are kept in a list of 3 (0x8295A0F4, nearest first);
-- the camera sitting inside the player still counts the player as near, so
-- the player is taken out of the list. A person's fade (1 = fully shown) is
-- at +2484.
local FADE_OUT_LIST = 0x8295A0F4
local function unfade_player()
  local player = wml.read_u32(PLAYER)
  if player == 0 then return end
  local kept = {}
  for i = 0, 2 do
    local p = wml.read_u32(FADE_OUT_LIST + i * 4)
    if p ~= 0 and p ~= player then kept[#kept + 1] = p end
  end
  for i = 0, 2 do wml.write_u32(FADE_OUT_LIST + i * 4, kept[i + 1] or 0) end
  wml.write_f32(player + 2484, 1.0)
end

-- The follow camera doesn't run every frame, and the preset changes for a
-- frame or two in some moves; judging each frame on its own switched the
-- view back and forth. So first person stays on through short gaps, and
-- only a shop, a cutscene or another camera type turns it off at once.
-- In a vehicle the view direction (final orientation, rows at +80/+92/+104)
-- sometimes jumped far off for a frame and back (seen as the view flickering
-- to random directions). Mouse turning there is limited to 6 radians per
-- second (kbm.cpp), a few degrees a frame, so a bigger jump in one frame is
-- not the player: the previous direction is kept. A new direction that holds
-- for several frames (looking back with X, a scripted turn) is taken.
local view_kept, view_pending, view_pending_frames = nil, nil, 0
local view_jumps_held = 0
local function read_rows()
  local r = {}
  for i = 0, 8 do r[i + 1] = wml.read_f32(CAMERA + 80 + i * 4) end
  return r
end
local function write_rows(r)
  for i = 0, 8 do wml.write_f32(CAMERA + 80 + i * 4, r[i + 1]) end
end
local function view_angle(a, b)
  local d = a[7] * b[7] + a[8] * b[8] + a[9] * b[9]
  return math.acos(math.max(-1, math.min(1, d)))
end
local VIEW_JUMP = math.rad(25)
local function steady_view(in_vehicle)
  if not in_vehicle then view_kept, view_pending = nil, nil return end
  local cur = read_rows()
  if not view_kept or view_angle(cur, view_kept) <= VIEW_JUMP then
    view_kept, view_pending = cur, nil
    return
  end
  if view_pending and view_angle(cur, view_pending) < math.rad(10) then
    view_pending_frames = view_pending_frames + 1
  else
    view_pending, view_pending_frames = cur, 1
  end
  if view_pending_frames >= 5 then
    view_kept, view_pending = cur, nil
  else
    write_rows(view_kept)
    view_jumps_held = view_jumps_held + 1
  end
end

local off_frames = 0
local last_vehicle_speed, was_in_vehicle, bail_until = 0, false, 0
local was_active = false
local skeleton_frames = 0
local jump_max, eye_missing, updates_this_frame = 0, 0, 0
local frames_no_update, frames_multi_update, diag_frames = 0, 0, 0
local frames_overwritten, frames_no_follow, overwrite_max = 0, 0, 0
-- Driving trace: per frame, whether the follow camera ran (F/-), how far the
-- vehicle moved, how far the eye moved, and how far the game's own camera
-- position was from the eye. A few traces per session.
local trace, traces_done, trace_wait = {}, 0, 0
local frame_eye_step, frame_game_dist = 0, 0
local trace_last_rel = nil
wml.on_frame(function()
  if not was_active then updates_this_frame = 0 return end
  if updates_this_frame == 0 then frames_no_update = frames_no_update + 1
  elseif updates_this_frame > 1 then frames_multi_update = frames_multi_update + 1 end
  updates_this_frame = 0
  diag_frames = diag_frames + 1
  if game_frame - follow_camera_frame > 1 then frames_no_follow = frames_no_follow + 1 end
  trace_wait = trace_wait + 1
  if debug and traces_done < 4 and trace_wait > 300 and last.vehicle and vehicle_moved > 0.15 then
    -- Where the view points relative to the vehicle, in degrees, and how
    -- far the view turned this frame.
    local fx, fy, fz = read_vec(CAMERA + 104)
    local player = wml.read_u32(PLAYER)
    local veh = player ~= 0 and player_vehicle(player)
    local rel = 0
    if veh then
      local cx, _, cz = read_vec(veh + 56)
      rel = math.deg(math.atan(fx * cz - fz * cx, fx * cx + fz * cz))
    end
    local turn = trace_last_rel and rel - trace_last_rel or 0
    trace_last_rel = rel
    trace[#trace + 1] = string.format("%s%.2f/%.2f/%.1f/%.1f", game_frame - follow_camera_frame > 1 and "-" or "F",
      vehicle_moved, frame_eye_step, rel, turn)
    if #trace >= 20 then
      wml.log("drive trace: " .. table.concat(trace, " "))
      trace = {}
      if trace_wait > 700 then traces_done, trace_wait = traces_done + 1, 0 end
    end
  end
  -- Something else moved the view after the first person camera placed it
  -- (seen as the view flicking to third person in vehicles): count it and
  -- put the view back.
  if last.eye then
    local x, y, z = read_vec(CAMERA + 44)
    local dx, dy, dz = x - last.eye[1], y - last.eye[2], z - last.eye[3]
    local d = math.sqrt(dx * dx + dy * dy + dz * dz)
    if d > 0.05 then
      frames_overwritten = frames_overwritten + 1
      if d > overwrite_max then overwrite_max = d end
      write_vec(CAMERA + 44, last.eye[1], last.eye[2], last.eye[3])
    end
  end
  if diag_frames >= 60 then
    local player = wml.read_u32(PLAYER)
    if debug then wml.log(string.format("first person: %d frames, %d without a camera update, %d with several, eye missing %d, biggest eye step %.2f m | preset %d, player y %.2f, eye y %.2f | moved by something else %d (up to %.2f m), no follow camera %d, view jumps held %d, lean %.2f",
      diag_frames, frames_no_update, frames_multi_update, eye_missing, jump_max, wml.read_u32(CAMERA_MODE),
      player ~= 0 and wml.read_f32(player + 24) or 0, last.eye and last.eye[2] or 0,
      frames_overwritten, overwrite_max, frames_no_follow, view_jumps_held, lean_blend)) end
    view_jumps_held = 0
    diag_frames, frames_no_update, frames_multi_update, eye_missing, jump_max = 0, 0, 0, 0, 0
    frames_overwritten, frames_no_follow, overwrite_max = 0, 0, 0
  end
end)
local active_switches = 0
wml.hook(CAMERA_UPDATE, function(ctx)
  local raw = enabled and follow_camera_recent() and first_person_allowed()
  -- Tumbling (bailing out of a moving car, knocked down, ragdoll): the eyes
  -- can't be followed well, so the normal camera shows it until the player
  -- is back on their feet.
  local tumbling = false
  local player = wml.read_u32(PLAYER)
  -- Bailing out of a moving vehicle: the dive and roll throw the head
  -- through the body. From leaving a vehicle that was moving (more than
  -- about 15 km/h) until 2.5 s after, the normal camera is used.
  if player ~= 0 then
    local in_vehicle = player_vehicle(player) ~= nil
    if in_vehicle then
      last_vehicle_speed = vehicle_moved
    elseif was_in_vehicle and last_vehicle_speed > 0.07 then
      bail_until = game_frame + 150
    end
    was_in_vehicle = in_vehicle
    if not in_vehicle and wml.read_u32(player + 2496) ~= 0 and last_vehicle_speed > 0.07 then
      bail_until = game_frame + 150  -- still on the way out
    end
    if game_frame < bail_until then tumbling = true end
  end
  if not tumbling and player ~= 0 and not player_vehicle(player) then
    local _, uy = read_vec(player + 44)
    local eyes = eyes_from_skeleton(player) ~= nil
    if eyes then skeleton_frames = skeleton_frames + 1 end
    -- A missing head only counts once this character's skeleton has been
    -- read fine (some models can't be read at all and use fixed heights).
    tumbling = uy < 0.5 or (not eyes and skeleton_frames > 30)
  end
  local hard_off = not enabled or store_open or wml.read_u32(CAMERA) ~= 0 or tumbling
  if raw then off_frames = 0 else off_frames = off_frames + 1 end
  local active = (raw and not tumbling) or (was_active and not hard_off and off_frames < 20)
  if active ~= was_active then
    active_switches = active_switches + 1
    if debug and active_switches <= 200 then
      wml.log(string.format("first person %s (follow camera %d frames ago, camera type %d, preset %d, shop %s, tumbling %s)",
        active and "on" or "off", game_frame - follow_camera_frame, wml.read_u32(CAMERA), wml.read_u32(CAMERA_MODE),
        tostring(store_open), tostring(tumbling)))
    end
  end
  was_active = active
  limit_vehicle_look(active)
  if active and not saved_fade_radius then
    saved_fade_radius = wml.read_f32(CAMERA_FADE_RADIUS)
    wml.write_f32(CAMERA_FADE_RADIUS, 0.0)
  elseif not active and saved_fade_radius then
    wml.write_f32(CAMERA_FADE_RADIUS, saved_fade_radius)
    saved_fade_radius = nil
  end
  ctx:call_original()
  if not active then view_kept = nil return end
  steady_view(last.vehicle)
  unfade_player()
  face_camera()
  -- A wider view in first person, so arms and weapons held in front of the
  -- body come into view. +188 is the field of view the game renders with.
  if fov > 0 then wml.write_f32(CAMERA + 188, fov) end
  local x, y, z = eye_position()
  if x then
    -- Diagnostics for view jumps: how far the eye moved relative to the
    -- vehicle / player since the previous camera update.
    if last.eye then
      local dx, dy, dz = x - last.eye[1], y - last.eye[2], z - last.eye[3]
      local d = math.sqrt(dx * dx + dy * dy + dz * dz)
      if d > jump_max then jump_max = d end
    end
    local gx, gy, gz = read_vec(CAMERA + 44)
    frame_game_dist = math.sqrt((gx - x) ^ 2 + (gy - y) ^ 2 + (gz - z) ^ 2)
    frame_eye_step = last.eye and math.sqrt((x - last.eye[1]) ^ 2 + (y - last.eye[2]) ^ 2 + (z - last.eye[3]) ^ 2) or 0
    write_vec(CAMERA + 44, x, y, z)
    last.eye = { x, y, z }
  else
    eye_missing = eye_missing + 1
  end
  updates_this_frame = updates_this_frame + 1
end)

local frames = 0
local last_mode = -1
wml.on_frame(function()
  if wml.key_pressed(key) then
    enabled = not enabled
    wml.log(enabled and "First person on" or "First person off")
  end
  if debug then
    local mode = wml.read_u32(CAMERA_MODE) + wml.read_u32(CAMERA) * 1000 + (store_open and 100000 or 0)
    if mode ~= last_mode then
      wml.log(string.format("camera preset %d, type %d%s%s", wml.read_u32(CAMERA_MODE), wml.read_u32(CAMERA),
        store_open and ", shop menu open" or "",
        first_person_allowed() and "" or " - third person here"))
      last_mode = mode
    end
    frames = frames + 1
    if frames % 120 == 0 and last.eye then
      local f = function(t) return string.format("%.2f %.2f %.2f", t[1], t[2], t[3]) end
      local player = wml.read_u32(PLAYER)
      wml.log(string.format("player %s | eye %s | %s %.2f%s | preset %d | fov %.1f | player fade %.2f flags %08X | fade list %08X %08X %08X (player %08X)",
        f(last.player), f(last.eye), last.bone and ("skeleton " .. f(last.bone) .. " height") or "fixed height", last.height, last.vehicle and " (vehicle)" or "",
        wml.read_u32(CAMERA_MODE), wml.read_f32(CAMERA + 188),
        player ~= 0 and wml.read_f32(player + 2484) or -1, player ~= 0 and wml.read_u32(player + 212) or 0,
        wml.read_u32(FADE_OUT_LIST), wml.read_u32(FADE_OUT_LIST + 4), wml.read_u32(FADE_OUT_LIST + 8), player))
    end
  end
end)

-- Debug helper for finding the player's state: press F7 a few times while
-- not shooting and F8 a few times while holding the fire button. Values in
-- the player's data that stay the same within each group and differ between
-- the two are logged.
if debug then
  local SIZE = 8192
  local groups = { [false] = {}, [true] = {} }
  local counts = { [false] = 0, [true] = 0 }
  wml.on_frame(function()
    local which
    if wml.key_pressed("F7") then which = false
    elseif wml.key_pressed("F8") then which = true
    else return end
    local player = wml.read_u32(PLAYER)
    if player == 0 then return end
    local g = groups[which]
    for i = 0, SIZE - 4, 4 do
      local v = wml.read_u32(player + i)
      local old = g[i]
      if old == nil then g[i] = v elseif old ~= v then g[i] = false end
    end
    counts[which] = counts[which] + 1
    wml.log(string.format("snapshot %s (%d normal, %d shooting)", which and "shooting" or "normal",
      counts[false], counts[true]))
    if counts[false] == 0 or counts[true] == 0 then return end
    local found = {}
    for i = 0, SIZE - 4, 4 do
      local a, b = groups[false][i], groups[true][i]
      if a and b and a ~= b then found[#found + 1] = string.format("+%d: %X -> %X", i, a, b) end
    end
    wml.log(string.format("%d differences", #found))
    for n = 1, math.min(#found, 80), 6 do
      wml.log(table.concat(found, " | ", n, math.min(#found, n + 5, 80)))
    end
  end)
end

wml.log("Press " .. key .. " to switch between first and third person")
