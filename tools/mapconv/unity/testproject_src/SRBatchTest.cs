// Batch-mode test for the Saints Reborn Map Exporter: builds a small scene and runs Export Map Pack.
// Unity.exe -batchmode -quit -projectPath <p> -executeMethod SRBatchTest.Run -srConverter <exe> -srOut <maps>
using System;
using System.IO;
using System.Linq;
using SaintsReborn.MapExporter;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

public static class SRBatchTest
{
    static string Arg(string name)
    {
        var a = Environment.GetCommandLineArgs();
        int i = Array.IndexOf(a, name);
        return i >= 0 && i + 1 < a.Length ? a[i + 1] : "";
    }

    static Material Mat(string name, Func<int, int, Color> px)
    {
        var t = new Texture2D(64, 64, TextureFormat.RGBA32, false) { name = name + "_tex" };
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) t.SetPixel(x, y, px(x, y));
        t.Apply();
        var m = new Material(Shader.Find("Standard")) { name = name, mainTexture = t };
        return m;
    }

    static GameObject Box(string name, Vector3 pos, Vector3 scale, Material m, Vector3 euler = default)
    {
        var g = GameObject.CreatePrimitive(PrimitiveType.Cube);
        g.name = name; g.transform.position = pos; g.transform.localScale = scale; g.transform.eulerAngles = euler;
        g.GetComponent<Renderer>().sharedMaterial = m;
        return g;
    }

    public static void Run()
    {
        EditorSceneManager.NewScene(NewSceneSetup.EmptyScene);
        // "F" pattern: shows whether textures come out mirrored or upside down
        var fmat = Mat("letterF", (x, y) => (x > 16 && x < 26 && y > 8 && y < 56) || (y > 46 && y < 56 && x > 16 && x < 50) || (y > 28 && y < 36 && x > 16 && x < 42) ? Color.black : new Color(1f, 0.85f, 0.2f));
        var grey = Mat("grid", (x, y) => (x % 32 < 2 || y % 32 < 2) ? new Color(0.9f, 0.8f, 0.2f) : new Color(0.35f, 0.37f, 0.4f));
        Box("Floor", new Vector3(0, -0.5f, 0), new Vector3(40, 1, 40), grey);
        Box("WallNorth", new Vector3(0, 2, 20), new Vector3(40, 4, 1), fmat);
        Box("MarkerBoxEastOfCentre", new Vector3(10, 1, 0), new Vector3(2, 2, 2), fmat);        // +X side in Unity
        Box("RotatedBox", new Vector3(-10, 1, 5), new Vector3(4, 2, 1), fmat, new Vector3(0, 30, 0));
        Box("MirroredBox", new Vector3(0, 1, -10), new Vector3(-3, 2, 2), fmat);                  // negative scale
        var red = Color.red;
        void Marker<T>(string n, Vector3 p, float yaw, Action<T> set = null) where T : SRMarker
        {
            var g = new GameObject(n); g.transform.position = p; g.transform.eulerAngles = new Vector3(0, yaw, 0);
            var c = g.AddComponent<T>(); set?.Invoke(c);
        }
        Marker<SRSpawnPoint>("SpawnFacingPlusX", new Vector3(-15, 0.1f, 0), 90);
        Marker<SRSpawnPoint>("SpawnFacingMinusZ", new Vector3(0, 0.1f, 15), 180);
        Marker<SRSpawnPoint>("Red1", new Vector3(-15, 0.1f, -15), 45, s => s.team = SRTeam.Team1Red);
        Marker<SRSpawnPoint>("Blue1", new Vector3(15, 0.1f, 15), 225, s => s.team = SRTeam.Team2Blue);
        Marker<SRWeaponPickup>("Rpg", new Vector3(10, 2.1f, 0), 0, s => s.weapon = SRWeapon.RpgLauncher);
        Marker<SRWeaponPickup>("Eagle", new Vector3(5, 0.1f, 5), 0, s => s.weapon = SRWeapon.DesertEagle);
        Marker<SRPlayerStart>("Start", new Vector3(-15, 0.1f, 2), 90);

        var w = new SRGlbWriter();
        w.AddOpenScenes();
        var glb = Path.GetFullPath("Temp/sr_batch_test.glb");
        w.Save(glb);
        Debug.Log($"SRTEST glb {glb} meshes {w.Meshes} tris {w.Triangles} markers {w.Markers} warnings {string.Join("; ", w.Warnings)}");
        var conv = Arg("-srConverter");
        if (File.Exists(conv))
        {
            var psi = new System.Diagnostics.ProcessStartInfo(conv, $"\"{glb}\" --name \"Unity Test\" --no-pause --out \"{Arg("-srOut")}\"")
            { UseShellExecute = false, RedirectStandardOutput = true, CreateNoWindow = true };
            using (var p = System.Diagnostics.Process.Start(psi)) { Debug.Log("SRTEST converter:\n" + p.StandardOutput.ReadToEnd()); p.WaitForExit(); Debug.Log("SRTEST exit " + p.ExitCode); }
        }
        File.Copy(glb, Path.Combine(Arg("-srOut"), "..", "tools", "MapConverter", "samples", "unity_test.glb"), true);
    }
}
