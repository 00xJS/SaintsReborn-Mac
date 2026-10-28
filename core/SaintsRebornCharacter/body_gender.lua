-- SR2's body-gender shape, transferred onto the player's SR1 skin and clothes.
-- Private render-only banks: no pack overrides or rig replacement.
--
-- Saints Reborn (built in): from SR Tweak Menu 2.6.6's body_gender.lua. The
-- value no longer comes from a file and a menu row: main.lua reads the
-- creator's own Body Type slider (saved with the character) and hands it in
-- with B.set_value. Also works in the character creator and at the Plastic
-- Surgeon (the Tweak Menu turned it off in the front end and while a screen
-- was up), so the shape previews live while you choose.
local G=require('game')
local C=require('body_gender_data')
-- Flat Top B's files hold the SR2 Medium Parted hair (patch.lua, sr2_hair):
-- its gender target was made for the stock mesh's 260 vertices, not this one.
if wml.setting('sr2_hair',false) then C.meshes['htop_flattopb']=nil;C.fixed['htop_flattopb']=true end
-- The built-in clothing (core/SaintsRebornClothing, boots = replace) puts the
-- Timberland boots' own mesh in the Hiking Boots' files: their bank was made
-- for the stock boot's vertices, so the boots stay unshaped.
C.meshes['cshoe_hikingboots']=nil;C.fixed['cshoe_hikingboots']=true
local R=require('juiced_runtime')
local B={value=0,banks={},enabled=function() return false end,inputs=function() return 0,0 end}
B.ADDR={alloc=0x8271B570,register=0x8222CB70,registry=0x82C3BE00,free=0x8370DB94}
local r32,r8,w32=wml.read_u32,wml.read_u8,wml.write_u32
-- The character creator (G.FRONT_END = 1) shows your own player, so the
-- shape works there too; the main menu (-1) has no player.
function B.in_creator() return wml.read_u32(G.FRONT_END)==1 end
local function front_end_blocked() return G.in_front_end() and not B.in_creator() end
local crc_table={}
for i=0,255 do local v=i;for _=1,8 do v=(v>>1)~((v&1)~=0 and 0xEDB88320 or 0) end;crc_table[i]=v end
local function crc(s)
 local v=0xFFFFFFFF
 for i=1,#s do v=(v>>8)~crc_table[(v~s:byte(i))&255] end
 return v~0xFFFFFFFF
end
function B.name(s)
 if not s then return nil end
 s=(s:lower():match('[^/\\]+$') or s):gsub('%.cmesh_xbox2$',''):gsub('%.cmesh_pc$',''):gsub('%.cmeshx$','')
 if not C.meshes[s] and not C.fixed[s] then s=s:gsub('_pc$','') end
 return s
end
local function playing(p)
 return p~=0 and p==G.player() and B.enabled() and not front_end_blocked()
end
local function identity(p)
 local owner=r32(p+G.ANIM_OWNER);local record=r32(p+G.CUSTOM)
 if not G.pointer_ok(owner) or not G.pointer_ok(record) or r8(owner+3740)~=1 then return nil end
 local model=r32(owner+3732)
 if not G.pointer_ok(model) or not G.pointer_ok(r32(model+88)) then return nil end
 local mesh=r32(model+88);local root=r32(mesh+48)
 if not G.pointer_ok(root) or wml.read_u16(root+28)~=1 then return nil end
 local buffers=r32(root+32);local resource=G.pointer_ok(buffers) and r32(buffers+8) or 0
 if not G.pointer_ok(resource) or wml.read_u16(resource+12)~=C.banks[C.meshes.pc_body.bank].vertices[1] then return nil end
 return {player=p,handle=r32(p+68),owner=owner,record=record,model=model,mesh=mesh}
end
function B.owns(p)
 local s=B.armed
 return s and playing(p) and s.player==p and s.handle==r32(p+68)
  and s.owner==r32(p+G.ANIM_OWNER) and s.record==r32(p+G.CUSTOM)
  and r8(s.owner+3740)==1 and s.model==r32(s.owner+3732) and s.mesh==r32(s.model+88)
