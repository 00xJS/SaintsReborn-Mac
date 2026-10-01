// Saints Reborn > Map Exporter: exports the open scene as a map pack for the game's maps folder.
// It writes a .glb (SRGlbWriter) and runs SaintsRebornMapConverter.exe on it.
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace SaintsReborn.MapExporter
{
    public class SRMapExporterWindow : EditorWindow
    {
        const string PrefConverter = "SaintsReborn.MapExporter.Converter";
        const string PrefOut = "SaintsReborn.MapExporter.OutFolder";
        const string PrefTex = "SaintsReborn.MapExporter.TextureSize";
        string mapName = "";
        string log = "";
        Vector2 scroll;
        string lastPack = "";

        [MenuItem("Saints Reborn/Map Exporter", false, 0)]
        public static void Open() { GetWindow<SRMapExporterWindow>("Saints Reborn Map"); }

        static string Converter
        {
            get => EditorPrefs.GetString(PrefConverter, "");
            set => EditorPrefs.SetString(PrefConverter, value);
        }

        void OnEnable()
        {
            if (string.IsNullOrEmpty(mapName)) mapName = EditorSceneManager.GetActiveScene().name;
        }

        void OnGUI()
        {
            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Saints Reborn multiplayer map", EditorStyles.boldLabel);
            mapName = EditorGUILayout.TextField(new GUIContent("Map name", "Shown in the game's System Link level list"), mapName);

            EditorGUILayout.BeginHorizontal();
            Converter = EditorGUILayout.TextField(new GUIContent("Converter", "SaintsRebornMapConverter.exe (in the game's tools\\MapConverter folder)"), Converter);
            if (GUILayout.Button("Browse", GUILayout.Width(64)))
            {
                var p = EditorUtility.OpenFilePanel("SaintsRebornMapConverter.exe", Path.GetDirectoryName(Converter) ?? "", "exe");
                if (!string.IsNullOrEmpty(p)) Converter = p;
            }
            EditorGUILayout.EndHorizontal();

            EditorGUILayout.BeginHorizontal();
            var outDir = EditorGUILayout.TextField(new GUIContent("Maps folder", "Empty = the game's maps folder next to the converter's game"), EditorPrefs.GetString(PrefOut, ""));
            EditorPrefs.SetString(PrefOut, outDir);
            if (GUILayout.Button("Browse", GUILayout.Width(64)))
            {
                var p = EditorUtility.OpenFolderPanel("Game maps folder", outDir, "");
                if (!string.IsNullOrEmpty(p)) EditorPrefs.SetString(PrefOut, p);
            }
            EditorGUILayout.EndHorizontal();
            int[] sizes = { 256, 512, 1024 };
            int tex = EditorPrefs.GetInt(PrefTex, 256);
            tex = EditorGUILayout.IntPopup(new GUIContent("Texture size"), tex, sizes.Select(s => new GUIContent(s + " px")).ToArray(), sizes);
            EditorPrefs.SetInt(PrefTex, tex);

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Markers", EditorStyles.boldLabel);
#if UNITY_6000_3_OR_NEWER
            var markers = FindObjectsByType<SRMarker>(FindObjectsInactive.Exclude);
#else
            var markers = FindObjectsByType<SRMarker>(FindObjectsInactive.Exclude, FindObjectsSortMode.None);
#endif
            int spawns = markers.Count(m => m is SRSpawnPoint), weapons = markers.Count(m => m is SRWeaponPickup);
            int vehicles = markers.Count(m => m is SRVehicleSpawn), drops = markers.Count(m => m is SRChainsDropOff);
            EditorGUILayout.LabelField($"{spawns} spawn points, {weapons} weapons, {vehicles} vehicles, {drops} chain drop-offs");
            if (spawns < 2) EditorGUILayout.HelpBox("Add spawn points (8 or more play best): Saints Reborn > Add > Spawn Point", MessageType.Warning);
            EditorGUILayout.BeginHorizontal();
            if (GUILayout.Button("+ Spawn")) SRMarkerMenu.Create<SRSpawnPoint>("Spawn Point");
            if (GUILayout.Button("+ Red spawn")) SRMarkerMenu.Create<SRSpawnPoint>("Red Spawn", s => s.team = SRTeam.Team1Red);
            if (GUILayout.Button("+ Blue spawn")) SRMarkerMenu.Create<SRSpawnPoint>("Blue Spawn", s => s.team = SRTeam.Team2Blue);
            EditorGUILayout.EndHorizontal();
            EditorGUILayout.BeginHorizontal();
            if (GUILayout.Button("+ Weapon")) SRMarkerMenu.Create<SRWeaponPickup>("Weapon");
            if (GUILayout.Button("+ Vehicle")) SRMarkerMenu.Create<SRVehicleSpawn>("Vehicle");
            if (GUILayout.Button("+ Drop-off")) SRMarkerMenu.Create<SRChainsDropOff>("Chains Drop-off");
            if (GUILayout.Button("+ Player start")) SRMarkerMenu.Create<SRPlayerStart>("Player Start");
            EditorGUILayout.EndHorizontal();

            EditorGUILayout.Space();
            using (new EditorGUI.DisabledScope(string.IsNullOrWhiteSpace(mapName)))
            {
                if (GUILayout.Button("Export Map Pack", GUILayout.Height(32))) Export(true);
                if (GUILayout.Button("Save .glb only")) Export(false);
            }
            if (!string.IsNullOrEmpty(lastPack) && Directory.Exists(lastPack) && GUILayout.Button("Show map pack folder"))
                EditorUtility.RevealInFinder(lastPack);

            EditorGUILayout.Space();
            scroll = EditorGUILayout.BeginScrollView(scroll);
            EditorGUILayout.TextArea(log, GUILayout.ExpandHeight(true));
            EditorGUILayout.EndScrollView();
        }

        void Export(bool convert)
        {
            var w = new SRGlbWriter();
            try
            {
                EditorUtility.DisplayProgressBar("Saints Reborn", "Collecting the scene...", 0.2f);
                w.AddOpenScenes();
                string glb;
                if (convert)
                {
                    var dir = Path.Combine(Path.GetFullPath("Temp"), "SaintsReborn");
                    Directory.CreateDirectory(dir);
                    glb = Path.Combine(dir, Safe(mapName) + ".glb");
                }
                else
                {
                    glb = EditorUtility.SaveFilePanel("Save map as .glb", "", Safe(mapName) + ".glb", "glb");
                    if (string.IsNullOrEmpty(glb)) return;
                }
                EditorUtility.DisplayProgressBar("Saints Reborn", "Writing " + Path.GetFileName(glb) + "...", 0.4f);
                w.Save(glb);
                var sb = new StringBuilder();
                sb.AppendLine($"Exported {w.Meshes} meshes, {w.Triangles} triangles, {w.Markers} markers to {glb}");
                foreach (var x in w.Warnings.Distinct()) sb.AppendLine("  - " + x);
                if (convert)
                {
                    if (!File.Exists(Converter))
                    {
                        sb.AppendLine("\nSet the converter first: SaintsRebornMapConverter.exe in the game's tools\\MapConverter folder.");
                        log = sb.ToString();
                        return;
                    }
                    EditorUtility.DisplayProgressBar("Saints Reborn", "Converting...", 0.6f);
                    var args = $"\"{glb}\" --name \"{mapName.Replace("\"", "'")}\" --no-pause --texture-size {EditorPrefs.GetInt(PrefTex, 256)}";
                    var outDir = EditorPrefs.GetString(PrefOut, "");
                    if (!string.IsNullOrEmpty(outDir)) args += $" --out \"{outDir}\"";
                    var psi = new ProcessStartInfo(Converter, args)
                    {
                        UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true,
                        StandardOutputEncoding = Encoding.UTF8, WorkingDirectory = Path.GetDirectoryName(Converter)
                    };
                    using (var p = Process.Start(psi))
                    {
                        string o = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
                        p.WaitForExit();
                        sb.AppendLine().Append(o);
                        var line = o.Split('\n').FirstOrDefault(l => l.StartsWith("Map pack written to "));
                        if (line != null) lastPack = line.Substring("Map pack written to ".Length).Trim();
                        if (p.ExitCode == 0) ShowNotification(new GUIContent("Map pack ready"));
                    }
                }
                log = sb.ToString();
            }
            catch (Exception e)
            {
                log = "Export failed: " + e.Message + "\n" + e.StackTrace;
            }
            finally
            {
                EditorUtility.ClearProgressBar();
            }
        }

        static string Safe(string s)
        {
            foreach (var c in Path.GetInvalidFileNameChars()) s = s.Replace(c, '_');
            return string.IsNullOrWhiteSpace(s) ? "map" : s;
        }
    }

    public static class SRMarkerMenu
    {
        public static T Create<T>(string name, Action<T> setup = null) where T : SRMarker
        {
            var go = new GameObject(name);
            var sv = SceneView.lastActiveSceneView;
            if (sv != null)
            {
                // on the ground under the scene view's centre when there is ground, else at the pivot
                var ray = new Ray(sv.camera.transform.position, sv.camera.transform.forward);
                go.transform.position = Physics.Raycast(ray, out var hit, 1000f) ? hit.point : sv.pivot;
            }
            var c = go.AddComponent<T>();
            setup?.Invoke(c);
            Undo.RegisterCreatedObjectUndo(go, "Add " + name);
            Selection.activeGameObject = go;
            return c;
        }

        [MenuItem("Saints Reborn/Add/Spawn Point", false, 20)] static void Spawn() => Create<SRSpawnPoint>("Spawn Point");
        [MenuItem("Saints Reborn/Add/Red Team Spawn", false, 21)] static void Red() => Create<SRSpawnPoint>("Red Spawn", s => s.team = SRTeam.Team1Red);
        [MenuItem("Saints Reborn/Add/Blue Team Spawn", false, 22)] static void Blue() => Create<SRSpawnPoint>("Blue Spawn", s => s.team = SRTeam.Team2Blue);
        [MenuItem("Saints Reborn/Add/Weapon Pickup", false, 23)] static void Weapon() => Create<SRWeaponPickup>("Weapon");
        [MenuItem("Saints Reborn/Add/Vehicle Spawn", false, 24)] static void Vehicle() => Create<SRVehicleSpawn>("Vehicle");
        [MenuItem("Saints Reborn/Add/Chains Drop-off", false, 25)] static void Drop() => Create<SRChainsDropOff>("Chains Drop-off");
        [MenuItem("Saints Reborn/Add/Player Start", false, 26)] static void Start() => Create<SRPlayerStart>("Player Start");
    }
}
