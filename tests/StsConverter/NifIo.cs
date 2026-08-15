using NiflySharp;
using NiflySharp.Blocks;

namespace StsConverter;

/// <summary>Loading and saving with loud, specific failures.</summary>
public static class NifIo
{
    /// <summary>
    /// Loads a NIF. Unknown block types are reported by name; by default an
    /// unknown block aborts a conversion because rewriting the file could drop
    /// data. <paramref name="allowUnknown"/> is for read-only inspection.
    /// </summary>
    public static NifFile Load(string path, bool allowUnknown = false)
    {
        if (!File.Exists(path))
            throw new StsConversionException($"Input NIF not found: {path}");

        var nif = new NifFile();
        var code = nif.Load(path);
        if (code != 0)
        {
            throw new StsConversionException(
                $"NiflySharp failed to load '{path}' (code {code}).");
        }

        if (!nif.Valid)
            throw new StsConversionException($"'{path}' loaded but is not valid.");

        if (nif.HasUnknownBlocks && !allowUnknown)
        {
            throw new StsConversionException(
                $"'{path}' contains block types NiflySharp does not understand " +
                $"({string.Join(", ", UnknownBlockTypes(nif))}). Rewriting the " +
                "file could drop data, so conversion is refused.");
        }

        if (nif.GetRootNode() is null)
        {
            throw new StsConversionException(
                $"'{path}' has no NiNode root; only NiNode-rooted scope meshes " +
                "can be converted.");
        }

        return nif;
    }

    /// <summary>Distinct block-type names that NiflySharp parsed as NiUnknown.</summary>
    public static List<string> UnknownBlockTypes(NifFile nif)
    {
        var names = new List<string>();
        for (var index = 0; index < nif.Blocks.Count; ++index)
        {
            if (nif.Blocks[index] is not NiUnknown)
                continue;
            var typeName = nif.Header.GetBlockTypeNameById(index);
            if (!names.Contains(typeName))
                names.Add(typeName);
        }

        return names;
    }

    public static void Save(NifFile nif, string path)
    {
        var directory = Path.GetDirectoryName(Path.GetFullPath(path));
        if (!string.IsNullOrEmpty(directory))
            Directory.CreateDirectory(directory);

        // SortBlocks reproduces NifSkope's Spells > Sanitize > Reorder Blocks,
        // which the STS documentation asks for after child reordering.
        var options = new NifFileSaveOptions
        {
            RemoveUnreferencedBlocks = false,
            SortBlocks = true,
            UpdateBounds = false,
        };

        var code = nif.Save(path, options);
        if (code != 0)
        {
            throw new StsConversionException(
                $"NiflySharp failed to save '{path}' (code {code}).");
        }
    }
}