end
function B.valid(index)
 local b=B.banks[index];local m=C.banks[index]
 if not b or not b.id or b.id>=256 then return nil end
 if r32(B.ADDR.registry+b.id*24+8)~=b.memory or r32(b.memory+8)~=m.targets
  or r32(b.memory+24)~=2 then return nil end
 return b
end
function B.validate(s,m)
 if not s or #s~=m.bytes or #s%4~=0 or crc(s)~=m.crc then return false end
 local function u(at) return string.unpack('>I4',s,at+1) end
 local nb=#m.vertices
 if u(0)~=0x1337BEEF or u(4)~=1 or u(8)~=m.targets or m.targets>256 or u(12)~=nb
  or u(16)~=40 or u(20)~=40 or u(24)~=2 or u(32)~=0xFFFFFFFF then return false end
 local table_at,refs=u(28),u(36);local total=0
 for i,n in ipairs(m.vertices) do
  if string.unpack('>I2',s,41+(i-1)*2)~=n then return false end
  total=total+n
 end
 if refs+total*4~=#s or refs%16~=0 or table_at<40+nb*2 or table_at+m.targets*48>refs then return false end
 for i=0,m.targets-1 do
  local at=table_at+i*48;local count,off=u(at+4),u(at+44)
  if count>total or off%16~=0 or off<table_at+m.targets*48 or off+count*16>refs then return false end
 end
 return true
end
local function room()
 local node=r32(B.ADDR.free);local seen={};local count=0
 while node~=0 and not seen[node] do
  local off=node-B.ADDR.registry
  if off<0 or off>=256*24 or off%24~=0 or r32(node+8)~=0 then return false end
  seen[node]=true;count=count+1;if count>=9 then return true end
  node=r32(node)
 end
 return false
