using NiflySharp;

namespace StsConverter;

public static class ConvertCommand
{
    public static int Run(string[] args)
    {
        var options = ParseArguments(args);
        if (options is null)
            return 2;

        var outcome = ConversionRunner.Run(options);
        var report = outcome.Report;

        if (outcome.UnknownBlockTypes.Count > 0)
        {
            Console.WriteLine(
                $"Note: {string.Join(", ", outcome.UnknownBlockTypes)} are kept as " +
                "opaque bytes by NiflySharp and written back unchanged.");
        }

        Console.WriteLine();
        Console.WriteLine($"Wrote {options.OutputPath}");
        foreach (var note in report.Notes)
            Console.WriteLine($"  - {note}");

        if (report.Renames.Count > 0)
        {
            Console.WriteLine("  renames:");
            foreach (var rename in report.Renames)
                Console.WriteLine($"    {rename.Key} -> {rename.Value}");
        }

        if (report.Aperture is not null)
        {
            var aperture = report.Aperture;
            Console.WriteLine(
                $"  measured aperture ({aperture.Method}): centre=({aperture.Centre.X:F4}," +
                $"{aperture.Centre.Y:F4},{aperture.Centre.Z:F4}) " +
                $"radius={aperture.Radius:F4} " +
                $"(slab {aperture.SlabVertexCount}/{aperture.TotalVertexCount} verts)");
            Console.WriteLine(
                $"  ScopeFade world outer radius = {report.FadeWorldRadius:F4}, " +
                $"node scale = {report.FadeWorldTransform.Scale:F6}");
        }

        VerifyCommand.Report(
            outcome.OriginalCheck, options.InputPath, options.OutputPath);

        if (outcome.CloneCheck is { } cloneCheck)
        {
            Console.WriteLine();
            Console.WriteLine("HIP-MODEL CLONE VERIFICATION");
            Console.WriteLine($"  shapes compared        : {cloneCheck.Compared}");
            Console.WriteLine(
                $"  max translation error  : {cloneCheck.MaxTranslationError:E6} units " +
                $"(worst: {cloneCheck.WorstShape})");
            Console.WriteLine(
                $"  max rotation error     : {cloneCheck.MaxRotationDegrees:E6} degrees");
            Console.WriteLine($"  max scale error        : {cloneCheck.MaxScaleError:E6}");
            if (cloneCheck.Missing.Count > 0)
            {
                Console.WriteLine(
                    $"  MISSING clones ({cloneCheck.Missing.Count}): " +
                    string.Join(", ", cloneCheck.Missing));
            }
        }

        Console.WriteLine();
        Console.WriteLine(outcome.Passed
            ? "VERIFICATION PASSED: every mesh kept its world transform."
            : "VERIFICATION FAILED: see the errors above. Do not ship this file.");
        return outcome.Passed ? 0 : 1;
    }

