// Saints Reborn map markers. Put these on (empty) GameObjects in your map scene; the exporter turns
// them into spawn points, weapon pickups, vehicles and Big Ass Chains drop-offs. A marker faces its
// blue Z arrow. Objects with a marker are not exported as geometry.
using UnityEngine;

namespace SaintsReborn.MapExporter
{
    public abstract class SRMarker : MonoBehaviour
    {
        public abstract string SrType { get; }
        public virtual string SrValue => "";
        protected abstract Color GizmoColor { get; }
        protected virtual float GizmoHeight => 1.8f;

        void OnDrawGizmos() { Draw(0.35f); }
        void OnDrawGizmosSelected() { Draw(1f); }

        void Draw(float alpha)
        {
            var c = GizmoColor;
            var p = transform.position;
            Gizmos.color = new Color(c.r, c.g, c.b, alpha);
            // body: a box the size of a standing character
            Gizmos.DrawWireCube(p + Vector3.up * GizmoHeight * 0.5f, new Vector3(0.6f, GizmoHeight, 0.6f));
            Gizmos.color = new Color(c.r, c.g, c.b, alpha * 0.35f);
            Gizmos.DrawCube(p + Vector3.up * GizmoHeight * 0.5f, new Vector3(0.6f, GizmoHeight, 0.6f));
            // facing arrow
            Gizmos.color = new Color(c.r, c.g, c.b, Mathf.Max(alpha, 0.7f));
            var f = transform.forward; f.y = 0;
            if (f.sqrMagnitude > 1e-6f)
            {
                f.Normalize();
                var a = p + Vector3.up * 0.1f; var b = a + f * 1.2f;
                var side = Vector3.Cross(Vector3.up, f) * 0.25f;
                Gizmos.DrawLine(a, b); Gizmos.DrawLine(b, b - f * 0.35f + side); Gizmos.DrawLine(b, b - f * 0.35f - side);
            }
        }
    }

    public enum SRTeam { Any, Team1Red, Team2Blue }

    [AddComponentMenu("Saints Reborn/Spawn Point")]
    public class SRSpawnPoint : SRMarker
    {
        [Tooltip("Any = used in every mode. Team spawns are used in the team modes.")]
        public SRTeam team = SRTeam.Any;
        public override string SrType => team == SRTeam.Team1Red ? "team1_spawn" : team == SRTeam.Team2Blue ? "team2_spawn" : "spawn";
        protected override Color GizmoColor => team == SRTeam.Team1Red ? new Color(1f, 0.25f, 0.2f) : team == SRTeam.Team2Blue ? new Color(0.25f, 0.45f, 1f) : new Color(0.2f, 1f, 0.35f);
    }

    [AddComponentMenu("Saints Reborn/Player Start")]
    public class SRPlayerStart : SRMarker
    {
        public override string SrType => "player_start";
        protected override Color GizmoColor => Color.white;
    }

    public enum SRWeapon
    {
        AK47, DesertEagle, M16, Mac10, Molotov, PipeBomb, PumpActionShotgun, RpgLauncher, SniperRifle, Spas12, Tec9
    }

    [AddComponentMenu("Saints Reborn/Weapon Pickup")]
    public class SRWeaponPickup : SRMarker
    {
        public SRWeapon weapon = SRWeapon.AK47;
        public override string SrType => "weapon";
        public override string SrValue => GameName(weapon);
        protected override Color GizmoColor => new Color(1f, 0.8f, 0.1f);
        protected override float GizmoHeight => 0.5f;
        public static string GameName(SRWeapon w)
        {
            switch (w)
            {
                case SRWeapon.AK47: return "ak47";
                case SRWeapon.DesertEagle: return "desert eagle";
                case SRWeapon.M16: return "m16";
                case SRWeapon.Mac10: return "mac10";
                case SRWeapon.Molotov: return "molotov";
                case SRWeapon.PipeBomb: return "pipe_bomb";
                case SRWeapon.PumpActionShotgun: return "pump_action_shotgun";
                case SRWeapon.RpgLauncher: return "rpg_launcher";
                case SRWeapon.SniperRifle: return "sniper_rifle";
                case SRWeapon.Spas12: return "spas12";
                default: return "tec9";
            }
        }
    }

    [AddComponentMenu("Saints Reborn/Vehicle Spawn")]
    public class SRVehicleSpawn : SRMarker
    {
        [Tooltip("Vehicle name from the game, e.g. car_2dr_sports03, car_2dr_muscle01, car_4dr_standard08, sp_metermaid01")]
        public string vehicleType = "car_2dr_sports03";
        public override string SrType => "vehicle";
        public override string SrValue => vehicleType;
        protected override Color GizmoColor => new Color(0.8f, 0.4f, 1f);
        protected override float GizmoHeight => 1.4f;
    }

    [AddComponentMenu("Saints Reborn/Chains Drop-off")]
    public class SRChainsDropOff : SRMarker
    {
        public override string SrType => "chains_dropoff";
        protected override Color GizmoColor => new Color(1f, 0.55f, 0.1f);
        protected override float GizmoHeight => 0.7f;
    }
}
