namespace StsConverter;

/// <summary>
/// Converts many scopes in one go using the heuristic glass/reticle guesses.
/// The GUI drives the identical path (<see cref="ScopeAnalysis"/> then
/// <see cref="ConversionRunner"/>); this verb exists so the batch flow is
/// scriptable and testable without a desktop session.
/// </summary>
public static class BatchCommand
{
    public static int Run(string[] args)
    {
        string? outputFolder = null;
        var force = false;
        var overwrite = false;
        var recurse = false;
        string? materialsRoot = null;
        string? reticleTexture = null;
        string? reticlePreset = null;
        var keepReticleMaterial = false;
        var inputs = new List<string>();

        for (var i = 0; i < args.Length; ++i)
        {
            switch (args[i])
            {
                case "--out" or "-o":
                    outputFolder = i + 1 < args.Length
                        ? args[++i]
                        : throw new StsConversionException("--out needs a value.");
                    break;
                case "--materials":
                    materialsRoot = i + 1 < args.Length
                        ? args[++i]
                        : throw new StsConversionException("--materials needs a value.");
                    break;
                case "--reticle-texture":
                    reticleTexture = i + 1 < args.Length
                        ? args[++i]
                        : throw new StsConversionException(
                            "--reticle-texture needs a value.");
                    break;
                case "--reticle-preset":
                    reticlePreset = i + 1 < args.Length
                        ? args[++i]
                        : throw new StsConversionException(
                            "--reticle-preset needs a value.");
                    break;
                case "--keep-reticle-material": keepReticleMaterial = true; break;
                case "--force": force = true; break;
                case "--overwrite": overwrite = true; break;
                case "--recurse": recurse = true; break;
                default:
                    if (args[i].StartsWith('-'))
                        throw new StsConversionException($"Unknown option '{args[i]}'.");
                    inputs.Add(args[i]);
                    break;
            }
        }

        if (inputs.Count == 0)
        {
            Console.Error.WriteLine(
                "Usage: StsConverter batch <file-or-folder>... --out <folder> " +
                "[--materials <folder>] [--recurse] [--force] [--overwrite]");
            return 2;
        }

        if (outputFolder is null)
            throw new StsConversionException("batch needs --out <folder>.");

        var files = new List<string>();
        foreach (var input in inputs)
        {
            if (Directory.Exists(input))
            {
                files.AddRange(Directory.EnumerateFiles(
                    input,
                    "*.nif",
                    recurse ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly));
            }
            else if (File.Exists(input))
            {
                files.Add(input);
            }
            else
            {
                throw new StsConversionException($"No such file or folder: {input}");
            }
        }

        if (files.Count == 0)
        {
            Console.Error.WriteLine("No .nif files matched.");
            return 2;
        }

        var succeeded = 0;
        var failed = 0;
        var skipped = 0;

        foreach (var file in files.Order(StringComparer.OrdinalIgnoreCase))
        {
            var name = Path.GetFileName(file);
            try
            {
                var analysis = ScopeAnalysis.Load(file);
                if (analysis.GlassGuess is null || analysis.ReticleGuess is null)
                {
                    ++skipped;
                    Console.WriteLine(
                        $"SKIP {name}: could not guess " +
                        (analysis.GlassGuess is null ? "a glass" : "a reticle") +
                        "; convert this one by hand or in the GUI.");
                    continue;
                }

                var outputPath = Path.Combine(outputFolder, name);
                if (File.Exists(outputPath) && !overwrite)
                {
                    ++skipped;
                    Console.WriteLine(
                        $"SKIP {name}: {outputPath} exists (pass --overwrite).");
                    continue;
                }

                var outcome = ConversionRunner.Run(new ConvertOptions
                {
                    InputPath = file,
                    OutputPath = outputPath,
                    GlassSelector = $"#{analysis.GlassGuess.Index}",
                    ReticleSelector = $"#{analysis.ReticleGuess.Index}",
                    DotSelector = analysis.DotGuess is null
                        ? null
                        : $"#{analysis.DotGuess.Index}",
                    Force = force,
                    MaterialsRoot = materialsRoot,
                    ReticleTexture = reticleTexture,
                    ReticlePreset = reticlePreset,
                    KeepReticleMaterial = keepReticleMaterial,
                });

                if (outcome.Passed)
                    ++succeeded;
                else
                    ++failed;

                Console.WriteLine(
                    $"{(outcome.Passed ? "OK  " : "FAIL")} {name}: " +
                    $"glass={analysis.GlassGuess.Name} " +
                    $"reticle={analysis.ReticleGuess.Name} " +
                    $"dot={analysis.DotGuess?.Name ?? "-"} " +
                    $"shapes={outcome.ShapesCompared} " +
                    $"maxErr={outcome.MaxTranslationError:E2}");
            }
            catch (StsConversionException exception)
            {
                ++failed;
                Console.WriteLine($"FAIL {name}: {exception.Message}");
            }
        }

        Console.WriteLine();
        Console.WriteLine(
            $"batch: {succeeded} converted, {failed} failed, {skipped} skipped.");
        return failed > 0 ? 1 : 0;
    }
}
