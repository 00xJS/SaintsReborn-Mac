// Writes the open scene as a binary glTF (.glb) for the Saints Reborn Map Converter: every enabled
// MeshRenderer as world-space geometry (one primitive per sub-mesh), each material's base texture and
// colour, and every SRMarker as a node with extras { sr_type, sr_value }.
// Unity is left-handed, glTF right-handed: x is negated and the triangle winding flipped.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace SaintsReborn.MapExporter
{
    public class SRGlbWriter
    {
        public int Triangles, Meshes, Markers;
        public readonly List<string> Warnings = new List<string>();

        readonly MemoryStream bin = new MemoryStream();
        readonly List<string> bufferViews = new List<string>(), accessors = new List<string>(), meshes = new List<string>(),
            nodes = new List<string>(), materials = new List<string>(), images = new List<string>(), textures = new List<string>();
        readonly Dictionary<Material, int> materialIndex = new Dictionary<Material, int>();
        readonly Dictionary<Texture, int> textureIndex = new Dictionary<Texture, int>();
        static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

        static string F(float v) => float.IsFinite(v) ? v.ToString("R", Inv) : "0";
        static string Esc(string s)
        {
            var b = new StringBuilder();
            foreach (char c in s ?? "")
            {
                if (c == '"' || c == '\\') b.Append('\\').Append(c);
                else if (c < 0x20) b.AppendFormat("\\u{0:x4}", (int)c);
                else b.Append(c);
            }
            return b.ToString();
        }

        int View(byte[] data, int? target = null)
        {
            while (bin.Length % 4 != 0) bin.WriteByte(0);
            long off = bin.Length;
            bin.Write(data, 0, data.Length);
            bufferViews.Add("{\"buffer\":0,\"byteOffset\":" + off + ",\"byteLength\":" + data.Length + (target.HasValue ? ",\"target\":" + target.Value : "") + "}");
            return bufferViews.Count - 1;
        }

        int Accessor(float[] data, int comps, bool minmax)
        {
            var bytes = new byte[data.Length * 4];
            Buffer.BlockCopy(data, 0, bytes, 0, bytes.Length);
            int view = View(bytes, 34962);
            string type = comps == 2 ? "VEC2" : "VEC3";
            string mm = "";
            if (minmax)
            {
                var mn = new float[comps]; var mx = new float[comps];
                for (int c = 0; c < comps; c++) { mn[c] = float.MaxValue; mx[c] = float.MinValue; }
                for (int i = 0; i < data.Length; i++) { int c = i % comps; mn[c] = Math.Min(mn[c], data[i]); mx[c] = Math.Max(mx[c], data[i]); }
                mm = ",\"min\":[" + string.Join(",", Array.ConvertAll(mn, F)) + "],\"max\":[" + string.Join(",", Array.ConvertAll(mx, F)) + "]";
            }
            accessors.Add("{\"bufferView\":" + view + ",\"componentType\":5126,\"count\":" + data.Length / comps + ",\"type\":\"" + type + "\"" + mm + "}");
            return accessors.Count - 1;
        }

        int IndexAccessor(int[] idx)
        {
            var bytes = new byte[idx.Length * 4];
            Buffer.BlockCopy(idx, 0, bytes, 0, bytes.Length);
            int view = View(bytes, 34963);
            accessors.Add("{\"bufferView\":" + view + ",\"componentType\":5125,\"count\":" + idx.Length + ",\"type\":\"SCALAR\"}");
            return accessors.Count - 1;
        }

        static Texture BaseTexture(Material m)
        {
            foreach (var p in new[] { "_BaseMap", "_MainTex", "_BaseColorMap", "_Albedo" })
                if (m.HasProperty(p) && m.GetTexture(p) != null) return m.GetTexture(p);
            return null;
        }
        static Color BaseColor(Material m)
        {
            foreach (var p in new[] { "_BaseColor", "_Color" })
                if (m.HasProperty(p)) return m.GetColor(p);
            return Color.white;
        }
        static void UvTransform(Material m, out Vector2 scale, out Vector2 offset)
        {
            scale = Vector2.one; offset = Vector2.zero;
            foreach (var p in new[] { "_BaseMap", "_MainTex" })
                if (m.HasProperty(p)) { scale = m.GetTextureScale(p); offset = m.GetTextureOffset(p); return; }
        }

        // Reads any texture (also non-readable or compressed ones) through the GPU.
        static byte[] Png(Texture tex)
        {
            int w = Mathf.Clamp(tex.width, 4, 2048), h = Mathf.Clamp(tex.height, 4, 2048);
            var rt = RenderTexture.GetTemporary(w, h, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.sRGB);
            var prev = RenderTexture.active;
            Graphics.Blit(tex, rt);
            RenderTexture.active = rt;
            var t = new Texture2D(w, h, TextureFormat.RGBA32, false);
            t.ReadPixels(new Rect(0, 0, w, h), 0, 0);
            t.Apply();
            RenderTexture.active = prev;
            RenderTexture.ReleaseTemporary(rt);
            var png = t.EncodeToPNG();
            UnityEngine.Object.DestroyImmediate(t);
            return png;
        }

        int MaterialIndex(Material m)
        {
            if (m != null && materialIndex.TryGetValue(m, out int found)) return found;
            string tex = "";
            if (m != null)
            {
                var t = BaseTexture(m);
                if (t != null)
                {
                    if (!textureIndex.TryGetValue(t, out int ti))
                    {
                        int view = View(Png(t));
                        images.Add("{\"bufferView\":" + view + ",\"mimeType\":\"image/png\",\"name\":\"" + Esc(t.name) + "\"}");
                        textures.Add("{\"source\":" + (images.Count - 1) + "}");
                        ti = textures.Count - 1;
                        textureIndex[t] = ti;
                    }
                    tex = ",\"baseColorTexture\":{\"index\":" + ti + "}";
                }
            }
            var c = (m != null ? BaseColor(m) : Color.white).linear;
            materials.Add("{\"name\":\"" + Esc(m != null ? m.name : "default") + "\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[" +
                          F(c.r) + "," + F(c.g) + "," + F(c.b) + ",1]" + tex + ",\"metallicFactor\":0,\"roughnessFactor\":1}}");
            int idx = materials.Count - 1;
            if (m != null) materialIndex[m] = idx;
            return idx;
        }

        void AddRenderer(MeshRenderer r)
        {
            var mf = r.GetComponent<MeshFilter>();
            var mesh = mf != null ? mf.sharedMesh : null;
            if (mesh == null) return;
            var M = r.transform.localToWorldMatrix;
            var N = M.inverse.transpose;
            bool mirrored = M.determinant < 0;
            var v = mesh.vertices; var n = mesh.normals; var uv = mesh.uv;
            if (v.Length == 0) return;
            var mats = r.sharedMaterials;
            var prims = new List<string>();
            for (int s = 0; s < mesh.subMeshCount; s++)
            {
                var topo = mesh.GetTopology(s);
                if (topo != MeshTopology.Triangles) { Warnings.Add(r.name + ": sub-mesh " + s + " is not triangles, skipped"); continue; }
                var tri = mesh.GetTriangles(s);
                if (tri.Length == 0) continue;
                Material mat = s < mats.Length ? mats[s] : (mats.Length > 0 ? mats[mats.Length - 1] : null);
                Vector2 sc = Vector2.one, of = Vector2.zero;
                if (mat != null) UvTransform(mat, out sc, out of);
                // compact the vertices this sub-mesh uses
                var remap = new Dictionary<int, int>();
                var pos = new List<float>(); var nrm = new List<float>(); var tc = new List<float>();
                var idx = new int[tri.Length];
                for (int i = 0; i < tri.Length; i++)
                {
                    int k = tri[i];
                    if (!remap.TryGetValue(k, out int o))
                    {
                        o = remap.Count; remap[k] = o;
                        var p = M.MultiplyPoint3x4(v[k]);
                        pos.Add(-p.x); pos.Add(p.y); pos.Add(p.z);
                        var nn = n.Length > k ? N.MultiplyVector(n[k]).normalized : Vector3.up;
                        nrm.Add(-nn.x); nrm.Add(nn.y); nrm.Add(nn.z);
                        var t = uv.Length > k ? Vector2.Scale(uv[k], sc) + of : Vector2.zero;
                        tc.Add(t.x); tc.Add(1f - t.y);   // glTF v runs down
                    }
                    idx[i] = o;
                }
                // mirroring x reverses the winding (a mirrored object reverses it once more)
                if (!mirrored)
                    for (int i = 0; i + 2 < idx.Length; i += 3) { int t = idx[i + 1]; idx[i + 1] = idx[i + 2]; idx[i + 2] = t; }
                int ap = Accessor(pos.ToArray(), 3, true), an = Accessor(nrm.ToArray(), 3, false), at = Accessor(tc.ToArray(), 2, false);
                int ai = IndexAccessor(idx);
                prims.Add("{\"attributes\":{\"POSITION\":" + ap + ",\"NORMAL\":" + an + ",\"TEXCOORD_0\":" + at + "},\"indices\":" + ai +
                          ",\"material\":" + MaterialIndex(mat) + ",\"mode\":4}");
                Triangles += tri.Length / 3;
            }
            if (prims.Count == 0) return;
            meshes.Add("{\"name\":\"" + Esc(r.name) + "\",\"primitives\":[" + string.Join(",", prims) + "]}");
            nodes.Add("{\"name\":\"" + Esc(r.name) + "\",\"mesh\":" + (meshes.Count - 1) + "}");
            Meshes++;
        }

        void AddMarker(SRMarker m)
        {
            var p = m.transform.position; var q = m.transform.rotation;
            nodes.Add("{\"name\":\"" + Esc(m.SrType + "_" + (Markers + 1)) + "\",\"translation\":[" + F(-p.x) + "," + F(p.y) + "," + F(p.z) +
                      "],\"rotation\":[" + F(q.x) + "," + F(-q.y) + "," + F(-q.z) + "," + F(q.w) + "],\"extras\":{\"sr_type\":\"" + Esc(m.SrType) +
                      "\",\"sr_value\":\"" + Esc(m.SrValue) + "\",\"unity_name\":\"" + Esc(m.name) + "\"}}");
            Markers++;
        }

        static bool UnderMarker(Transform t)
        {
            for (; t != null; t = t.parent) if (t.GetComponent<SRMarker>() != null) return true;
            return false;
        }

        public void AddOpenScenes()
        {
            for (int si = 0; si < SceneManager.sceneCount; si++)
            {
                var scene = SceneManager.GetSceneAt(si);
                if (!scene.isLoaded) continue;
                foreach (var root in scene.GetRootGameObjects())
                {
                    if (!root.activeInHierarchy) continue;
                    foreach (var r in root.GetComponentsInChildren<MeshRenderer>(false))
                        if (r.enabled && !r.CompareTag("EditorOnly") && !UnderMarker(r.transform)) AddRenderer(r);
                    foreach (var m in root.GetComponentsInChildren<SRMarker>(false))
                        if (m.enabled) AddMarker(m);
                    if (root.GetComponentsInChildren<Terrain>(false).Length > 0)
                        Warnings.Add("Unity terrain is not exported yet: convert it to a mesh (or model the ground)");
                    if (root.GetComponentsInChildren<SkinnedMeshRenderer>(false).Length > 0)
                        Warnings.Add("skinned meshes are not exported (map geometry must be static meshes)");
                }
            }
        }

        public void Save(string path)
        {
            var nodeIdx = new List<string>();
            for (int i = 0; i < nodes.Count; i++) nodeIdx.Add(i.ToString(Inv));
            var j = new StringBuilder();
            j.Append("{\"asset\":{\"version\":\"2.0\",\"generator\":\"Saints Reborn Map Exporter for Unity\"},");
            j.Append("\"scene\":0,\"scenes\":[{\"nodes\":[" + string.Join(",", nodeIdx) + "]}],");
            j.Append("\"nodes\":[" + string.Join(",", nodes) + "]");
            if (meshes.Count > 0) j.Append(",\"meshes\":[" + string.Join(",", meshes) + "]");
            if (materials.Count > 0) j.Append(",\"materials\":[" + string.Join(",", materials) + "]");
            if (textures.Count > 0) j.Append(",\"textures\":[" + string.Join(",", textures) + "],\"images\":[" + string.Join(",", images) + "]");
            if (accessors.Count > 0) j.Append(",\"accessors\":[" + string.Join(",", accessors) + "]");
            if (bufferViews.Count > 0) j.Append(",\"bufferViews\":[" + string.Join(",", bufferViews) + "]");
            while (bin.Length % 4 != 0) bin.WriteByte(0);
            j.Append(",\"buffers\":[{\"byteLength\":" + bin.Length + "}]}");
            var json = Encoding.UTF8.GetBytes(j.ToString());
            int jsonPad = (4 - json.Length % 4) % 4;
            using (var f = new BinaryWriter(File.Create(path)))
            {
                int total = 12 + 8 + json.Length + jsonPad + 8 + (int)bin.Length;
                f.Write(0x46546C67u); f.Write(2u); f.Write((uint)total);
                f.Write((uint)(json.Length + jsonPad)); f.Write(0x4E4F534Au); f.Write(json);
                for (int i = 0; i < jsonPad; i++) f.Write((byte)0x20);
                f.Write((uint)bin.Length); f.Write(0x004E4942u); f.Write(bin.ToArray());
            }
        }
    }
}
