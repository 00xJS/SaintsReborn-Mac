-- Private raw libraries, registered only from a loaded game's menu/update.
-- Never replace packfiles, native libraries, mesh bindings or streaming pools.
local G=require('game')
local C=require('juiced_faces')
local R={banks={},pending={},failed={},ADDR={alloc=0x8271B570,register=0x8222CB70,
 free=0x8370DB94,registry=0x82C3BE00}}
local r32,r8,w32=wml.read_u32,wml.read_u8,wml.write_u32
local crc_table={}
for i=0,255 do
 local v=i
 for _=1,8 do v=(v>>1)~((v&1)~=0 and 0xEDB88320 or 0) end
 crc_table[i]=v
end
local function crc(s)
 local v=0xFFFFFFFF
 for i=1,#s do v=(v>>8)~crc_table[(v~s:byte(i))&255] end
 return v~0xFFFFFFFF
end
function R.text(p)
 if not G.pointer_ok(p) then return nil end
 local s={}
 for i=0,95 do
  local b=r8(p+i)
  if b==0 then return table.concat(s):lower() end
  if b<32 or b>126 then return nil end
  s[#s+1]=string.char(b)
 end
end
function R.component(p,mesh)
 local record=r32(p+G.CUSTOM)
 if not G.pointer_ok(record) then return nil end
 local first=r32(record);local node=first;local seen={}
 for _=1,64 do
  if not G.pointer_ok(node) or seen[node] then break end
  seen[node]=true
  local c=r32(node+12)
  if G.pointer_ok(c) and r32(c+8)==mesh then return c end
  node=r32(node+16)
  if node==first then break end
 end
end
function R.mesh(mesh,p)
 if not G.pointer_ok(mesh) or not p or p~=G.player() then return nil end
 -- 824659E0 draws *(model_resource+88), not a wear-option record.
 local owner=r32(p+G.ANIM_OWNER)
 if G.pointer_ok(owner) and r8(owner+3740)==1 then
  local model=r32(owner+3732)
  if G.pointer_ok(model) and r32(model+88)==mesh then return 'pc_body' end
 end
 -- 8248E118 queues component+8 as the rendered worn mesh. Its name is
 -- component+0; reading render_mesh+4 as a name silently loses every match.
 local c=R.component(p,mesh)
 local name=c and R.text(r32(c))
 if not name then return nil end
 name=name:match('[^/\\]+$') or name
 -- Worn pieces are named like 'hbear_bushyb.cmeshx' (as body_gender.lua's B.name);
 -- without stripping '.cmeshx' no facial-hair bank ever matched (2.5.7 in game).
 name=name:gsub('%.cmesh_xbox2$',''):gsub('%.cmesh_pc$',''):gsub('%.cmeshx$',''):gsub('_pc_facial$',''):gsub('_pc$','')
 return C.banks[name] and name or nil
end
-- Saints Reborn: also on the creator / surgeon screens (personality.lua sets R.screen_ok).
function R.screen_ok() return false end
function R.playing(p)
 return p~=0 and p==G.player() and ((not G.in_front_end() and not G.screen_up()) or R.screen_ok())
  and not G.in_cutscene() and wml.read_f32(p+G.P.HEALTH)>0
end
function R.valid(mesh)
 local b=R.banks[mesh]
 if not b or not b.id or b.id>=256 then return nil end
 local entry=R.ADDR.registry+b.id*24
 local m=C.banks[mesh]
 if r32(entry+8)~=b.memory or r32(b.memory+8)~=(m and m.keys or 42) or r32(b.memory+24)~=2 then return nil end
 return b
end
function R.owns(p)
 local s=R.armed
 return s and R.playing(p) and s.player==p and s.handle==r32(p+68)
  and s.owner==r32(p+G.ANIM_OWNER)
end
function R.disarm() R.armed=nil;R.pending={};R.failed={} end
-- A linked free-list check keeps headroom for the game's normal clothing.
local function room()
 local first=r32(R.ADDR.free);local node=first;local seen={};local n=0
 while node~=0 and not seen[node] do
  local off=node-R.ADDR.registry
  if off<0 or off>=256*24 or off%24~=0 or r32(node+8)~=0 then return false end
  seen[node]=true;n=n+1
  if n>=9 then return true end
  node=r32(node)
 end
 return false
end
function R.validate(s,m)
 if #s~=m.bytes or #s%4~=0 or crc(s)~=m.crc then return false end
 local function u(at) return string.unpack('>I4',s,at+1) end
 -- 42 imported keys, then (facial pieces) the piece's own native targets
 local keys=m.keys or 42
 if u(0)~=0x1337BEEF or u(4)~=1 or u(8)~=keys or keys<42 or keys>255 or u(12)~=1 or u(16)~=40
  or u(20)~=44 or u(24)~=2 or u(28)~=48 or u(32)~=0xFFFFFFFF then return false end
 local refs=u(36)
 if refs<48+keys*48 or refs%16~=0 or refs+m.vertices*4~=#s then return false end
 local i=0
 for _,clip in ipairs(C.clips) do for _,key in ipairs(clip.keys) do
  local at=48+i*48;local n,off=u(at+4),u(at+44)
  if u(at)~=key[2] or n>m.vertices or off%16~=0 or off<48+keys*48 or off+n*16>refs then return false end
  i=i+1
 end end
 for k=42,keys-1 do
  local at=48+k*48;local n,off=u(at+4),u(at+44)
  if n>m.vertices or off%16~=0 or off<48+keys*48 or off+n*16>refs then return false end
 end
 return i==42
end
function R.load(ctx,mesh)
 local b=R.valid(mesh)
 if b then return b end
 local m=C.banks[mesh]
 if not m then return nil,'No converted facial mesh for '..tostring(mesh) end
 if not room() then return nil,'Facial library slots are busy; keeping the native face' end
 local file=io.open(wml.mod_folder..'/juiced_runtime/'..m.file,'rb')
 if not file then return nil,'Expression data is missing' end
 local s=file:read(m.bytes+1);file:close()
 if not s or not R.validate(s,m) then return nil,'Expression data failed validation' end
 b=R.banks[mesh] or {}
 -- native target hash -> index in this library (facial pieces, for the six native expressions)
 b.native={}
 for k=42,(m.keys or 42)-1 do b.native[string.unpack('>I4',s,48+k*48+1)]=k end
 if not b.memory then
  local memory=G.call(ctx,R.ADDR.alloc,{0,m.bytes,0x3000,4})
  if not G.pointer_ok(memory) or memory+m.bytes>=0xC0000000 then return nil,'No memory for facial expression' end
  b.memory=memory;R.banks[mesh]=b
 end
 -- Rewrite from checked relative offsets whenever the registry was reset.
 for at=0,#s-4,4 do w32(b.memory+at,(string.unpack('>I4',s,at+1))) end
 local id=G.call(ctx,R.ADDR.register,{G.cstr(ctx,'srt_face_'..mesh,0),b.memory})
 if id>=256 or r32(R.ADDR.registry+id*24+8)~=b.memory then return nil,'Could not register facial expression' end
 b.id=id
 -- Same persistent bit used by the native player's face (8247CF90).
 local flag=R.ADDR.registry+id*24+20
 wml.write_u8(flag,r8(flag)|0x80)
 wml.log('Juiced: private facial library '..mesh..' ready')
 return b
end
function R.prepare(ctx,p)
 if not R.playing(p) then return false,'Load your save before choosing an expression' end
 local owner=r32(p+G.ANIM_OWNER)
 if not G.pointer_ok(owner) then return false,'Player face is not ready' end
 local b,why=R.load(ctx,'pc_body')
 if not b then return false,why end
 R.failed={}
 R.armed={player=p,handle=r32(p+68),owner=owner}
 return true
end
function R.update(ctx,p)
 if not R.owns(p) then
  if p==0 or (G.in_front_end() and not R.screen_ok()) or (R.armed and
   (p~=R.armed.player or r32(p+68)~=R.armed.handle or r32(p+G.ANIM_OWNER)~=R.armed.owner)) then R.disarm() end
  return
 end
 -- Draws queue only meshes actually worn. No file I/O or allocation on render threads.
 local mesh=next(R.pending)
 if mesh then
  R.pending[mesh]=nil
  local _,why=R.load(ctx,mesh)
  if why then R.failed[mesh]=why;wml.log('Juiced: '..why) end
 end
end
function R.for_mesh(mesh,p)
 if not R.owns(p) or not C.banks[mesh] then return nil end
 local b=R.valid(mesh)
 if not b and not R.failed[mesh] then R.pending[mesh]=true end
 return b
end
function R.note_draw(mesh)
 local b=R.valid(mesh)
 if b and not b.submitted then
  b.submitted=true
  wml.log('Facial expression: commands submitted for '..mesh)
 end
end
return R
