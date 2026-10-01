# Saints Reborn Map Exporter for Blender: place spawn points, weapons, vehicles and Big Ass Chains
# drop-offs, then export the scene as a Saints Reborn multiplayer map pack (glTF + the converter).
# View3D > Sidebar (N) > Saints Reborn.
import os
import subprocess
import tempfile

import bpy
from bpy.props import EnumProperty, StringProperty

WEAPONS = [
    ("ak47", "AK47", ""), ("desert eagle", "Desert Eagle", ""), ("m16", "M16", ""), ("mac10", "Mac-10", ""),
    ("molotov", "Molotov", ""), ("pipe_bomb", "Pipe bomb", ""), ("pump_action_shotgun", "Pump shotgun", ""),
    ("rpg_launcher", "RPG", ""), ("sniper_rifle", "Sniper rifle", ""), ("spas12", "SPAS-12", ""), ("tec9", "Tec-9", ""),
]
KINDS = {
    "spawn": ("Spawn", (0.2, 1.0, 0.35, 1.0)),
    "team1_spawn": ("Red Spawn", (1.0, 0.25, 0.2, 1.0)),
    "team2_spawn": ("Blue Spawn", (0.25, 0.45, 1.0, 1.0)),
    "player_start": ("Player Start", (1.0, 1.0, 1.0, 1.0)),
    "weapon": ("Weapon", (1.0, 0.8, 0.1, 1.0)),
    "vehicle": ("Vehicle", (0.8, 0.4, 1.0, 1.0)),
    "chains_dropoff": ("Chains Drop-off", (1.0, 0.55, 0.1, 1.0)),
}


def prefs(context):
    return context.preferences.addons[__package__].preferences


class SRPreferences(bpy.types.AddonPreferences):
    bl_idname = __package__
    converter: StringProperty(name="Converter", subtype="FILE_PATH",
                              description="SaintsRebornMapConverter.exe in the game's tools\\MapConverter folder")
    maps_folder: StringProperty(name="Maps folder", subtype="DIR_PATH",
                                description="Empty = the game's maps folder next to the converter's game")

    def draw(self, context):
        self.layout.prop(self, "converter")
        self.layout.prop(self, "maps_folder")


def marker_type(ob):
    return ob.get("sr_type", "") if ob else ""


class SR_OT_add_marker(bpy.types.Operator):
    """Add a Saints Reborn marker at the 3D cursor (it faces its -Y axis, like a character)"""
    bl_idname = "saintsreborn.add_marker"
    bl_label = "Add Saints Reborn Marker"
    bl_options = {"REGISTER", "UNDO"}
    kind: EnumProperty(items=[(k, v[0], "") for k, v in KINDS.items()])
    weapon: EnumProperty(name="Weapon", items=WEAPONS)
    vehicle: StringProperty(name="Vehicle", default="car_2dr_sports03")

    def execute(self, context):
        label, colour = KINDS[self.kind]
        value = self.weapon if self.kind == "weapon" else self.vehicle if self.kind == "vehicle" else ""
        ob = bpy.data.objects.new(label + (" " + value if value else ""), None)
        ob.empty_display_type = "ARROWS" if self.kind not in ("weapon", "chains_dropoff") else "SPHERE"
        ob.empty_display_size = 0.5 if self.kind in ("weapon", "chains_dropoff") else 1.0
        ob.location = context.scene.cursor.location
        ob.color = colour
        ob["sr_type"] = self.kind
        ob["sr_value"] = value
        context.collection.objects.link(ob)
        for o in context.selected_objects:
            o.select_set(False)
        ob.select_set(True)
        context.view_layer.objects.active = ob
        return {"FINISHED"}


class SR_OT_mark_selected(bpy.types.Operator):
    """Turn the selected objects into Saints Reborn markers (they are then not exported as geometry)"""
    bl_idname = "saintsreborn.mark_selected"
    bl_label = "Mark Selected as"
    bl_options = {"REGISTER", "UNDO"}
    kind: EnumProperty(items=[(k, v[0], "") for k, v in KINDS.items()] + [("none", "Map geometry (no marker)", "")])
    weapon: EnumProperty(name="Weapon", items=WEAPONS)
    vehicle: StringProperty(name="Vehicle", default="car_2dr_sports03")

    def execute(self, context):
        for ob in context.selected_objects:
            if self.kind == "none":
                for k in ("sr_type", "sr_value"):
                    if k in ob:
                        del ob[k]
                continue
            ob["sr_type"] = self.kind
            ob["sr_value"] = self.weapon if self.kind == "weapon" else self.vehicle if self.kind == "vehicle" else ""
        return {"FINISHED"}


def export_glb(context, path):
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", export_extras=True, export_apply=True,
                              use_visible=True, export_cameras=False, export_lights=False)


