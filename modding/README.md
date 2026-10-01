# Whompay's Mod Loader

Whompay's Mod Loader (WML) lets you turn mods for Saints Reborn on and off, and
gives mods four ways to change the game:

| Kind | What it is | Needs compiling |
|---|---|---|
| File mod | Replacement game files | No |
| Patch script | A Lua script that edits files inside the game's packfiles | No |
| Script mod | A Lua script | No |
| Code mod | A DLL written in C or C++ | Yes |

A single mod can use any combination of them.

## Using mods

Mods live in `dist\mods`, one folder per mod. Run `dist\WhompaysModLoader.exe`,
tick the mods you want, use **Move up** / **Move down** to change the load
order, and press **Play**. Your choices are saved to `mods\modlist.ini`, so
starting `saintsrow.exe` directly uses the same mods.

Mods lower in the list load later. When two mods replace the same file, the one
lower in the list wins.

Each run writes a log to `mods\wml.log`. Check it first when a mod doesn't
work.

## Making a mod

Create a folder in `dist\mods` with a `mod.ini`:

```ini
[mod]
name = Big Money
author = you
version = 1.0
description = One line of text.\nUse \n for a new line.
```

Then add any of the following to the folder.

### File mods

A `files` folder mirrors the game folder (`dist\game`). A file at
`files\data\example.xtbl` replaces `game\data\example.xtbl`. New files can be
added the same way.

### Script mods (Lua)

