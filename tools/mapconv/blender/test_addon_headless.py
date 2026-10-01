# Headless test: installs the extension zip, builds a scene with the add-on's operators, exports a pack.
# blender -b --factory-startup --python test_addon_headless.py -- <zip> <converter exe> <maps folder>
import sys, bpy, addon_utils
zip_path, exe, maps = sys.argv[sys.argv.index("--") + 1:][:3]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.extensions.package_install_files(repo="user_default", filepath=zip_path, enable_on_install=True, overwrite=True)
pkg = "bl_ext.user_default.saints_reborn_map_exporter"
print("ADDON ENABLED", pkg in bpy.context.preferences.addons)
pr = bpy.context.preferences.addons[pkg].preferences
pr.converter = exe
pr.maps_folder = maps
sc = bpy.context.scene
sc.sr_map_name = "Blender Addon Test"
bpy.ops.mesh.primitive_cube_add(size=1, location=(0, 0, -0.25)); f = bpy.context.active_object; f.scale = (40, 40, 0.5); f.name = "Floor"
bpy.ops.mesh.primitive_cube_add(size=2, location=(8, 0, 1)); bpy.context.active_object.name = "BoxPlusX"
bpy.ops.mesh.primitive_cube_add(size=2, location=(0, 8, 1)); bpy.context.active_object.name = "BoxPlusY"
for i, (x, y) in enumerate(((-15, -15), (15, 15), (-15, 15), (15, -15))):
    sc.cursor.location = (x, y, 0.1)
    bpy.ops.saintsreborn.add_marker(kind="spawn")
sc.cursor.location = (-15, 0, 0.1); bpy.ops.saintsreborn.add_marker(kind="team1_spawn")
sc.cursor.location = (15, 0, 0.1); bpy.ops.saintsreborn.add_marker(kind="team2_spawn")
sc.cursor.location = (0, -5, 0.1); bpy.ops.saintsreborn.add_marker(kind="weapon", weapon="rpg_launcher")
sc.cursor.location = (3, -5, 0.1); bpy.ops.saintsreborn.add_marker(kind="weapon", weapon="desert eagle")
sc.cursor.location = (-3, -5, 0.1); bpy.ops.saintsreborn.add_marker(kind="chains_dropoff")
r = bpy.ops.saintsreborn.export(glb_only=False)
print("EXPORT RESULT", r, sc.sr_last_result)
print("REPORT >>>\n" + bpy.data.texts["Saints Reborn export"].as_string())
