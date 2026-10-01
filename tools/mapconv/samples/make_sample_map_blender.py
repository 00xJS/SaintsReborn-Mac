# Builds a small sample multiplayer map in Blender and exports it as .glb for the Saints Reborn
# Map Converter. Run: blender -b --python make_sample_map_blender.py -- <out.glb>
# It shows the conventions: 1 unit = 1 metre, markers are empties named spawn_*, team1_spawn_*,
# team2_spawn_*, weapon_<type>, player_start. A marker faces its -Y axis (like a character).
import bpy, bmesh, math, sys, random

out = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "sample_map.glb"
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene

def image(name, fn, n=256):
    img = bpy.data.images.new(name, n, n)
    px = [0.0] * (n * n * 4)
    for y in range(n):
        for x in range(n):
            r, g, b = fn(x, y, n)
            i = (y * n + x) * 4
            px[i:i + 4] = [r / 255, g / 255, b / 255, 1.0]
    img.pixels = px
    img.pack()
    return img

rnd = random.Random(1)
noise = [[rnd.random() for _ in range(256)] for _ in range(256)]
def grid(x, y, n):
    if x % 64 < 2 or y % 64 < 2: return (200, 170, 60)
    k = 0.92 + 0.16 * noise[y][x]; return (80 * k, 86 * k, 94 * k)
def brick(x, y, n):
    row = y // 32; bx = (x + (row % 2) * 32) % 64
    if y % 32 < 3 or bx < 3: return (175, 170, 158)
    k = 0.85 + 0.3 * noise[y][x]; return (150 * k, 68 * k, 46 * k)
def concrete(x, y, n):
    k = 0.8 + 0.25 * noise[y][x]
    if x % 128 < 2 or y % 128 < 2: k *= 0.7
    return (140 * k, 140 * k, 136 * k)
def hazard(x, y, n):
    return (235, 185, 25) if ((x + y) // 32) % 2 == 0 else (30, 30, 30)

def material(name, img):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    tex = m.node_tree.nodes.new("ShaderNodeTexImage")
    tex.image = img
    m.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    return m

MAT = {k: material(k, image(k + "_tex", f)) for k, f in
       (("floor", grid), ("wall", brick), ("block", concrete), ("ramp", hazard))}

def box(name, mat, cx, cy, sx, sy, sz, z0=0.0, uv_scale=0.5):
    """Box centred at (cx, cy) with size sx x sy x sz standing on z0 (Blender: Z up)."""
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co.x = cx + v.co.x * sx; v.co.y = cy + v.co.y * sy; v.co.z = z0 + (v.co.z + 0.5) * sz
    uv = bm.loops.layers.uv.new()
    for f in bm.faces:   # world-space box mapping: texture repeats every 1 / uv_scale metres
        nx, ny, nz = (abs(c) for c in f.normal)
        for l in f.loops:
            p = l.vert.co
            if nz >= nx and nz >= ny: l[uv].uv = (p.x * uv_scale, p.y * uv_scale)
            elif nx >= ny: l[uv].uv = (p.y * uv_scale, p.z * uv_scale)
            else: l[uv].uv = (p.x * uv_scale, p.z * uv_scale)
    me = bpy.data.meshes.new(name); bm.to_mesh(me); bm.free()
    me.materials.append(MAT[mat])
    ob = bpy.data.objects.new(name, me); scene.collection.objects.link(ob)
    return ob

H = 30
box("Floor", "floor", 0, 0, 2 * H, 2 * H, 0.5, z0=-0.5, uv_scale=0.25)
for i, (cx, cy, sx, sy) in enumerate(((0, -H, 2 * H + 1, 1), (0, H, 2 * H + 1, 1), (-H, 0, 1, 2 * H), (H, 0, 1, 2 * H))):
    box("Wall%d" % i, "wall", cx, cy, sx, sy, 6, uv_scale=0.25)
for i, (cx, cy, sx, sy, sz) in enumerate(((-12, -12, 4, 1.2, 1.4), (12, 12, 4, 1.2, 1.4), (-12, 12, 1.2, 4, 1.4),
                                          (12, -12, 1.2, 4, 1.4), (-22, -22, 3, 3, 3), (22, 22, 3, 3, 3),
                                          (-22, 22, 3, 3, 3), (22, -22, 3, 3, 3))):
    box("Cover%d" % i, "block", cx, cy, sx, sy, sz)
box("Tower", "block", 0, 0, 8, 8, 3)
# ramp: a box whose top slopes from the tower (z 3) down to the floor
r = box("Ramp", "ramp", 0, -9, 3, 10, 3)
for v in r.data.vertices:
    if v.co.z > 1 and v.co.y < -9: v.co.z = 0.02
# a second level: bridge between two corner blocks
box("Bridge", "block", 0, 22, 30, 3, 0.4, z0=3.0)

def empty(name, x, y, z, yaw_deg=0.0):
    e = bpy.data.objects.new(name, None)
    e.empty_display_type = 'ARROWS'
    e.location = (x, y, z)
    e.rotation_euler = (0, 0, math.radians(yaw_deg))  # faces its -Y axis
    scene.collection.objects.link(e)
    return e

spawns = [(-25, -25), (25, 25), (-25, 25), (25, -25), (0, -26), (0, 26), (-26, 0), (26, 0)]
for i, (x, y) in enumerate(spawns):
    empty("spawn_%d" % (i + 1), x, y, 0.1, math.degrees(math.atan2(-x, y)))   # -Y towards the centre
for i, (x, y) in enumerate(((-25, -20), (-20, -25), (-25, -15))):
    empty("team1_spawn_%d" % (i + 1), x, y, 0.1)
for i, (x, y) in enumerate(((25, 20), (20, 25), (25, 15))):
    empty("team2_spawn_%d" % (i + 1), x, y, 0.1)
empty("player_start", 0, -20, 0.1)
for name, x, y, z in (("weapon_rpg_launcher", 0, 0, 3.1), ("weapon_ak47", -16, -16, 0.1), ("weapon_ak47", 16, 16, 0.1),
                      ("weapon_pump_action_shotgun", -16, 16, 0.1), ("weapon_spas12", 16, -16, 0.1),
                      ("weapon_sniper_rifle", 0, 22, 3.5), ("weapon_tec9", 0, -24, 0.1), ("weapon_m16", -24, 0, 0.1),
                      ("weapon_mac10", 24, 0, 0.1), ("weapon_molotov", -8, 0, 0.1), ("weapon_pipe_bomb", 8, 0, 0.1)):
    empty(name, x, y, z)

bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', export_extras=True, export_apply=True)
print("SAMPLE MAP WRITTEN", out)
