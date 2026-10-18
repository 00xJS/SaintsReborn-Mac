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
| Missions, cutscenes, HUD, pause | v1's host-driven system, generalized to N; other players as arrows on the minimap and the pause map, a colour per slot (2.1.4) | v1 | v1 |

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
in the log), 2.1 bumped it to 3 (a 2.0 game can't join), 2.1.1 to 4, 2.1.2 to 5 (2.1.3 and 2.1.4 keep it), 2.2 to 6
(section 11).

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
did, H). A copy without its physics set up (byte +50300, far off or parked: `82542650` returns at once then, and
the game's own MP wreck client `8238BDE0` doesn't set it up either, H) is tried 4 times (1 s), then waits quietly
and is wrecked without the explosion the moment its physics is set up (2.1.4; before, it was tried 4 times a
second for as long as it lasted). One of this game's own cars driven from another game is blown up with `825397E0` as before (3 tries 1 s
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
Car crews are passive like the people (`MakePassive`); a crew member getting out on its own is counted and its asker logged (2.1.1 refused it; 2.1.3 lets it through again: the first asker seen was the game's own character removal 824789A8, which takes a human out of its car before it goes)
(below; 2.1 logged and counted it).

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
player, so the game's lock, delays and queue apply. The record's flag byte (+12) means witnessed: when it is 0 and
the activity's entry has Check_Detection (byte +16 of the 20-byte sub-record at 0x82B2BAC8 + activity*100 +
entry*20, with +0 Points, +4 Delay, +8 Min_Level, +12 Max_Level; the entry is the gang's own for a gang team, 4 for
everything else), `821D2840` runs the detection test `821D1F98(local player, entry)` as the record is applied, 500 ms
after the crime: a living member of the entry's team (5 for the police) on the people list sees the player within
8 m unless it is behind it, within 40 m in its front 120-degree cone, or within 40 m of its car, through walls
(ai_rank_attribs.xtbl, no line of sight). An unseen crime gives a quarter of its points (at least 1), a seen one at
least what its Min_Level takes (H). Since 2.1.2 the host judges this for a client: it runs the test on the
client's character when it forwards a crime and sends the flag set (`WitnessedFlag`), and tells each client
whether a cop sees its character (`kEventSeen`), during which the client queues its own crimes as witnessed (below). `824828C0` (carjack) and `82482290` (a cop sees a gun fired)
put their record in the queue themselves (H), so their record is taken back out after the original (with the
queue full, 30, they would apply it to the local player at once: another player's is lost and counted instead).
No double credit: a player's own game counts its melee hits and run-overs on copies (`82481D50`, from `82457900`
/ `82451C98` / `8256A138`) and its shots near them (`82481EE8`, from the bullet's `824581C8` before its damage
call, so a copy's refused damage doesn't skip it; H), so hit credit with another player's character as the
attacker is dropped, and so are their carjacks (the jacker's game sees the pulled-out driver's team on the copy;
its own queued record is logged, "Crime queued here by this player", since `824828C0` never reaches `821D3148`);
kills, government-car crimes, cop sightings and cops' alerts (`820DB9D0`, 2.1.2) are forwarded; a police car
taken (`82482400`, 2.1.2) is dropped like a carjack (the thief's own game counts the act: see 2.1.2 below); on a client, kills of copies and sightings by copied cops are dropped (the host's game sees
them). Every caller of `821D3148` and of `82481CD8` is logged once with
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
taken / old, credits sent / received / dropped / refused / lost (queue full), since 2.1.2 the crimes seen by this
game's cops and this player's own queued witnessed, arrests sent / applied / refused, forgivings and skipped decay
steps.

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

**2.1.1** (after the first 2.1 test, 2026-10-16). A car a script or cheat makes for a client (`82569700` with source 7
or 3; not co-op's own copies, `making_car`) is that player's own (`personal_cars`, at most 8, the oldest forgotten,
forgotten when the game removes it) and the client's removals of its own parked and traffic cars while it shows the
host's world leave it alone (the mod menu's car was deleted before it showed); the other players see it as any
player's car once it is driven. A client's hits on the crews of the host's traffic (drivers, passengers, seat
stand-ins) are zeroed there like hits on its people and sent to the host as the car's handle and seat
(`kEventCrewDamage`; the traffic record names their models, not handles), where the real crew member takes the hit
from the client's character (`824470D0` under the host's own `DamageHook`), so it can die there and the kill is
credited to the client through `82482098` (2.1 let the copy take the bullets and die on the client alone: the host's
cop drove on and the client's game asked to remove the body every frame). A crew member's own exit request on a
client was refused in 2.1.1 and 2.1.2 and is let through again since 2.1.3 (the first asker seen in a test was the game's own character removal 824789A8, which takes a human out of its car before it goes; refused, a deleted character stayed in the seat) - the refusal was safe as far as the return value goes (r3 = 0: `8257B8E0` returns no value of its own, r3 is whatever its last call left, 0 from
`82448010` when the character isn't in a car, and no caller reads it; the generic exit `8244D5F8` passes it through to
nine callers that read none), except the driver this player pulls out and co-op's own replays; the asker behind
`8244D5F8` is logged from its 128-byte frame (the request at r1+80, the saved LR at r1+120). The get-up spots
(+792/+808) are set where the owner stands, before each knockdown and right before each get-up, when the character
lies over 3 m from them (the client's car ploughed the host's ragdolled character 42 m along one screen, and it got
up there): the game's own sub-state-16 pass (`82443808` -> `8243C780`) lerps it to those spots. The "Police:" line
shows the police points (entry 4's +12) with each game's levels, so credit short of a star is visible (the first
test's crimes were all Check_Detection ones and none was detected on the client). Seating another player's character
is retried every 500 ms; from the second refusal in a row the character is reset first (out of any car with `8257C400`
mode 19, its current action ended with `82445778`, placed at the car - what the game's own seat path does) and after
five refusals it is made again; and a client moving its own player over to a passenger seat (the car's own game keeps
its driver) takes it off the wheel with `8257C400` mode 19 first, while the car is slow, because `8257B2C0` refuses
the local player a direct seat while the driver's seat holds anyone, itself included (`82576D20` at `825771E8`; a
fast car keeps it at the wheel). Known: why the host refused the seat in the first test is not pinned yet; the
refusal line names the state (`seat N refused: +508, state, +780, action, melee, weapon, car/seat, +2569; car: list,
+120, +53976, ai, the seat's occupant and reservation`).

**2.1.2** (2026-10-17, protocol 5). Detection by the host's cops. A client's crimes gave no stars in the first 2.1
test: the game credits an unseen crime a quarter of its points, and its test runs in the game applying the record -
the client's, whose cops near the host are passive copies (whether those pass the test's own skips is open; the
"crime detection" lines show it). Now the host, which has the real cops, runs `821D1F98` on the culprit's character
when it forwards a crime whose flag is 0 and whose entry has Check_Detection, from the hook's own context
(`GuestScratch scratch(c)`), and sends the record's witnessed flag set when it holds (`WitnessedFlag`, in `CrimeHook`
and `RunQueuingCrime`); the client's `821D2840` then skips its own test. For the crimes a client credits itself (its
hits and carjacks with a civilian victim, flag 0) the host tells each client whether a cop sees its character
(`821D1F98(character, 4)` every 250 ms in `UpdateNotoriety`; `kEventSeen` on change and once a second), and for
1.5 s after a positive the client queues those witnessed (`CrimeHook` sets r5 = 1 before `821D3148`;
`RunQueuingCrime` sets +12 of the record just added), only in the host's world (`OwnPoliceOff`): far from the host
its own cops judge. Two more queuers are hooked: `82482400` (a police car, +53992 == 5, taken by a player-type
human, from the seat completion `825786A0`: government car theft queued for the local player whoever sat down) is
dropped for another player's character like a carjack: the thief's own game already counts the act (a pull-out
there is a carjack, after which the car's 0x200 bit blocks its own `82482400`; a copy whose driver copy sat at the
wheel carries team 5 in +53992, written by `825786A0`), so forwarding credited one act twice (found in review); a
never-crewed police-car copy counts nowhere, accepted. And `820DB9D0` (a cop's police alert or a gang member's gang alert at
a player-type subject, the handle at event+36, flag 1, queued for the local player; never raised by copies, whose
+3692 bit 0x08 is set) is forwarded to the subject's player. `821D1F98` is hooked for a diagnostic: "<who>: crime
detection: seen / not seen (entry N)" with the nearest member of the entry's team here (handle, distance, on foot
or in a car, +120, +2996), 20 lines. The Police line counts other players' crimes seen by this game's cops and this
player's own crimes queued witnessed (with the tellings sent and taken).

