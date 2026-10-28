# Saints Reborn character (built in)

Steps of the character-creation plan in *SaintsReborn New Base Assets/CONCEPTS.md*, in the
game's own creator: **Body Type**, **movement styles** and **personality** (voice, facial
expression, D-pad taunts). Later steps (presets, multiplayer sync of personality) go in this same mod.

## What it does

- **BODY tab, third slider "Body Type"**: from 0 (left, male, the default) to 1 (right, fully female).
  The same row appears on the Plastic Surgeon's Body page and is exported to multiplayer with the rest
  of the character.
- **Female shape**: Saints Row 2's body-gender shape on the player's body and on every stock clothing
  mesh (596), blended over the Muscle × Fat corners, with breast physics past 25 % female. Live in the
  creator and at the surgeon while the slider moves.
- **Movement rows** under Body Type: Stance, Walk, Run, Sprint, Melee Run, Combat, Melee Combat,
  shown as menu rows with the style chosen ("Walk: Pimp swagger"). **A opens a list** of every style
  for that part (zip 08's list: gang, street, cop, mascot, skater, women's sets, variants; "Default" =
  the game's own); the cursor previews each on your character (walking / running on the spot), A
  keeps it, B goes back. Left / right on the row also step through them. The same on the creation / Plastic
  Surgeon screen (builder `0x8231FCE0`, input `0x8231C3D0`, which new games use) and the older creator
  screen (`0x822FFCA8`, `0x822FDF40`).
- **Personality rows** under the movement rows (`personality.lua`, story mode), shown and chosen the
  same way (row shows the choice, A opens the list, the cursor previews it):
  - **Voice**: Default (your race's own) or any of 30 gang voices - men's, or women's when Body Type
    is past the middle (zip 05's list: by race, across Vice Kings, Los Carnales, Rollerz, Saints).
    Set with the game's `human_set_persona`, put back after respawns / cutscenes. Preview: the voice
    with a sample line.
  - **Expression**: None, SR1's six emotions, or one of 24 SR2 faces baked onto the SR1 head (zip 02;
    `facial_expressions.lua`, banks in `juiced_runtime/`, facial hair follows). Preview: the face.
  - **Taunts**: Off (D-pad switches weapons, the default) or a taunt set on D-pad down / left / right
    (Trash Talk, Gang Signs, Multiplayer), played standing still with a voice line 3 times in 4
    (zip 09). Preview: the set's first taunt.
- **Saved with the character**: they are normal creator sliders, so the save game keeps them (no side file).

## How

| Part | Where | What |
| --- | --- | --- |
| Slider | `patch.lua` | Adds a `body type` Morph_Info to the Body set of `player_creation.xtbl` (misc + misc2) and `PLAYER_MORPH_BODY_TYPE_TEXT=Body Type` to `US_Strings.txt` (entry count + 1). Its morph key names no mesh target, so the game's own morphing ignores it (as with Skin's `none` keys). |
| Old saves | `main.lua`, hook `0x82491388` | Sliders are numbered in file order and saves hold (index, value) pairs (`0x82491230` writes, `0x82491388` applies; story save, MP profile, MP sync). Body Type is index 67, the movement rows 68–74, so Hair (67→75) and Skin (68,69→76,77) move. A list of 70 (before Body Type) or 71 (before movement) pairs has its last three shifted in a copy before the game applies it. |
| Budget | wraps the screens' input handlers `0x822FDF40` (creator), `0x8231C3D0` (surgeon) | Body's `Max_Total_Value` (1, at `0x83C59410`) caps Muscle + Fat; the budget code (`0x82300210` / `0x823204C0`, which MinHook can't detour) takes the excess from row `1 - row`, so it only handles 2 rows. While the input runs with Body Type selected the cap is lifted; with Muscle/Fat selected it's raised by Body Type's value. |
| Randomize | hook `0x823227E8` | Body Randomize calls (return addresses `0x822FE8E0` creator, `0x82320864`/`0x82320880` surgeon) are rescaled into the real budget; the creator's call for Body Type is skipped and its row display put back. |
| Movement | `movement_style.lua`, hook `0x82226DA8`; `body_rows.lua` | Tweak Menu 2.6.6 movement override: the player's body instance gets the empty `SRB_Player` set (patch.lua, GML_CUST.xtbl) and its lookups are answered from the chosen parts' sets, then the player's own. Row names: each movement row's shown name (morph info +4) points at our UTF-16 text (allocated with `0x8271B570`), rewritten on change; the row widget keeps the pointer (`0x8228BAB0` stores it at node +8). A step past a row's last style is held there (`0x823227E8` calls returning to `0x82300348` / `0x82320630`). |
| Style lists | `movement_menu.lua` (inside the creator input wrapper) | BODY page rows 3–9 lose their slider widget (menu row node +16 cleared). A (action 55) on one starts a list page of our own in the same menu: `0x8228B9D0` (as the BODY builder calls it, title text `0x830331E0`), rows `0x8228BAB0` with no widget; while open the creator input is answered there: up/down `0x8228CBA0`/`0x8228CC90` (game cursor, scrolling, sound), A keeps (`0x823227E8`), B (56) back; leaving rebuilds the tab with `0x822FEDB8`. |
| Personality | `personality.lua`, `row_lists.lua`, `facial_expressions.lua` | Rows 10-12 (`personality voice/face/taunts`). Labels like the movement rows. Voice: `human_set_persona("#PLAYER#", "HM_LC2")` through the game's Lua (`G.script`), own voice remembered from `SP_??PLAYER` at human+3552 and written back for Default. Expression: Tweak Menu 2.6.6 `facial_expressions.lua` (hooks `0x82432250` / `0x82432460`, SR2 banks registered by `juiced_runtime.lua`), allowed on the creator / surgeon screens (`screen_ok`). Taunts: `XamInputGetState` hook hides D-pad down/left/right while a set is chosen and you're playing; `ACTION_PLAY` `0x82444F88` + `audio_play_persona_line_do`. Lists of up to 30 after 0 (row_lists.lua; old saves with 78 sliders shift Hair/Skin by 3). |
| Shape | `body_gender.lua` + render hook `0x82195330` (`facial_attachments.lua`) | Tweak Menu 2.6.6 runtime: private morph banks (`body_gender/*.bin`, format-2, validated), commands added to the player's mesh draws. Value = −50 × slider. Ticks from the game update `0x82209E30` and the creator screen's update `0x822FDC88`. |

Creator facts used (for later steps):

- Morph sets: 44 bytes each at `0x83C593E8` (+0 name, +4 slider count, +8 first slider, +40 Max_Total_Value);
  the 11 set names are a fixed list at `0x827ACBA8` (Body, Brow, Ears, Eyes, Nose, Cheeks, Mouth, Jaws, Face_scale, Hair, Skin).
- Sliders (morph infos): 36 bytes at `0x831169B8`, count `0x83117BB8` (max 128): +8 set, +12 initial, +16 base, +20 type, +24 index.
  The record's slider array is `record(+4)` → 12-byte entries {?, wanted +4, baked +8}.
- Creator tabs: names at `0x827AEFAC` (PRESETS, HEAD, EYES, BODY, SKIN); interface handlers at `0x820939A0`
  (open `0x822FD8B0`, close `0x822FDB80`, update `0x822FDC88`, input `0x822FDF40`); page builders by tab via `0x822FEDB8`:
  PRESETS `0x822FF068`, HEAD `0x822FF5A0`/`0x822FFA48`, BODY `0x822FFCA8`, EYES `0x822FFE68`, SKIN `0x822FF3C8`.
  Rows are added with `0x8228BAB0`; the tab bar with `0x822903E8`.
- Surgeon: Body page `0x8231FCE0` (rows {info, value} at page +128, items +456), randomize `0x82320750`.

## Not done yet / to check in game

Personality (added 2026-10-27, untested in game):
- BODY page shows Voice / Expression / Taunts rows after Melee Combat, with names; A opens each list.
- Voice preview plays a line in the creator (if the game's script state or audio isn't running in
  the creator, the voice still applies once you play). In play: lines, grunts and screams in that voice.
- Expression preview on the creator character; SR2 faces need `juiced_runtime/` and register private
  banks (log "Juiced: private facial library ... ready").
- Taunts: with a set chosen, D-pad down/left/right taunt (not weapon switching); Off gives the D-pad back.
- An older save (78 sliders): log "a character saved with 78 sliders - Hair and Skin moved up by 3", hair/skin unchanged.
- Not in multiplayer yet (G.player() is 0 there): other players don't hear/see these.

- **Needs an in-game test.** Nothing here has run yet. Check `mods\wml.log` (set `debug = true` in `mod.ini`):
  1. New game → creator → BODY: three sliders (Muscle, Fat, Body Type); Body Type moves on its own;
     Muscle/Fat still share their budget; X Randomize leaves Body Type alone.
  2. The female shape shows **in the creator** while sliding (the Tweak Menu never drew it there, so this
     is the first try: if the game update doesn't tick there, the creator's own update hook should).
  3. Save, reload: Body Type kept. Load an **older save**: hair length and skin tone unchanged (log line
     "sliders saved before Body Type existed moved up by one").
  4. Plastic Surgeon: the slider is there, shape previews, cancel/pay behave.
  5. Choosing a race preset after setting Body Type: does it reset Body Type? (presets set sliders by name.)
- Movement: the rows show their style names (if they show only "Stance", "Walk" ..., the label
  pointer isn't re-read: then the name only updates on re-entering the page); a chosen walk is used
  in free roam; the preview walks on the spot in the creator and stops when you leave the row.
- Other players don't see your female shape in multiplayer/co-op yet (render is local player only; the
  value already travels with the appearance).
- SR Tweak Menu still running on top: its own Face & Body slider list is now off by one for Hair/Skin;
  its own Body Type row and this one would both draw (leave its row at 0).
- A **Sex** Male/Female selector before Race (CONCEPTS.md) is the slider for now.
