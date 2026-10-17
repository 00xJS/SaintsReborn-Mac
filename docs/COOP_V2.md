# Co-op v2: design

Status: approved by whompay as a revamp; first build (co-op 2.0, milestone 1) written, **not tested yet**. Built on
branch `coop-v2` of the user's fork; nothing goes to whompay/SaintsReborn until it is tested and confirmed on two
machines (Windows PC and Mac). What 2.0 does is in section 10.

Goal: **up to 12 players in the story city who all see the same world**: the same traffic and parked cars, the
same people, the same police, the same explosions and wrecks, the same knockdowns, and every weapon. One mod
(WhompaysCoop); no companion mods.

Research behind every decision (private research repo, reports with H/M/L confidence marks):
- **P** `P_coop_inventory.md`: what co-op 1.71 syncs, how, and its two-player assumptions.
- **Q** `Q_game_multiplayer.md`: how the game's own 12-player multiplayer replicates the world.
- **R** `R_weapons_sync.md`: all 51 weapons, their engine paths, aim, and per-category sync recipes.
- **S** `S_world_sim_sync.md`: traffic, people, police, explosions, pickups, randomness, streaming.

## 1. Why v1 can't get there

v1 (co-op 0.57-1.71) is a two-game mirror: each player runs a full single-player game, and the mod copies the
host's ambient world into one joiner's game while the two are within 120-200 m, suppressing the joiner's own
simulation with about 25 separate hooks. It assumes exactly one remote game everywhere (one peer, one avatar, one
proxy car, a lobby of 2). Its packets carry no sender or version, events are sent three times and hoped for,
and the host sends about 190 KB/s to one joiner. Police, rockets, grenades, prop explosions, pickups, knockout
state and car hits are not synced at all (P sections 3-4).

## 2. The model: the game's own multiplayer as the guide

The game's multiplayer cannot run on the city (its memory overlaps the city's streaming memory, and story
missions and MP modes can't run together; Q section 7). But it is the blueprint, and parts of it are a library:

| From the game's multiplayer | v2 |
|---|---|
| Star topology: the server talks to every client | Same: the host (slot 0) relays; clients 1..11 |
| 8 object types, each with create / update / apply / destroy callbacks | Same object kinds; call the game's per-type functions where they work in story mode (NPC create 823A7AB0/823A7DB0, vehicle create 823BA888, player creation as 823AF420, projectile copy as 823ACB08), own code where they need MP state |
| Every object the server creates is registered at its constructor (82352D38 call sites) | Hook those constructors (8247A6D0 humans, 8253BF08 vehicles, 8241FCB0 projectiles, 82408190 items) instead of setting the MP flags |
| Snapshot scheduler: priority = -10 x distance, +500 in view or within 10 m, sorted, byte budget per client per packet | Same algorithm, per peer, 20 Hz |
| Only the server applies damage, creates explosions and spawns projectiles; clients detect their own hits and the server replays them through the normal hit functions | Same rules, with "server" replaced by "owner" (section 4) |
| Message bundles with a reliable part (acked) and an unreliable part | Same: reliable channel for creates, destroys and events; unreliable for snapshots |

## 3. Network layer

- **Transport**: co-op's own Epic link (`eos_link.h`), made multi-peer: lobby of up to 12, stand-in address per
  peer (10.255.0.N, already there), outgoing queue capped (1.71). Direct UDP stays for LAN and debugging and
  speaks the same protocol. The runtime's mod channel (`SrModSend`, used by Zombies) is the fallback option.
