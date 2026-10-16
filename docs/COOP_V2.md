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
  - global state (clock, weather, notoriety, missions, cutscenes, HUD): the host.
- **Puppets**: every object a game does not own is a puppet. One predicate, `IsPuppet(obj)`, drives every
  suppression hook (no own AI actions, no own damage, no own knockdowns/ejections/get-ups, no own despawn, no own
  explosions), replacing v1's separate avatar/copy checks.

## 5. What is synced, and how

| Area | State (snapshot) | Events (reliable) | Suppression on puppets / non-owners |
|---|---|---|---|
| Players | position, yaw, velocity, movement state (on foot / car + seat / ragdoll or knocked out / swimming / climbing / dead), anim set, action, crouch, health, weapon, aiming bit, camera eye/rows (+4112/+4124) | actions, knockdown, get-up, ejection, enter/exit, door, fire, trigger press/release, throw, reload, death, respawn | v1 hooks, unified; ragdoll/KO shown from state with no move orders (the "KO'd vs running into a wall" fix) |
| Cars | model, position, rows, linear/angular velocity, hit points, damage bits (smoke/fire/wreck), lights/siren, doors; paint once | impacts, ejection, wreck | car damage 825397E0, ejection 82540780, own physics steered to the owner's state |
| Traffic, parked cars | as cars + occupants | create/destroy | traffic byte 0x8309A35D = 0, parked spawner hook 8241E378 (S 1c) |
| People | model, look (once), position, yaw, velocity, movement state, anim set, action, dead, ragdoll, car + seat | actions, hit reactions, knockdown, get-up, death, ignite | people bytes 0x8309A364..68 + 82413398, gang manager 824189E0, despawn 8244BC40/824789A8, MakePassive |
| Police | cops and police cars are ordinary people/cars from the area owner | - | response manager 8242ABA0, roadblocks *0x8370F808, cop/gang spawn bytes 0x827AD06C/6D |
| Notoriety | one shared value (5 x level, points, min, max), host-owned | joiners' notoriety gains go to the host | 821D3148 / 82481B28 forwarded |
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
2. **M2**: police and shared notoriety, pickups and money, car hits/ejection/fire, NPC gunfire, weather, props.
3. **M3**: area ownership for players spread across the city, 12-player load and bandwidth tests, delta
   compression, more of the game's own per-type functions.

## 9. Code layout (for upstream)

`coop.cpp` keeps the mod entry, settings and the menu exports (`WhompaysCoop*`, so coop_menu.cpp keeps working).
New files next to it: `net.*` (transport, sessions, reliability, bundling), `protocol.h` (messages), `registry.*`
(NetIds, ownership, puppets), `players.*`, `vehicles.*`, `people.*`, `weapons.*`, `world.*` (spawner control,
clock, weather, notoriety), `missions.*`, `engine.h` (addresses and game-call helpers). The network layer has a
stand-alone test program that runs several peers over loopback without the game.

## 10. Co-op 2.0 (milestone 1): what is built

Every player needs 2.0: it speaks a new protocol (a 1.71 game knocking is told so on screen and in the log).

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

**Weapons** (research report R): bullets go from the character's muzzle to the shooter's own hit point (first
pellet, `8246FD18`), spread off, gun raised first, the game's own deferred shot swallowed, refused shots retried for
750 ms; automatic guns get the trigger release; rockets, grenades, pipe bombs, Molotovs and bottles: the owner's
projectile is the only real one, other games make a copy (`8241FCB0`) that never explodes by itself and explodes
once at the owner's spot when the owner's does; explosion damage to the host's people is decided by the host,
each game applies explosions to its own player. The fire and throw animations are no longer sent twice.

**Knockdowns** (research S 2d): each player's game decides their ragdoll, knockdown and crash ejection. The state
(+780) goes with the pose; other games knock the character down / get it up to match, never seat or move it while
it lies, and skip crash ejections for other games' cars and characters (the "knocked out in the car on one screen,
running into a wall on the other" case).

**Built in from Co-op Car Guard 0.5**: the car-removal guard and car-list checks (already in 1.64), and the memory
check speed-up under Wine (`memory_speedup`). Car Guard is no longer needed (co-op says so if it is installed).

**Fixed on the way**: car smoke and fire were applied the wrong way round; the "game took the other player's
character for ours" check (1.34) never matched.

**Spawners** on a client while the host's world is shown: traffic spawner byte off (no spawn/despawn churn),
parked-car spawns refused, gang manager skipped (plus v1's density 0 and removals as a backstop).

Not in 2.0 yet (milestones 2-3): police and shared notoriety, pickups, car hit forwarding, NPC gunfire and NPC
projectiles, weather, area ownership for players spread across the city, bandwidth scheduling for 12 players.
