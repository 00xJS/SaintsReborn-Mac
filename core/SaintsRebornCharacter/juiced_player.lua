-- Play the baked source poses through SR1's normal facial command stream.
-- The body stays under the native animation player, including its weapons.
local G=require('game')
local J=require('juiced_faces')
local R=require('juiced_runtime')
J.ADDR={clock=0x827AA6E0}
J.BLEND=250      -- ms to blend from one face to the next
J.TALKING=0.4    -- how much of a held face stays while you talk
J.indices={}
do
 local index=0
 for _,clip in ipairs(J.clips) do for _,key in ipairs(clip.keys) do J.indices[key[2]]=index;index=index+1 end end
end
local r32=wml.read_u32
function J.library(ctx,p)
 if not R.owns(p) then return nil end
 local b=R.valid('pc_body')
 if b then return b.memory,42 end
end
function J.ready(ctx,p,name)
 return J.names[name]~=nil and J.library(ctx,p)~=nil
end
function J.prepare(ctx,p) return R.prepare(ctx,p) end
function J.update(ctx,p) R.update(ctx,p) end
function J.disarm() R.disarm();J.release() end
function J.release() J.state=nil end
function J.sample(ctx,p,name)
 local c=J.names[name]
 if not c or not J.ready(ctx,p,name) then J.release();return nil end
 local now=r32(J.ADDR.clock);local handle=r32(p+68);local owner=r32(p+G.ANIM_OWNER)
 local s=J.state
 if not s or s.name~=name or s.player~=p or s.handle~=handle or s.owner~=owner then
  -- (the face it replaces fades out while this one fades in: J.BLEND ms)
  if s and s.name~=name and s.player==p then J.prev={name=s.name,ended=now} end
  s={name=name,player=p,handle=handle,owner=owner,start=now};J.state=s
 end
 local elapsed=(now-s.start)&0xFFFFFFFF
 -- A new session can reset the native clock instead of wrapping it.
 if elapsed>0x7FFFFFFF then s.start=now;elapsed=0 end
 local frame=math.min(c.frames,elapsed*0.03)
 local keys=c.keys
 if frame<=keys[1][1] then return {{keys[1][2],1}} end
 for i=2,#keys do
  if frame<=keys[i][1] then
   local a,b=keys[i-1],keys[i];local f=(frame-a[1])/(b[1]-a[1])
   return {{a[2],1-f},{b[2],f}}
  end
 end
 return {{keys[#keys][2],1}}
end
function J.commands(ctx,p,mesh,name)
 local b=R.for_mesh(mesh,p)
 if not b then return nil end
 local sample=J.sample(ctx,p,name)
 if not sample then return nil end
 local now=r32(J.ADDR.clock)
 local fade=J.BLEND>0 and math.min(1,((now-J.state.start)&0xFFFFFFFF)/J.BLEND) or 1
 -- while you talk the mouth follows the speech, so the held face eases off
 local TALK=package.loaded.talk_face
 local hold=(TALK and TALK.active()) and J.TALKING or 1
 local commands={}
 for _,c in ipairs(sample) do
  local w=c[2]*fade*hold
  if w>0 then commands[#commands+1]={id=b.id,target=J.indices[c[1]],weight=math.floor(math.min(1,w)*255)} end
 end
 local pv=J.BLEND>0 and J.prev and J.names[J.prev.name]
 if pv then
  local out=1-((now-J.prev.ended)&0xFFFFFFFF)/J.BLEND
  if out<=0 or out>1 then J.prev=nil
  else
   local key=pv.keys[#pv.keys][2]
   commands[#commands+1]={id=b.id,target=J.indices[key],weight=math.floor(out*hold*255)}
  end
 end
 return commands
end
return J