- **Packet**: `{magic 'SRC2', u8 version, u8 sender slot, u16 seq, u16 ack, u32 ack bits}` then messages
  `{u8 type, u16 len, payload}`; at most 1100 bytes (Epic's limit is 1170).
- **Channels**: reliable-ordered per peer (resent until acked, at most 1024 messages in flight, a peer more than
  16384 behind is dropped) for joins/leaves, creates, destroys and events; unreliable newest-wins for snapshots.
- **Session**: join with a version check (mismatched versions are refused with a clear message), slot
  assignment by the host, leave and timeout per slot, host relays client messages to the other clients. Each join
  attempt carries an epoch, so a client that lost the host (or left and came back) from the same address is taken
  as a new join at once instead of being stuck behind its old link.
- **Budget**: per peer and per tick, filled by the scheduler in priority order; far objects update less often.

## 4. Identity and ownership

- **NetId** = owner slot (8 bits) + owner's serial (24 bits). Every game keeps NetId <-> local handle maps.
- **Owners**:
  - a player character: that player's game;
  - a car being driven: the driver's game (ownership moves on enter/exit, arbitrated by the host);
  - the ambient world (traffic, parked cars, people, police, props, pickups): the **area owner**. M1: the host
    owns it within its world radius. M3: each game owns the chunks it has loaded (host first, then lowest slot),
    with handover (S section 7);
  - a projectile: its shooter's game; an explosion: the owner of its source;
  - global state (clock, weather, missions, cutscenes, HUD): the host;
  - notoriety: per player (2.1). Each player's game owns its own player's stars, decay and HUD; every other game
    mirrors them onto that player's character, so its police and gangs go after the wanted player.
- **Puppets**: every object a game does not own is a puppet. One predicate, `IsPuppet(obj)`, drives every
  suppression hook (no own AI actions, no own damage, no own knockdowns/ejections/get-ups, no own despawn, no own
  explosions), replacing v1's separate avatar/copy checks.

## 5. What is synced, and how

| Area | State (snapshot) | Events (reliable) | Suppression on puppets / non-owners |
|---|---|---|---|
| Players | position, yaw, velocity, movement state (on foot / car + seat / ragdoll or knocked out / swimming / climbing / dead), anim set, action, crouch, health, weapon, aiming bit, camera eye/rows (+4112/+4124) | actions, knockdown, get-up, ejection, enter/exit, door, fire, trigger press/release, throw, reload, death, respawn | v1 hooks, unified; ragdoll/KO shown from state with no move orders (the "KO'd vs running into a wall" fix) |
| Cars | model, position, rows, linear/angular velocity, hit points, damage bits (smoke/fire/wreck), lights/siren, doors; paint once | impacts, ejection, wreck | car damage 825397E0, hit points 8252D2A8, explosion push 82536B00, ejection 82540780, own physics steered to the owner's state |
| Traffic, parked cars | as cars + occupants | create/destroy | traffic byte 0x8309A35D = 0, parked spawner hook 8241E378 (S 1c) |
| People | model, look (once), position, yaw, velocity, movement state, anim set, action, dead, ragdoll, car + seat | actions, hit reactions, knockdown, get-up, death, ignite | people bytes 0x8309A364..68 + 82413398, gang manager 824189E0, despawn 8244BC40/824789A8, MakePassive |
| Police | cops and police cars are ordinary people/cars from the area owner | - | response manager 8242ABA0, roadblocks *0x8370F808, cop/gang spawn bytes 0x827AD06C/6D |
| Notoriety | per player (2.1): 5 x level, min, max, points of the owner's +3868 block, written onto that player's character everywhere (+16 = -1) | a player's crimes seen in another game go to their game (`821D3148` there); an arrest of their character goes to them (`82483440` there) | decay `821D24F0` skipped on other players' characters, their hit credit `82481CD8` dropped, `82483440` on them intercepted |
| Bullets (single, auto, shotgun, sniper) | - | fire with the shooter's hit point (r5 aim point), trigger held/released for automatics | replay via 82470A50 with spread off; aiming stance raised first (82449708) so no deferred shot escapes the guard |
| Rockets, grenades, pipe bombs, Molotovs | projectile copy position/velocity | spawn (pos, rows, velocity), detonate | one live projectile (the owner's); copies never detonate (825BF418 skipped); the owner's explosion played once everywhere (82138AE8 with a replay flag) |
| Melee | - | attack/punch (as v1) | v1 hooks |
| Damage | - | hit requests from the attacker's game to the victim's owner, applied there through the game's hit functions (MP's bullet_hit / melee_hit model) | 824470D0 refuses on non-owners |
| Pickups, money and weapon drops | position, value | create/collect/destroy | pickup creation/collection routed to the owner |
| Clock, weather | 0x82B2EF30, weather entries | time jumps | 82157598 / 82156DB0 |
| Missions, cutscenes, HUD, pause | v1's host-driven system, generalized to N | v1 | v1 |

Randomness is never synced as a seed: the owner decides, everyone else is told the outcome (S section 6).

## 6. Engine limits (per game)

32 vehicles, 75 humans, 12 people-model slots, 11 remote player objects. The scheduler sends the nearest first
and the receiver never exceeds these; models that can't load get same-sex lookalikes (v1's model manager).

