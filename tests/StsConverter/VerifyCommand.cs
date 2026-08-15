using System.Numerics;
using NiflySharp;

namespace StsConverter;

public sealed record ShapeWorldState(
    string Name,
    int Index,
    Transform World,
    int VertexCount,
    int TriangleCount,
    Vector3 BoundsCenterWorld,
    float BoundsRadiusWorld);

public sealed record VerifyResult(
    int Compared,
    float MaxTranslationError,
    float MaxRotationDegrees,
    float MaxScaleError,
    float MaxBoundsCentreError,
    string WorstShape,
    List<string> Missing,
    List<string> Added)
{
    public bool Passed(float tolerance) =>
        Missing.Count == 0 &&
        MaxTranslationError <= tolerance &&
        MaxScaleError <= tolerance &&
        MaxRotationDegrees <= 1e-3f;
}

/// <summary>
/// Numerical proof that reparenting did not move anything. Compares the
/// composed root-relative transform of every shape between two files.
/// </summary>
public static class VerifyCommand
{
    public static int Run(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine(
                "Usage: StsConverter verify <input.nif> <output.nif>");
            return 2;
        }

        var before = Snapshot(NifIo.Load(args[0], allowUnknown: true));
        var after = Snapshot(NifIo.Load(args[1], allowUnknown: true));
        var result = Compare(before, after);
        Report(result, args[0], args[1]);
        return result.Passed(1e-4f) ? 0 : 1;
    }

    /// <summary>
    /// World transform of every shape reachable from the root, keyed by name.
    /// Shapes renamed by the conversion are matched via
    /// <paramref name="renames"/> supplied by the caller.
    /// </summary>
    public static Dictionary<string, ShapeWorldState> Snapshot(NifFile nif)
    {
        var tree = new NifTree(nif);
        var result = new Dictionary<string, ShapeWorldState>(StringComparer.Ordinal);

        foreach (var index in tree.ShapeIndices())
        {
            var name = tree.NameOf(index);
            var world = tree.WorldTransform(index);
            var shape = (NiflySharp.INiShape)nif.Blocks[index];

            var centre = Vector3.Zero;
            var radius = 0.0f;
            if (nif.Blocks[index] is NiflySharp.Blocks.BSTriShape triShape)
            {
                centre = world.Apply(triShape.Bounds.Center);
                radius = triShape.Bounds.Radius * world.Scale;
            }

            var key = name;
            var suffix = 1;
            while (result.ContainsKey(key))
                key = $"{name}#{suffix++}";

            result[key] = new ShapeWorldState(
                name, index, world, shape.VertexCount, shape.TriangleCount,
                centre, radius);
        }

        return result;
    }

    /// <summary>
    /// Compares two snapshots. <paramref name="renames"/> maps a name in the
    /// "before" snapshot to its name in the "after" snapshot.
    /// </summary>
    public static VerifyResult Compare(
        Dictionary<string, ShapeWorldState> before,
        Dictionary<string, ShapeWorldState> after,
        IReadOnlyDictionary<string, string>? renames = null,
        IReadOnlySet<string>? ignoreAdded = null)
    {
        var compared = 0;
        var maxTranslation = 0.0f;
        var maxRotation = 0.0f;
        var maxScale = 0.0f;
        var maxCentre = 0.0f;
        var worst = "<none>";
        var missing = new List<string>();
        var matchedAfterKeys = new HashSet<string>(StringComparer.Ordinal);

        foreach (var entry in before)
        {
            var targetKey = entry.Key;
            if (renames is not null && renames.TryGetValue(entry.Key, out var renamed))
                targetKey = renamed;

            if (!after.TryGetValue(targetKey, out var outputState))
            {
                missing.Add($"{entry.Key} (expected as '{targetKey}')");
                continue;
            }

            matchedAfterKeys.Add(targetKey);
            ++compared;

            var inputWorld = entry.Value.World;
            var outputWorld = outputState.World;

            var translationError =
                (inputWorld.Translation - outputWorld.Translation).Length();
            var rotationError = Transform.RotationAngleDegrees(
                inputWorld.Rotation, outputWorld.Rotation);
            var scaleError = MathF.Abs(inputWorld.Scale - outputWorld.Scale);
            var centreError =
                (entry.Value.BoundsCenterWorld - outputState.BoundsCenterWorld)
                .Length();

            if (translationError > maxTranslation)
            {
                maxTranslation = translationError;
                worst = $"{entry.Key} -> {targetKey}";
            }

            maxRotation = MathF.Max(maxRotation, rotationError);
            maxScale = MathF.Max(maxScale, scaleError);
            maxCentre = MathF.Max(maxCentre, centreError);
        }

        var added = after.Keys
            .Where(key => !matchedAfterKeys.Contains(key))
            .Where(key => ignoreAdded is null || !ignoreAdded.Contains(key))
            .ToList();

        return new VerifyResult(
            compared, maxTranslation, maxRotation, maxScale, maxCentre,
            worst, missing, added);
    }

    public static void Report(VerifyResult result, string beforeLabel, string afterLabel)
    {
        Console.WriteLine();
        Console.WriteLine("WORLD-TRANSFORM VERIFICATION");
        Console.WriteLine($"  before : {beforeLabel}");
        Console.WriteLine($"  after  : {afterLabel}");
        Console.WriteLine($"  shapes compared        : {result.Compared}");
        Console.WriteLine(
            $"  max translation error  : {result.MaxTranslationError:E6} units " +
            $"(worst: {result.WorstShape})");
        Console.WriteLine(
            $"  max rotation error     : {result.MaxRotationDegrees:E6} degrees");
        Console.WriteLine($"  max scale error        : {result.MaxScaleError:E6}");
        Console.WriteLine(
            $"  max bounds-centre error: {result.MaxBoundsCentreError:E6} units");

        if (result.Missing.Count > 0)
        {
            Console.WriteLine(
                $"  MISSING in output ({result.Missing.Count}): " +
                string.Join(", ", result.Missing));
        }

        if (result.Added.Count > 0)
        {
            Console.WriteLine(
                $"  added by conversion ({result.Added.Count}): " +
                string.Join(", ", result.Added));
        }
    }
}
