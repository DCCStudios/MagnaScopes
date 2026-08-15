namespace StsConverter;

/// <summary>
/// Lists a scope's shapes so the user can pick the glass and the reticle.
/// The analysis itself lives in <see cref="ScopeAnalysis"/> so the GUI shares
/// it; this verb only renders the result.
/// </summary>
public static class InspectCommand
{
    public static int Run(string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("Usage: StsConverter inspect <input.nif>");
            return 2;
        }

        var path = args[0];
        var analysis = ScopeAnalysis.Load(path);

        Console.WriteLine($"File   : {path}");
        Console.WriteLine($"Root   : \"{analysis.RootName}\" ({analysis.BlockCount} blocks)");
        if (analysis.UnknownBlockTypes.Count > 0)
        {
            Console.WriteLine(
                $"Note   : block types NiflySharp keeps as opaque bytes: " +
                $"{string.Join(", ", analysis.UnknownBlockTypes)}. " +
                "They round-trip unchanged.");
        }

        if (analysis.AlreadyConverted)
        {
            Console.WriteLine(
                $"Note   : already contains STS nodes " +
                $"({string.Join(", ", analysis.ExistingStsNodes)}). " +
                "'convert' will refuse this file unless --force is given.");
        }

        if (analysis.Shapes.Count == 0)
        {
            Console.WriteLine("No shapes found.");
            return 0;
        }

        Console.WriteLine();
        Console.WriteLine(
            "  idx  name                            parent               " +
            "tris   world centre                 radius  guess");
        Console.WriteLine(new string('-', 118));
        foreach (var row in analysis.Shapes)
        {
            var guess = new List<string>();
            if (row == analysis.GlassGuess)
                guess.Add("GLASS?");
            if (row == analysis.ReticleGuess)
                guess.Add("RETICLE?");
            if (row == analysis.DotGuess)
                guess.Add("DOT?");
            Console.WriteLine(
                $"  {row.Index,-4} {Truncate(row.Name, 31),-31} " +
                $"{Truncate(row.Parent, 20),-20} {row.Triangles,-6} " +
                $"({row.Centre.X,7:F3},{row.Centre.Y,8:F3},{row.Centre.Z,7:F3}) " +
                $"{row.Radius,7:F3}  {string.Join(" ", guess)}");
        }

        Console.WriteLine();
        Console.WriteLine("Shape detail");
        foreach (var row in analysis.Shapes)
        {
            Console.WriteLine(
                $"  #{row.Index} \"{row.Name}\"  verts={row.Vertices} " +
                $"tris={row.Triangles} flatness={row.Flatness:F4} " +
                $"shader={row.ShaderType} material=\"{row.Material}\"");
            if (!string.IsNullOrEmpty(row.SourceTexture))
                Console.WriteLine($"      sourceTexture=\"{row.SourceTexture}\"");
            if (row.Aperture is not null)
            {
                var aperture = row.Aperture;
                Console.WriteLine(
                    $"      aperture ({aperture.Method}): centre=({aperture.Centre.X:F4}," +
                    $"{aperture.Centre.Y:F4},{aperture.Centre.Z:F4}) " +
                    $"radius={aperture.Radius:F4} " +
                    $"axis=({aperture.Axis.X:F3},{aperture.Axis.Y:F3},{aperture.Axis.Z:F3}) " +
                    $"slabVerts={aperture.SlabVertexCount}/{aperture.TotalVertexCount} " +
                    $"axialExtent={aperture.AxisExtent:F3}");
                Console.WriteLine(
                    $"      -> generated ScopeFade would sit here with outer " +
                    $"radius {aperture.Radius * 0.94f:F4} at the default " +
                    "--fade-scale 0.94");
            }
        }

        Console.WriteLine();
        if (analysis.GlassGuess is not null && analysis.ReticleGuess is not null)
        {
            Console.WriteLine("Suggested command:");
            Console.WriteLine(
                $"  StsConverter convert \"{path}\" " +
                $"--glass \"#{analysis.GlassGuess.Index}\" " +
                $"--reticle \"#{analysis.ReticleGuess.Index}\" --out \"<output.nif>\"");
        }
        else
        {
            Console.WriteLine(
                "Could not guess both a glass and a reticle; pick them by hand.");
        }

        Console.WriteLine();
        Console.WriteLine(
            "The guesses are heuristics only. Glass favours a large, thick, " +
            "many-triangle shape with 'glass' or 'lens' in its name or an " +
            "effect shader; reticle favours a small flat quad near the sight " +
            "axis with 'reticle' or 'dot' in its name.");
        return 0;
    }

    private static string Truncate(string text, int width) =>
        text.Length <= width ? text : text[..(width - 1)] + "~";
}
