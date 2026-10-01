# Saints Reborn Map Editor

Build multiplayer maps without Blender or Unity. Block out a level with boxes, ramps, stairs,
cylinders and wedges, place props and car bodies from the game, paint everything with the game's
own textures, add spawns and weapons, then press **Export map pack**. The pack lands in the game's
`maps` folder and shows up in the System Link lobby's Level list the next time you start the game.

## First start: choose your Saints Reborn folder
The editor asks for the folder Saints Reborn is installed in (the one with `game\packfiles`, or the
`packfiles` folder itself) and will not start without it: the props, textures and the multiplayer
map template are read from your own copy of the game. Change it later in **File > Choose game folder**.

Game models and textures are read by the Saints Row Asset Viewer, which must sit next to the editor
as `SaintsRowAssetViewer.exe` (another path can be set in **File > Settings**). What it extracts is
cached in `%LOCALAPPDATA%\SaintsReborn\MapEditor\cache`.

## Controls (like Unity's scene view)
| | |
|---|---|
| Right mouse + WASD, Q/E | fly (Shift = faster, wheel while flying = fly speed) |
| Mouse wheel | zoom |
| Middle mouse | pan |
| Alt + left mouse / Alt + right mouse | orbit / zoom |
| F | frame the selection (orbiting then turns around it) |
| Axis widget (top right) | click an axis to look along it |
| Q / W / E / R | view (hand) / move / rotate / scale tool |
| Pivot/Center, Local/Global (toolbar) | where the gizmo sits and how it is turned |
| G | snap on/off; hold Ctrl while dragging to flip it |
| Left click, Ctrl+click, Shift+click | select, add/remove, add |
| Drag in empty space | rectangle select |
| Del or Backspace | delete the selection (also: toolbar, Inspector, right-click menu) |
| Ctrl+D | duplicate in place |
| Ctrl+C / Ctrl+V / Ctrl+X, Ctrl+A | copy / paste / cut, select all |
| End | drop the selection onto what is below it |
| Ctrl+Z / Ctrl+Y | undo / redo |
| Ctrl+S, Ctrl+O, Ctrl+N, Ctrl+E | save, open, new, export |

Numbers in the Inspector and toolbar: **click to type a value**, or drag left/right to change it
(the coloured X/Y/Z labels can be dragged too).

## Assets panel
* **Shapes**: blocks and the map markers (spawns, weapons, drivable vehicles, drop-offs).
* **Props** and **Vehicles**: models from your game. Characters are not offered. Vehicles are the
  car bodies as scenery; for a car players can drive, use a Vehicle marker.
* **Game textures**: every colour texture of the game (without the character archives). Click one to
  paint the selected shapes, or drag it onto a shape.
* **Map textures**: the textures this map uses, the built-in ones and imported images.

Click a tile to add it under the screen centre (double-click for props), or drag it into the view.

## Things a map needs
* **Spawn points** (green). Add at least 4. For team modes add **Red** and **Blue** team spawns;
  without them team modes use the shared spawns.
* **Weapons** (yellow) are optional pickups. **Vehicles** and **Chains drop-offs** are optional too.
* The arrow on a spawn shows which way the player faces.

## Sharing maps
A `.srmap` file only stores the *names* of the game models and textures it uses, never the game
data itself, so it can be shared freely: the editor rebuilds the pack from the other player's own
game. An exported map pack contains data from your copy of the game.

## Files
* `.srmap` is the editable map (JSON). Keep it; the exported pack can't be edited back.
* `samples\ExampleYard.srmap` is a small finished map to look at.