**2.1.4** (2026-10-17, protocol 5 kept). How other players' characters move. The 2.1 tests' "floats" lines (a
character placed on its path every update and not moving by itself for up to 14 s while its player walked with
the gun up, sliding with its legs still) came from the way it was moved: a move order (`82441670`, `OrderMove`)
along the path. Its state `+516 == 2` is not an aiming or strafing state but "walking a move order": `82441670`
ends with `82441308(h, 2, 0)` and the state-2 handler `82464DF8` is the path follower (H); the walk, strafe and
aim-walk animation under it comes from the same table the player's own locomotion uses (`8244E018` -> armed
`824342B0` / unarmed `82434D48`), but the layer never got going on the character (which step stops it isn't
settled; `notes/coop-2.1/aiming_movement_investigation.md`). The game never moves a player that way: the local
player (`82168D60`) and multiplayer's remote players (`8237B7B8`, from the snapshot apply `823AFA88`) are moved by
a stick input every frame - the point to walk to 5 m ahead in `+464` (`8216C9E0`), state 1 and
`8245FC08(h, f1 side, f2 forward, r6 sprint, r7 standing)`, which picks the gait from the stick's magnitude (walk
up to 0.81 squared, run above, sprint by flag), plays the aim-walk and strafe animations with the body facing the
aim (the camera rows `+4148`, which `WriteRemoteCamera` sets from the pose) and the run turn-around on a reversal
of the stick (H). `DriveAsPlayer` (from `Follower::Drive`, for other players' characters only, after `MoveHuman`,
which stays the position authority; its teleport `8243E6F8` writes `+464` too) does the same every update: it
cancels a running move order with `82441308(h, 0, 0)` (the game's own cancel, which frees the path), writes `+464`
= the path position + 5 m along the path velocity (the position itself when standing), the stick in the owner's
camera frame (its right and forward rows flat: `+3028` / `+4424` forward, `+3032` / `+4420` sideways, as
`82168D60` stores them; 0.6 for a walk, 1.0 for a run or sprint, from the path speed with the old gait hysteresis
and the 150 ms stop debounce), sets state 1 when it is 0 and calls `8245FC08`, all under `replicated_depth`.
States 3-8 (car walk, jump, climb, landing, knocked down) are left to their own handling, as are the gap
placement, the knockdown, get-up and car paths, and the host's people copies (move orders as before). `mod.ini`
`walk = 0` goes back to move orders. Diagnostic "<who> walks here (update N): ..." at the first, third and tenth
update after each start (10 lines per character): state, mode `+508`, gait `+512`, the layer-0 action `+576` at
its weight `+580`, layer 1 `+764` (the turn-around 335/336), `+756`, the gun, the stick and its band angle and
`8245FC08`'s r3. Expected: `+516` 1, layer 0 an aim-walk (147-156) or walk action at 1.00 while moving, and no
more "floats" lines (that line keeps watching). Also in 2.1.4: only the sync knocks another player's character
down - `KnockdownHook` dropped an avatar's knockdown only when `!replicated_depth`, so a replay (the host replaying
that player's own rocket's explosion) knocked it down while its player stood and fired there (55 of their shots
refused in the second test); now only `SyncKnockdown`'s two `824497F0` calls pass (`knocking_avatar`). The Shots
line counts the refused shots by reason (lying, not set up, other gun, no ammo, fire refused, gun raising, in a
car, other: `Remote::Shot::held`, the last reason a shot waited for before the 750 ms ran out), and a wreck
replayed on a copy without its physics set up waits quietly for it (the car damage paragraph above). Three more
2.1.4 fixes from the second test. (1) A client's hits on the host's traffic crews land now: the host resolved the
seat with the car's own seat slots (`+52320`, `OccupantHandle`), which are empty for most AI crews - the reason
`CaptureTraffic` names the crews by every person's own `+2496` / `+2500` - so all 352 of a test's crew hits were
dropped without a counter; `SeatOccupant` tries the slot, then that scan, and the "Copies" line counts the hits that
found no one in the seat, no such car, or a player there (the first 20 empty-seat hits are logged). The same slot
test picked the car a response unit drives for `CaptureTraffic`; its own `+2500` comes first now. (2) The host's
mission-end message is forwarded only while a mission runs (`MissionActive()`, `0x82B3106C`: `82215360`'s own
first test); the player's death calls it "failed" in free roam too, and every death of the host showed "Host's
mission failed" on the clients (a capped log says when it isn't forwarded). (3) The Police line's "helicopters
after it" and the helicopter call-off test the helicopter AI block (`+54840`) for mode `+684` 1 as well as 2, with
`+160` the handle: 2 is the pursuit (`8259DCC8` / `825A0560`), 1 is over the target (`825A0F18`, reached from the
pursuit update `8259DD80`, which keeps `+160`), and `825A3000`'s mode 4 (target gone, or called off) leaves `+160`
as it was (H); the test counted mode 2 alone and showed 0 while four helicopters attacked. The cars' test
(`+50376` 4 / 10 / 11 with `+50468` the handle) matches `825A91A0`'s stores (mode 4, sub-mode 17, `+92` = the
order's handle).

