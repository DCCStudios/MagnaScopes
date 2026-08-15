namespace StsConverter;

/// <summary>
/// The reticle sets See Through Scopes ships in
/// <c>Materials\Scope\Defaults\</c>, as matched Cross/Dot pairs.
///
/// This is the STS documentation's "easiest way" of setting up a reticle, and
/// it is the route to prefer. The materials genuinely exist, so nothing depends
/// on the missing-material fallback, no TextureLoader is needed, and in-game
/// reticle customisation still works because a real material is still there for
/// the engine's material swap to replace.
///
/// The documentation calls the folder "Default"; it is actually "Defaults".
/// The list below was read out of the shipped 3dscopes - Main.BA2 name table:
/// 62 materials, 31 complete Cross/Dot pairs, none unpaired.
///
/// The docs also warn against pointing a reticle at a non-Defaults material
/// such as Materials\Scope\ReticleCrossMildot.BGSM.BGEM -- it appears to work
/// and then breaks reticle customisation in game.
/// </summary>
public static class StsReticlePresets
{
    public const string Folder = @"Materials\Scope\Defaults\";

    /// <summary>Preset stems, without the Cross/Dot suffix.</summary>
    public static readonly IReadOnlyList<string> Names = new[]
    {
        "10mm2x", "10mm4x",
        "442x", "444x",
        "AlienBlaster",
        "CombatRifle2x", "CombatRifle4x", "CombatRifle8x",
        "CombatShotgun2x", "CombatShotgun4x",
        "Deliverer2x", "Deliverer4x",
        "HMAR2x", "HMAR4x",
        "HuntingRifle2x", "HuntingRifle4x", "HuntingRifle8x",
        "Institute2x",
        "Laser4x",
        "LeverAction4x", "LeverAction8x",
        "MachineGun2x", "MachineGun4x",
        "Pipe4x", "Pipe8x",
        "Plasma2x", "Plasma4x",
        "RadiumRifle2x", "RadiumRifle4x",
        "Railway",
        "Recon",
    };

    public static string CrossMaterial(string preset) =>
        $"{Folder}{preset}Cross.BGSM.BGEM";

    public static string DotMaterial(string preset) =>
        $"{Folder}{preset}Dot.BGSM.BGEM";

    /// <summary>
    /// Resolves a user-typed preset case-insensitively. Unknown names are
    /// returned as given rather than rejected: STS versions differ and this
    /// list is a snapshot, so a name this build has not heard of may still be
    /// perfectly valid in the user's install.
    /// </summary>
    public static string Normalise(string preset, out bool known)
    {
        var trimmed = preset.Trim();
        foreach (var name in Names)
        {
            if (string.Equals(name, trimmed, StringComparison.OrdinalIgnoreCase))
            {
                known = true;
                return name;
            }
        }

        known = false;
        return trimmed;
    }
}
