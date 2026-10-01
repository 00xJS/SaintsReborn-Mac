# Whompays Co-op 1.19 (test build)

Two players, each with their own Saints Reborn install.

## Install

1. Copy this `WhompaysCoop` folder into your game's `dist\mods` folder.
2. Start the mod loader (Saints Reborn shortcut) and tick **Whompays Co-op**.
3. Both players must use this same version.

## Playing over the internet (Radmin VPN)

1. Both install Radmin VPN and join the same network.
2. Host: load a save and press **F6**, then **1** (direct host). (Pause >
   Options > **Host Co-op** hosts online with a join code instead, see below.)
3. Joiner: on the main menu pick **Join Co-op**, type the host's Radmin IP
   (the 26.x.x.x address), pick a save to load; it joins once the save is in.
   (Or in game: pause > Options > **Join Co-op**.) End or leave from the same
   place (**End Co-op** / **Leave Co-op**). F6 with 1/2/3 still works too.
   If Windows Firewall asks about saintsrow.exe, allow it on private and public
   networks. UDP port 27015 is used.

## What is shared

- The other player, their clothes and body, their car (with its colours) and
  riding along as a passenger.
- While close together: traffic and pedestrians (the host's), with the same
  looks.
- Story missions follow the host: the joiner can't start missions during a
  session (their mission markers are hidden until the session ends) and is
  brought next to the host when the host starts one. The joiner's game loads
  the same mission, but the host's game is in charge: its objectives, help
  messages and mission end are shown on both screens. Mission characters are
  shown on both sides; the joiner's hits on them count in the host's game,
  their hits on the joiner hurt the joiner, and people who die in the host's
  game die on the joiner's screen too.
- The host's cutscenes play for the joiner too; only the host can skip one, and it
  ends for both, and the joiner is placed next to the host afterwards.
- When the host pauses, or a prompt pauses the host's game, the joiner can't
  move until the host carries on.
- Pedestrians and mission characters use the host's walk and idle styles and
  only do what the host's do. If a mission character's model hasn't
  loaded on the joiner's side yet, a similar one stands in until it has.
  Knockdowns and hit reactions of people show on both screens.
- The joiner's game never pauses the world: its pause menu and prompts still
  open, but the game keeps running (the host's world doesn't stop either).
- If the other player's character looks wrong, press **F8** to write a report to
  the log (send it along).
- **F9** records 15 seconds of what both players' characters do (climbing,
  getting in and out of cars) to the log, for reporting problems with those.
- Avoid saving on the joiner's side during a session.

Settings are in `mod.ini`. Each game writes `coop-<number>.log` here; send
those along when reporting a problem.

## Online (join code) - 1.50

- Host: pause > Options > **Host Co-op** (or F6, then **4**). After a few seconds the overlay shows a **join code** (6 letters/digits).
- Joiner: **Join Co-op** (main menu or pause > Options) and type the code instead of an IP.
- No port forwarding or VPN needed: it goes through Epic Online Services (free; no Epic account needed). The `eos` folder and `eos.ini` must be next to the DLL.
- An IP address in `join_ip.txt` still joins directly like before (1 = host direct).