The people-model references on a client (a leak since `6c88c8a`, the first `CreateActor`). Every character co-op
makes - a copy of one of the host's people, a stand-in, a traffic driver or passenger - took one streaming
reference of kind 5 on its model (`8250C750(group, id, 5, 0, 1)`, as the game's own network NPC creator `823A7DB0`
does) and nothing gave it back: no human destroy path in the image releases a people-model reference (the three
`8250CA48` callers are the population sets' `82419C08` and the vehicles' `8247BA28` / `825324D0`; H), so a model's
count grew by one per character made with it (BF 1 -> 139, 120 1 -> 221 in 12 min of the second 2.1.2 test) and only
`ClearUnusedModelRefs` (nobody wearing it) ever brought it down. The harm, in two sentences: a request with a count
never dies, and a kind-5 request paired with a slot in the group's pass (`8250D3E0`) evicts that slot at once, so a
dead model's leftover request could take the slot of a model thirty copies wore and park them invisible
(`8247AA70`). A leaked, worn model also kept its request, slot and queue place for the whole session. 2.1.4 keeps
character handle -> model id and reference (`actor_models`: `NoteActorModel` after each `CreateActor` and after the
stuck copy's re-request, which starts over since the first reference went with the dropped request) and releases
exactly one `8250CA48(group, id, 5, 0, 0)` per reference in `DestroyObject` and `DestroyActor` (`ReleaseActorModel`),
with a 1 s sweep (`SweepActorModels`) for the characters the game removed itself; `ClearUnusedModelRefs` forgets the
entries of a model it clears. A guard every 10 s (`ModelRefGuard`, client) compares each model's kind-5 count with
its wearers plus its pin and lets any excess go (capped "Model X: N references of kind 5 more than its wearers and
pin here, let go"); the People models line counts the references given back and the excess, and the slots dump
shows each slot's level (`L`) and the per-level slot quota table `0x827ACFFC` (14 words) - the other half of the
"waiting for room" picture: the pass pairs a level's requests only with that level's slots, co-op asks at the top
level (`PinModel`, level 13) and level 13 owns only the slots `8241BD40` last moved there (8, later 4-6 in the PC
logs) while the rest sit idle at levels nobody asks at (H for the mechanism, M for the numbers over time). Not
changed in this build, the next step once the counts are flat: ask at level 0 like the game (and give level 0 all
12 slots in the table) or call the game's own quota recompute `8241BD40(0,0,0)` each second. The car copies' model
references have the same shape (`CreateVehicle`: a direct `8250C750` like `823BA888`; the game's own cars go through
`82532360(v, kind)`, which notes the kind at `v+50304` for the destructor `825324D0` to release): measured only, in a
10 s "Car models here" line (references taken by copies against the vehicle group's kind-5 count) and a capped line
with `+50304` of the first copies (-1 expected). Review fixes on the 2.1.4 batch: `DriveAsPlayer` runs only on
characters from the player pool (`IsPlayerObject`: the stick block at `+4420..+4448` lies past the end of a
4252-byte NPC human; an NPC-class character shown for a player keeps the move orders); the owner's replayed
`82441308(0/1, 0)` no longer holds such a character for 120 ms (it is in state 1 by itself); the stop debounce keeps
the last direction of travel instead of stepping toward the camera forward; the Car damage line's "cars waiting for
it now" is counted at print time (copies with 4 tries without physics and no wreck bit); each character made gets
its 10 "walks here" lines; and the crew-hit seat lookup is remembered per car and seat for 250 ms (one 4096-slot
scan per burst of hits, not one per hit).

Other players on the maps (2.1.4). The other player showed on the minimap as a purple dot, like a homie, and not at
all on the pause map: the minimap's people pass `822E8D00` draws every human on the people list by its team `+232` -
a player-pool character has team 0, which gets icon [4] `map_other_blip_revive` through `822E8C78(pos = obj+20,
params)` - and the people, car and follower passes are called from the HUD minimap draw `822ECDB0` only, so the
pause map (`822B3F70`, the render of the MAP tab's view `0x8206169C`) shows the icon lists, the route, the waypoint
and the player's own arrow and nobody else (H). Now every other player in the session is an arrow in their slot's
colour on both maps, drawn the way the minimap draws homies (`822E8FE8`: `map_blip`, HUD item `0x83023660[9]`,
turned by the heading - `atan2` of the forward row's x and z, as `8262EB58(obj+32)` - plus pi, `8227E720(bitmap,
x, y, angle, scale, params)` with the colour from `8263E7D8(r, g, b, a)`) at the player's own size (homies are 0.8 of
it): a hook after the followers pass projects each position with `822E8A78(&xy, &aux, pos, 0)` (a point past the
minimap's radius is put on its edge, so a far player sits at the rim), adding the map's turn `*0x8370E438` while the
map doesn't turn with the player (byte `0x8370E407`), and a hook after `822B3F70` uses the pause map's own transform
(block `0x8370DD60`, as its icon pass `822EDC50` and its arrow: `px = +1072 + +1080*zoom + (+1104 - x)*zoom`, `py =
+1076 + +1084*zoom + (z - +1108)*zoom`, zoom `*0x827AF7B4`; the page's x runs against the world's) with its arrow
bitmap `0x8300FC78[0]` at `HUD scale * (clamped zoom *0x827AF7A8 * 0.333 + 0.667)`, kept on the page by hand (the
game sets its scissor back to the whole screen before it returns). The position is their character's `+20` while it
exists here, else their newest pose - so a player beyond 130 m, not yet made, hidden in a cutscene or dead is on the
map where they are or were last seen - with half alpha while they lie or are dead (`MapMarker`, filled by
`UpdateAvatar` from every pose, cleared when the slot leaves); the character's purple dot is skipped (`822E8C78`
returns without drawing when the position is an avatar's). Nothing is added to the icon lists, so no mission clears
it and no icon slot is used. The hook on `822B3F70` fires although nothing calls it directly (it is reached through
the view descriptor's pointer): the loader detours the host function (MinHook), as it does for `824789A8` and
`825324D0`, which have no direct caller either. Log: "Map markers:" once per map (the bitmap handles, HUD scale,
the rotate byte, MP blip 108's bitmap, the zoom and page), "Map marker: Player N from their character here / their
last pose" on each change of source, and a 10 s "Markers:" line (arrows drawn per map, how many at the edge,
purple dots skipped, failed draw calls).

Not in 2.1 yet (milestones 2-3): roadblocks for a wanted client, pickups, car hit forwarding, NPC gunfire and NPC
projectiles, weather, area ownership for players spread across the city, bandwidth scheduling for 12 players.

## 11. Co-op 2.2: the shared area mirrored (in progress)

Goal (the user's): players who share an area see the same cars, people, cops, actions and effects, the way the game's
multiplayer mirrors its world, instead of 2.1's streams with their 90 / 200 / 250 m gates and fixed-size packets
(which dropped and re-made about 8 cars and 20 people every 10 s on a two-player test). The model is the MP's (research
Q 2.1): every object the authority simulates is created once on each member (reliable), updated by priority within a
byte budget per member (unreliable, newest wins) and removed once (reliable); the member decides what it can hold and
says so. In 2.2 the host is the owner of the one area; a client is a member of it or runs its own world (solo), as
in 2.1. Package A of the round is built in steps; steps 1 to 4 are in, Package B/1 (content around every member) and
Package C (the area's actions and effects: gunfire, driver inputs, burning, explosions; with A/9).
The 2.1 streams are kept for players with mod.ini `world = 0` (switchable for the tests) and go to nobody else.

**Step 1: `world.h`** (no game code; `tests/world_test.cpp`). The records: `WWS1` updates (25-byte header with the
owner slot, the OwnerMap epoch, the owner's clock and a reference point; `CarRec` 29 bytes (30 with Package C's drive byte): id, vehicle-table index,
position in 2 cm from the reference point, smallest-three quaternion, velocity 1/128 m/s, angular velocity, the
damage / siren bits of 2.1 and the hit points; `PedRec` 20 bytes on foot: position, heading, flags (orientation sign,
mission, dead, crouching, ragdoll, burning, hidden or cutscene, seated), state `+516 << 4 | +520`, animation set,
action, velocity, team, gun in hand; `SeatedRec` 12 bytes: id, car id, seat, team, gun, flags), `WWC1` creates
(kind, authority, model, team, name, the look block `8238B0B8` writes, the car paint, a car's crew ids and the first
record with the reference point its position is from), `WWR1` removes (fade time, reason: gone or the area left),
`WWA1` member reports (cars and people held and the caps, the people models resident, the loaded chunks, the ids
declined, the copies shown within 50 m of its camera), `WWD1` set digests (the sorted ids a
member was offered, every 10 s) and `WOM1` (the OwnerMap: per slot its area and owner). The `Registry` (keyed owner
slot + handle, idempotent adds, removals queued for the members the id was created on) and the `Scheduler`: per
member and 50 ms tick the MP's priority (-10 a metre, +500 in the view cone or within 10 m, `82353DB0`'s constants,
plus the age), bands by distance to the member's camera (within 50 m 20 Hz, 150 m 10 Hz, beyond 2 Hz; parked and
resting cars and seated people 2 Hz unless something changed), shares of the budget per band in proportion when it is
short, a fair queue inside a band so nothing starves, at most 4 creates a tick (a car before its crew), no records for
a car a player drives (`kEntryPlayerCar`, or another player's authority), declined ids offered again after 3 s when
the member has room, and the digest. The budget per member is `world_upload_kbps` shared by the members, 6-24 KB/s
each, halved for 2 s on backpressure. Tested: 12 members x 200 objects keep 20 / 10 / 2 Hz within 50 / 150 m / beyond
at 1200 bytes a tick and slow every band alike at 570, never over the budget, deterministic.

**Step 2: the owner side** (`coop.cpp`, "The shared area mirrored" next to the network thread). Every game keeps a
registry of the cars and people its own game makes, fed where the game makes and removes them (the MP registers its
objects from the constructors through `82352D38` when both MP bytes are set; co-op hooks the same points and never
sets those bytes, H):
- cars: `82569700(source, ...)` after the call (`VehicleCreateHook`). The source at its 17 call sites (H): 2 traffic
  (the traffic creator `82411310` passes 4 for its police cars), 4 the response (`82411118`) and the gangs' cars
  (`825611D8`), 5 parked (`8241E378`), 6 special (`824224F0`), 7 scripts and cheats, 3 co-op's own copies
  (`making_car`; the MP's creators don't run in story mode) and 9 the El Train (`8242D248` under the train manager
  `8242EFB0`), which is left out and counted (trains as records are later). Cars a script makes during a mission are
  left out too (each game's mission makes its own).
- people: `8247A6D0(human, -, park)` after the call. It is the activation (H): the human goes on the active list
  `0x8309ABB8` (+3592/+3596) when `+200` bits 0 and 1 are set, else (r5 = 1) on the inactive list `0x8309ABB0`. It is
  called by the constructor `82477B78` (only when `+216` 0x4000 says the model is set up, else `8247A8E0` parks the
  human inactive), by `8247A9F8` - the people stream group's model-loaded callback (`+140` of its config in
  `82185498`), which activates every inactive human of that model - and by `8240B228` for queued mission
  characters once their model is in. So the same human comes again whenever its model is unloaded (`8247AA70`) and
  loaded again: the add is idempotent and counted as a re-activation. The kind is `+4216`, written before (report
  L: 1 ambient, 2 traffic crews, 4 police response, 6 special, 8 hood, 0/3/7 system and script characters). Co-op's
  own copies (`making_actor` around its `82479AB0` call, and the copies by handle), the players' characters
  (player-pool objects) and the test characters are left out.
- removal: `824789A8` (`HumanDestroyHook`) and `825324D0` (`VehicleRemoveHook`), when the game really removes the
  object. A fade-out the game begins is a removal at once, with the time it has left: a car's `82537D40` sets `+53976`
  0x40000000 with the clock `+54436` and the length `+54444` and fades every occupant with it; a person's `824517A0`
  sets `+212` 0x80 with `+2488` / `+2492`; both clocks are `*0x827AA6E0`, less 1800000000 past that (H).
- the registry is filled from the object table when the session starts (cars named by the traffic list, the parked
  records at `0x83AD346C`, the response squads, an El Train car by its vehicles.xtbl row 65; people only once
  active) and checked against it every 10 s ("audit": what the hooks missed both ways; it should stay 0).

On the host, 20 times a second (`WorldOwnerTick`): the members are the clients whose pose says they show the host's
world (`kPoseHostWorld`, 2.1's rule for the host's police: the client shows the host's traffic or people - from 120 m,
kept to 200 m - and the host's character, made within 60 m and removed past 130 m, held 2 s; with mod.ini police = 0
the bit is never set and nobody is a member yet); their camera from the pose (`eye`, the forward row of `cam`). The
registry's state is read from the game: positions; the car's chunk bit (`+120` 0x04000000, what
`vehicle_in_loaded_chunk` `824DAB38` reads: a car whose chunk unloaded is parked without physics and doesn't stream);
resting (slower than 0.3 m/s or no physics: 2 Hz) and a change of hit points, damage, siren or driver; a person's
activity (`+200`), seat (`+2496`/`+2500`: a seated person is a crew record of its car), and the rules 2.1 kept: mission
and script characters stream only while a mission runs, the story characters' models (Gat, Dex, Julius, Troy) never
on the street. Authority (F2): a car a player sits in or is getting into (this player's `+2496`, another player's
character's car or the car it is getting into) is theirs: its pose carries it, so no records go for it, and the
area's members are told with `kEventAuthority` (owner, id, the player's slot, 1); it comes back to the host's game
1 s after it rests with nobody in it (`kEventAuthority` ..., 0). It is never removed for that. Then `Scheduler::Plan`
with each member's budget (for the time since the last tick) and the stream point (the camera `0x827D9778` +44, where
the game's own stream point `82485DB0` starts and what its despawn radii are measured from), and the messages:
creates with their tail (a car's paint read again for each, `ReadCarLook`; a person's name and look block, `ReadLook`),
removes, the records packed into `WWS1` messages of at most 1146 bytes (each object described once a tick, a record
past 655 m skipped and counted), the digest every 10 s and the OwnerMap (reliable when someone joins or leaves the
area, else once a second). The network thread sends them in order, before the events. Backpressure: a member's link
whose unreliable backlog stays past 16 KB after a flush, that drops snapshots (`Session::LinkStats` per link, new in
`net.h`), or Epic's outgoing queue past 16 KB halves that member's budget for 2 s. A member's `WWA1` is taken (what it
holds and declined). The host still sends the 2.1 streams.

After the step 2 review (notes/coop-2.2/review_A2.md), with step 3:
- an entry that stops streaming after its create (its chunk or model unloaded, a mission person after the mission)
  stays in the digest and gets no records; after 5 s it is removed (reason 0) where it was created and created again
  when it streams (a model reload, under 5 s, costs nothing) (A2-2, world.h, world_test);
- the owner tick runs on a 50 ms grid that catches up (a 30 fps host ticks after 66 and 33 ms: 20 a second) with the
  plain budget per tick; the owner line's rates are sends per object per second over the ticks that ran ("t ticks")
  and the budget is what the member had over the window (A2-1);
- backpressure also when a link's reliable bytes not acked pass 16 KB or its RTT passes twice its lowest of the last
  10 s (at least 250 ms); Epic's queue and the link counters start from 0 each session (A2-3, A2-6);
- a Stop or a lost host turns the registry off; a client's own registry no longer keeps removals nobody takes (A2-4,
  A2-5);
- the audit and the activation hook skip objects queued for deletion (`+120` 0xC0000000, set by `823FE4A8`; the object
  stays in the table until `823FDBE8`) (A2-7); `kEventAuthority` goes only to members the car was created on (A2-8); a
  passenger or a player getting in takes a car's authority only while nobody but a player is at the wheel (A2-9); a
  handle still here 30 s after its fade began is counted when the audit adds it back ("back after a fade", A2-10); a
  car a script made during a mission is streamed once the mission is over, and only with `missions = host` (A2-11);
  the activation hook counts a re-activation only for a person active after it and logs which callers reach it
  (A2-15); "the first member in the area" once per session (A2-18).

**Step 3: the members' side** (`coop.cpp`, "The shared area mirrored (co-op 2.2, step 3: the members' side)"). A
client shows the host's world stream while it is in the host's area (mod.ini `world = 1`, the default; with `0`, or
while the host streams nothing - no OwnerMap from it for 3 s, the host's `world = 0` - 2.1's streams show the host's
world as before):
- membership: the client asks for the area within 90 m of the host's character and leaves it past 140 m, after 2 s /
  5 s (`kPoseHostWorld` in its pose: the spec's Package A numbers; step 4 moves the rule to the host); the host's
  OwnerMap says who is in. While in it, and until the last copy here is gone, this game's own traffic, people (and
  their models' own references), parked cars and police stay off as 2.1 kept them off (UpdateSharedTraffic /
  UpdateSharedPeds / OwnPoliceOff, now keyed to the area), and none of 2.1's copies are made (those there go, two cars
  / three people an update);
- one table of copies keyed (owner slot, id) with a reverse index by this game's handle, next to 2.1's maps; the
  predicates the hooks use (`IsSharedCarHandle`, `SharedCarHostHandle`, `SharedPedHostHandle`, `CrewSeat`,
  `OurCharacter`, `FindCar`, `CarId`...) know its copies, so a copy is protected, named and driven as 2.1's were: a
  player who gets into one gets into the host's car there (its pose names `(0, id)`);
- admission is the member's (spec 3.6): cars up to 32 less this player's own, one per other player and 4 spare (26
  with two players), people up to 56, made only within the live counts (vehicles `0x83710238` of 32 less 2, humans
  `0x8309ABAC` of 75 less 8; fading copies keep their place until they go, and a create waits for room, declined
  after 3 s); a person whose model isn't loaded here is declined while the models the people copies here want fill
  the people group's slots (12, one kept for the players); when full, a new copy nearer the camera than the farthest
  held one of its kind by 20 m (people 10 m) takes its place (that one fades, its crew with it), else it is declined;
  a crew member goes with its car (declined when its car isn't held, made after it); a car a player drives (the
  create's authority bit) is always taken, and a car copy a player's pose made before it came is taken into the table
  instead of a second car. Declined, evicted, lost (removed here by the game) and missing ids go back in `WWA1`; the
  host offers them again after 3 s while there is room;
- creates, 4 an update at most, with co-op's own sequences: `CreateVehicle` (and its model at the top priority) with
  the paint, `CreateActor` with the look block once it is set up (hidden until then, 3 s at most) and the host's team,
  crews seated with `SeatInVehicle`; a driver whose model hasn't loaded after 2 s in a moving car gets a lookalike
  (`LoadedLookalike`); a person not set up in 20 s is let go (offered again);
- updates: per id only a newer owner time; records before their create are held 1 s, after a remove dropped (5 s),
  for declined ids dropped, from before the OwnerMap's last change (its epoch) dropped. Cars move the way the game's
  MP frame moves a remote car (`823BB068`, H): the body (`+244`) is woken with `826CD960` - not a velocity setter: it
  takes only the body, returns at once when the body's island (`+92`, byte `+40`) is active or the body is fixed
  (`+153`), and otherwise puts the island on the world's active list (`826DDE40`; with no island it would write
  through 0, so co-op calls it only for an island that sleeps) - and the velocities are set through the body's motion
  (`+88`): vtable `+88` the linear, `+92` the angular velocity, 16-byte vectors, as `823BB068` does after each of its
  two `826CD960` calls. The velocities steer the car towards its path (2.1's error terms); it is placed (`82535C68`)
  only when off by more than 8 m + 0.35 s of its speed or turned by about 60 degrees, and left asleep when it rests
  where it should. A car without physics (`+50300 != 1`, which `823BB068` leaves alone) is placed with `82535C68(v,
  pos, rows, 1)`, which for such a car writes `+20`, `+32..+64`, bit 0x80 of `v+53984` and places its trailer
  (`+54628`) with it (H; review A3-9: the direct write left the trailer behind), at most twice a second unless the
  record moved.
  Hit points, smoke, fire, wreck and siren through `ApplyCarDamage` / `ApplyCarSiren`; a car a player drives (here, a
  character here, or named by `kEventAuthority`) gets no records. People follow 2.1's path follower (the same
  per-frame smoothing), with the host's flags (crouching; hidden or a cutscene actor there: hidden here, never
  removed; hidden too while a cutscene plays here), dead from the owner's hit points (killed as 2.1's copies), team,
  animation set, the gun in hand (`MatchWeapon`), seated or out of a car as the owner says (not while a player pulls
  them out here);
- removes fade the copy (cars `82537D40(v, ms, 1)`, its crew with it; people `824517A0(h, ms, 1)`, H) and co-op
  destroys it at the end of the fade, 4 an update (the game's own deleters never reach a copy: the traffic
  housekeeping `82410B80` walks the traffic list only; `HumanDestroyHook` lets the corpse clean-up's ask through for a
  fading copy, spec C2); a copy not made or not shown goes at once; a car a player sits in or gets into is let go (it
  stays as that player's car, named as the host's). Leaving the area (the OwnerMap, no OwnerMap for 3 s, the session
  over, the host lost) fades everything;
- the host's person events (actions, knockdowns, hit reactions, effects) and this player's hits on the copies work on
  the world stream's copies as on 2.1's (the host counts the people of its world stream as sent); a hit on a crew
  member goes as `kEventPedDamage` with its own id (not by seat);
- `kEventAuthority`: a car a player drives gets no records here until the host says it is back with its game;
- `WWA1` every second while in the area, and 200 ms after a decline; the digest is diffed against the table: missing
  ids go back as declined (the host creates them again), extra ones are removed.

Logs (every 10 s on a member):
- `World (member of slot 0's area, epoch e): held cars a of o offered (declined d: reasons), people ...; now cars,
  people (queued, fading, driven by players); copies made / faded / destroyed (at once, past deadline), let go (their
  crews), gone here; stand-ins, waiting for a model, models loaded l of the people group's 12 slots (11 for copies;
  player bodies, waiting); creates applied (held for a car,
  replaced), updates applied (old, before the create: used / expired, after a remove, declined, old epoch), removes
  (gone, area, digest); placements p per car per second (placed, steered, woken, written without physics); crews
  seated (refused), out of a car; authority taken / back; bytes in; malformed`;
- `World set match: held h of n (declined d, model-wait m, chunk c) = x%; missing k [ids]; extra k [ids]` (each digest);
- `World update age: p50 a ms, p95 b ms` (apply time less the owner time, less the window's least receive delay);
- `World match: within 50 m of me the host lists cars c / people p, I show c' / p' (fading, waiting, declined)`;
- `World models: per id refs (pins, copies) ...; n with more refs than pins + copies + 1`.
One-shot: the first create and remove applied, `full: cars 26/26, declined id` (20), area joined / left (20), the
chunk list (`World chunks here`: `8211E380(id)` says a chunk is loaded when its record's `+20` bit 0x80 is clear, H:
the mask's bit is the list index), `Copy never finished loading` (25), authority changes (30). The owner adds a
`World (owner) match slot S` line from each member's `WWA1` (two clocks). Pass numbers: spec section 6.

Measured offline (world.h with a two-player street: 12 traffic cars with drivers, 6 parked, 26 people, churn of a car
and two people every 2 s; budget 1200 bytes a tick): about 11 KB/s per member with every moving object at its band's
rate (20 / 10 / 2 Hz), resting ones at 2 Hz, 3 creates a second, never over the budget; 11 members at 568 bytes a tick:
10 KB/s each, 19 / 10 / 2 Hz. In step 2 that comes on top of the 2.1 streams (about 41 KB/s to each client near the
host) and the poses.

Log (every 10 s): `World (owner): area: ...; set: cars (traffic, parked, response, special, script; trains and mission
cars left out, inactive, players' cars), people (ambient, traffic crews, response, hood, special, mission/script;
seated; story characters left out, mission/script outside a mission, inactive); audit +a/-b; made cars / people,
removed cars / people (of them as their fade began), re-activated people` (the host's own churn: the baseline the
members' numbers are judged against); per member `World (owner) slot S: offered (cars, people), held (from WWA1),
declined (re-offered), created (KB; catch-up left, deferred for the budget), removed (gone, area), near / mid / far Hz
of the moving objects (resting apart), records in messages, bytes of the budget (and the plan's own count),
backpressure halvings, digest (ids), records skipped (past 655 m), WWA1`; `World (owner) authority: n cars driven by
players now (slots), taken, returned`. One-shot: `World registry on` (the count at the start and the MP net time
`*0x83061944`, a probe), `World (owner): the first member in the area` (what it catches up), `slot S joined / left
the area` (40), the authority changes (30). A client: `World (this game's own, a client): registry; ...` and
`World (member, 2.2 step 2: counted, not applied yet): from the host n update messages, creates, removes, digests,
owner maps, KB/s`.

Protocol 6 (2.1 can't join; 7 since the review fixes below): message types `kMsgWorld` (WWS1), `kMsgWorldCreate` (WWC1), `kMsgWorldRemove` (WWR1),
`kMsgWorldAdmit` (WWA1), `kMsgWorldDigest` (WWD1), `kMsgOwnerMap` (WOM1) after `kMsgNotoriety`; events
`kEventCarDamage` (25, the members' side) and `kEventAuthority` (26). The session's relay needed nothing new (the host
sends to one slot; a client's message for the host stays there). mod.ini: `world_upload_kbps = 1000` (each member
gets at least 6 KB/s, more than the setting in all when many share it, and at most 24 KB/s) and `world = 1` (the world
stream shown on a member; 0: 2.1's streams, and on the host no world stream; set it the same everywhere). `WWC1`
carries its reference point (39 bytes), `WWA1` the near counts (26 bytes).

**Step 4: the cut-over, kept switchable** ("The shared area mirrored (co-op 2.2, step 4: the cut-over)").
- Membership is the host's (spec 3.1/3): a client whose pose says it shows the world stream (`kPoseWorldStream`, pose
  bit 23: mod.ini `world = 1` there and the host's world messages within 3 s) joins the area within `member_join_m` of
  the host's character (2D, `PosePacket.pos`) after 2 s in a row and leaves past `member_leave_m` after 5 s; at once
  when its pose is 3 s old, it stops showing the stream or it left. mod.ini on the host, 40-400 m, leave at least join
  + 20; 150 / 200 m with Package B (90 / 140 m for a test of A alone). The OwnerMap's epoch changes with each join and
  leave as before.
- 2.1's traffic and people streams (`CaptureTraffic` / `CapturePeds`, `kMsgTraffic`, `kMsgTrafficNames`, `kMsgPeds`,
  `kMsgPedLook`) go only to the clients that don't show the world stream (their `world = 0`, or no OwnerMap seen yet;
  sent to those slots alone, or to everyone when that is everyone), and nothing is captured or named as sent when no
  client needs them. The 2.1 code stays for that case; the owner line says per client whether they still go.
- `kPoseHostWorld` has its 2.1 meaning again ("this client's own police are off": `OwnPoliceOff`), the input of the
  host's police response. On a client in world mode `OwnPoliceOff` = a member by the OwnerMap and the host's character
  shown here (held 2 s), one membership rule. Another player's character is kept to `member_leave_m + 30` m (the host:
  a member's; a member: the host's) instead of 130 m, still made within 60 m, so the host's police can come for a
  wanted member anywhere in the area.
- A member's levers of spec 3.8/36 that were missing: `824290D0` (the spawn-region manager: hood gang members and cops
  `82427548`, the special spawns; one caller, `824131F0`; the MP client's skip) returns while the area is shown (its
  enable bytes `0x83AE3A90/91` are left to the scripts), and `spawn_trains` `0x827ACF87` (read by the train manager
  `8242EFB0` as `+10407` off `0x827AA6E0` right before it starts a train, `8242E3D0`) is 0 on a member and on a host
  with members (3.8/37: trains are not mirrored; running ones finish their route); put back as it was afterwards. The
  one-time removal of a member's own parked / response / special cars is `RemoveOwnParked`'s sweep (every unoccupied
  car of this game's own within 160 m, every 500 ms) and the response removal at `OwnPoliceOff`'s edge, as in step 3.
- The games compare their vehicle tables when a player joins (`kEventVehicleTable` 27: the row count `*0x8371023C` and
  a hash of the rows' name hashes `+120`): a difference is logged (`Vehicle table:`) and shown (review A2-12). A car's
  paint is read once a tick whoever its create goes to (A2-16); `Session::LinkStats`' reliable bytes in flight are kept
  as they change instead of walked (A2-17).
- The step 3 review (notes/coop-2.2/review_A3.md): the stream counts as there while the host is paused
  (`HostPauseHold`) and for 10 s while its state packets still come (its game thread stalled or loading), and from any
  world message, not only the OwnerMap; a member that leaves on its own says it doesn't show the stream until the
  host's OwnerMap has it out, so the host starts its offers over and the two never disagree silently (A3-1); the
  member's area, epoch, declines, tombs, early records and digest start over at each session's start and end, and an
  OwnerMap more than 1000 epochs behind is a new host's (A3-2); world.h re-offers a declined id only into the room the
  member reported, counting the tick's own creates as held until the next WWA1, doubles the hold on each repeat decline
  (3, 6, 12 ... 30 s; forgotten once a create has held 30 s) and re-offers a person only while the member can load its
  model (it has it, or fewer than 11 people models there) (A3-3, world_test 6b); a car named driven gets its owner's
  records again 1 s on (a lost "back" event, A3-4); a copy a create replaces is destroyed within the 4-an-update budget
  (A3-6); the velocity vectors handed to the motion's setters are 16-byte aligned (`lvx`, A3-8).

Logs: the owner's per-member line starts `World (owner) slot S (in the area at d m; 2.1's traffic and people streams to
it: off | still sent)`; one-shot `World (owner): slot S joined the area at d m ... / left the area at d m (why)` (40),
`World (owner): 2.1's traffic and people streams to slot S on | off (why)` (40), `World: new trains off here ...` /
`trains back`, `Vehicle table: slot S has n rows (hash), this game n (hash): the same | DIFFERENT`; the member line ends
`this game's hood / special spawn passes skipped n, trains off | as the game has them`; `World (member): the host's
OwnerMap has this game out too: it can join again` (20).

**Package B/1: content around every member** ("The shared area: content around every member", notes/coop-2.2/plan_B.md).
The engine fills the world only around the viewpoint its spawners read (parked cars 80 m in 2D from the player
`*0x8309ABEC`, traffic 130-140 m and people 90 m from the camera `0x827D9778`, research S 0.2), so on the host each
spawner also runs for one member's viewpoint at a time, its radii unchanged and its caps shared by the area (one street
of 12 cars, 26 people, 6 parked). The viewpoints: this game's camera and player, and each member's pose (eye, the
camera's forward row, position, forward speed from its car's velocity or its camera's moves), refreshed by the owner
tick. A member's viewpoint stays with its last pose while it is in the area (pose under 3 s, as the membership): every
protection below uses it, so a short gap of lost poses doesn't empty its street; only the spawn passes need a fresh one
(under 500 ms) (review AB-2). Nothing runs without members, in a cutscene, or with the host's `world = 0`; the
traffic and people passes also need the host's `traffic = shared` / `peds = shared` (H for every address below, read
in the asm):
- **Parked cars**: `8241D930` (one caller, `824131F0`) only spawns records already queued (state 2, list `0x8309A480`;
  record `+100` the state) and re-states those beyond 80 m of `*0x8309ABEC` (`r26`, read once: `+20`/`+28`) through
  `8241D210(rec, 0|1)` at `8241DA10` / `8241DA24`; `8241DC78(0)` queues new ones from the 6 nearest candidates
  `8241DAE0` picks (state 1, `0x8309A47C`; `+20/+24/+28` of the player). After the game's own pass the hook runs
  `8241D930` and `8241DC78(0)` once more for one member (each at most every 200 ms) with `*0x8309ABEC` on a stand-in
  that holds only the member's position (a human object's 4252 bytes, zero elsewhere: handle 0, type 0, so a reader
  outside the checked closure - a `bctrl`, another thread - sees zeros, not old stack; review AB-4); `8241E378` (the
  spawn: the vehicle constructor, seats, model references) and `8241D210` (model references, the car's destroy) run
  with the real player. `8241D930`'s re-state of a queued record to the candidates (state 1, LR `8241DA14`) is vetoed
  so one pass doesn't undo another's queue: in a member's pass for a record within 80 m of this game's player (its own
  street), and in any pass for one within 80 m of another member only while its chunks are resident here (`8241DA30`'s
  own test: `8250F3C8` on the chunk ids of the table `0x83E8ABD8` at the record's `+88` / `+104`; else it can't spawn
  and would hold one of the queue's 6 for good) and for 3 s at most per queueing; never with `member_spawns` lacking 1
  and never its state 0 (LR `8241DA28`: no chunk table entry). `8241D6B0`'s array (`0x8309A444`, {record, distance
  squared} x 6, count `0x8309A474`; the only input of `8241D5C0`, which makes room) loses only the spawned records
  (`+100` = 3) near a member, so a queued one can still be made room from (review AB-1: queued records near a member
  could fill the area's 6 for good, the host's street with them).
- **Traffic**: `824129C8` asks `82412298(out, &centre, &dir, f1 speed, r7 ahead)` (one call, LR `82412F30`) for spots
  around the camera; `82412298` reads no camera and the player only for its highway flag (`+21260`). On a member's turn
  the hook gives it the member's eye (written to the caller's free `r1+80`), its forward (into the caller's copy at
  `r1+96`, which `r5` points to), its speed and "ahead" (over 2.2352 m/s, or - the game's own sparse-street rule at
  `82412EB0` - fewer cars near it than a quarter of the wanted; review AB-11); the turn goes to the member furthest
  below its share of the wanted count (the cars within 140 m of each eye), not a full one (its cap spent on copies
  while it shows its share near its camera by its `WWA1`; a member full of the host's far copies still gets cars:
  admission swaps a far one for a nearer, review AB-7), the host's own at least every other tick. One car per tick
  (200-300 ms) for the whole area, as the game spawns. `82412878` (the despawn test: r4 the car, returns 1 to remove)
  keeps a car within 140 m (3D) of a member's eye.
- **People**: `8240FBB0` spawns on the updates where its timer `0x82832744` hasn't run out against `*0x827AA6E0` and
  `*0x8370FEBC` is set; on those, after it, a member below its share of the wanted people (90 m of its eye; full by the
  same rule as traffic; 10 passes a second in all, each member at most every 250 ms) gets `8240F148(1, !moving)` with
  the camera's eye (`+44`) and
  forward row (`+104`) the member's: its picker `8240EC88` and the spawn `8240DD40` read only those of the camera (and
  the view's FOV). The constructor `82479AB0` (LR `8240E178`) and the set-up `820E9550` (LR `8240E1AC`), whose callees
  reach the camera code, run with the real pose; never the make-room path (`8240E448`) for a member: the pass runs only
  below the wanted count and `8240E338` says "not removable" inside it. `8240FA90` (returns 1 beyond 90 m of the camera)
  keeps a person within 90 m (2D) of a member's eye; `8240E338` (make room's "removable", one caller) says no for one.
  A person made in a chunk the host hasn't loaded is queued for deletion inside the constructor (the region check
  `8243EB60`): counted.
- **The slot allocator**: `82400968` calls `823FFF78(sys, kind, level)` when a creation finds its pool full; it tries
  the categories of a lower priority through their evict functions `823FFA48` (people, list `+0`) / `823FFC48` (cars,
  `+4`), which take the valid record (`+12` handle, `+20` state, not a visible one in pass 1) with the largest `+24`,
  the distance squared from the host's player: a member's first. The hooks give the records near a member the lowest
  score for the call, so an unprotected one goes first; with nothing else there the creation still takes one when its
  kind's priority (`0x82038858 + 28 x kind`) is below 3: priority 0 (a mission's, a script's, co-op's copies), police
  (kind 4, priority 1) and special / gang (6, 2) (review AB-6). Only the street's own kinds (priority 3: ambient,
  traffic, parked) would be refused, and those evict nothing (plan_B 6), so in practice nothing is refused.