## 7. Everything from v1 and Co-op Car Guard is built in

No companion mods. Kept from v1: player-pool characters with own body models and clothing (one component set
per slot, 11 slots), dressing and colours, the movement smoothing (Follower), car copies and steering, seats and
enter/exit, the gun handover as the game's multiplayer does it, shot replay rules, missions/cutscenes/HUD/pause,
chat, diagnostics (Perf lines, crash/freeze reports, F9 trace, debug_cmd.txt).

Fixes carried over: car-removal freeze guard (1.64), car-list checks (debug_cars), object-table scans that read
the type first and the 4096-page memory-check cache (1.65), shooting (1.66-1.69), no crash after a death and
no thrown-weapon handover (1.70), capped Epic queue and link diagnostics (1.71), and from Car Guard 0.4-0.5: under
Wine, skip QueryWorkingSetEx once it has never answered and cache VirtualQuery by region.

Fixed on the way: v1 passes car smoke and fire the wrong way round (S).

## 8. Milestones

1. **M1, first playable v2** (the first build to test, 2+ players): network layer and sessions for up to 12;
   players complete (state-based puppets, weapons with aim, projectiles and explosions, damage rules); cars driven
   by players; the host's ambient world (traffic, parked cars, people) through the new scheduler within its world
   radius, other games' spawners off there; missions/cutscenes/HUD for N; all fixes in section 7.
