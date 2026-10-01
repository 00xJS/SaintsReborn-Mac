# Saints Reborn Map Converter

Turns a 3D scene into a multiplayer map pack for Saints Reborn. The pack goes into the game's `maps`
folder; the game lists it in the System Link lobby like the original downloadable maps.

## From Unity
Window > Package Manager > + > **Add package from disk...** > `Unity\com.saintsreborn.mapexporter\package.json`.
Then **Saints Reborn > Add** for markers (spawn points, team spawns, weapons, vehicles, drop-offs, player
start) and **Saints Reborn > Map Exporter** > Export Map Pack. Point it at this folder's
`SaintsRebornMapConverter.exe` once.

## From Blender (4.2 or newer)
Edit > Preferences > Get Extensions > the arrow menu (top right) > **Install from Disk...** >
`Blender\saints_reborn_map_exporter.zip`. Set the converter path in the add-on preferences. The panel is
in the 3D view sidebar (N) > **Saints Reborn**: add markers at the 3D cursor, then Export Map Pack.

## From anything else
Drag a `.glb` / `.gltf` file onto `SaintsRebornMapConverter.exe`, or run

    SaintsRebornMapConverter.exe MyMap.glb --name "My Map"

Options: `--name` (lobby name, default: file name), `--out <maps folder>`, `--game <game folder>`,
`--texture-size 256|512|1024` (default 256), `--no-pause`.

## Building a map
- 1 unit = 1 metre. Up is up (glTF is Y-up; Blender and Unity exporters convert automatically).
- Every mesh is map geometry and gets collision. Textures: the base colour texture (and colour) of each
  material. Keep single faces below ~30 texture repeats.
- Markers are empty objects (or any object) named:
  - `spawn_1`, `spawn_2`, ... respawn points (8 or more recommended)
  - `team1_spawn_...`, `team2_spawn_...` team spawns
  - `player_start` where the host starts
  - `weapon_<type>`: `ak47`, `desert eagle`, `m16`, `mac10`, `molotov`, `pipe_bomb`, `pump_action_shotgun`,
    `rpg_launcher`, `sniper_rifle`, `spas12`, `tec9`
  - `vehicle_<type>` (vehicle names from the game, e.g. `car_2dr_sports03`)
  - `chains_dropoff_...` Big Ass Chains drop-off points (adds the Big Ass Chains modes)
  A marker faces its forward axis: Blender -Y (like a character), Unity blue Z arrow.
  Custom properties `sr_type` (`spawn`, `team1_spawn`, `team2_spawn`, `player_start`, `weapon`, `vehicle`,
  `chains_dropoff`) and `sr_value` (weapon / vehicle type) work instead of names.

## How it works
The converter reads a small stock map from your own game files as a template (no game files are
shipped), replaces its geometry with yours, builds textures, materials and Havok collision, and writes
the level list, name and gameplay layout the game's downloadable-content loader reads.

Source: `src/` (C++17). Third-party: cgltf (MIT), stb_image and stb_dxt (public domain / MIT),
puff from zlib (zlib licence).

## Map editor
`SaintsRebornMapEditor.exe` (source: `editor/`, see `editor/README.md`) builds maps without a 3D
program, using props and textures from your own game through the Saints Row Asset Viewer.
