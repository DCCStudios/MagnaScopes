namespace StsConverter;

/// <summary>
/// The outcome of one conversion, as data rather than console text.
/// </summary>
public sealed record ConversionOutcome(
    string InputPath,
    string OutputPath,
    bool Passed,
    ConvertReport Report,
    VerifyResult OriginalCheck,
    VerifyResult? CloneCheck,
    IReadOnlyList<string> UnknownBlockTypes)
{
    /// <summary>Worst world-space translation error across both checks.</summary>
    public float MaxTranslationError => MathF.Max(
        OriginalCheck.MaxTranslationError,
        CloneCheck?.MaxTranslationError ?? 0.0f);

    public float MaxRotationDegrees => MathF.Max(
        OriginalCheck.MaxRotationDegrees,
        CloneCheck?.MaxRotationDegrees ?? 0.0f);

    public float MaxScaleError => MathF.Max(
        OriginalCheck.MaxScaleError,
        CloneCheck?.MaxScaleError ?? 0.0f);

    public int ShapesCompared =>
        OriginalCheck.Compared + (CloneCheck?.Compared ?? 0);
}

/// <summary>
/// Runs a conversion and its verification without writing to the console, so
/// the GUI and the CLI share one implementation. The CLI verb is a renderer
/// over this; the GUI is another.
/// </summary>
public static class ConversionRunner
{
    public static ConversionOutcome Run(ConvertOptions options)
    {
        if (string.Equals(
                Path.GetFullPath(options.InputPath),
                Path.GetFullPath(options.OutputPath),
                StringComparison.OrdinalIgnoreCase))
        {
            throw new StsConversionException(
                "The output path is the same file as the input. Converting in " +
                "place is refused; choose a different output path.");
        }

        var nif = NifIo.Load(options.InputPath, allowUnknown: true);
        var unknown = NifIo.UnknownBlockTypes(nif);

        // Reference snapshot taken from a second, untouched load so the
        // verification cannot be fooled by in-place edits.
        var before = VerifyCommand.Snapshot(
            NifIo.Load(options.InputPath, allowUnknown: true));

        var report = StsBuilder.Convert(nif, options);

        var outputDirectory = Path.GetDirectoryName(Path.GetFullPath(options.OutputPath));
        if (!string.IsNullOrEmpty(outputDirectory))
            Directory.CreateDirectory(outputDirectory);

        NifIo.Save(nif, options.OutputPath);

        var after = VerifyCommand.Snapshot(
            NifIo.Load(options.OutputPath, allowUnknown: true));

        // Every original shape must be present in the output, either under its
        // original name or under the name the conversion renamed it to.
        var renames = new Dictionary<string, string>(report.Renames, StringComparer.Ordinal);
        var originalCheck = VerifyCommand.Compare(
            before, after, renames, report.AddedShapeNames);

        VerifyResult? cloneCheck = null;
        if (!options.NoDuplicate)
        {
            // Shapes named by HideWhenAiming are deliberately not cloned: their
            // originals went straight to ScopeNormal, so the pass above covers
            // them.
            var hidden = new HashSet<string>(
                options.HideWhenAiming, StringComparer.OrdinalIgnoreCase);
            // Aiming-only shapes (reticle, dot) deliberately have no hip clone,
            // so the clone pass must not report them as missing.
            hidden.UnionWith(report.AimingOnlyShapeNames);
            var cloneRenames = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var key in before.Keys)
            {
                if (hidden.Contains(before[key].Name))
                {
                    // No hip clone exists for these. Point them at whatever the
                    // conversion renamed the original to, rather than leaving
                    // them to fall through to an identity match -- a renamed
                    // aiming-only shape (Reticle:004 -> Reticle:0) would
                    // otherwise be reported missing.
                    cloneRenames[key] = renames.GetValueOrDefault(key, key);
                    continue;
                }

                cloneRenames[key] = key + options.NormalSuffix;
            }

            cloneCheck = VerifyCommand.Compare(
                before, after, cloneRenames, after.Keys.ToHashSet(StringComparer.Ordinal));
        }

        var passed =
            originalCheck.Passed(options.Tolerance) &&
            (cloneCheck?.Passed(options.Tolerance) ?? true);

        return new ConversionOutcome(
            options.InputPath,
            options.OutputPath,
            passed,
            report,
            originalCheck,
            cloneCheck,
            unknown);
    }
}
