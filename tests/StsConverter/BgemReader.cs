using System.Text;

namespace StsConverter;

/// <summary>
/// Reads the diffuse/source texture out of a Bethesda material file.
///
/// This is the authoritative source for a non-STS reticle's texture. The NIF's
/// own BSEffectShaderProperty Source Texture field is ignored by the engine
/// while a real material exists, and is routinely stale -- the M4A1 ACOG names
/// a RU556 path in the NIF while its material names the MK18 one that actually
/// loads.
/// </summary>
public static class BgemReader
{
    /// <summary>
    /// Texture paths inside a material are relative to Data\Textures, while a
    /// BSEffectShaderProperty wants the path including that folder.
    /// </summary>
    public static string ToShaderTexturePath(string materialTexture)
    {
        var text = materialTexture.Trim().Replace('/', '\\');
        if (text.Length == 0)
            return text;
        return text.StartsWith("textures\\", StringComparison.OrdinalIgnoreCase)
            ? text
            : "Textures\\" + text.TrimStart('\\');
    }

    /// <summary>
    /// Returns the first texture path in the file, which is the diffuse slot.
    ///
    /// Deliberately a signature scan rather than a field-by-field header parse:
    /// the header layout shifts between material versions (the test corpus
    /// alone has BGEM v1 and v2), while the string encoding does not. A hit
    /// requires a uint32 length, exactly length-1 printable bytes, a NUL
    /// terminator, and a .dds extension -- specific enough that a false
    /// positive on float or flag data is not a practical concern.
    /// </summary>
    public static bool TryReadDiffuseTexture(string path, out string texture)
    {
        texture = string.Empty;
        byte[] data;
        try
        {
            data = File.ReadAllBytes(path);
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }

        if (data.Length < 12)
            return false;

        var signature = Encoding.ASCII.GetString(data, 0, 4);
        if (!signature.Equals("BGEM", StringComparison.OrdinalIgnoreCase) &&
            !signature.Equals("BGSM", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        for (var offset = 8; offset + 4 < data.Length; ++offset)
        {
            var length = BitConverter.ToUInt32(data, offset);
            if (length is < 5 or > 512)
                continue;
            if (offset + 4 + length > data.Length)
                continue;
            if (data[offset + 4 + (int)length - 1] != 0)
                continue;

            var printable = true;
            for (var i = 0; i < (int)length - 1; ++i)
            {
                var value = data[offset + 4 + i];
                if (value is < 0x20 or > 0x7E)
                {
                    printable = false;
                    break;
                }
            }

            if (!printable)
                continue;

            var candidate = Encoding.ASCII.GetString(
                data, offset + 4, (int)length - 1);
            if (!candidate.EndsWith(".dds", StringComparison.OrdinalIgnoreCase))
                continue;

            texture = candidate;
            return true;
        }

        return false;
    }

    /// <summary>
    /// Finds the material file a NIF names, under a user-supplied materials
    /// folder. The name in the NIF is Data-relative
    /// ("Materials\Weapons\...\X.BGSM.BGEM"); the folder the user points at may
    /// be that Materials folder, or the Data folder containing it, so both are
    /// tried before falling back to a recursive search on the file name alone.
    /// </summary>
    public static string? Resolve(string materialName, string materialsRoot)
    {
        if (string.IsNullOrWhiteSpace(materialName) ||
            string.IsNullOrWhiteSpace(materialsRoot) ||
            !Directory.Exists(materialsRoot))
        {
            return null;
        }

        var relative = materialName.Trim().Replace('/', '\\').TrimStart('\\');

        // materialsRoot IS the Materials folder.
        if (relative.StartsWith("materials\\", StringComparison.OrdinalIgnoreCase))
        {
            var stripped = relative["materials\\".Length..];
            var candidate = Path.Combine(materialsRoot, stripped);
            if (File.Exists(candidate))
                return candidate;
        }

        // materialsRoot is the parent (a Data folder).
        var direct = Path.Combine(materialsRoot, relative);
        if (File.Exists(direct))
            return direct;

        // Last resort: the file name is usually unique enough within one mod.
        var fileName = Path.GetFileName(relative);
        if (string.IsNullOrEmpty(fileName))
            return null;

        try
        {
            return Directory
                .EnumerateFiles(materialsRoot, fileName, SearchOption.AllDirectories)
                .FirstOrDefault();
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }
}
