-- Reuse the face's native command stream on worn facial pieces. The normal
-- clothing draw resolves morphs through a second name/mesh registry; the
-- component +44 owns its persistent render library; +48 is a temporary
-- customization library that is released once the mesh is rebuilt.
local G=require('game')
local J=require('juiced_player')
local R=require('juiced_runtime')
local B=require('body_gender')
local A={draw=0x82195330,player_for=0x826F0BC0,find_target=0x8222C418,
 library=0x82C3BE00}
local r32,r8,w8,w16=wml.read_u32,wml.read_u8,wml.write_u8,wml.write_u16
local function pointer(p) return G.pointer_ok(p) end
local function facial_piece(p)
 if not pointer(p) then return false end
 local s={}
 for i=0,63 do local b=r8(p+i);if b==0 then break end;s[#s+1]=string.char(b) end
 local name=table.concat(s):lower()
 return name:match('^heyeb[_%.]') or name:match('^hbear[_%.]')
  or name:match('^hmust[_%.]') or name:match('^hsbur[_%.]')
end
function A.commands(ctx,p,mesh)
 local component=R.component(p,mesh)
 if not component or not facial_piece(r32(component)) then return nil end
 local id=r32(component+44)
 if id>=256 then return nil end
 local data=r32(A.library+id*24+8)
 if not pointer(data) then return nil end
 local format,total=r32(data+24),r32(data+8)
 if (format~=0 and format~=2) or total<1 or total>256 or not pointer(r32(data+28)) then return nil end
 local owner=r32(p+G.ANIM_OWNER)
 if not pointer(owner) then return nil end
 local anim=G.call(ctx,A.player_for,{r32(owner+3592)})
 if not pointer(anim) or r32(anim+7160)~=0x82432C70 or r32(anim+7164)~=p then return nil end
 local count=r8(anim+7253)
 if count<1 or count>20 then return nil end
 local result={}
 for i=0,count-1 do
  local command=anim+6912+i*12
  local weight=wml.read_f32(command+8)
  if weight==weight and weight>0 then
   local target=G.call(ctx,A.find_target,{data,r32(command+4)})
   if target<total and target<256 then
    result[#result+1]={id=id,target=target,weight=math.floor(math.min(1,weight)*255)}
   end
  end
 end
 -- No matching targets is a legitimate native fallback (old/custom assets).
 return #result>0 and result or nil
end
-- The face's native command stream (blink, lipsync, the six native emotions) as
-- {hash, weight 0..255}. Read from the player's animation player, as A.commands does.
function A.face_stream(ctx,p)
 local owner=r32(p+G.ANIM_OWNER)
 if not pointer(owner) then return nil end
 local anim=G.call(ctx,A.player_for,{r32(owner+3592)})
 if not pointer(anim) or r32(anim+7160)~=0x82432C70 or r32(anim+7164)~=p then return nil end
 local count=r8(anim+7253)
 if count<1 or count>20 then return nil end
 local out={}
 for i=0,count-1 do
  local command=anim+6912+i*12
  local weight=wml.read_f32(command+8)
  if weight==weight and weight>0 then out[#out+1]={hash=r32(command+4),weight=math.floor(math.min(1,weight)*255)} end
 end
 return out
end
-- The same stream on a worn facial piece, through the piece's private library
-- (juiced_runtime: its 42 imported keys plus its own native targets). The game's
-- own piece library is not wired to the face in SR1, so pieces never followed.
function A.private_native(ctx,p,mesh)
 local name=R.mesh(mesh,p)
 if not name or name=='pc_body' then return nil end
 local b=R.for_mesh(name,p)
 if not b or not b.native or not next(b.native) then return nil end
 local stream=A.face_stream(ctx,p)
 if not stream then return nil end
 local out={}
 for _,c in ipairs(stream) do
  local k=b.native[c.hash]
  if k then out[#out+1]={id=b.id,target=k,weight=c.weight} end
 end
 return #out>0 and out or nil
end
function A.install(E)
 wml.hook(A.draw,function(ctx)
  local p=ctx:r(4)&0xFFFFFFFF
  local descriptor=ctx:r(5)&0xFFFFFFFF
  local commands,imported_mesh
  if p==G.player() and pointer(descriptor) then
   local active=E.active(ctx,p)
   if active then
    local mesh=ctx:r(3)&0xFFFFFFFF
    commands=A.commands(ctx,p,mesh) or A.private_native(ctx,p,mesh)
    if active==-2 then
     local name=R.mesh(mesh,p)
     local imported=name and J.commands(ctx,p,name,E.imported_name())
     if imported then
      imported_mesh=name
      local native=commands
      if not native then
       local n=wml.read_u16(descriptor+52)
       if n>20 then ctx:call_original();return end
       native={}
       for i=0,n-1 do
        local at=descriptor+54+i*4
        native[#native+1]={weight=r8(at),target=r8(at+1),id=wml.read_u16(at+2)}
       end
      end
      commands={}
      for i=1,math.min(#native,20-#imported) do commands[#commands+1]=native[i] end
      for _,c in ipairs(imported) do commands[#commands+1]=c end
     end
    end
   end
  end
  -- Talking: SR2-style mouth movement on the face and on worn facial pieces.
  local TALK=package.loaded.talk_face
  if TALK and TALK.active() and p==G.player() and pointer(descriptor) then
   local name=R.mesh(ctx:r(3)&0xFFFFFFFF,p)
   local bank=name and R.for_mesh(name,p)
   local talk=bank and TALK.commands(bank)
   if talk then
    if not commands then
     local n=wml.read_u16(descriptor+52)
     if n<=20 then
      commands={}
      for i=0,n-1 do
       local at=descriptor+54+i*4
       commands[#commands+1]={weight=r8(at),target=r8(at+1),id=wml.read_u16(at+2)}
      end
     end
    end
    if commands then
     for _,c in ipairs(talk) do if #commands<20 then commands[#commands+1]=c end end
    end
   end
  end
  local body=pointer(descriptor) and B.commands(p,ctx:r(3)&0xFFFFFFFF)
  if body and #body>0 then
   if not commands then
    local count=wml.read_u16(descriptor+52)
    if count>20 then ctx:call_original();return end
    commands={}
    for i=0,count-1 do
     local at=descriptor+54+i*4
     commands[#commands+1]={weight=r8(at),target=r8(at+1),id=wml.read_u16(at+2)}
    end
   end
   if #commands+#body>20 then
    table.sort(commands,function(a,b) return a.weight>b.weight end)
    while #commands>20-#body do table.remove(commands) end
   end
   for _,command in ipairs(body) do commands[#commands+1]=command end
  end
  if not commands then ctx:call_original();return end
  -- The renderer copies these 80 bytes into its own draw packet. Restore
  -- the caller's descriptor afterwards, including when an error propagates.
  local saved={}
  for i=0,81 do saved[i]=r8(descriptor+52+i) end
  w16(descriptor+52,#commands)
  for i,c in ipairs(commands) do
   local at=descriptor+54+(i-1)*4
   w8(at,c.weight);w8(at+1,c.target);w16(at+2,c.id)
  end
  local ok,err=pcall(function() ctx:call_original() end)
  for i=0,81 do w8(descriptor+52+i,saved[i]) end
  if not ok then error(err,0) end
  if imported_mesh then R.note_draw(imported_mesh) end
 end)
end
return A
