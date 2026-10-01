-- Multiplayer (built in): mod.ini [settings] solo_start lets one player start
-- a System Link match (to try out maps alone). The lobby's "needs at least 2
-- player(s)" check (sub_82392758) is skipped while byte 0x8370F28A is set.
if wml.setting("solo_start", true) then
  wml.on_frame(function()
    if wml.read_u8(0x8370F28A) == 0 then wml.write_u8(0x8370F28A, 1) end
  end)
end
