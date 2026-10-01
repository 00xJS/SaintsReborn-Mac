# Whompays Trainer

Enable **Whompays Trainer** in Whompays Mod Loader and launch the game.
Press **F4** to show or hide the white-text menu over the game. Press **1–5**
(top row or numpad) to activate an option while the menu is open. The game keeps
focus and mouse control; there is no separate window or cursor to fight.

- **1 — Max Reputation:** sets respect to the game's 99-bar limit.
- **2 — All Guns:** applies the game's weapon-giving cheats, one per game update.
  The normal inventory slot limits still apply; later guns replace earlier guns
  in the same slot. This does not expand the weapon wheel or unlock a new storage menu.
- **3 — Money:** adds $100,000 each press, capped at the game's cash limit.
- **4 — God Mode:** toggles the player's invulnerability flag. Starts off each run.
- **5 — No Clip:** fly through walls and floors. W/S move where the camera looks,
  A/D sideways, Space up, C down, Shift faster. Not in cars. Switching it off
  in the air drops you without fall damage.

Use in single-player gameplay, after loading a save. Money, respect and weapons
can persist if you save. God Mode does not prevent scripted mission failures.
The menu reports action results. If gameplay is suspended, requests expire after
three seconds instead of applying unexpectedly after a later loading transition.
Automated tests cover memory changes, hotkey-to-update dispatch, weapon filtering,
register preservation and menu visibility. Gameplay effects still need verification.

Build only this mod (no game rebuild): `powershell -ExecutionPolicy Bypass -File Build.ps1`.
Version 0.2 also needs the host's text-overlay API extension. Build and install
both with `Build.ps1 -BuildHost` after closing the game, or use the main setup.
Run its tests with `Build.ps1 -Test`. Requires the project's installed SDK and
Visual Studio Clang tools. The DLL must match the SDK used to build the game.