class SR_OT_export(bpy.types.Operator):
    """Export the scene as a Saints Reborn map pack (the game's maps folder)"""
    bl_idname = "saintsreborn.export"
    bl_label = "Export Map Pack"
    glb_only: bpy.props.BoolProperty(default=False)
    filepath: StringProperty(subtype="FILE_PATH")

    def invoke(self, context, event):
        if self.glb_only:
            self.filepath = (context.scene.sr_map_name or "map") + ".glb"
            context.window_manager.fileselect_add(self)
            return {"RUNNING_MODAL"}
        return self.execute(context)

    def execute(self, context):
        sc = context.scene
        name = sc.sr_map_name.strip() or os.path.splitext(os.path.basename(bpy.data.filepath))[0] or "My Map"
        if self.glb_only:
            export_glb(context, bpy.path.abspath(self.filepath))
            self.report({"INFO"}, "Saved " + self.filepath)
            return {"FINISHED"}
        p = prefs(context)
        exe = bpy.path.abspath(p.converter)
        if not os.path.isfile(exe):
            self.report({"ERROR"}, "Set the converter (SaintsRebornMapConverter.exe) in the add-on preferences")
            return {"CANCELLED"}
        glb = os.path.join(tempfile.gettempdir(), "saints_reborn_" + "".join(c if c.isalnum() else "_" for c in name) + ".glb")
        export_glb(context, glb)
        args = [exe, glb, "--name", name, "--no-pause", "--texture-size", sc.sr_texture_size]
        if p.maps_folder:
            args += ["--out", bpy.path.abspath(p.maps_folder)]
        r = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace",
                           cwd=os.path.dirname(exe), creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        text = bpy.data.texts.get("Saints Reborn export") or bpy.data.texts.new("Saints Reborn export")
        text.clear()
        text.write(r.stdout + r.stderr)
        sc.sr_last_result = next((l for l in r.stdout.splitlines() if l.startswith(("Map pack written", "Error"))), "")
        if r.returncode != 0:
            self.report({"ERROR"}, sc.sr_last_result or "conversion failed (see the 'Saints Reborn export' text)")
            return {"CANCELLED"}
        self.report({"INFO"}, "Map pack ready: start the game and pick \"%s\" in the System Link lobby" % name)
        return {"FINISHED"}


class SR_PT_panel(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Saints Reborn"
    bl_label = "Saints Reborn Map"

    def draw(self, context):
        sc, lay = context.scene, self.layout
        lay.prop(sc, "sr_map_name")
        p = prefs(context)
        if not p.converter:
            lay.prop(p, "converter")
        col = lay.column(align=True)
        col.label(text="Add marker at the 3D cursor:")
        row = col.row(align=True)
        for k in ("spawn", "team1_spawn", "team2_spawn"):
            row.operator("saintsreborn.add_marker", text=KINDS[k][0]).kind = k
        row = col.row(align=True)
        op = row.operator("saintsreborn.add_marker", text="Weapon")
        op.kind = "weapon"
        op.weapon = sc.sr_weapon
        row.prop(sc, "sr_weapon", text="")
        row = col.row(align=True)
        op = row.operator("saintsreborn.add_marker", text="Vehicle")
        op.kind = "vehicle"
        op.vehicle = sc.sr_vehicle
        row.prop(sc, "sr_vehicle", text="")
        row = col.row(align=True)
        row.operator("saintsreborn.add_marker", text="Drop-off").kind = "chains_dropoff"
        row.operator("saintsreborn.add_marker", text="Player Start").kind = "player_start"
        lay.operator_menu_enum("saintsreborn.mark_selected", "kind", text="Mark Selected as...")
        ob = context.active_object
        if ob and marker_type(ob):
            box = lay.box()
            box.label(text="Marker: %s %s" % (marker_type(ob), ob.get("sr_value", "")))
        counts = {}
        for o in sc.objects:
            t = marker_type(o)
            if t:
                counts[t] = counts.get(t, 0) + 1
        spawns = counts.get("spawn", 0) + counts.get("team1_spawn", 0) + counts.get("team2_spawn", 0)
        lay.label(text="%d spawns, %d weapons, %d vehicles, %d drop-offs" % (spawns, counts.get("weapon", 0),
                                                                             counts.get("vehicle", 0), counts.get("chains_dropoff", 0)))
        if spawns < 2:
            lay.label(text="Add spawn points (8+ play best)", icon="ERROR")
        lay.prop(sc, "sr_texture_size")
        lay.operator("saintsreborn.export", icon="EXPORT").glb_only = False
        lay.operator("saintsreborn.export", text="Save .glb only").glb_only = True
        if sc.sr_last_result:
            lay.label(text=sc.sr_last_result)


classes = (SRPreferences, SR_OT_add_marker, SR_OT_mark_selected, SR_OT_export, SR_PT_panel)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.sr_map_name = StringProperty(name="Map name", description="Shown in the game's System Link level list")
    bpy.types.Scene.sr_weapon = EnumProperty(name="Weapon", items=WEAPONS)
    bpy.types.Scene.sr_vehicle = StringProperty(name="Vehicle", default="car_2dr_sports03")
    bpy.types.Scene.sr_texture_size = EnumProperty(name="Texture size", default="256",
                                                   items=[("256", "256 px", ""), ("512", "512 px", ""), ("1024", "1024 px", "")])
    bpy.types.Scene.sr_last_result = StringProperty()


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
    for a in ("sr_map_name", "sr_weapon", "sr_vehicle", "sr_texture_size", "sr_last_result"):
        delattr(bpy.types.Scene, a)