end
local function load_step(ctx,index)
 local m=C.banks[index];local b=B.banks[index] or {};B.banks[index]=b
 if not b.raw then
  if not room() then return false,'Body shape: native clothing slots are busy' end
  local file=io.open(wml.mod_folder..'/body_gender/'..m.file,'rb')
  if not file then return false,'Body shape data is missing' end
  local s=file:read(m.bytes+1);file:close()
  if not B.validate(s,m) then return false,'Body shape data failed validation' end
  if not b.memory then
   local memory=G.call(ctx,B.ADDR.alloc,{0,m.bytes,0x3000,4})
   if not G.pointer_ok(memory) or memory+m.bytes>=0xC0000000 then return false,'No memory for body shape' end
   b.memory=memory
  end
  b.raw=s;b.at=0;b.id=nil
 end
 -- Limit guest-memory writes per update; render hooks never load files.
 local last=math.min(#b.raw,b.at+32768)
 for at=b.at,last-4,4 do w32(b.memory+at,(string.unpack('>I4',b.raw,at+1))) end
 b.at=last
 if last<#b.raw then return true end
 local id=G.call(ctx,B.ADDR.register,{G.cstr(ctx,'srt_body_'..index,0),b.memory})
 b.raw=nil
 if id>=256 or r32(B.ADDR.registry+id*24+8)~=b.memory then return false,'Could not register body shape' end
 b.id=id
 local flag=B.ADDR.registry+id*24+20;wml.write_u8(flag,r8(flag)|0x80)
 return true
end
function B.disarm()
 B.armed=nil;B.session=nil;B.ready=false;B.draws={};B.needed={}
 -- An interrupted write must restart from checked relative offsets.
 for _,b in pairs(B.banks) do if b.raw then b.raw=nil;b.at=nil end end
end
-- The Body Type slider, as -50 (female) .. 0 (male). Arming happens in
-- B.update once the player's own body is ready.
function B.set_value(value)
 value=tonumber(value)
 if not value or value~=value then return end
 value=math.floor(math.max(-50,math.min(0,value))+.5)
 if value==B.value then return end
 if value>=0 then B.disarm() end
 B.value=value;B.error=nil;B.failed={}
end
function B.update(ctx,p)
 -- A session starts from the slider or, for a saved female shape, by itself. Rebind
 -- the same player after the outfit/cutscene rebuilds its render instance.
 if p==0 or front_end_blocked() or not B.enabled() then B.disarm();return end
 if not B.owns(p) then
  local session=B.session
  local next_identity=session and session.player==p and (session.handle==r32(p+68) or G.in_cutscene()) and identity(p)
  -- A female shape arms on its own once your own body is ready (after
  -- loading a save too), also with the creator or a store screen up.
  if not next_identity and B.value<0 and not G.in_cutscene() then
   next_identity=identity(p)
   if next_identity then B.session={player=p,handle=r32(p+68)} end
  end
  if next_identity then B.armed=next_identity;B.error=nil
  else B.ready=false;return end
 end
 B.warning=nil
 local s=B.armed;local draws={[s.mesh]='pc_body'};local needed={};needed[C.meshes.pc_body.bank]=true
 local first=r32(s.record);local node=first;local seen={}
 for _=1,64 do
  if node==0 then break end
  if not G.pointer_ok(node) then B.warning='Clothing list rebuilding';break end
  if seen[node] then if node~=first then B.warning='Clothing list rebuilding';break end;break end
  seen[node]=true
  local component=r32(node+12)
  if G.pointer_ok(component) and G.pointer_ok(r32(component+8)) then
   local name=B.name(R.text(r32(component)));local info=name and C.meshes[name]
   if not info and not (name and C.fixed[name]) then
    B.warning='Unconverted clothing: '..tostring(name)
   end
   if info or (name and C.fixed[name]) then draws[r32(component+8)]=name end
   if info then needed[info.bank]=true end
  end
  node=r32(node+16)
 end
 -- Keep already prepared parts shaped while the next garment uploads.
 B.draws=draws;B.needed=needed;B.ready=B.valid(C.meshes.pc_body.bank)~=nil;B.pending=false
 local missing
 for index in pairs(needed) do if not B.valid(index) then B.pending=true;if not missing or index<missing then missing=index end end end
 if missing and not (B.failed and B.failed[missing]) then
  local ok,why=load_step(ctx,missing)
  if not ok then B.error=why;B.failed=B.failed or {};B.failed[missing]=why;wml.log(why) end
 end
 local fat,muscle=B.inputs(p);fat=math.max(0,math.min(1,fat or 0));muscle=math.max(0,math.min(1,muscle or 0))
 B.weights={(1-fat)*(1-muscle),fat*(1-muscle),(1-fat)*muscle,fat*muscle}
end
function B.commands(p,mesh)
 if not B.ready or B.value>=0 or not G.pointer_ok(p) then return nil end
 if not B.owns(p) then
  -- Some cutscenes draw a second actor with the player's customization
  -- record. Only that shared record and the native body signature qualify.
  local own=B.armed
  local record=r32(p+G.CUSTOM)
  local sliders=G.pointer_ok(record) and r32(record+4) or 0
  local same_record=own and (record==own.record or (G.pointer_ok(sliders) and sliders==r32(own.record+4)))
  if not own or not G.in_cutscene() or front_end_blocked() or not B.enabled()
   or not same_record or not identity(p) then return nil end
 end
 local name=B.draws and B.draws[mesh]
 if not name then
  local who=identity(p)
  if who and who.mesh==mesh then name='pc_body'
  else local c=R.component(p,mesh);name=c and B.name(R.text(r32(c))) end
 end
 local m=name and C.meshes[name]
 if not m then return nil end
 local bank=B.valid(m.bank);if not bank then return nil end
 local result={};local amount=-B.value*255/50
 for i,w in ipairs(B.weights or {}) do
  local weight=math.floor(amount*w+.5)
  if weight>0 then result[#result+1]={id=bank.id,target=m.target+i-1,weight=weight} end
 end
 -- breast physics (breast_physics.lua): this mesh's 8 bounce targets, past 25 % female
 local bounce=m.jiggle and require('breast_physics').commands(bank.id,m.jiggle,B.value)
 if bounce then for _,c in ipairs(bounce) do result[#result+1]=c end end
 return result
end
function B.note()
 if B.error then return B.error end
 if B.value>=0 then return 'Male' end
 if not B.armed then return 'Saved; applies when your body is ready' end
 if B.warning then return B.warning end
 return B.ready and (B.pending and 'Preparing new clothing; body stays shaped' or 'Female -50 / Male 0') or 'Preparing body and clothing...'
end
return B
