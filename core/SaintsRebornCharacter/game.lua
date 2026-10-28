-- SR Tweak Menu - game access layer.
--
-- Everything that touches the game lives here: addresses, the calling
-- convention for game functions from inside the update hook, the two big
-- levers (the game's own Lua 5.0 state and its debug console), and the usual
-- player / vehicle lookups. Features and the menu build on this and never
-- hold an address of their own.
--
-- All addresses are Xbox 360 guest addresses found in the recompiled game
-- code on the author's machine. Confidence is noted where it is not certain.

local G = {}

-- ------------------------------------------------------------ addresses
G.PLAYER      = 0x8309ABEC  -- pointer to the player object (0 = no game loaded)
G.OBJECTS     = 0x830866C8  -- handle table, 16 bytes per entry
G.UPDATE      = 0x82209E30  -- game update, ~50 Hz, main thread (every mod hooks it)
G.MULTIPLAYER = 0x8370E9F6  -- byte: a multiplayer session is active
G.CUTSCENE_A, G.CUTSCENE_B = 0x8370D991, 0x8370D990
G.MENU_A, G.MENU_B = 0x839E0DF8, 0x839E0FB0  -- a screen (pause menu, map ...) is up
G.FRONT_END   = 0x827B04D4  -- 0 in play, -1 in the front end, 1 in the character creator
G.CAMERA      = 0x827D9778  -- +80 right row, +104 forward row (3 floats each)
G.LOOK_SPEED_X, G.LOOK_SPEED_Y = 0x827D9778 + 312, 0x827D9778 + 316  -- floats: Options > Controls look sensitivity (1 = normal)

G.HUD_MESSAGE   = 0x82367568  -- (UTF-16 text*, f1 = seconds): the script function message()
G.LUA_STATE     = 0x8370F8C4  -- the game's own lua_State* (Lua 5.0)
G.LUA_DOSTRING  = 0x824F1C70  -- (L, const char*) -> 0 on success
G.CONSOLE       = 0x82626AD8  -- (const char* line) -> 0 ok / -1 error: the debug console
G.INPUT_GET_STATE = 0x82788EC4  -- XamInputGetState(user, flags, XINPUT_STATE*)
G.TAKE_DAMAGE   = 0x824470D0  -- (victim, attacker, ..., f1 = damage); every damage path ends here

-- Player object fields.
G.P = {
  POS = 20, TYPE = 72, DIRTY = 212, FLAGS = 216, NAV_NODE = 392, FALL_START = 404,
  PLAYER_MARK = 224,   -- 1 on the player (SAY_LINE then skips the talking face animation)
  CONTROLLER = 476, STATE = 508, HEALTH_MAX = 1908, HEALTH = 1912, DAMAGE_FACTOR = 2068, CASH = 2320, INVENTORY = 2324,
  VEHICLE = 2496, VEHICLE_BYTE = 2569, NOT_DRIVING = 3456, EQUIPPED = 3548,
  RESPECT = 3848, RESPECT_BAR = 3860, NOTORIETY = 3868,
}
-- Bits in the flags word at +216.
G.FLAG = { INVULNERABLE = 0x00100000, PERFECT_AIM = 0x00200000, NO_RAGDOLL = 0x00800000, NO_FLINCH = 0x40 }
-- Bits in the word at +212 (bit 1 is the game's dirty flag).
G.FLAG212 = { NO_EXPLOSION_FLING = 2, NO_KNEECAP = 4 }

-- Notoriety: 24 bytes per gang at player+3868: +0 level, +4 min, +8 max, +12 points.
G.GANG = { carnales = 0, vice_kings = 1, rollerz = 2, players = 3, police = 4 }
G.NOTORIETY_SET = 0x821D2F50  -- (player+3868, gang, f1 = level)
G.NOTORIETY_MAX = 0x821D3070  -- (player+3868, gang, max)

-- Vehicle object fields.
G.V = {
  ENGINE = 224, ENGINE_OK = 50300, HP_MAX = 53508, HP = 53512, EXPLOSION_MOD = 53540,
  FLAGS_A = 53976, FLAGS_B = 53980,
}
G.VFLAG_A = { NO_EXPLOSION_FLING = 0x00010000, INVULNERABLE = 0x00020000 }
G.VFLAG_B = { INFINITE_MASS = 0x00080000, NEVER_FLAT = 0x00100000, BULLETPROOF_GLASS = 0x02000000 }
-- Engine object fields (engine = *(vehicle+224)). The game's engine update
-- (0x825839B8) multiplies the nitrous force at +384 by +396 while nitrous is
-- on (+380 == 1), and its speed cap (0x8253DE30) adds +400 (m/s) to the
-- model's top speed only while bit 0x40 of the byte at +440 is set.
G.ENGINE = { NITRO_STRENGTH = 396, EXTRA_TOP_SPEED = 400, FLAGS = 440 }
G.ENGINE_FLAG_EXTRA_SPEED = 0x40
G.MPH_TO_MS = 0.44704
G.VEHICLE_REPAIR = 0x82536F30  -- (vehicle, f1 = fraction)
G.VEHICLE_PART_DAMAGE = 0x825447D0  -- (part, vehicle, ...) -> 0; blast damage to one part (from the explosion code 0x825397E0)
G.VEHICLE_NITRO  = 0x8253FE38  -- (vehicle, bool)
G.VEHICLE_INDEX  = 0x8256A638  -- (name) -> vehicle info index, or -1
G.GARAGE_DROP_STATE, G.GARAGE_DROP_INDEX = 0x827E5E44, 0x827E5E48  -- the vehicle cheat's delivery queue
G.LETTERBOX      = 0x8229AB20  -- (bool)

-- Teleporting (the Teleport to Waypoint mod's sequence).
G.STREAM_AT       = 0x82510520  -- (pos*, 0, 0): load the world around a spot first
G.PLAYER_TELEPORT = 0x82481000  -- (player, pos*, rows*): the script teleport; car and homies come along

-- People (the co-op mod creates its players' copies this way).
G.MODEL_LOOKUP   = 0x82114928  -- (name, 0) -> character definition, 0 if unknown
G.NUDGE_SPOT     = 0x824C2CF8  -- (pos*): moves a spawn point off anyone standing there
G.HUMAN_CREATE   = 0x82479AB0  -- (kind, 2, def, pos*, rows*, 255, 0, 0) + stack {+84 u32 0, +94 u16 8, +103 u8 0} -> object
G.STREAM_REF     = 0x8250C750  -- (0x83E876B0 + (id>>24)*204, id, 5, 0, 1): keep a model loaded
G.STREAM_RELEASE = 0x8250CA48  -- (same entry, id, 5, 0, 0): give one such reference back (last arg 1 = all of them)
G.CAN_DESPAWN    = 0x8244BC40  -- (human) -> 1 if the game may delete them (no script record, not in your party)
G.STREAM_TABLE   = 0x83E876B0
G.HUMAN_DEF      = 228         -- object+228 = definition; definition+608 = model id
G.TEAM_LOOKUP    = 0x8244A6A8  -- (team name) -> team id; human+232 = team ("Playas" = the Saints)
G.WEAPON_ITEM    = 0x823F7840  -- (weapon name) -> the inventory record a weapon is added with
G.INV_ADD        = 0x82445C80  -- (human, record, count) -> inventory item
G.INV_ADD_AMMO   = 0x82445B88  -- (human, item, reserve, magazine)
G.PLAYER_EQUIP   = 0x82481300  -- (player, inventory item or 0, 0); no scripted holster lock
G.NPC_EQUIP      = 0x82446280  -- (human, item): an NPC takes the item in hand
G.PARTY_ADD      = 0x82483828  -- (player, npc, forced) -> added? (sets the follower up)
G.HUD_SLOT       = 0x822D0188  -- (npc) -> HUD head slot or -1
G.HUD_ADD        = 0x822D1418  -- (npc) registers the follower's head
G.PARTY_COUNT    = 2096        -- player+2096: followers in the party list (10 at most)

-- Car paint (the garage's own path). A car's customisation state is at
-- vehicle+54688; state+0 is the ring of fitted parts (node +0 part, +8 next),
-- state+8 the car's customisation data (+0 slot count, +4 slots of 16 bytes:
-- +0 slot type, +8 part count, +12 parts of 52 bytes). A part has its colour
-- slots at +36 (count) / +40 (60 bytes each: +20 count and +24 list of
-- allowed palettes) and its slot index at +44.
G.CUST_STATE     = 54688
G.PALETTES       = 0x8371010C  -- *: 12 palettes of 24 bytes (+0 name, +16 colour count, +20 first colour, 80 bytes each)
G.PALETTE_COUNT  = 0x83710108
G.SLOT_TYPES     = 0x8371012C  -- *: slot types of 16 bytes; 46 = Body Color, 47 = Trim Color
G.FIT_PART       = 0x8254C848  -- (state, part, 0)
G.SET_COLOUR     = 0x8254CA48  -- (state, part, colour slot, colour record)

-- Guns. The fire function decides bullet or projectile by the weapon's
-- Ammo_Type (+100: 0 bullet, 1 projectile); the projectile path spawns the
-- weapon's rocket from the fields at +312..+351 and blows it up with the
-- explosion at +144. A rocket keeps its weapon pointer at +208 while it flies.
G.FIRE           = 0x82470A50  -- (human, target, r5, bool): fires the equipped gun
G.WEAPON_INFOS, G.WEAPON_COUNT = 0x832ABCA8, 0x832ABCAC   -- *: 520 bytes each
-- One bullet's ray (per pellet): writes where the shot stopped (the last
-- solid hit, or the end of its range) into r3.
G.BULLET_RAY     = 0x8246FD18  -- (out pos*, out handle*, shooter, info, start*, end*, r9)

-- Pickups on the ground. Every drop goes through ITEM_CREATE with an
-- 80-byte record: +0 flags (0x90 = dropped, deleted when taken; 0x80|0x10
-- also keeps the player from taking it for a second), +4 item info (a
-- weapon info's +28), +8 pos, +20 rows (3x3), +56 owner (255 = work it
-- out), +60 physics group, +64 mass (< 0 = the item's), +68 rounds (-1 =
-- random). The game keeps at most 50 cash pickups and 15 other drops
-- lying around, deleting the oldest.
G.CASH_AT        = 0x82408C58  -- (pos*, rows*, 0, pop, 255, f1 = dollars) -> item: a bill on the ground
G.ITEM_CREATE    = 0x82408190  -- (record*) -> item or 0; item+116 physics body, +220 ammo
G.ITEM_POP       = 0x82406D48  -- (item): the little hop a dropped item makes
G.SET_VELOCITY   = 0x82253F10  -- (body handle, vec3*)
G.ROWS_ID        = 0x828398C4  -- the game's identity 3x3
G.PHYS_GROUP     = 0x827E79EC  -- *: the physics group drops use
-- Chance (0..1) that a dead civilian / gang member drops cash, read at
-- every death; the game's table sets 0.25 for both.
G.CASH_CHANCE    = 0x838F8520  -- f32 civilians, +4 gang members

-- Explosions (explosions.xtbl defs), as the console's `explosion` makes one:
-- EXPLOSION(def, owner, 0, rows*, pos*, 0, 0, 0) + stack {+84 u32, +92 u32,
-- +103 u8, +111 u8} all 0. The owner gets the kills.
G.EXPLOSION_DEF  = 0x82138FB0  -- (name*) -> def or 0
G.EXPLOSION      = 0x82138AE8
-- Every melee hit lands here, whatever was hit (person, car, door).
G.MELEE_HIT      = 0x824560D8  -- (target object, attacker, ...)
-- Weapon record fields read at every shot: +20 class (3 = shotgun), +122 u16
-- Refire_Delay (ms), +304 f32 Ragdoll_Force_Shoot (the push of a killing shot).
G.W = { CLASS = 20, REFIRE = 122, FORCE = 304, SHOTGUN = 3 }
-- Melee moves (melee.xtbl): array at *G.MELEE_MOVES, count at +4, 88 bytes
-- each: +0 name*, +44 f32 ImpactForce, +48 NPCDamage, +52 PlayerDamage,
-- +56 OnlineDamage. A player's hit deals +56 in multiplayer, +52 offline
-- (0x82459CD0); a person's hit +48.
G.MELEE_MOVES, G.MELEE_MOVE_SIZE = 0x8370F854, 88
G.MELEE = { FORCE = 44, NPC_DAMAGE = 48, PLAYER_DAMAGE = 52, ONLINE_DAMAGE = 56 }
-- Multiplayer taunts (0x8216A038): actions Taunt 1/2/3 played with
-- ACTION_PLAY(human, n, 0, 1, 0, 0, 0, -1, f1 0, f2 1, f3 -1), only while
-- human+508 isn't 23 and no melee move runs (human+1956 = -1); the taunt
-- playing is kept at human+4400 (1..3, 0 none).
G.H_MELEE_MOVE, G.H_TAUNT = 1956, 4400
-- Live tunables. Panic fall: falling faster than this (m/s) flails
-- (gameplay_constants, read per character every frame). Crouch: the follow
-- camera's crouch rate cap and damping (camera_free, read every frame).
G.PANIC_FALL = 0x838F837C
G.CROUCH_ACCEL, G.CROUCH_DAMPING = 0x827D9370, 0x827D9374
-- Downforce: a car's Extra_Gravity is copied into the action object at
-- engine object (G.engine_object) +216, float +28 (m/s^2, negative pulls
-- down); read every physics step.
G.ENGINE_ACTION, G.ACTION_DOWNFORCE = 216, 28
G.PROJECTILES    = 0x8309A494  -- *: ring of live projectiles, next at +204
G.IGNITE         = 0x8245E3A0  -- (victim, 0, attacker, 0, f1 = seconds) -> 1 if they caught fire
G.BURN_TIME      = 0x838F83E4  -- float: the game's burn time (set at level load)
G.BURNING        = 0x00400000  -- bit at human+216 while on fire

-- Picture: the game's own colour grading (written every frame by the
-- render setup unless a mod keeps writing it).
G.TINT       = 0x827DDD0C  -- 3 floats: RGB multiplier, 1 1 1 = neutral
G.TIME_DIV   = 0x827AA6D4  -- float, 1.0 normally; World Studio's world pause sets it huge

-- Everyone in the world: a ring of people from *0x8309A788, next at +204
-- (the debug console's super_ragdoll walks it the same way).
G.HUMAN_LIST, G.HUMAN_NEXT = 0x8309A788, 204
-- Animation states (anim_states.xtbl), as the script function
-- set_animation_state applies them: the state's number at human+952, then
-- the apply call.
G.ANIM_STATE_INDEX = 0x822292F8  -- (state name) -> number, or -1
G.ANIM_APPLY       = 0x82440200  -- (human, -1, -1, 0)
G.ANIM_STATE       = 952
-- Actions and animation files, played on any person directly (the engine
-- calls behind the script functions play_action_do and
-- play_custom_action_do, without the script engine).
G.ACTION_INDEX     = 0x82229380  -- (action name) -> number (anim_actions), or -1
G.ACTION_PLAY      = 0x82444F88  -- (human, action, flags, replace, 0, 0, 0, -1, f1 0, f2 1, f3 -1) -> played?
G.ANIM_OWNER       = 568         -- human+568: animation owner; owner+3592 = its player handle
G.ANIM_STOP        = 0x826F5BC0  -- (player handle)
G.ANIM_RESET       = 0x826F5B88  -- (player handle, 0)
G.ANIM_FILE_INFO   = 0x826F8658  -- (file id) -> record; +24 nonzero when paged in
G.ANIM_TRACK_DONE  = 0x826F63D0  -- (animation player, track handle) -> remaining time is zero
G.ANIM_FILE_ID     = 0x826F6BA8  -- ("name.anim") -> animation id, or -1 when not loaded
G.ANIM_PLAY_FILE   = 0x826F5C20  -- (player handle, id, 16, -, -, -, -, -1, f1 0, f2 1, f3 1, f4 -1) -> handle
G.ANIM_SLOT_FOR    = 0x82226DA8  -- (owner+4, owner+0, n): what the custom-action path stores at human+592
G.CUSTOM_STATE     = 297         -- human+756 while a custom animation plays

-- Animation sets (anim_set_filenames.xtbl). The game keeps them in a table
-- of 12-byte entries: +0 name, +4 parent name, +8 parent entry (0 = none).
-- A person's body instance (human+568) holds the set it animates with at
-- +4 and its weapon group at +0; every state and action is looked up as
-- (set, group, state) by 0x82226DA8, which searches the set and its
-- parents in the group, then the same in each parent group. States are
-- numbered as anim_states.xtbl (0..226), actions follow at 227+.
G.ANIM_SET_FIND   = 0x82229278   -- (name) -> set entry, 0 if unknown (names compare without case)
G.ANIM_SETS       = 0x82C06D58
G.ANIM_LOOKUP     = 0x82226DA8   -- (set entry, group, state) -> animation, or -1
G.ANIM_GROUPS     = 0x82BC5BD8   -- 22 groups of 2608 bytes: +0 parent group*, +4 u8 count per state, +872 u16 first per state
G.ANIM_GROUP_SIZE = 2608
G.ANIM_POOL_SET   = 0x82BBDBD8   -- u32 set index per pool slot
G.ANIM_POOL_ANIM  = 0x82BC1BD8   -- u32 animation per pool slot
G.OWNER_GROUP, G.OWNER_SET, G.OWNER_VARIANT, G.OWNER_ANIM_PLAYER = 0, 4, 3720, 3592
-- The body's mesh: +3740 is 1 when +3736 holds it. Its variant comes from
-- +3720 when that isn't -1 (else rolled), and every roll of its parts is
-- read from a replay list instead while 0x8370DC0C is set (0x82C079F0:
-- +0 how far along, +4 the bytes).
G.OWNER_MESH, G.OWNER_MESH_KIND = 3736, 3740
G.REPLAY_ON, G.REPLAY = 0x8370DC0C, 0x82C079F0
-- 0x8222F7F8(mesh, variation record, ...) rolls a variation's part and material groups.
G.VARIANT_ROLL = 0x8222F7F8
-- Stores and wardrobes: a trigger by name (0x82637B58 hashes the name,
-- 0x824EC080 finds the trigger), the game's trigger handler (trigger, you,
-- 1) opens its screen, 0x82494208 says whether a store screen is open.
G.NAME_HASH, G.TRIGGER_FIND, G.TRIGGER_USE, G.STORE_OPEN = 0x82637B58, 0x824EC080, 0x824E9120, 0x82494208
-- Your wardrobe: { item, variant } pairs at player+4488, how many at +20872.
G.ITEM_FIND, G.OWNED, G.OWNED_COUNT, G.OWNED_MAX = 0x82318070, 4488, 20872, 2048
-- Wearing: an item's wear option and variant by name, then on your record.
G.WEAR_OPTION_FIND, G.VARIANT_FIND, G.WEAR_ITEM = 0x823180D8, 0x82318158, 0x823182A8
-- Face and body sliders: the creator's table (36-byte entries, the slider
-- index at +24), how many at 0x83117BB8.
G.MORPHS, G.MORPH_COUNT = 0x831169B8, 0x83117BB8
-- Hair colours: the list (pointer, count), kept per hair slot (counts, entries),
-- kept with 0x82321E88(record, slot, colour), put on with 0x823220D0(same).
G.HAIR_COLOURS = 0x8370E804
G.HAIR_KEPT_COUNT, G.HAIR_KEPT = 0x8304D160, 0x8304D190
G.HAIR_COLOUR_KEEP, G.HAIR_COLOUR_PUT = 0x82321E88, 0x823220D0
G.HUMAN_STATE_NOW, G.HUMAN_ANIM_NOW = 576, 592  -- the state playing and the animation cached for it

-- A person's model (human+228 = definition: +8 name, +16 model file,
-- +20 animation set name, +32 loaded rig, +608 stream id). The game's own
-- way of taking a body out of the world and putting it back (what
-- teleport_do and the streamer do for people whose area isn't loaded):
-- deactivate each set bit of human+200 (bit 0 detaches the mesh), then
-- activate, which attaches the model of the current definition (it must be
-- resident) and rebuilds the hand, foot and IK bones from its rig.
-- These are the base person versions. The wrappers the streamer uses for
-- NPCs (0x8247A8E0 / 0x8247A6D0) also link the person into NPC lists at
-- +3592/+3596, which on the player are control flags: used on the player
-- they locked the controls and updated the body twice per frame.
G.HUMAN_DEACTIVATE  = 0x8244BCE8  -- (human, bit, 0)
G.HUMAN_ACTIVATE    = 0x8244B268  -- (human, 0, 0, 0, 0)
G.PERSONA_NOW       = 3552        -- voice persona (a name string); activating picks one only for characters with variant tables
-- Animation simulations (anim_simulation.xtbl: hair, ponytails, coats,
-- chains). A body instance lists its character's at +3688 (count) / +3692;
-- the game's simulation manager (0x8212F6C0) runs those for NPCs, but for a
-- player it runs the list at player+20876 (count +20924, at most 12), which
-- the outfit fills. Without them a swapped model's hair or coat bones are
-- left undriven and those vertices stretch away.
G.OWNER_SIM_COUNT, G.OWNER_SIMS = 3688, 3692
G.PLAYER_SIMS, G.PLAYER_SIM_COUNT, G.PLAYER_SIM_MAX = 20876, 20924, 12
G.PLAYER_FLAGS      = 3592        -- 8 bytes of player state flags (+3593 bit 0x10 = controls locked)
G.HUMAN_ACTIVE      = 200
G.HUMAN_PLACE       = 0x8243E6F8  -- (human, pos*, 0, nav node or -1): puts the body (and its physics) at pos
G.MODEL_RESIDENT_TABLE = 0x82C06F68  -- 24 loaded character models of 112 bytes; stream id at +108
G.MODEL_LOAD_NOW    = 0x8250F160  -- (stream id, 5, 0): load it now (the co-op mod's own-body loader)
G.MODEL_RESIDENT    = 0x8250F3C8  -- (stream id) -> loaded? (low byte)
G.RIG_BONE          = 0x826F5A68  -- (rig, bone name) -> bone index
G.D = { NAME = 8, MODEL = 16, ANIM_SET = 20, RIG = 32, PERSONA_COUNT = 136, PERSONAS = 140, STREAM_ID = 608 }  -- personas: name strings (SP_JULIUS ...)
-- The player's own look-at bones, set from the rig when the player is made
-- (0x8247BA28): neck, both upper-arm twists and both feet.
G.PLAYER_BONES = { { 3664, "neck" }, { 3668, "l-upperarmtwist1" }, { 3672, "r-upperarmtwist1" }, { 3676, "L-Foot" }, { 3680, "R-Foot" } }
-- The body's collision: controller (human+476) +16 = physics entity, whose
-- collision layer word is at +56; 0x8244BFA8 picks the layer and hands the
-- entity to 0x826CA870(world, entity, 1). Deactivating moves the player to
-- the "inactive" layer and activating doesn't always move them back.
G.CONTROLLER_ENTITY, G.ENTITY_LAYER = 16, 56
G.COLLISION_UPDATE, G.COLLISION_WORLD = 0x826CA870, 0x8370DC88

-- The player's clothes are separate pieces worn through the customization
-- record (player+4484, +8 = slot): they're drawn on whatever skeleton the
-- body has, so a swapped model wears them twisted. The co-op mod's calls:
G.CUSTOM          = 4484
G.OUTFIT_PACK     = 0x8248DCA8  -- (writer {u32 length, u32 buffer*}, record): count byte + 4 bytes per worn item
G.OUTFIT_WEAR     = 0x8248DE28  -- (record, reader {u32 length, u32 buffer*}, cursor*, 0)
G.OUTFIT_REMOVE   = 0x8248D340  -- (record): takes everything off
G.CLOTHING_FINISH = 0x8248CFC8  -- (): works through the clothing queue (loads, takes off)
G.OUTFIT_REMOVE_ONE = 0x8248D198  -- (record, item)
G.WORN_COUNT      = 0x83036140 + 13056  -- + slot*4
-- The worn table: 20-byte entries at 0x83039470 + slot*1080 (part, record,
-- wear option, variant, item; the item's +32 is the slot it fills, 0 to 4
-- for hair, beard, mustache, sideburns, eyebrows). WORN_LIST is the items'.
G.WORN_TABLE      = 0x83039470
G.WORN_LIST       = 0x83039480
G.WORN_REMOVE_AT  = 0x823183C8  -- (record, index, 1 = more to come / 0 = rebuild): off the table and the record
G.WORN_REMOVE_SLOT = 0x82318498 -- (record, slot, same): everything in a slot
-- The slider bake: every update, each customization record whose sliders
-- changed is baked into its body's model (record +16 = body instance).
G.SLIDER_BAKE = 0x824904B0

-- Homie heads on the HUD: human+4220 caches the head bitmap (-1 = look it
-- up again). The lookup (0x822DE800) goes by the voice persona at
-- human+3552 through follower_heads.xtbl; with no persona there is no
-- head, and an unknown one shows the first table entry. 0x827AF20C is the
-- HUD's own Saints fleur-de-lis (hud_health_ico_saints) once the HUD loaded.
G.HUD_HEAD, G.PERSONA = 4220, 3552
G.SAINTS_ICON = 0x827AF20C        -- the small one in the health ring (fallback)
-- The gang logos the HUD loads once (act_ico_saints / lc / vk / wr: name,
-- then bitmap handle): the Saints one is the size of a homie's head.
G.GANG_ICONS = 0x827AF0D0
G.SAINTS_LOGO = G.GANG_ICONS + 4

-- Notoriety on the HUD. 0x82481990(player, 1) picks the gang to show (it
-- only looks at Los Carnales, Vice Kings and Rollerz) as a "type"; 0x822EA408
-- (player, slot) draws one meter, with the icon at 0x83023660 + index*4
-- (slot 0 -> 5, slot 2 -> 6, others -> 4). Notoriety slots: 0 Los Carnales,
-- 1 Vice Kings, 2 Rollerz, 3 Saints, 4 police (24 bytes each at player+3868).
G.NOTORIETY_PICK = 0x82481990
G.NOTORIETY_METER = 0x822EA408
G.NOTORIETY_ICONS = 0x83023660
G.DRAW_COLOR = 0x8263E7D8         -- (r, g, b, a): colour of the next HUD drawing
G.D.TEAM = 108                    -- definition: the team people made from it are on

-- The music player. 0x826595E0 is the game's call that hands the player a
-- playlist (XMPCreateTitlePlaylist): r4 = songs (36 bytes each, +0 = the
-- UTF-16 file path), r5 = how many. Every playlist and every preview goes
-- through it.
G.MUSIC_PLAYLIST = 0x826595E0
-- The music store's songs: genres of 12 bytes at *0x83032FDC (+4 songs,
-- +8 how many), how many at 0x83032FD8; a song is 44 bytes, +24 its state:
-- 0 locked, 1 in the store, 2 yours (the game's own unlock-all, 0x822FA3D8,
-- sets every song to 2).
G.MUSIC_GENRES, G.MUSIC_GENRE_COUNT = 0x83032FDC, 0x83032FD8
-- The 60 hidden CDs: how many you have at player+21228; every 10 unlock a
-- bonus song (cd_collectible1..6) through 0x82226738(name hash); the "CDs
-- found" statistic is stat 31, added to by 0x82221660(31, &amount).
G.P_CDS, G.CD_MAX = 21228, 60
-- The local player's sprint meter, as the HUD shows it: the value at
-- 0x8370E3F8, kept between 0x8370E3F0 and 0x8370E3F4 (empty and full) every
-- update by the sprint code (0x8247D3B0 / 0x82484F68, from its timer at
-- 0x82832B40), refill included. (0x8309ABC4 only changes as you start or
-- stop sprinting.)
G.SPRINT_METER, G.SPRINT_METER_MAX = 0x8370E3F8, 0x8370E3F4
G.UNLOCK, G.UNLOCK_HASH, G.STAT_ADD, G.STAT_CDS = 0x82226738, 0x82637C18, 0x82221660, 31
G.DAMAGE_SEQUENCE = 0x827D5FB0     -- not -1 while the game holds all damage back
G.SAY_LINE = 0x82219298            -- (human, situation, -1, -1, ...): a person says a persona line
G.PLAY_LINE = 0x8221AC00           -- (human, situation, f1 chance ...): picks a line for the situation, then SAY_LINE

-- HUD pictures by name (bitmap_sheets.xtbl), as the HUD itself looks them
-- up: 0x82643430(table, name) -> handle (16-bit, -1 if unknown).
G.BITMAP_FIND, G.BITMAP_TABLE = 0x82643430, 0x82816484
-- The HUD's follower list: slots of 888 bytes from 0x83018D18, the
-- follower's handle at +188; how many at 0x8370E240.
G.FOLLOWER_SLOTS, G.FOLLOWER_SLOT_SIZE, G.FOLLOWER_COUNT = 0x83018D18, 888, 0x8370E240

-- Minimap people (0x822E8D00 draws them, each with its position = person+20):
-- Saints through 0x822E8C78 (purple dot), other gangs through 0x822E8BF0
-- (red dot). Dot pictures at 0x83027258: +8 blue (cars), +12 red, +16 purple.
G.DOT_GANG, G.DOT_SAINT = 0x822E8BF0, 0x822E8C78
G.DOT_IMAGES = 0x83027258
-- Followers on the minimap (0x822E8FE8): each a white arrow (map_blip,
-- handle at 0x83023684) tinted by 0x8213D988(f1 r, f2 g, f3 b, f4 a) with
-- (160, 0, 255) - Saints purple - and drawn by 0x8227E720(handle, f1 x,
-- f2 y, f3 turn, f4 size, r8 clip). The follower is in r31 (person+20) at
-- the tint.
G.FOLLOWER_ARROWS, G.ARROW_TINT, G.DRAW_BITMAP, G.ARROW_IMAGE = 0x822E8FE8, 0x8213D988, 0x8227E720, 0x83023684

-- Animation players (0x826F0BC0: handle -> 0x8283E7D0 + (handle % 77) * 8028):
-- +3072 one 48-byte pose per slot (3x3 turn, then position at +36) in the
-- model's space, +7152 the rig, +7156 the slot map (+4 slots, +12 + 2*slot
-- bone, +140 + 2*bone slot). 0x826F1F78(player, locals) turns the
-- animation's local poses (48 bytes per slot, parent-relative) into those,
-- slot by slot down the skeleton (0x826F1E30 is the reverse).
-- Rigs: +32 flags (bit 4: 36-byte bones, else 32), +36 bones, +40 first
-- loose bone (hair, sleeves, hood, lapels, coat panels - 41), +44 how many
-- (23), +56 bones: +0 name, +16 rest offset from the parent, +28 parent.
-- Every rig's rest pose is unturned, so a bone held at rest relative to its
-- parent is (no turn, rest offset). Cloth sims set their bones after
-- (0x826F5EE0), so the ones a sim runs still move.
G.ANIM_PLAYERS, G.ANIM_PLAYER_SIZE, G.ANIM_PLAYER_COUNT = 0x8283E7D0, 8028, 77
G.POSE_BUILD = 0x826F1F78
G.TEAM_ID = { Playas = 0, ["Vice Lords"] = 1, ["Los Carnales"] = 2, Rollerz = 3, Police = 6, Civilian = 7, ["Neutral Gang"] = 8 }

