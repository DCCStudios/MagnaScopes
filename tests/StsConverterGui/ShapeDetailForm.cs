using System.Text;

namespace StsConverter.Gui;

/// <summary>
/// The full per-shape report for one file — the same information the CLI's
/// 'inspect' verb prints, for when a guess looks wrong and the user needs to
/// decide by hand.
/// </summary>
internal sealed class ShapeDetailForm : Form
{
    public ShapeDetailForm(FileEntry entry)
    {
        Text = $"Shapes — {entry.FileName}";
        Size = new Size(1000, 640);
        StartPosition = FormStartPosition.CenterParent;
        MinimizeBox = false;

        var text = new TextBox
        {
            Dock = DockStyle.Fill,
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Both,
            WordWrap = false,
            Font = new Font(FontFamily.GenericMonospace, 9f),
            Text = Describe(entry),
        };

        var close = new Button
        {
            Text = "Close",
            Dock = DockStyle.Right,
            DialogResult = DialogResult.OK,
            AutoSize = true,
        };

        var footer = new Panel { Dock = DockStyle.Bottom, Height = 36, Padding = new Padding(6) };
        footer.Controls.Add(close);

        Controls.Add(text);
        Controls.Add(footer);
        AcceptButton = close;
        CancelButton = close;
    }

    private static string Describe(FileEntry entry)
    {
        var builder = new StringBuilder();
        builder.AppendLine($"File : {entry.InputPath}");

        if (entry.AnalysisError is not null)
        {
            builder.AppendLine();
            builder.AppendLine($"Analysis failed: {entry.AnalysisError}");
            return builder.ToString();
        }

        if (entry.Analysis is not { } analysis)
        {
            builder.AppendLine();
            builder.AppendLine("Still analysing.");
            return builder.ToString();
        }

        builder.AppendLine($"Root : \"{analysis.RootName}\" ({analysis.BlockCount} blocks)");
        if (analysis.UnknownBlockTypes.Count > 0)
        {
            builder.AppendLine(
                $"Note : block types NiflySharp keeps as opaque bytes: " +
                $"{string.Join(", ", analysis.UnknownBlockTypes)}. " +
                "They round-trip unchanged.");
        }

        if (analysis.AlreadyConverted)
        {
            builder.AppendLine(
                $"Note : already contains STS nodes " +
                $"({string.Join(", ", analysis.ExistingStsNodes)}). " +
                "Conversion needs Force.");
        }

        builder.AppendLine();
        builder.AppendLine(
            "  idx  name                            parent               " +
            "tris   world centre                 radius  chosen");
        builder.AppendLine(new string('-', 122));

        foreach (var shape in analysis.Shapes)
        {
            var marks = new List<string>();
            if (shape.Index == entry.GlassIndex)
                marks.Add("GLASS");
            if (shape.Index == entry.ReticleIndex)
                marks.Add("RETICLE");
            if (shape.Index == entry.DotIndex)
                marks.Add("DOT");

            builder.AppendLine(
                $"  {shape.Index,-4} {Truncate(shape.Name, 31),-31} " +
                $"{Truncate(shape.Parent, 20),-20} {shape.Triangles,-6} " +
                $"({shape.Centre.X,7:F3},{shape.Centre.Y,8:F3},{shape.Centre.Z,7:F3}) " +
                $"{shape.Radius,7:F3}  {string.Join(" ", marks)}");
        }

        builder.AppendLine();
        builder.AppendLine("Shape detail");
        foreach (var shape in analysis.Shapes)
        {
            builder.AppendLine(
                $"  #{shape.Index} \"{shape.Name}\"  verts={shape.Vertices} " +
                $"tris={shape.Triangles} flatness={shape.Flatness:F4} " +
                $"shader={shape.ShaderType} material=\"{shape.Material}\"");
            if (!string.IsNullOrEmpty(shape.SourceTexture))
                builder.AppendLine($"      sourceTexture=\"{shape.SourceTexture}\"");
            if (shape.Aperture is { } aperture)
            {
                builder.AppendLine(
                    $"      aperture ({aperture.Method}): centre=({aperture.Centre.X:F4}," +
                    $"{aperture.Centre.Y:F4},{aperture.Centre.Z:F4}) " +
                    $"radius={aperture.Radius:F4} " +
                    $"axis=({aperture.Axis.X:F3},{aperture.Axis.Y:F3},{aperture.Axis.Z:F3}) " +
                    $"slabVerts={aperture.SlabVertexCount}/{aperture.TotalVertexCount} " +
                    $"axialExtent={aperture.AxisExtent:F3}");
                builder.AppendLine(
                    $"      -> a generated ScopeFade would sit here with outer " +
                    $"radius {aperture.Radius * 0.94f:F4} at fade scale 0.94");
            }
        }

        if (entry.Outcome is { } outcome)
        {
            builder.AppendLine();
            builder.AppendLine("Last conversion");
            builder.AppendLine($"  output   : {outcome.OutputPath}");
            builder.AppendLine(
                $"  verdict  : {(outcome.Passed ? "PASSED" : "FAILED")}");
            builder.AppendLine(
                $"  compared : {outcome.ShapesCompared} shapes");
            builder.AppendLine(
                $"  max translation error : {outcome.MaxTranslationError:E6} units");
            builder.AppendLine(
                $"  max rotation error    : {outcome.MaxRotationDegrees:E6} degrees");
            builder.AppendLine(
                $"  max scale error       : {outcome.MaxScaleError:E6}");
            foreach (var note in outcome.Report.Notes)
                builder.AppendLine($"  - {note}");
        }

        builder.AppendLine();
        builder.AppendLine(
            "Guesses are heuristics. Glass favours a large, thick, " +
            "many-triangle shape named glass/lens or carrying an effect " +
            "shader; reticle favours a small flat quad named reticle/dot.");

        return builder.ToString();
    }

    private static string Truncate(string text, int width) =>
        text.Length <= width ? text : text[..(width - 1)] + "~";
}