2. **M2**: pickups and money, car hits/ejection/fire, NPC gunfire, weather, props (notoriety per player and the host's police response for a wanted client near it are in since 2.1).
3. **M3**: area ownership for players spread across the city, 12-player load and bandwidth tests, delta
   compression, more of the game's own per-type functions.

## 9. Code layout (for upstream)

`coop.cpp` keeps the mod entry, settings and the menu exports (`WhompaysCoop*`, so coop_menu.cpp keeps working).
New files next to it: `net.*` (transport, sessions, reliability, bundling), `protocol.h` (messages), `registry.*`
(NetIds, ownership, puppets), `players.*`, `vehicles.*`, `people.*`, `weapons.*`, `world.*` (spawner control,
clock, weather, notoriety), `missions.*`, `engine.h` (addresses and game-call helpers). The network layer has a
stand-alone test program that runs several peers over loopback without the game.

## 10. Co-op 2.0 and 2.1 (milestone 1): what is built

Every player needs the same version: 2.0 brought a new protocol (a 1.71 game knocking is told so on screen and
in the log), 2.1 bumped it to 3 (a 2.0 game can't join).

**Session** (`net.h`, tested by `tests/net_test.cpp`: 12 players through 20% loss, refusals, leave/timeout,
rejoin, a lost join reply, a one-way outage, a leave lost before a rejoin, 3000 reliable messages through a 5 s
outage, an oversized name): host = slot 0, up to 11 clients, the host relays. Events (actions, calls, car
enter/exit, hits, fire, throws) and projectiles are reliable and in order (v1 sent each event three times);
poses, traffic and people snapshots are unreliable, newest wins. Epic lobby of 12. Packets up to Epic's 1170 bytes.

**One record per other player** (`RemoteIn` from the network, `Remote` on the game thread): character (player-pool
object with its own body model and clothing set per pool slot), clothes, colours, body sliders, gun, movement
follower, car bookkeeping, shot queue, knockdown state. Hooks ask "is this any other player's character"
(`AvatarSlotOf`) instead of comparing with one avatar.

**Cars** are named by owner slot + handle in the owner's game (`CarId`, `FindCar`, `car_copies`), so any player can
drive or ride in any other player's car or the host's traffic; copies go when unused, far, or their owner leaves.
The host shares its traffic and people around every client (nearest client counts).

**Car damage** (2.1, round 1): the game driving a car decides its damage. It sends the hit points (out of 255:
`TrafficCar.hp`, pose bits 24-31) and the damage bits of +53976: fire 0x800, smoke 0x04000000, wreck 0x80000000,
heavy smoke 0x02000000. 2.0 sent 0x40000000 as the wreck, but that is the fade-out before a car is removed (H), so
intact cars fading out blew up on the other games, and real wrecks only lost their smoke there. On a car another
game drives, a game writes the hit points it is sent (never 0 without the wreck bit), sets smoke and fire with
`8253CD70` (a refusal waits 2 s; fire also gets the burn rate `8252D2A8` gives with it, +54408 = 0.03 of the most
a second, which `8253CD70` leaves at 0 (H), so the car burns down once this game decides again), loses no hit
points of its own while that game sends its damage (`8252D2A8`, which also covers the burn tick, pending damage and
`82540370`/`82540458`; another player's car nobody sends for after they got out burns down here as in 2.0) and
isn't pushed by explosions (`82536B00` runs with the game's no-fling bit set for that call only; the owner's car is
pushed and the copy follows it). A copy is wrecked with the game's MP wreck replay `82542650` (no cash, no script
callbacks), without the explosion when it wasn't shown intact just before (made of a wrecked car, or without
physics until long after the wreck), and keeping its motion then (`82542650` lifts the car whatever the explosion
did, H). One of this game's own cars driven from another game is blown up with `825397E0` as before (3 tries 1 s
apart when refused, then every 30 s; each try damages every part); only that car passes the hooks (2.0 let the
whole explosion's area pass damage every copy: chain wrecks). Protocol 3: 2.0 sent 0 hit points and the fade-out
bit, so it can't join a 2.1 game. Dents and broken parts aren't sent yet.

**Weapons** (research report R): bullets go from the character's muzzle to the shooter's own hit point (first
pellet, `8246FD18`), spread off, gun raised first, the game's own deferred shot swallowed, refused shots retried for
750 ms; automatic guns get the trigger release; rockets, grenades, pipe bombs, Molotovs and bottles: the owner's
projectile is the only real one, other games make a copy (`8241FCB0`) that never explodes by itself and explodes
once at the owner's spot when the owner's does; explosion damage to the host's people is decided by the host,
each game applies explosions to its own player. The fire and throw animations are no longer sent twice.

**Knockdowns** (research S 2d): each player's game decides their ragdoll, knockdown and crash ejection. The state
(+780) goes with the pose; other games knock the character down / get it up to match, never seat or move it while
it lies, and skip crash ejections for other games' cars and characters (the "knocked out in the car on one screen,
running into a wall on the other" case). 2.1: the pose's ragdoll bit means lying (+780 in state 8/15), not getting
up; other games knock on its rising edge and get the character up with its own get-up (`82449FE0`) when it clears.
The owner's knockdown, state 8 (8/15-17) and get-up animations (0xE0/0xE2/0x232) are never replayed, nor anything
else while the character lies or gets up there. The replayed 8/16 put a character that never got up itself at the
world origin, ~700 m off: its get-up spots (+792/+808) were still the constructor's zero vectors (H); a state set
mid-ragdoll could leave it ragdolled in state 0 for good (H), which is now caught and knocked again so it gets up.

**Built in from Co-op Car Guard 0.5**: the car-removal guard and car-list checks (already in 1.64), and the memory
check speed-up under Wine (`memory_speedup`). Car Guard is no longer needed (co-op says so if it is installed).

**Fixed on the way**: car smoke and fire were applied the wrong way round; the "game took the other player's
character for ours" check (1.34) never matched.

**Spawners** on a client while the host's world is shown: traffic spawner byte off (no spawn/despawn churn),
parked-car spawns refused, gang manager skipped (plus v1's density 0 and removals as a backstop). 2.1: its own
police response `8242ABA0` ends the way its multiplayer exit does (the response group the spawners serve next,
0x8309A540, one of the 12 group pointers at 0x8309A510, cleared; `824129C8` / `82411310` skip it when 0) and its
roadblocks `82424708` are skipped (the world update ignores both results), so no police only that client sees.
Only while the client's character stands in the host's game, which is what the host's police come after: it is
made within 60 m of the host and removed past 130 m (`UpdateAvatar`, alike both ways), so the host's character
shown on the client (slot 0) stands for it, held 2 s after it goes; beyond, the client's own police come back for
its own stars (2.0 left no police at all from 130 m to the shared world's 200 m). Not with mod.ini `police = 0`:
the client's police then run as in 2.0.
With `8242ABA0` skipped nothing updates or calls off its groups, so what they had out when the host's world came
(the client wanted first) is removed once then: each group's 8 squads (44 bytes from +92: active byte, car +4,
8 units +12, read from `82429E38`) (H), never a player, copy, follower or mission character, nor a car a player
uses. `allow_cop_spawning` (0x827AD06C) and the roadblock counter `82425150` are left alone: mission scripts own them.

**Copies of the host's people and traffic crews** (2.1, on clients): they wear the host's team. The minimap draws a
person's dot from their team (+232) after its own skips (`822E8D00`: +120 flags, dead, players, followers): 0 the
purple "revive" dot, 6 none, 4 red only for the definition at 0x8370D020, any other red; a car by its driver's
(`822E8E80`) (H). Copies took theirs from their model (definition +108), stand-ins and lookalikes from whatever
loaded model they use (L that this was the purple police). The host sends each person's team (`PedState.team`)
and each car seat's (`TrafficCar.teams`, 4 bits a seat), and the client writes it the way set_team `824DB708`
does on whatever shows that person: copy, lookalike, stand-in, seat stand-in (seated or not) or own character
taken over (never one of the player's homies). The host's police shown here are logged once each. Copies play only
the host's actions: their own actions (crews' car moves excepted), shots (`82470A50`, deferred shot cleared,
drive-bys too) and hits on the client's player (not explosions, fire or a car hitting them, `82451C98`) are refused
outside co-op's replays, which include co-op's own seating and exit orders. Hits from the host's people on foot
come across as before; since the notoriety round, hits from people it doesn't share as people (its crews, its
police response's units) come across too (their own hits: not a car hitting the character, explosions or fire),
whatever mod.ini `police` says.
Car crews are passive like the people (`MakePassive`); a crew member getting out on its own is logged and counted.

**Notoriety and arrests, per player** (2.1, round 1; mod.ini `police = 0` turns it all off: no stars sync, crime
credit, arrests across games or response steering, and a client's own police stay on near the host; the host's
stars still rise for the crimes seen in its game, as in 2.0, and hits from the host's people come across either
way). Each game owns its
player's notoriety: the +3868 block (5 entries of 24 bytes, one per gang - 0 Los Carnales, 1 Vice Kings, 2 Rollerz,
3 Playas, 4 police: +0 level, +4 min, +8 max, +12 points, +16 decay timer, -1 off, +20 timer), its decay
(`8247E850` -> `821D24F0`), HUD (`822F5300`) and stars stay native. Level, min, max and points go to everyone
(`kMsgNotoriety`: reliable at a change of a level, min or max, else once a second unreliable; clamped on receipt),
and every game writes them onto that player's character when it is a pool player (+224 == 1), always with +16 = -1:
the police there go after a player-type target with its own entry 4 >= 1 (`820D28E0` / `823FEBE8`), the gangs
through `82481AB8` (H). Never through the game's setters (`821D2F50`, `821D3070`, `821D2D38`, `821D2950`): they
end in `821D1A20`, which acts on the local player (H), as does each decay step, so `821D24F0` is skipped on other
players' characters, and a character's levels are cleared (level 0, points 0, +16 = -1) before it goes back to the
pool and before a pool object is respawned (`8247E248` -> `8247BA28` -> `821D2D38` would run `821D1A20(0, gang)` on
the local player for a level left in it, H; `821D2D38` also empties the local crime queue, count 0x82B2BA6C, so the
queued records at 0x838F9348 are saved around that respawn and put back). A mirrored level dropping to 0 is forgiven the way `821D1A20` does
it: every person of that gang's team (0x8203A29C) on the list *0x8309ABB8 takes the character's handle off its
enemy lists (`820D8398` / `820D8538`).

Crimes are credited to the culprit's own game. `821D3148(activity, team, flag, weapon info)` queues a crime for
the local player; it is reached from `82482098` (kills), `82481CD8` (hits, shots near people, cars hit) and
`82482568` (government cars, helicopters, trains destroyed), all with the culprit in r3, so those are hooked and
the culprit noted around them: another player's character's crime is sent to that player (`kEventNotoriety`:
activity, team, flag, rocket launcher) behind the game's own gates (*0x8370D1A0 lock, multiplayer byte) and the
original skipped; the receiver validates (activity < 24, team -1..6) and queues it with `821D3148` on its own
player, so the game's lock, delays and queue apply. `824828C0` (carjack) and `82482290` (a cop sees a gun fired)
put their record in the queue themselves (H), so their record is taken back out after the original (with the
queue full, 30, they would apply it to the local player at once: another player's is lost and counted instead).
No double credit: a player's own game counts its melee hits and run-overs on copies (`82481D50`, from `82457900`
/ `82451C98` / `8256A138`) and its shots near them (`82481EE8`, from the bullet's `824581C8` before its damage
call, so a copy's refused damage doesn't skip it; H), so hit credit with another player's character as the
attacker is dropped, and so are their carjacks (the jacker's game sees the pulled-out driver's team on the copy;
its own queued record is logged, "Crime queued here by this player", since `824828C0` never reaches `821D3148`);
kills, government-car crimes and cop sightings are forwarded; on a client, kills of copies and sightings by copied
cops are dropped (the host's game sees them). Every caller of `821D3148` and of `82481CD8` is logged once with
its return address and name (`82481D50` hits, LR 82481EDC; `82481EE8` shots near people, LR 82482084; `82482760`
cars hit, LR 824828B4); the second tells whether a client's bullets at copies reach `82481EE8` there at all (M:
proven for melee only) - if they never do, hit credit from that caller has to be forwarded instead of dropped.

Arrests: `82483440(target)` (from the cops' arrest `820B1A28`; the bust check `820B1F90` / `820B1EB8` never asks
whether the target is the local player, H) sets the target's busted bit (+3592 0x20) but then acts on the local
player and screen (mission callbacks, `82215360`). With another player's character as the target it is
intercepted and `kEventBusted` sent to that player (at most every 3 s); their game arrests its own player behind
the bust check's own gates there (busting on, byte 0x827D95C5; no multiplayer; lying down, +780, and not getting
up, 8/16), with a police level left and not busted already, and the native busted screen, respawn and notoriety
reset follow. A client showing the host's world refuses an arrest of its own player by its copies, and never
forwards one its copies attempt on another player's character: only the host's cops arrest there.
A "Police:" line every 10 s shows the levels here and on other players' characters, notoriety packets sent /
taken / old, credits sent / received / dropped / refused / lost (queue full), arrests sent / applied / refused,
forgivings and skipped decay steps.

