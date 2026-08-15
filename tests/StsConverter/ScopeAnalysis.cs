using System.Numerics;
using NiflySharp;
using NiflySharp.Blocks;

namespace StsConverter;

/// <summary>
/// One shape in a scope NIF, described well enough for a human to decide
/// whether it is the glass or the reticle.
/// </summary>
public sealed record ScopeShapeInfo(
    int Index,
    string Name,
    string Parent,
    int Vertices,
    int Triangles,
    Vector3 Centre,
    float Radius,
    float Flatness,
    string ShaderType,
    string Material,
    string SourceTexture,
    Aperture? Aperture,
    float GlassScore,
    float ReticleScore)
{
    /// <summary>How this shape reads in a dropdown.</summary>
    public string Label => $"#{Index}  {Name}  ({Triangles} tris)";
}

/// <summary>
/// Everything the inspect step learns about one file. Shared by the console
/// 'inspect' verb and by the GUI, so the two can never disagree about which
/// shape is the likely glass.
/// </summary>
public sealed record ScopeAnalysis(
    string Path,
    string RootName,
    int BlockCount,
    IReadOnlyList<string> UnknownBlockTypes,
    IReadOnlyList<string> ExistingStsNodes,
    IReadOnlyList<ScopeShapeInfo> Shapes,
    ScopeShapeInfo? GlassGuess,
    ScopeShapeInfo? ReticleGuess,
    ScopeShapeInfo? DotGuess)
{
    public bool AlreadyConverted => ExistingStsNodes.Count > 0;

    public static ScopeAnalysis Load(string path)
    {
        var nif = NifIo.Load(path, allowUnknown: true);
        return Analyze(path, nif);
    }

    public static ScopeAnalysis Analyze(string path, NifFile nif)
    {
        var tree = new NifTree(nif);

        var existingSts = new[]
        {
            "ScopeNormal", "ScopeAiming", "ScopeViewParts", "ScopeFade:0",
        }.Where(name => tree.FindByName(name) >= 0).ToList();

        var shapes = new List<ScopeShapeInfo>();
        foreach (var index in tree.ShapeIndices())
            shapes.Add(Describe(nif, tree, index));

        var glassGuess = shapes
            .OrderByDescending(shape => shape.GlassScore)
            .FirstOrDefault(shape => shape.GlassScore > 0);
        var reticleGuess = shapes
            .OrderByDescending(shape => shape.ReticleScore)
            .FirstOrDefault(shape => shape.ReticleScore > 0);

        // The dot is the runner-up reticle candidate, but only when it looks
        // like one in its own right. Guessing a second reticle element wrongly
        // is worse than not guessing: the converter will not invent Dot:0
        // geometry, so a bad guess silently promotes an unrelated shape.
        var dotGuess = shapes
            .Where(shape => shape != reticleGuess)
            .Where(shape => shape.Name.Contains("dot", StringComparison.OrdinalIgnoreCase))
            .OrderByDescending(shape => shape.ReticleScore)
            .FirstOrDefault(shape => shape.ReticleScore > 0);

        return new ScopeAnalysis(
            path,
            tree.NameOf(tree.RootIndex),
            nif.Blocks.Count,
            NifIo.UnknownBlockTypes(nif),
            existingSts,
            shapes,
            glassGuess,
            reticleGuess,
            dotGuess);
    }

    private static ScopeShapeInfo Describe(NifFile nif, NifTree tree, int index)
    {
        var name = tree.NameOf(index);
        var parent = tree.NameOf(tree.Parent(index));
        var world = tree.WorldTransform(index);
        var shape = (INiShape)nif.Blocks[index];

        var centre = Vector3.Zero;
        var radius = 0.0f;
        var flatness = 1.0f;
        Aperture? aperture = null;

        if (nif.Blocks[index] is BSTriShape triShape)
        {
            centre = world.Apply(triShape.Bounds.Center);
            radius = triShape.Bounds.Radius * world.Scale;
            var fit = PlaneFit.Fit(triShape.VertexPositions);
            flatness = fit.ThicknessRatio;
            try
            {
                aperture = ApertureMeasurement.Measure(
                    triShape, world, ApertureMeasurement.DefaultSlabFraction);
            }
            catch (StsConversionException)
            {
                aperture = null;
            }
        }

        var shaderType = "-";
        var material = string.Empty;
        var sourceTexture = string.Empty;
        if (nif.Blocks[index] is BSTriShape withShader && withShader.HasShaderProperty)
        {
            var shaderIndex = withShader.ShaderPropertyRef.Index;
            if (shaderIndex >= 0 && shaderIndex < nif.Blocks.Count)
            {
                var shader = nif.Blocks[shaderIndex];
                shaderType = shader.GetType().Name;
                material =
                    (shader as NiflySharp.Interfaces.INiNamed)?.Name?.String ?? string.Empty;
                if (shader is BSEffectShaderProperty effect)
                {
                    sourceTexture =
                        BlockReflection.GetNiString4(effect, "_sourceTexture");
                }
            }
        }

        var lower = name.ToLowerInvariant();
        var materialLower = material.ToLowerInvariant();

        var glassScore = 0.0f;
        if (lower.Contains("glass") || lower.Contains("lens") || lower.Contains("lense"))
            glassScore += 100.0f;
        if (materialLower.Contains("glass") || materialLower.Contains("lens"))
            glassScore += 20.0f;
        if (shaderType == nameof(BSEffectShaderProperty))
            glassScore += 5.0f;
        if (lower.Contains("rear") || lower.Contains("black"))
            glassScore -= 30.0f;
        if (shape.TriangleCount >= 100)
            glassScore += 10.0f;
        if (radius > 0.5f)
            glassScore += 5.0f;

        var reticleScore = 0.0f;
        if (lower.Contains("reticle") || lower.Contains("reticule"))
            reticleScore += 100.0f;
        if (lower.Contains("crosshair"))
            reticleScore += 80.0f;
        if (materialLower.Contains("reticle"))
            reticleScore += 30.0f;
        if (lower.Contains("dot"))
            reticleScore += 20.0f;
        if (shape.TriangleCount is > 0 and <= 32)
            reticleScore += 25.0f;
        if (flatness < 0.02f)
            reticleScore += 25.0f;
        if (shaderType == nameof(BSEffectShaderProperty))
            reticleScore += 10.0f;
        if (lower.Contains("parallax") || lower.Contains("paralax"))
            reticleScore -= 60.0f;

        return new ScopeShapeInfo(
            index, name, parent, shape.VertexCount, shape.TriangleCount,
            centre, radius, flatness, shaderType, material, sourceTexture,
            aperture, glassScore, reticleScore);
    }
}