-- Vehicles made in the world (the co-op mod creates cars this way).
G.VEHICLE_CREATE = 0x82569700  -- (3, 2, info index, pos*, rows*, 0, 0, variant name* or 0, f1 = 0) + stack {+84 u32 0, +95 u8 0, +100 u32 0} -> handle
G.VEHICLE_FROM_HANDLE = 0x82569630  -- (handle) -> vehicle object
G.VEHICLE_MODEL_REF = 0x82532360  -- (vehicle, 5): the game's own model request for a car; recorded at
                                  -- vehicle+50304 so the car gives the reference back when it goes
G.WEAPON_CHEAT   = 0x821F56C0  -- (params*): the weapon cheats' routine; params {u32 name*, u32 rounds}
G.VEHICLE_PLACE  = 0x82535C68  -- (vehicle, pos*, rows*, 0)
G.VEHICLE_INFO_TABLE, G.VEHICLE_INFO_SIZE = 0x83E8ABD8, 1196  -- first word of an entry = model id
G.OBJECT_DESTROY = 0x823ACF50  -- (handle): removes a car or person the mod made
G.RELOAD         = 0x82474AB0  -- (human): starts a reload of the equipped gun
G.POLICE_TEAM    = 0x828316F0  -- team object; byte +645 = no notoriety spawns