**The host's police response for a wanted client** (2.1, round 1). The response manager `8242ABA0` (every frame
from `824131F0`; a client in the host's world skips its own) works for `*0x8309ABEC` alone: the level a group
runs at (`823FCE40`; the spawn timers and squad pick `823FBFE0` read the entry themselves), the vehicle budgets
(`823FCC80`, the gangs' `823FD008` / `823FCEB0`), the spawn rings of 200 / 150 / 100 / 50 m (`8242A540`) and the
pursuit orders (`8242A228` -> `825A91A0(car+50376, &order)` / `8259DCC8(helicopter, handle)`) all read the local
player (H); so do the pursuit's own level reads while a car chases (`825A8828`, the timers; `825A5C00`, a
manoeuvre gate), through `82481AB8` alone. A client sends `kPoseHostWorld` in its pose while its own police are
off (its character stands in the host's game: within 130 m of the host once made; see Spawners); the host picks a response
target every update: the highest police level (gang levels among equals) among itself and those clients'
characters within 200 m, kept 10 s at least and until it is over 230 m away, no longer wanted or gone, or a
strictly higher level takes over (the character made again keeps it; the cars after its old handle are called
off). While the target is a client's character, each of the six readers gets `*0x8309ABEC` swapped to that
character around the original alone (`PlayerSwap`: back on every path, never across a frame; nothing under them
reaches `821D1A20`, `822F5300`, the crime queue, camera, HUD, save or input: their callees are tables, timers,
model lookups and geometry, H), the two pursuit orders get the character's handle at their call sites in
`8242A228` while `8242A228` runs for the police, helicopter or a gang group (slots 0-4 of 0x8309A510, a flag set
around it; the activity spawners, 5-11, keep the player), the spawn point at `8242ABA0`'s call (LR 8242AC94)
while the pending group `*0x8309A540` is one of those, and `825A8828` / `825A5C00` get the swap for a car whose
pursuit target (+50468, modes 4 / 10 / 11) is the character, so the chase runs at the client's level, not the
host's. The gang groups work at the character's gang levels (the swap, not a max with the host's). Not one swap
around `8242ABA0`: it would run the car and crew creation and the activity spawners under it, and its read of
the host's +21252 (the host's nearest road node, -1 indoors, swimming or off the roads): `8242ABA0` starts no
new group while that is -1, so a client's response comes only while the host stands near a road (counted in
the "Police:" line; the value is the game's own and is never written). The game's own level-0 clean-up
`821D1A20` only calls off cars aimed at the local player (`820C6A58`), so when a mirrored level drops to 0, or
the target changes or goes (`DestroyAvatar` too), the host does that pass for the character's handle
(`CallOffPursuit`, on the game's vehicle list `*0x83289904` / +50308 with `821D1A20`'s skips: `825B9B10(car+50376,
0)`, `825419F0` / `825255E0` for the team's cars after it in modes 4 / 10 / 11 (3), and the helicopter leaves
with `825A3000`, its own end of a pursuit); `Forgive` also tells the team's people on foot whose target is the
character to stop (event 61, as `821D1A20` does for the player). Roadblocks (`82424708`) stay the host's. The
host sends the response to its clients with its people and traffic: the 12 groups' squad units on foot
(`CapturePeds`; seated ones with their car) and the squad cars and cars a unit drives, with their crews
(`CaptureTraffic`; `82411118` makes them off the traffic list (M), and the table scan is off during a mission).
The "Police:" line adds, on the host, the target, spawn points, pursuits steered, cars and helicopters after
it now, cars and helicopters called off, target changes, updates with the host off the roads, and the units
and cars of the response sent; "units told to stop" goes with the forgivings. The client's blocked response
runs stay in the "Own spawners" line.

Not in 2.1 yet (milestones 2-3): roadblocks for a wanted client, pickups, car hit forwarding, NPC gunfire and NPC
projectiles, weather, area ownership for players spread across the city, bandwidth scheduling for 12 players.
