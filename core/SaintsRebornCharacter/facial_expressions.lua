-- SR1's emotion getter (82432250) deliberately rejects player handles.
-- Supply the selected local player's emotion to the native facial builder;
-- leave its morph commands, fluctuation, lipsync and all NPCs native.
--
-- Saints Reborn (built in): SR Tweak Menu 2.6.6's facial_expressions.lua.
-- The choice comes from the creator's Expression row (personality.lua), and
-- it also shows in the character creator and on the Plastic Surgeon's Body
-- page (E.screen_ok), where the Tweak Menu turned faces off.
local G=require('game')
local J=require('juiced_player')
local E={enabled=function() return false end,screen_ok=function() return false end,selected=nil,preview=nil,cache={}}
-- the front end or a screen up: no face, except the creator / surgeon screens
local function blocked() return (G.in_front_end() or G.screen_up()) and not E.screen_ok() end
E.ADDR={lookup=0x8243D578,get=0x82432250,build=0x82432460,incapacitated=0x82448D18,
 table=0x8309A5B8,count=0x8370F840}
local r32,w32,rf=wml.read_u32,wml.write_u32,wml.read_f32
E.names={None=true,Angry=true,Disgust=true,Fear=true,Joy=true,Sad=true,Surprise=true}
for name in pairs(J.names) do E.names[name]=true end
local function desired() return E.preview or E.selected end
function E.resolve(ctx,name)
 if J.names[name] then return J.ready(ctx,G.player(),name) and -2 or nil end
 if not E.names[name] or name=='None' then return nil end
 local n=r32(E.ADDR.count)
 if n<1 or n>127 then return nil end
 local item=E.cache[name]
 if item and item.count==n and item.id<n and r32(E.ADDR.table+item.id*28)==item.hash then return item.id end
 local id=G.call(ctx,E.ADDR.lookup,{G.cstr(ctx,name,0)})
 if id>=n then return nil end
 local record=E.ADDR.table+id*28
 if r32(record+4)==0 or r32(record+12)==0 then return nil end
 E.cache[name]={id=id,count=n,hash=r32(record)}
 return id
end
local function owns(p)
 return E.state and E.state.player==p and r32(p+68)==E.state.handle
  and r32(p+G.ANIM_OWNER)==E.state.owner
end
function E.release()
 J.release()
 local s=E.state
 if s and G.player()==s.player and owns(s.player) then
  for a,v in pairs(s.words) do w32(a,v) end
 end
 E.state=nil
end
function E.request(ctx,name,preview)
 if not E.names[name] then return false,'Unknown facial expression' end
 if J.names[name] then
  if not E.enabled() then return false,'Only on your own character' end
  local ok,why=J.prepare(ctx,G.player())
  if not ok then return false,why end
 elseif name~='None' and E.enabled() then
  -- Native emotions too: worn facial pieces follow them through their private libraries.
  J.prepare(ctx,G.player())
 else
  J.disarm()
 end
 if name~='None' and not E.resolve(ctx,name) then return false,'Facial morph data is not ready' end
 if preview then E.preview=name else E.selected=name~='None' and name or nil;E.preview=nil end
 if name=='None' then E.release() end
 return true
end
function E.tick(selected)
 E.selected=E.names[selected] and selected~='None' and selected or nil
 if G.player()==0 or (G.in_front_end() and not E.screen_ok()) then E.release();J.disarm();E.cache={};return end
 if not E.enabled() then E.release();J.disarm() end
 if G.in_cutscene() or blocked() or rf(G.player()+G.P.HEALTH)<=0 then E.release() end
 if not desired() or desired()=='None' then E.release() end
end
function E.active(ctx,p)
 if p==0 or p~=G.player() or not E.enabled() or not desired() or desired()=='None'
  or blocked() or G.in_cutscene() or rf(p+G.P.HEALTH)<=0 then return nil end
 if (G.call(ctx,E.ADDR.incapacitated,{p})&255)~=0 then return nil end
 return E.resolve(ctx,desired())
end
wml.hook(E.ADDR.get,function(ctx)
 local p=ctx:r(3)&0xFFFFFFFF
 local id=E.active(ctx,p)
 if id then ctx:set_r(3,id==-2 and 0xFFFFFFFF or id) else ctx:call_original() end
end)
wml.hook(E.ADDR.build,function(ctx)
 local p=ctx:r(3)&0xFFFFFFFF
 local id=E.active(ctx,p)
 if id==-2 then
  if E.state then E.release() end
  -- Keep the native blink/lipsync stream intact. Private library commands
  -- are added to the local player's draw descriptor, after save loading.
  ctx:call_original()
  return
 end
 if id then
  J.release()
  if not owns(p) then
   E.release()
   E.state={player=p,handle=r32(p+68),owner=r32(p+G.ANIM_OWNER),words={}}
   for _,off in ipairs({3564,3568,3584}) do E.state.words[p+off]=r32(p+off) end
  end
  -- The native player path has never initialized an emotion. Its first
  -- transition would otherwise index the previous emotion at -1. Seed
  -- valid indices before allowing the normal native transition builder.
  local n=r32(E.ADDR.count)
  if r32(p+3564)>=n or r32(p+3568)>=n then
   w32(p+3564,id);w32(p+3568,id);w32(p+3584,0xFFFFFFFF)
  end
 elseif E.state and E.state.player==p then E.release() end
 ctx:call_original()
end)
function E.imported_name() local n=desired();return J.names[n] and n or nil end
require('facial_attachments').install(E)
return E