- **Density**: `824194A8` (traffic wanted, <= 12) and `82419208` (people, <= 26) return `max(x, min(cap, x (1 +
  member_density x members)))`: never past the game's limits, never below what the game wants (a script's density
  can take it past them), and a base of 1 or less is left alone: the game clamps to at least 1 after multiplying by
  the density (`82419304`), so a mission's density 0 (an empty street) comes out as 1 and stays 1. The member traffic /
  people passes don't run while `set_traffic_density` (`0x827AD060`) / `set_ped_density` (`0x827AD064`) is 0
  (review AB-3).
- **The host's cars another player drives** (review A2-14, the B/2 half): `82412878` (traffic far from the host's
  camera) and the parked removals keep a car of the host's another player's character sits in or gets into, in any
  session (members or not, `world` 0 or 1): removed, it came back on the host as a car copy outside the registry and
  never streamed again. The allocator's car score `823FF730` already leaves out cars with people other than ambient
  ones in them. A car the game removes anyway (a script, an explosion) is still re-made outside the registry (open).
- mod.ini (host): `member_spawns` 1 parked, 2 traffic, 4 people (7; 0: only the keep-alive and the allocator's cover),
  `member_density` (1.0; 0 off), `member_join_m` / `member_leave_m` 150 / 200.
- Scaling (review AB-10): the area is one street (12 cars, 26 people, 6 parked), so with the host and 3 members each
  has about 3 cars, 6-7 people and 1-2 parked cars near it, and at 12 players about 1 car and 2 people; the B pass
  numbers below are for 2-3 players.
- Visibility (review AB-13): the people pass's frustum test (`8218DE20` in `8240DD40`) and `823FE380` use the render
  view, not the swapped camera, so people made for a member are kept out of the host's view only and can pop in
  inside the member's (it fades its copies in anyway). Threads: `*0x8309ABEC` and the camera's eye / forward are
  swapped for 20-150 us inside the world update; a reader on another thread (a render-side reader, another mod's
  on_frame) could see the member's pose for that window.

Logs (every 10 s with the World lines): `World (owner) B: viewpoints n (members m: slots, "(stale)" after one whose
pose is 0.5-3 s old; went stale k); wanted cars w (boosted from w0) / people p (from p0); parked: member passes,
candidates near members (of the state-1 records), made for members, re-states vetoed (not vetoed: chunk not here,
held 3 s), removals kept; traffic: turns t of T ticks (0 spots z), made for members (in their view); people: member
passes (full), made (deleted by the region check at birth); member passes skipped for density 0; kept alive people /
cars; make room spared; evictions refused (protected taken for a creation of priority 0-2: per kind; covered); cars
another player sits in kept` (without members, on any host: `World (owner): cars another player's character sits in
kept from this game's traffic / parked removals n`); per member `World (owner) B slot S: d m from
this player (in since t s); near its eye (people 50 m / cars 100 m): this game has a / b (registry; streaming); made for
it parked / traffic / people`; on the member `World (member) B: near me (people 50 m / cars 100 m of my eye) by distance
to the host: <40 m: people a0, cars b0 (n0 s); 100-150 m: people a1, cars b1 (n1 s); session ratio a1/a0 = x%, b1/b0 =
y%` (the pass: x >= 60 %, y >= 80 % with n0, n1 >= 60 s, A's numbers unchanged). One-shots: `World (owner) B: hooks on
(parked/traffic/people, member_density; the traffic ring *0x82832724 and the removal radii *0x83AD2640/44)`, the first
pass of each kind, `parked re-state vetoed for record R (spot S) near slot S` (10), `eviction refused for kind K` (10),
`people made for slot S deleted at birth` (10), and any call a member's people pass makes to `82479AB0` / `820E9550`
from elsewhere than the spawn's own call (plan_B's check). Perf parts `B parked`, `B people`, `B keep` and `B keep in a
pass` (an eviction or keep test inside a member's pass: its ms are part of `B parked` / `B people` too; review AB-12).
The start-up line names `member_join_m` / `member_leave_m` instead of a literal 150 / 200 m (AB-12).

**Package C: the area's actions and effects** ("The shared area: actions and effects", notes/coop-2.2/plan_C.md). What
the host's people and cars do shows on every member's screen the way the game's own MP mirrors its NPCs (`823A8240`
applies a fire byte, a burn byte and the aim from the NPC's state, not an event per shot). mod.ini `world_actions` (1;
0 turns all of it off on that game, sent and applied alike; each game tells the others its setting with
`kEventVehicleTable` a[4] when a player joins, and Package C runs between two games only when both have it on).
Protocol 6 was kept for Package C itself (no protocol-6 build went to testers); the review fixes below make it 7, so a
2.2 build from an earlier step is refused at the join with the version message instead of silently showing nothing
(reviews A3-16, AB-8).
H for every address below (read in the asm):
- **Wire**: `PedRec` / `SeatedRec` keep their sizes; the weapon byte is the gun's weapon-table index + 1 in bits 0-5
  (an index past 62 goes as 0, counted), bit 6 the gun raised (`+216` 0x80000000, the 2.5 s hold `82449708` sets) and
  bit 7 "a `FireExt` follows" (7 bytes: the person's running round count mod 16, trigger held, blank, aim valid,
  drive-by mode, and the last round's first-pellet hit point in 2 cm from the header's reference). A person who fired in
  the last second carries one in every record (so the last count and the release always arrive), never in a create;
  the scheduler charges it (`Entry.extra`, 27 / 19 bytes a record). `CarRec` is 30 bytes: damage bits 6-7 brake lights
  and reversing, and a drive byte (the steering `E+412 + E+416` in 6 bits, the horn, the engine bit `+53976`
  0x08000000). `kEventExplosion` (28) and WPJ1's `shooter` are new; `kEventVehicleTable` also carries the explosion
  defs (`*0x8370D114`) and weapon counts, compared at the join.
- **Gunfire**: on the host `FireHook` runs a world person's `82470A50` call with the rounds counted (a round re-arms
  the refire timer, ammo `+24`, or uses one, `+16`: 1-4 per call), its blank flag (`r6`), its gun's Trigger_Type
  (`+96`) and the first pellet's hit point (`BulletRayHook` by the shooter `r5` of `8246FD18`); a seated person's
  records go at the band rate while it fires. The member takes the count difference per record (exact while records
  come: a record without a fire part means no round for a second, so the first round after quiet plays too; from the
  first fire part or after a gap of 1.5 s with no records only onwards); single-trigger guns queue up to 4 rounds,
  each played as other players' shots are (`TryRemoteShots`: the gun matched, never empty, raised on foot, the "run
  180" turn stopped, the trigger latch `ammo+12` 0xA0 cleared, `+24` = the clock, `+40` = 0) with `82470A50(copy, 0,
  &hit point, blank)`; automatic guns hold the trigger (`+3592` 0x40, a call each update when the refire timer is due,
  so the gun's own rate) and release it (`82474738` + `82476B70`) when the hold bit goes or its fire part is 300 ms
  old. All under `fire_replay_depth` (no damage, no blood or shove) and `copy_fire_replay_depth` (`DetonateHook`: no
  car takes the round's hit: the car this player drives gets the host's round by `kEventCarHitByPed` below); within
  150 m of the camera, 8 calls an update in all, a round older than 750 ms dropped with its last wait. A seated copy fires blanks (flash and sound; drive-by mode
  on copies is later). Its gun settles like the other players' (400 ms to another, 1.5 s to none, kept while rounds
  wait); `MatchWeapon` logs copies as "World copy gun" (their own cap) and stops after 3 tries of a gun a copy's
  model doesn't take, until it should hold another (review A3-11). The copy's own replayed rocket or throw is refused
  (`ProjectileCreateHook`); the host's person's projectile comes as WPJ1 with its id (unicast to each member of the
  area; the member's copy is credited to the copy of that person), its explosion too after the person is gone.
- **Driver inputs**: the member answers the brake getter `82533DA8` (r3 handle, f1 = `E+408` with `E = *(v+224)`
  while byte `v+50300` is 1) and the reversing test `82535C00` (r3 the vehicle: physics up, `info+284` != 6, `(int8)
  E+180`) for a car copy with the host's bits from the last second (a lock-free slot per handle index, since any thread
  may ask): the tail lights `8255A730` read the 10-frame brake average `v+53500` that `82527ED8` fills from the getter
  (> 0.2 lights them), the reverse lights `8255ACA0` need `82535C00` and the engine bit, which is written to the
  host's while it brakes or reverses. The horn is held with `825241F0(copy)` each update while the host's sounded in
  the last 150 ms (`825231A0` stops it on a frame without a call); the host notes its own horns with a post-hook on
  `825241F0`. `E+412` (only `825872E0` writes it; the physics' packer `82583810` steers by `E+412 + E+416`) gets the
  host's sum less this copy's own `E+416` once per record, for a copy with physics.
- **Burning**: PedRec flag 5 (`+216` 0x00400000) lights the copy with `8245E3A0(copy, 0, 0, 0, f1 = 0.0)` as
  `823A8240` does, its refusal bits `+120` 0xD0000000 masked for the call (counted), once a second while it differs;
  `8245E0B8(handle, 0)` puts it out. `IgniteHook` refuses the member's own ignites of copies (explosions `82458A50`,
  fire patches `82142E18`, spreading `8245ED40`, burning cars `82529838`, by their return addresses). When the 4 burn
  slots (`0x8309A9E8`, 24 bytes, 12 with the MP byte) are full the ignite puts out a dead or gone one, else the one
  furthest from the camera; a copy is not lit when that would be this player or another player's character.
- **Explosions**: on the host `ExplosionHook` sends a prop's death (`82138AE8` from `823FAA38`, LR `823FAC68`: the
  def `*(*(state+136)+140)`, the mover's uid `*(*(mover+216)+40)` and sub `+224`) and a car's second explosion
  (`8252B870`, LR `8252B9B0`: def `info+872`, the call's `+92` word `info+24`) as `kEventExplosion` to the members
  within 200 m (def index `(def - 0x829ACEB8) / 140` below `*0x8370D114`, position, the forward row, the attacker:
  the host's player, a player's character or a person of its world); wrecks (`8252CBAC`: the member's wreck replay
  explodes), projectiles (WPJ1), the deferred queue and replays are counted, not sent. The member plays it with
  `82138AE8(def, source, attacker, rows, pos, 0, 0, 0)` under `world_boom_depth` (the source: its mover by
  `824995B0(uid, sub)`, or its car copy, whose own `info+24` goes in the `+92` word; never this player), skips one
  this player caused (its own game exploded it) and a prop that exploded here in the last 120 s; a local explosion of
  a prop the host's exploded here, or of one a copy's replayed round broke (the host's round decides), is held
  (`823FAA38` then shows only its own visual fallback), and a copy car's own explosion (not the wreck replay, LR
  `82542BFC`) is held: the host's comes. The host no longer forwards its people's explosion and fire hits on a member's
  character (`kEventPlayerHitByPed` under `explosion_depth` / `fire_update_depth`) to a member that plays them itself:
  each game plays explosions and fire on its own player (R 3.0).
- **A/9, a member's bullets on the host's cars**: the member's `DetonateHook` sends `825397E0`'s arguments from the
  bullet path (`82536270`, LR `82536470`, the caller's `+84` = 2) for this player's round on a car copy of the
  host's world (`kEventCarDamage` 25: the car's id, the weapon's table index, the damage `r10`, the hit point `r7`,
  the normal `r8`, the push `r9`) and the copy takes nothing; the host applies it with `825397E0(v, the member's
  character, 0, weapon info, ...)` and the call's constants (`f1` `*0x820380CC`, `f2` `*0x8208A354`, `+84` = 2,
  `+92` the push) under `replicated_depth`, and its replay of that member's round leaves a car of its world alone (the
  member's screen decides the hit). A car a player drives is left to the replays of the round, as in 2.1.
- **The other direction: the host's people's rounds on the car a member drives** (`kEventCarHitByPed` 29, built with
  the review fixes; the mapping is A/9's): on the host a real round (not a replay) of a person of its registry on a
  car another player's character drives (seat 0) - that player's own car's copy, or a car of the host's they took -
  reaches `DetonateHook` from the same bullet path (LR `82536470`, `+84` = 2); for a member of the area with Package C
  on it goes to that member with the car as its pose names it (owner slot and id: `CarCopyRef`, else the host's own
  handle), the weapon's table index, the damage (16 bits), the hit point, the normal, the shooter's id and the push
  (direction and length), and nothing happens on the host (2.1's block refused it there anyway: the car is another
  game's). The member finds the car (`FindCar`), applies it only to the car this player is in, with the copy of that
  person as the attacker (skipped when the copy isn't here: `825397E0`'s attacker paths want a person), through the
  same `825397E0` call as A/9 under `replicated_depth`. The copy's replay of that round here still hits no car
  (`copy_fire_replay_depth`), so the car takes the round once. Before, a member's car took no NPC bullets at all.

Logs (every 10 s): the owner's `World (owner) gunfire: rounds r by p people (bursts, blanks, hit points, aim past 655 m,
guns past index 62), fire parts in n records (KB)`, `World (owner) driving: car records with brake / reverse / horn /
steering (of n with physics, n in all); lights forced on / off`, `World (owner) explosions: sent (props, cars' second),
skipped as wrecks / projectiles / queue / replays / far / other, bad def; NPC projectiles sent, exploded`, `World
(owner) members' bullets on its cars: applied (car gone, a player drives it, bad); their replayed rounds' car hits left
to their events; its people's rounds on cars members drive sent to them (weapon not named)`; the member's `World
(member) gunfire: rounds played (single, held frames, releases, blanks) of n counted from the host; waited/dropped:
not set up, lying, other gun, no ammo, raising, refused, too old, past 150 m, frame cap; guns given (carried, failed;
given up after 3 tries), raises; cost ms/s (us a call)`, `World (member) driving: brake shown (getter answers),
reverse shown (answers), horns frames, steer written (no physics, mean |E+416|), engine bit written`, `World
(member) burning: lit, put out, relit; refused (in a car, slots full kept own, +120 masked, +3694, dead, water, other);
own ignites of copies refused (explosions, fire patches, spreading, cars, other)`, `World (member) explosions: played
(props with the prop here / without; cars' second, copy missing), skipped here already / own act, held again here,
held from copies' rounds, chain props recorded, local copy-car explosions held, bad def; NPC projectile copies made (exploded), copies' own
launches refused; this player's bullets on the host's cars sent (weapon not named), copies' replayed rounds on cars
skipped; the host's people's rounds on this player's car applied (car not here, not this player's, shooter not here,
bad)`. One-shots: `World copy ID (model): round of GUN not played: why` (20, the first of each reason), `World copy
gun: ...` (20), `World copy ID not set alight: why (+120, +216, +3694, +2496)` (10), `World explosion from the host: def
n (kind k) at x y z, source found|none, attacker who` (10), `World (owner): slot S's bullet hit car ID here (weapon,
damage)` (10), `World (owner): a round of person P hit car C (owner slot o) that slot S drives: sent to it (weapon w)`
(10), `Explosion defs / weapons: slot S has a / b, this game c / d: the same | DIFFERENT`. Perf part `world
fire` (in `world apply`).

**People models and car copies: where the references go** (notes/coop-2.1/model_refcount_investigation.md 7.2a and
9.4; 2.1.4 balanced the per-character reference and measured the rest). H for the calls below (read in the asm):
- **Pins at level 0.** `PinModel` asks a people model (group 0) with `8250C750(group, id, 5, level 0, now)` instead of
  the top level (13): the pass `8250D3E0(group, level)` pairs a level's requests only with that level's slots, and
  the top level had only the slots `8241BD40` last moved there (the "8 loaded" plateau of every client log, then 4-6,
  with slots idle at levels 1, 2, 4, 5 that nobody asks at). `ReleasePins` releases at the pin's own level when a
  reference was taken (`ModelPin.held`). Other groups (car models) keep the top level (`pin_top`). With mod.ini
  `people_level0 = 0` or `world = 0` people are pinned at the top level again and the table is left alone (2.1.4).
- **The quota table** (as corrected by the final review, F1/F3). The game's own people do *not* all ask at level 0:
  `82419A28(desc, kind, level)` picks a group-0 request's level from the character's category (`8241CBF0`: 2-11 by
  category, 12 with `desc+600` 0x800; 0 only in MP or without a descriptor) - homies, script-prioritised mission
  characters, this game's own police and gangs. The live table `0x827ACFFC` (14 words) is rebuilt by `8241BE28`
  (zeroed, then 12 at level 0 in MP, else from its defaults), copied into the group's `+196` by `8241CA80`, and
  raised by `8241BD40(0, flag, n)`: for levels 1-13 each level is raised to its count `8250E208(group, level, 5)` (the
  level's queued requests - bound or waiting, the pass pushes them all back - whose highest kind is exactly 5) out of
  level 0's spare (`table[0] - n`), never shrinking one, then `8250F780(0, table, 14)`; `82413398` runs all three
  (co-op's own `SetOwnPedSpawning(false)` and missions call it). On a client, once a second and only while it shows
  the host's people (`peds_shared`, or in the host's world area), level 0 gets the slots of the levels nobody asks
  at: a level with anything in its request queue (`*(group+56) + level * 24`, `+0` the count) keeps the game's own
  slots and at least its kind-5 count (`8250E208`, as `8241BD40` would give it), and keeps them 10 s after its last
  request (a set that comes and goes doesn't move slots to and fro); the rest go to level 0 (the group's
  slot count `+0` less the kept ones, `people_models` included). Applied with `8250F780(0, 0x827ACFFC, 14)` (it copies
  each changed word to the group's array and sets its dirty bit `+200` 0x80, so the next pass's balancer `8250D0A8`
  moves the surplus slots down); skipped while the group's pass runs (`+200` 0x20). A table that isn't co-op's last
  was rebuilt by the game: it is the game's (the levels it raised kept for the put-back) and co-op's is applied again
  10 s after co-op's last write at the soonest, logged. When the client stops showing the host's people, and when the
  session ends, the game's table goes back, then `8241BD40(0, 0, 0)` as `82413398` does after `8241BE28` / `8241CA80`,
  so levels with kind-5 requests get their slots at once (single player keeps the game's 8 1 1 0 1 1 0 ...).
- The 2.1.4 guard (`ModelRefGuard`) stays log-only; it now counts a pin by its reference, at any level. The 10 s
  `slots here` dump keeps each slot's level and the quota line (`slot quota per level 827ACFFC`).
- **Car copies.** `CreateVehicle` takes the model reference through the game's own `82532360(vehicle, 5)`
  (`8250C750(group, *(*(v+50324)+0), kind, 0, 1)`, then `v+50304` = kind) instead of `8250C750` directly (the
  network creator `823BA888`'s way, which leaves the constructor's -1 at `+50304`): the vehicle destructor `825324D0`
  releases the kind noted at `+50304` when it isn't -1, so a copy's reference goes with the copy.

Logs: `People-model slot quota set: 827ACFFC was q0 .. q13, now w0 .. w13 (level 0: the slots of the levels nobody
asks at here; kept for this game's own requests at level: L (a asking, k kind 5) ...)`, `People-model slot quota
rebuilt by the game: q0 .. q13 (kept; co-op's applied again 10 s after the last time | co-op's applied again | the
game's own from now on)`, `People-model slot quota put back: 827ACFFC was ..., the game's ..., after its recompute
8241BD40 ...` (30 in all); `Model priority: n models pinned (people p at level 0, the rest at the top level), r
released, h held; slot quota written w times, rebuilt by the game g times (co-op's applied now: yes|no)`; `Car copy ID made: model M, reference kind noted at +50304 = 5` (3; `-1`
would mean the old way), `Car copy ID made with a model reference noted already` (5), and every 10 s `Car models: per
model of a copy here its kind-5 references (copies, this game's own; copies noted at +50304): ID k (c, o; noted n) ...;
m with more references than its cars + 1` next to the 2.1.4 `Car models here` line.

**Review fixes** (notes/coop-2.2/review_A3.md minors, review_A4_B.md, review_A2.md A2-14, impl_C_notes open point 2;
the list with what was applied is in impl_C_notes.md, "Review fixes"). Besides the B and C changes above:
- **Let-go cars** (A3-5, A3-7): `WorldFade` and `WorldDestroy` share one rule, `WorldLetGoCar`: a car copy this
  player sits in becomes its taken car (named as the host's), one another player's character sits in or is getting
  into becomes a car copy that player's pose drives; the crew copies sitting in it leave the table with it as 2.1's
  pulled-out drivers (removed 4 s on, once out of the car; `ReleaseOrphanDrivers` now runs every update on a client,
  world mode too) and go back to the host as "gone here", so its offer starts over. Before, they stayed copies of a car
  no longer in the table and were faded and destroyed next to this player.
- **2.1's copies dropped with a player in them** (A3-12, the `world = 1` drop and the out-of-range drop): this
  player's becomes its taken car with the crew as pulled-out drivers, another player's character's a car copy
  (`AdoptSharedCar`); before, the car became an anonymous car of this game and its crew this game's own people.
- **A seated create whose seat a player has here** waits (`WorldMake` returns made / wait / failed), declined "pool"
  after 3 s like a full pool, instead of three failed tries (A3-10).
- **The host's person events** (actions, calls, hit reactions, effects) go to every game only while 2.1's people stream
  has that person (players with `world = 0`), else only to the members of the area (`QueuePersonEvent`, A3-13).
- **Diagnostics** (A3-15): `WWA1`'s dropped "model" / "chunk" counts are the ones since the last `WWA1`; the per-type
  counts of the host's world messages are kept only for the `world = 0` line; a stand-in copy's original model no longer
  counts against the 11 people-model slots (the stand-in replaced it for good).
- **The chunk mask** (A3-17) is sent and not used yet: world.h skips an entry whose chunk a member hasn't loaded, and
  coop.cpp names no entry's chunk. The bit is the index in the list `0x829A97E8`, the same chunk on both games only if
  that list keeps the image's order; the two games' `World chunks here` lines decide that first. Then the owner can
  set `Entry.chunk` from the object's chunk and hold offers a member can't load.
- **Another player's character kept past 130 m** (AB-5): out there its clothing isn't put on again (the game unloads
  it at that distance); once it is back within 60 m its slot is checked at once and dressed again when anything went
  (`R back within 60 m after d m (kept past 130 m: a member of the host's area): clothing lost, dressing again | all
  there (...)`, 20).
- **Trains** (AB-9): `spawn_trains` is a console variable in the image's data: its one writer is the console handler
  `8242C128` and its one reader the train manager `8242EFB0` (H), so no script sets it and the save can't keep the 0
  (M: no other reference to `0x827ACF87`, direct or as `+10407`). A 0 written while it is held stays unknown to the put-back.
- **Protocol 7** (A3-16, AB-8): a step build of 2.2 is refused at the join; the same build on every game.

**Final review fixes** (notes/coop-2.2/review_final.md; dispositions in impl_C_notes.md section 8):
- **People-model quota** (F1, F3): as described under "The quota table" above - only while the host's people are
  shown, only the idle levels' slots, never a level with requests, the game's rebuilds left alone for 10 s, the game's
  table and its own recompute `8241BD40(0,0,0)` back at the end. mod.ini `people_level0 = 0` turns the level-0 pins
  and the table writes off (2.1.4's top-level pins).
- **What `world = 0` undoes** (F2). With `world = 0` on a game: no world stream shown or sent (2.1's streams), no B
  passes or keep-alives, no C replays; and, since the final review, people pinned at the top level and the people
  quota never written (as `people_level0 = 0`). What stays with `world = 0` (fixes, or harmless): the registry, the
  vehicle-table compare and the train lever (steps 2-4, accepted in review A2-A4); the car copies' model reference
  through `82532360` (a leak fix: the destructor `825324D0` gives back exactly the one taken, and co-op releases no
  vehicle-group reference anywhere else, review_final "verified clean"; 2.1.4 kept every car copy's model count up for
  good); the host keeping cars another player's character sits in from its traffic and parked removals (A2-14, a fix);
  `QueuePersonEvent` (no effect: with `world = 0` the person is in 2.1's stream); the pulled-out drivers removed after
  a session (F5, below). So a `world = 0` fallback on both games is 2.1.4 plus those fixes.
- **Far car copies placed with flag 0** (F4): `WorldSteerCar` calls `82535C68(v, pos, matrix, 0)` for a copy without
  physics, as the MP car code (`823BB068`, `823BA888`) and `CreateVehicle` do. Flag 1 set `v+53984` bit 0x80, which the
  physics set-up `8253BF08` reads once to lift the car by its lowest wheel (scripts' "on the ground" teleports): the
  host's body position is lifted already, so the copy popped up and dropped when its physics came.
- **Pulled-out drivers after a session** (F5): `ReleaseOrphanDrivers` runs after the session too while any are left
  (they count in `leftovers`), and the session's end skips their 4 s wait (one still seated: 10 s more, as before).
- **A let-go crew member offered again on foot** (F6): `WorldMake` takes over its orphan (within 5 m, out of the car,
  same model) instead of making a second copy next to it, 2.1's pulled-out driver rule (`let go n (their crews m;
  offered again on foot and carried on k)`).
- **Nothing put on past 130 m** (F7): the clothing check's dressing (record or count changed, colours) waits while
  another player's character is out past 130 m; the back-within-60 m check runs first.
- **Two 10 s lines split** (F8): `World (member of slot 0's area ...)` (held, made, models) and `World (member)
  applied: ...`; `World (owner) B: viewpoints ... parked ...` and `World (owner) B: traffic ... kept ...`.
- The start-up line's history names Packages B and C protocol 7 (F9).

**2.2.1: the car-look freeze** (the first 2.2 test; notes/coop-2.2/impl_C_notes.md "2.2.1"). The PC client froze for
good after 6 minutes: "inside game function 8254DCC0, last game function called 82535C68" (the paint apply of a car
copy `WorldMake` had just made; `82535C68` is `CreateVehicle`'s placement). The minidump's game thread was not in the
blob's walk but in its last step `8254DEB8`, the rebuild that takes the car's physics body out of the world and back:
`825422D8` -> `8252FD88(*(v+224))` -> `826C86A8(*0x8370DC88, *(*(P+60)+16))` -> `826DC780`, which calls each of the
body's `+104` entries at `+100`; there `+100` was 0xC0 and `+104` 0x43CB13xx (a float): no live body, 1.1 billion
turns (H). The look went on in the update the car was made in; the game never does that - the garage (`821C12D8`)
waits for the car's physics (`8253A378`) and the MP code (`8238B258`) applies a later message's (H). Why the body
wasn't there yet is not proven (M: the car's set-up not done in its first update; 2.2's balanced car-model references
let models come and go, 8 loaded of 13-14 asked).
- **The fix**: `WorldMake` no longer applies the paint; `WorldApplyCar` does from a later update. `ApplyCarLook` (every
  caller: 2.1's `kMsgCarLook` copies and traffic too) puts a look off (`CarLookWait`) while the car was made by
  `CreateVehicle` under 250 ms ago, its look state isn't its own (`+4` its handle, `+8` its info + 208, as `8254C650`
  makes it), its model isn't loaded (stream entry `+0`), or, with physics (`+50300`), the body `826DC780` would walk
  isn't one (at most 256 entries, each 0 or an object with a vtable) or `*(*(v+244)+88)` isn't; tried every 250 ms,
  given up after 20 tries (a model: every 1 s, 2 minutes). It returns put on / refused / put off; a refused or given-up
  look isn't tried again.
- **The guard**: the look is walked first as `8254DCC0` walks it (`coopworld::PaintWalk` in `world.h`: magic
  0x0A55B00B, a variant block of at most 64 bytes, the parts and their colours by their own counts, ending exactly at
  its end, 320 bytes at most) and checked against the car's tables the way `8254DAF8` wrote it (`CarLookMismatch`:
  slot below the info's slot count, part below the slot's with `part+44` = the slot, colours no more than its colour
  slots, palette below `*0x83710108`, colour below the palette's count with `colour+0` = the palette); the 320-byte
  buffer is cleared before the copy. A refused look is logged ("Car look refused: car, model (n bytes, why; where)",
  20 a session) and so is a given-up one ("Car look given up: ..."). The owner never reads a malformed look into a
  `WWC1` or `kMsgCarLook` (`ReadCarLook` drops it). No wire change: protocol 7.
- **Log**: every 10 s while any happened, "Car looks: n put on, n refused (not this car's), n put off (just made, look
  state, model not loaded, physics body), n given up after 20 tries; n read here malformed (not sent)".
- `world_test`: a full 320-byte look round-trips in a `WWC1` and walks clean; 321 bytes aren't written; a cut tail, a
  cut look, a stray byte, another magic and a variant block over 64 are refused.

Not built yet: Package B/2-6 (personal cars as records, occupant events, the pose relay filter); Package C's later
parts (wreck looks without physics, props and movers as records, pickups; drive-by mode and the aim pose on copies).