    private static ConvertOptions? ParseArguments(string[] args)
    {
        if (args.Length == 0)
        {
            Help.Print();
            return null;
        }

        string? input = null;
        string? output = null;
        string? glass = null;
        string? reticle = null;
        string? dot = null;
        string? fadeFrom = null;
        string? reticleTexture = null;
        string? dotTexture = null;
        string? reticlePreset = null;
        string? materialsRoot = null;
        string normalSuffix = "_full";
        var renameGlass = false;
        var renameFade = false;
        var noScopeFade = false;
        var noDuplicate = false;
        var flatViewParts = false;
        var keepReticleMaterial = false;
        var noTextureLoader = false;
        var force = false;
        var segments = ScopeFadeGeometry.CanonicalSegments;
        var fadeScale = 0.94f;
        float? fadeRadius = null;
        var slabFraction = ApertureMeasurement.DefaultSlabFraction;
        var tolerance = 1e-4f;
        var hide = new List<string>();

        for (var i = 0; i < args.Length; ++i)
        {
            var argument = args[i];
            string Next(string name) =>
                i + 1 < args.Length
                    ? args[++i]
                    : throw new StsConversionException($"{name} needs a value.");

            switch (argument)
            {
                case "--glass": glass = Next(argument); break;
                case "--reticle": reticle = Next(argument); break;
                case "--dot": dot = Next(argument); break;
                case "--fade-from": fadeFrom = Next(argument); break;
                case "--reticle-preset": reticlePreset = Next(argument); break;
                case "--materials": materialsRoot = Next(argument); break;
                case "--reticle-texture": reticleTexture = Next(argument); break;
                case "--dot-texture": dotTexture = Next(argument); break;
                case "--out" or "-o": output = Next(argument); break;
                case "--normal-suffix": normalSuffix = Next(argument); break;
                case "--rename-glass": renameGlass = true; break;
                case "--rename-fade": renameFade = true; break;
                case "--no-scope-fade": noScopeFade = true; break;
                case "--no-duplicate": noDuplicate = true; break;
                case "--flat-viewparts": flatViewParts = true; break;
                case "--keep-reticle-material": keepReticleMaterial = true; break;
                case "--no-texture-loader": noTextureLoader = true; break;
                case "--force": force = true; break;
                case "--segments": segments = ParseInt(Next(argument), argument); break;
                case "--fade-scale": fadeScale = ParseFloat(Next(argument), argument); break;
                case "--fade-radius": fadeRadius = ParseFloat(Next(argument), argument); break;
                case "--slab-fraction": slabFraction = ParseFloat(Next(argument), argument); break;
                case "--tolerance": tolerance = ParseFloat(Next(argument), argument); break;
                case "--hide-when-aiming":
                    hide.AddRange(Next(argument).Split(
                        ',', StringSplitOptions.RemoveEmptyEntries |
                             StringSplitOptions.TrimEntries));
                    break;
                default:
                    if (argument.StartsWith('-'))
                    {
                        throw new StsConversionException(
                            $"Unknown option '{argument}'. Run --help.");
                    }

                    if (input is null)
                        input = argument;
                    else
                        throw new StsConversionException(
                            $"Unexpected extra argument '{argument}'.");
                    break;
            }
        }

        if (input is null)
            throw new StsConversionException("No input NIF was given.");

        // Interactive fallback so the tool is usable without memorising names.
        if (glass is null || reticle is null || output is null)
        {
            if (!Environment.UserInteractive || Console.IsInputRedirected)
            {
                throw new StsConversionException(
                    "convert needs --glass, --reticle and --out. Run " +
                    $"'StsConverter inspect \"{input}\"' to list the shapes.");
            }

            var nif = NifIo.Load(input, allowUnknown: true);
            var tree = new NifTree(nif);
            Console.WriteLine($"Shapes in {input}:");
            foreach (var index in tree.ShapeIndices())
                Console.WriteLine($"  #{index}  {tree.NameOf(index)}");

            glass ??= Prompt("glass shape (name or #index)");
            reticle ??= Prompt("reticle shape (name or #index)");
            output ??= Prompt("output path");
        }

        if (renameFade && renameGlass)
        {
            throw new StsConversionException(
                "--rename-fade and --rename-glass both rename the chosen glass; " +
                "pick one.");
        }

        if (renameFade && noScopeFade)
        {
            throw new StsConversionException(
                "--rename-fade and --no-scope-fade contradict each other.");
        }

        return new ConvertOptions
        {
            InputPath = input,
            OutputPath = output!,
            GlassSelector = glass!,
            ReticleSelector = reticle!,
            DotSelector = dot,
            FadeFromSelector = fadeFrom,
            ReticleTexture = reticleTexture,
            DotTexture = dotTexture,
            ReticlePreset = reticlePreset,
            MaterialsRoot = materialsRoot,
            NormalSuffix = normalSuffix,
            RenameGlass = renameGlass,
            RenameFade = renameFade,
            NoScopeFade = noScopeFade,
            NoDuplicate = noDuplicate,
            FlatViewParts = flatViewParts,
            KeepReticleMaterial = keepReticleMaterial,
            NoTextureLoader = noTextureLoader,
            Force = force,
            Segments = segments,
            FadeScale = fadeScale,
            FadeRadius = fadeRadius,
            SlabFraction = slabFraction,
            Tolerance = tolerance,
            HideWhenAiming = hide,
        };
    }

    private static string Prompt(string label)
    {
        Console.Write($"{label}: ");
        var line = Console.ReadLine();
        if (string.IsNullOrWhiteSpace(line))
            throw new StsConversionException($"No value given for {label}.");
        return line.Trim();
    }

    private static int ParseInt(string text, string name) =>
        int.TryParse(text, out var value)
            ? value
            : throw new StsConversionException($"{name} expects an integer, got '{text}'.");

    private static float ParseFloat(string text, string name) =>
        float.TryParse(text, System.Globalization.NumberStyles.Float,
            System.Globalization.CultureInfo.InvariantCulture, out var value)
            ? value
            : throw new StsConversionException($"{name} expects a number, got '{text}'.");
}