-- Spawn density (how many cars / people the game wants around the player).
G.TRAFFIC_WANTED = 0x824194A8  -- () -> number of traffic cars wanted
G.PEDS_WANTED    = 0x82419208  -- () -> number of ambient people wanted
G.PEDS_COUNT     = 0x8240DB70  -- () -> number of ambient people now
G.TRAFFIC_MAX    = 0x8208978C  -- float: cars wanted at density 1.0
G.PEDS_MAX       = 0x82089998  -- float: people wanted at density 1.0
G.AMBIENT_PED_LIMIT = 26       -- compiled into the game
G.TRAFFIC_TRIM     = 0x82411E30  -- () removes the furthest traffic car when over 12; r3 = removed
G.SAME_MODEL_COUNT = 0x82413AD8  -- () -> cars of one model on the road (the spawner allows 3)
G.TRAFFIC_COUNT    = 0x8309A1C0  -- int: traffic cars now

-- Globals the game reads every frame (debug console variables).
G.VAR = {
  UNLIMITED_AMMO = 0x8370F86A,  -- byte
  ALLOW_COPS = 0x827AD06C,      -- byte
  ALLOW_GANGS = 0x827AD06D,     -- byte
  ALLOW_BUST = 0x827D95C5,      -- byte
  TRAFFIC_DENSITY = 0x827AD060, -- float 0..1
  PED_DENSITY = 0x827AD064,     -- float 0..1
  JUMP_HEIGHT = 0x827D099C,     -- float, 0.85
  SWIM_SPEED = 0x827ACF34,      -- float, 4.5
  FALL_DAMAGE = 0x827ACE58,     -- float, 0.2
  CHAR_GRAVITY = 0x827D0988,    -- float, 1.5
  SATURATION = 0x827D6C0C,      -- float, 1.0
  CLOCK_RATE = 0x82B2EF40,      -- float, 40 (0 freezes the day/night clock)
  CLOCK_HOUR = 0x82B2EF34,      -- byte
  CLOCK_MINUTE = 0x82B2EF35,    -- byte
  CHEAT_FLAGS = 0x82B2E0A8,     -- word; 0x02000000 = infinite sprint (cheat #), 0x04000000 = infinite ammo
}

-- ------------------------------------------------------------ helpers
local read_u8, read_u16, read_u32, read_f32 = wml.read_u8, wml.read_u16, wml.read_u32, wml.read_f32
local write_u8, write_u16, write_u32, write_f32 = wml.write_u8, wml.write_u16, wml.write_u32, wml.write_f32

function G.pointer_ok(v) return v >= 0x10000 and v < 0xC0000000 end
function G.vec(a) return read_f32(a), read_f32(a + 4), read_f32(a + 8) end

function G.set_bits(addr, mask, on)
  local v = read_u32(addr)
  if on then v = v | mask else v = v & ~mask & 0xFFFFFFFF end
  write_u32(addr, v)
end
function G.has_bits(addr, mask) return (read_u32(addr) & mask) ~= 0 end

-- Player flags carry the game's dirty bit so the change is noticed.
function G.player_flag(p, mask, on)
  G.set_bits(p + G.P.FLAGS, mask, on)
  write_u32(p + G.P.DIRTY, read_u32(p + G.P.DIRTY) | 1)
end

-- The player object, or 0 when no single-player game is loaded.
function G.player()
  local p = read_u32(G.PLAYER)
  if p == 0 or not G.pointer_ok(p) or read_u32(p + G.P.TYPE) ~= 1 then return 0 end
  if read_u8(G.MULTIPLAYER) ~= 0 then return 0 end
  return p
end

function G.in_cutscene()
  return read_u8(G.CUTSCENE_A) ~= 0 or read_u8(G.CUTSCENE_B) ~= 0
end
function G.screen_up()
  return read_u32(G.MENU_A) ~= 0 or read_u32(G.MENU_B) ~= 0
end
function G.in_front_end() return read_u32(G.FRONT_END) ~= 0 end

-- The vehicle the player sits in (object, handle), or nil. The same test
-- the keyboard controls use.
function G.vehicle_object(p)
  if read_u32(p + G.P.NOT_DRIVING) ~= 0 then return nil end
  local handle = read_u32(p + G.P.VEHICLE)
  if handle == 0 or (read_u8(p + G.P.VEHICLE_BYTE) & 0x10) ~= 0 then return nil end
  local state = read_u32(p + G.P.STATE)
  if state == 9 or state == 12 then return nil end
  local index = handle & 0xFFFF
  if index >= 4096 then return nil end
  local object = read_u32(G.OBJECTS + 12 + index * 16)
  if object ~= 0 and read_u32(object + 68) == handle and read_u32(object + 72) == 5 then return object, handle end
  return nil
end

-- True while the handle still names that vehicle object.
function G.vehicle_alive(object, handle)
  local still = read_u32(G.OBJECTS + 12 + (handle & 0xFFFF) * 16)
  return still == object and read_u32(still + 68) == handle
end

-- The engine object of a vehicle (torque multiplier, max speed), or nil.
function G.engine_object(vehicle)
  if read_u8(vehicle + G.V.ENGINE_OK) ~= 1 then return nil end
  local e = read_u32(vehicle + G.V.ENGINE)
  if not G.pointer_ok(e) then return nil end
  return e
end

-- On foot and in control: not in a car, menu, cutscene or the front end.
function G.playing(p)
  if p == 0 then return false end
  if G.in_cutscene() or G.screen_up() or G.in_front_end() then return false end
  return G.vehicle_object(p) == nil
end

-- --------------------------------------------------- calling the game
--
-- Game functions are called from inside the update hook with the hook's
-- own CPU context, the way every mod does it: r1 is lowered to leave the
-- callee stack room, our own buffers live in the gap just below the hook's
-- r1, arguments go in r3.. and f1, the result comes back in r3 / f1.
G.SCRATCH = 0x6000          -- stack room for the callee (the collision probe alone takes 11 KB)
local BUF_BASE = 0x1000     -- our buffers: 4 slots of 1 KB at r1 - 0x1000 .. r1

-- Writes a NUL-terminated string into buffer slot 0..3 and returns its address.
function G.cstr(ctx, s, slot)
  local buf = ctx:r(1) - BUF_BASE + (slot or 0) * 0x400
  local n = math.min(#s, 0x3F0)
  local i = 1
  -- Four bytes per write cuts the Lua/C round trips by four.
  while i + 3 <= n do
    write_u32(buf + i - 1, (string.byte(s, i) << 24) | (string.byte(s, i + 1) << 16) | (string.byte(s, i + 2) << 8) | string.byte(s, i + 3))
    i = i + 4
  end
  while i <= n do write_u8(buf + i - 1, string.byte(s, i)); i = i + 1 end
  write_u8(buf + n, 0)
  return buf
end

-- A float vector written into slot 3 (addresses stay valid during one call).
-- `at` offsets several vectors within the slot: 0, 16, 32 ...
function G.floats(ctx, values, at)
  local buf = ctx:r(1) - BUF_BASE + 3 * 0x400 + (at or 0)
  for i, v in ipairs(values) do write_f32(buf + (i - 1) * 4, v) end
  return buf
end

-- The address of buffer slot 0..3 (1 KB each), for small records.
function G.buffer(ctx, slot) return ctx:r(1) - BUF_BASE + slot * 0x400 end

-- Writes a UTF-16 string (ASCII only) into slot 2..3 and returns its address.
function G.wstr(ctx, s)
  local buf = ctx:r(1) - BUF_BASE + 2 * 0x400
  local n = math.min(#s, 0x1F0)
  for i = 1, n do write_u16(buf + (i - 1) * 2, string.byte(s, i)) end
  write_u16(buf + n * 2, 0)
  return buf
end

-- Calls fn with r3.. = args and f1 = f (optional; a list sets f1, f2 ...). `stack` is an optional
-- list of { offset, width, value } written relative to the callee's r1, for
-- the few game functions that take arguments past r10. Returns r3 and f1.
function G.call(ctx, fn, args, f, stack)
  local r1 = ctx:r(1)
  local saved = {}
  for i = 3, 10 do saved[i] = ctx:r(i) end
  local saved_f = {}
  for i = 1, 4 do saved_f[i] = ctx:f(i) end
  for i, v in ipairs(args or {}) do ctx:set_r(2 + i, v & 0xFFFFFFFF) end
  if type(f) == "table" then
    for i, v in ipairs(f) do ctx:set_f(i, v) end
  elseif f ~= nil then
    ctx:set_f(1, f)
  end
  local callee_r1 = r1 - G.SCRATCH
  if stack then
    for _, a in ipairs(stack) do
      if a[2] == 1 then write_u8(callee_r1 + a[1], a[3])
      elseif a[2] == 2 then write_u16(callee_r1 + a[1], a[3])
      else write_u32(callee_r1 + a[1], a[3]) end
    end
  end
  ctx:set_r(1, callee_r1)
  ctx:call(fn)
  local r3, f1 = ctx:r(3) & 0xFFFFFFFF, ctx:f(1)
  ctx:set_r(1, r1)
  for i = 3, 10 do ctx:set_r(i, saved[i]) end
  for i = 1, 4 do ctx:set_f(i, saved_f[i]) end
  return r3, f1
end

-- Hooks a game function like wml.hook. A hook the loader refuses (it can't
-- detour every compiled function, and which ones depends on the build of
-- saintsrow.exe a PC runs) switches off only what uses it, logged, and the
-- menu still starts. (The loader's own error can itself fail to format, so
-- it's caught.) G.broken[address] is true for the refused ones.
G.broken = {}
function G.hook(addr, fn)
  local ok, err = pcall(wml.hook, addr, fn)
  if not ok then
    G.broken[addr] = true
    wml.log(string.format("couldn't hook %08X (%s) - what uses it is off, the rest of the menu works", addr, tostring(err)))
  end
  return ok
end

-- Runs a line of the game's debug console ("super_ragdoll", "R_wireframe on").
-- Returns true on success. Names are case-sensitive.
function G.console(ctx, line)
  local rc = G.call(ctx, G.CONSOLE, { G.cstr(ctx, line, 0) })
  local ok = rc == 0
  if G.debug then wml.log(string.format("console: %s -> %s", line, ok and "ok" or "error")) end
  return ok
end

-- Runs Lua code in the game's own script state, where all 643 mission
-- script functions (set_weather, notoriety_set, inv_item_add, teleport ...)
-- are globals. Errors are caught by the game's pcall and shown on the HUD.
--
-- The script globals table raises an error on reading an undefined global
-- and the error reporter looks up _ALERT, which the game never defines, so
-- the first thing run is a definition of _ALERT: without it a bad string
-- could take the interpreter down. Code that yields (delay, teleport with
-- a chunk name) must be wrapped in a global and started with thread_new.
local alert_defined = false
function G.script(ctx, code)
  local L = read_u32(G.LUA_STATE)
  if L == 0 then return false end
  if not alert_defined then
    alert_defined = true
    G.call(ctx, G.LUA_DOSTRING, { L, G.cstr(ctx, 'rawset(_UGGlobals, "_ALERT", function(m) message("Script error: " .. tostring(m), 5) end)', 1) })
  end
  -- Logged before the call too, so a call that never returns still shows up.
  if G.debug then wml.log("script start: " .. code) end
  local rc = G.call(ctx, G.LUA_DOSTRING, { L, G.cstr(ctx, code, 1) })
  local ok = rc == 0
  if G.debug or not ok then wml.log(string.format("script: %s -> %s", code, ok and "ok" or ("error " .. rc))) end
  return ok
end

-- ---------------------------------------------------------- HUD messages
--
-- Queued here, shown from the update hook where a CPU context is available.
local hud_queue = {}
G.hud_seconds = 3
function G.hud(text) hud_queue[#hud_queue + 1] = text end
function G.flush_hud(ctx)
  if #hud_queue == 0 then return end
  local text = table.remove(hud_queue, 1)
  G.call(ctx, G.HUD_MESSAGE, { G.wstr(ctx, text:sub(1, 200)) }, G.hud_seconds)
  if G.debug then wml.log("hud: " .. text) end
end

return G
