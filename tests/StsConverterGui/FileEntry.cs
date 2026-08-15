namespace StsConverter.Gui;

internal enum EntryStatus
{
    Analysing,
    Ready,
    NeedsSelection,
    Converting,
    Succeeded,
    Failed,
}

/// <summary>
/// One input NIF in the batch, plus whatever we have learned about it.
/// </summary>
internal sealed class FileEntry
{
    public FileEntry(string path)
    {
        InputPath = Path.GetFullPath(path);
    }

    public string InputPath { get; }

    public string FileName => Path.GetFileName(InputPath);

    public ScopeAnalysis? Analysis { get; set; }

    public string? AnalysisError { get; set; }

    public EntryStatus Status { get; set; } = EntryStatus.Analysing;

    /// <summary>Shape index chosen as the glass, or -1.</summary>
    public int GlassIndex { get; set; } = -1;

    public int ReticleIndex { get; set; } = -1;

    /// <summary>Shape index chosen as Dot:0, or -1 for none.</summary>
    public int DotIndex { get; set; } = -1;

    /// <summary>
    /// Reticle texture the user has verified, enabling STS reticle swapping.
    /// Empty leaves the original material alone, which renders correctly but
    /// without swap support. See ConvertOptions.ReticleTexture for why this
    /// cannot be derived.
    /// </summary>
    public string ReticleTexture { get; set; } = string.Empty;

    public string DotTexture { get; set; } = string.Empty;

    /// <summary>
    /// Folder holding this mesh's materials. Per-file because a batch can span
    /// mods, and the reticle's real texture lives in its .BGEM.
    /// </summary>
    public string MaterialsRoot { get; set; } = string.Empty;

    /// <summary>
    /// Looks for a Materials folder beside the mesh or in its ancestors, which
    /// is where a mod's loose files normally sit relative to its meshes.
    /// </summary>
    public static string GuessMaterialsRoot(string nifPath)
    {
        var directory = Path.GetDirectoryName(Path.GetFullPath(nifPath));
        for (var depth = 0; depth < 5 && !string.IsNullOrEmpty(directory); ++depth)
        {
            var candidate = Path.Combine(directory, "Materials");
            if (Directory.Exists(candidate))
                return candidate;
            directory = Path.GetDirectoryName(directory);
        }

        return string.Empty;
    }

    /// <summary>
    /// What the mesh currently names as its source texture. Shown as a
    /// starting point only — in a non-STS scope this field is ignored by the
    /// engine and is frequently stale.
    /// </summary>
    public string SuggestedReticleTexture =>
        ShapeByIndex(ReticleIndex)?.SourceTexture ?? string.Empty;

    public string Detail { get; set; } = string.Empty;

    public ConversionOutcome? Outcome { get; set; }

    public string StatusText => Status switch
    {
        EntryStatus.Analysing => "Analysing…",
        EntryStatus.Ready => "Ready",
        EntryStatus.NeedsSelection => "Needs selection",
        EntryStatus.Converting => "Converting…",
        EntryStatus.Succeeded => "Converted",
        EntryStatus.Failed => "Failed",
        _ => string.Empty,
    };

    /// <summary>
    /// True when the entry has everything it needs for a conversion. A missing
    /// glass or reticle is the common case on an unusual scope, and it must
    /// block that one file rather than the whole batch.
    /// </summary>
    public bool IsConvertible =>
        Analysis is not null &&
        GlassIndex >= 0 &&
        ReticleIndex >= 0 &&
        Status is not EntryStatus.Analysing and not EntryStatus.Converting;

    public void ApplyGuesses()
    {
        if (Analysis is null)
            return;

        GlassIndex = Analysis.GlassGuess?.Index ?? -1;
        ReticleIndex = Analysis.ReticleGuess?.Index ?? -1;
        DotIndex = Analysis.DotGuess?.Index ?? -1;

        Status = GlassIndex >= 0 && ReticleIndex >= 0
            ? EntryStatus.Ready
            : EntryStatus.NeedsSelection;

        Detail = Status == EntryStatus.NeedsSelection
            ? (GlassIndex < 0 && ReticleIndex < 0
                ? "No glass or reticle could be guessed — pick both."
                : GlassIndex < 0
                    ? "No glass could be guessed — pick one."
                    : "No reticle could be guessed — pick one.")
            : Analysis.AlreadyConverted
                ? $"Already has STS nodes ({string.Join(", ", Analysis.ExistingStsNodes)}); needs Force."
                : string.Empty;
    }

    public ScopeShapeInfo? ShapeByIndex(int index) =>
        Analysis?.Shapes.FirstOrDefault(shape => shape.Index == index);
}
