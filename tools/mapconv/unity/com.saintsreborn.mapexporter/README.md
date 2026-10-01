# Saints Reborn Map Exporter (Unity)

Build a Saints Reborn multiplayer map in Unity and export it as a map pack.

## Install
Unity: Window > Package Manager > + > Add package from disk... > pick `package.json` in this folder.
(Or copy the folder into your project's `Packages` folder.)

## Use
1. Model the map with normal GameObjects (MeshRenderer + MeshFilter). 1 unit = 1 metre. Every mesh
   gets collision. Each material's base texture (Base Map / Albedo) and colour are used.
2. Add markers: menu **Saints Reborn > Add** (Spawn Point, Red / Blue Team Spawn, Weapon Pickup,
   Vehicle Spawn, Chains Drop-off, Player Start). A marker faces its blue Z arrow. Aim for 8+ spawns.
3. **Saints Reborn > Map Exporter**: set the map name and the converter
   (`SaintsRebornMapConverter.exe` in the game's `tools\MapConverter` folder), press **Export Map Pack**.
   The pack goes into the game's `maps` folder; start the game and pick the map in the System Link lobby.

Not exported: terrain (convert it to a mesh), skinned meshes, lights (the game lights the map itself).