A `main.lua` script (or the file named by `script = ...` in `mod.ini`) runs
once when the game starts. It uses [Lua 5.4](https://www.lua.org/manual/5.4/)
and a global `wml` table:

| Function | Description |
|---|---|
| `wml.log(...)` | Writes its arguments to `mods\wml.log`. |
| `wml.read_u8(addr)`, `read_u16`, `read_u32`, `read_f32` | Reads game memory. |
| `wml.write_u8(addr, value)`, `write_u16`, `write_u32`, `write_f32` | Writes game memory. |
| `wml.key_down(key)` | True while a key is held. |
| `wml.key_pressed(key)` | True only on the frame a key goes down. |
| `wml.take_key(key [, taken])` | Takes a key over: the built-in keyboard controls stop using it, so the mod can give it a new job. `wml.take_key(key, false)` gives it back. |
| `wml.force_key(key, down)` | Holds a key down (or lets go of it) for the built-in keyboard controls, as if the player pressed it. |
| `wml.turn_camera(radians)` | Turns the camera, like moving the mouse (positive turns right). |
| `wml.limit_camera(yaw, pitch, yaw_limit, pitch_up, pitch_down)` | Stops mouse turning past the given limits (all in radians; `yaw` and `pitch` are where the view points now). Call it every frame while it should apply; `wml.limit_camera()` turns it off. |
| `wml.set_look({sharpen, bloom, bloom_threshold, exposure, contrast, saturation, vibrance, filmic, temperature, tint, vignette, black_level, fxaa, anisotropic_filtering})` | Changes the finished picture (see the Modern Look mod for what each value does). `wml.set_look()` restores the original picture. Native mods: `set_look` in `wml.h`. |
| `wml.mouse_look()` | Returns how far the mouse (or the controller's right stick) would have turned the camera since the last call, as `yaw, pitch` in radians (positive = right / up). For mods that steer a view themselves. |
| `wml.on_frame(function)` | Calls the function once every frame. |
| `wml.hook(addr, function(ctx) ... end)` | Replaces the game function at `addr`. |
| `wml.setting(name, default)` | A value from the mod's `[settings]` (see Settings below). |
| `wml.mod_name`, `wml.mod_folder` | This mod's name and folder. |

Keys are names such as `"F8"`, `"A"`, `"5"`, `"SPACE"`, `"ENTER"`, `"SHIFT"`,
`"CTRL"`, `"ALT"`, `"UP"`, `"NUMPAD4"`, or a Windows virtual-key code. Keys
only count while the game window is focused.

Addresses are Xbox 360 addresses, for example `0x82123456`. Values in game
memory are converted from the console's big-endian format for you.

Inside a hook, `ctx` is the state of the game's CPU when the function was
called:

| Method | Description |
|---|---|
| `ctx:r(n)`, `ctx:set_r(n, value)` | Integer registers r0-r31 (arguments are in r3-r10, the return value in r3). |
| `ctx:f(n)`, `ctx:set_f(n, value)` | Floating-point registers f0-f31 (arguments in f1-f13, return value in f1). |
| `ctx:lr()` | Return address. |
| `ctx:call_original()` | Runs the game's own function (or the previous mod's hook). |
| `ctx:call(addr)` | Calls another game function with the current registers. |

A hook replaces the game's function completely; call `ctx:call_original()` to
run it as well. Example:

```lua
-- Double whatever the function at 0x82123456 returns.
wml.hook(0x82123456, function(ctx)
  ctx:call_original()
  ctx:set_r(3, ctx:r(3) * 2)
end)
```

Keep hooks short: they run on the game's own threads, and the game waits for
them.

`require("name")` loads `name.lua` from the mod's folder.

### Patch scripts (changing files inside packfiles)

Most of the game's data (tables, scripts, textures) is stored inside packfiles
(`game\packfiles\*.vpp_xbox2`). A `patch.lua` script runs before the game
starts and can change files inside them. Changes are applied to the player's
own copy of the game and cached in `mods\.cache`, so a mod only ships the
script, never game files.

| Function | Description |
|---|---|
| `wml.packfile_files(pack)` | List of the files in a packfile. |
| `wml.packfile_read(pack, name)` | Contents of a file (or `nil`), including earlier mods' changes. |
| `wml.packfile_write(pack, name, data)` | Replaces a file. |
| `wml.game_file_read(path)` | Contents of a loose file in the game folder (or `nil`). |
| `wml.setting(name, default)` | A value from the mod's `[settings]`, as a number or boolean if `default` is one. |
| `wml.log(...)`, `wml.mod_name`, `wml.mod_folder` | As in script mods. |

`pack` is a packfile name such as `"misc.vpp_xbox2"`. Many of the game's
tables are XML (`.xtbl`), which Lua's `string.gsub` handles well:

```lua
-- Make the game's random street encounters twice as likely.
local xml = wml.packfile_read("misc.vpp_xbox2", "special_spawns.xtbl")
xml = xml:gsub("<Spawn_chance>([%d%.]+)</Spawn_chance>", function(v)
  return "<Spawn_chance>" .. math.min(1, tonumber(v) * 2) .. "</Spawn_chance>"
end)
wml.packfile_write("misc.vpp_xbox2", "special_spawns.xtbl", xml)
```

### Settings

A `[settings]` section in `mod.ini` holds values players can change without
editing the script:

```ini
[settings]
; Pedestrian density multiplier.
pedestrians = 2.0
```

Read them with `wml.setting("pedestrians", 1.0)` in `patch.lua` or `main.lua`
(or `wml.settings.pedestrians` as a string).

### Code mods (C/C++ DLL)

Any `.dll` in the mod folder is loaded. It must export `wml_mod_init`:

```c
#include "wml.h"

static const WmlApi* api;
static WmlGuestFunction original;

static void my_hook(WmlContext* ctx, uint8_t* base) {
  original(ctx, base);  // run the game's function
  api->set_r(ctx, 3, api->get_r(ctx, 3) * 2);
}

WML_EXPORT int wml_mod_init(const WmlApi* loader, const WmlMod* mod) {
  api = loader;
  api->hook(0x82123456, my_hook, &original);
  api->log(mod, "Loaded!");
  return 0;  // anything else unloads the mod
}
```

`modding/include/wml.h` documents every function. The API matches the Lua one:
memory access, hooks, registers, per-frame callbacks and keys. Native mods
can also show plain text over the game with `overlay_text`. The example in
`modding/examples/ExampleNative` is built by `setup.bat`; to build your own
DLL, compile it as a 64-bit Windows DLL with any compiler and add
`modding/include` to the include path.

## Bundled mods

- **Living Stilwater** makes the city busier: more pedestrians and traffic,
  and random street encounters far more often. The boost follows how busy a
  place already is, so the city streets by day get the most and quiet places
  and times stay close to normal. It edits the spawn tables (patch.lua) and
  lifts the game's fixed limits on how many cars and people it keeps around
  you (main.lua). Tune it in its `mod.ini`.
- **60 FPS** raises the game's frame rate limit from 30 to 60. With it on, F10
  cycles the cap between 30, 60, 90 and 120.
- **Modern Look** gives the picture a cleaner, more modern look: sharper image,
  a soft glow around bright lights, richer colours and contrast, smoother edges
  (FXAA) and sharper textures at a distance (16x anisotropic filtering). It is
  off by default; turn it on in the mod manager, and press F3 in game to
  compare with the original look. Every effect can be tuned or switched off in
  its `mod.ini`.
- **Whompays Trainer** (F4) gives respect, all guns, money and god mode.

## Example mods

`setup.bat` installs three examples into `dist\mods`, all turned off:

- **Example: Lua script** writes to the log when you press F8.
- **Example: C code mod** hooks the game's present function and logs the frame
  rate when you press F9.
- **Example: file replacement** is a template for file mods.

## Rules

Don't put unmodified game files, recompiled game code or game executables in a
mod you share. Share your own work: scripts, code, and the files you created or
changed.
