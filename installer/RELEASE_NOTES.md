## What's new in 1.2

- Much faster, most of all in busy streets and while driving.
  - The game no longer waits for the GPU thread: up to 32 command buffers are
    queued, still at most 4 ms behind.
  - The GPU work is recorded and sent from its own thread.
  - Shared memory uploads are batched, and unchanged memory is no longer
    uploaded again.
  - Resolved images go straight into the textures that use them.
  - Less work per draw: cached texture and render target lookups.
  - A timer thread that kept a CPU core busy now sleeps.
  - Memory-watch faults are cheaper.
  - The shop menus draw the scene once instead of four times.
- F10 can also turn the frame rate cap off.
- Much faster startup: mods load in a fraction of a second (was over 5 s), and
  mods that change game files are only rebuilt when something changed.

## What's new in 1.1

- SaintsRowPC is now called **Saints Reborn**. Old links still work, and
  the updater moves your shortcuts to the new name.
- Keyboard and mouse controls laid out like Saints Row 2 on PC, and button
  prompts that switch between controller and keyboard pictures (made from your
  own game files during setup).
- Tagging can be done with the mouse: move it in circles the way the arrows
  show.
- F10 cycles the frame rate cap (30 / 60 / 90 / 120 with the 60 FPS mod), F1
  shows the frame rate.
- New bundled mods: **First Person** (V) and **Whompays Trainer** (F4); Living
  Stilwater draws cars and people further away.
- Smoother and faster: cheaper guest locks, native memory copies, a RAM cache
  for the game's packfiles, and less background work for the button prompts.
- Character creator controls for keyboard and mouse, and X / Y keys in the
  menus (for example Select Device).
- Cutscenes can be skipped with a mouse click.
- Smoother frame pacing: more GPU work can be queued safely, fewer stutters.
- Fixed crashes from the GPU reading command memory the game had already reused.
- Fixed garbled graphics and crashes on PCs where the GPU thread fell behind
  the game: it now stays at most a few milliseconds behind.
- Fixed the game closing right after start when an overlay such as MSI
  Afterburner / RivaTuner is running.
- First Person: better seat position in vehicles with a look limit, stays
  above the water while swimming, switches to the normal camera when bailing
  out of a moving car, and no more camera jitter while driving.
- The Saints Reborn logo on the startup, main menu and loading screens is now
  part of the game instead of a mod (it is applied to your own game files
  while the game runs).

Update an existing install with **SaintsReborn-Updater.exe** (or the **Update
Saints Reborn** shortcut); your saves and mod list are kept.

## New install

1. Download **SaintsReborn-Setup.exe** below and run it.
2. Select your Saints Row (Xbox 360) disc image (.iso) and an install folder.
3. Press **Install**. Anything missing (Git, the Visual Studio C++ build
   tools, the Visual C++ runtime) is downloaded and installed for you.
   Windows asks for permission when the build tools are installed.

The build takes 30-90 minutes and needs about 25 GB of free space (including
the build tools). When it finishes, start the game from the **Saints Reborn**
shortcut.

## Already built the game?

Download **SaintsReborn-Updater.exe**, choose your Saints Reborn folder (the
one with `setup.bat` and `dist`) and press **Update**. It downloads the latest
patches and Whompay's Mod Loader and rebuilds only what changed. Your game
files, saves and mod list are kept. Installs made with Setup can also use the
**Update Saints Reborn** shortcut in the Start menu.

## Notes

Windows may show a SmartScreen warning because Setup is not code-signed.
Choose **More info** > **Run anyway**.

You need your own copy of the game. Nothing from the game is included.
