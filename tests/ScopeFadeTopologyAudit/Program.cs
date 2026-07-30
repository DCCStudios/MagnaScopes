using NiflySharp;
using NiflySharp.Blocks;
using NiflySharp.Interfaces;
using System.Numerics;

if (args.Length is < 1 or > 2 || !Directory.Exists(args[0]))
{
    Console.Error.WriteLine(
        "Usage: ScopeFadeTopologyAudit <extracted-STS-root> [--dump-first]");
    return 2;
}

var dumpFirst = args.Length == 2 &&
    string.Equals(args[1], "--dump-first", StringComparison.Ordinal);
var dumpedFirst = false;
var nifCount = 0;
var loadFailures = 0;
var scopeFadeCount = 0;
var failures = new List<string>();

foreach (var path in Directory.EnumerateFiles(
             args[0],
             "*.nif",
             SearchOption.AllDirectories))
{
    ++nifCount;
    var nif = new NifFile();
    if (nif.Load(path) != 0)
    {
        ++loadFailures;
        continue;
    }

    foreach (var block in nif.Blocks)
    {
        if (block is not BSTriShape shape ||
            (shape as INiNamed)?.Name?.String != "ScopeFade:0")
        {
            continue;
        }

        ++scopeFadeCount;
        var relative = Path.GetRelativePath(args[0], path);
        if (shape.VertexPositions.Count != 48 ||
            shape.Triangles.Count != 48)
        {
            failures.Add(
                $"{relative}: vertices={shape.VertexPositions.Count}, " +
                $"triangles={shape.Triangles.Count}");
            continue;
        }

        var center = shape.Bounds.Center;
        var radii = shape.VertexPositions
            .Select(position =>
            {
                var delta = position - center;
                return MathF.Sqrt(delta.X * delta.X + delta.Z * delta.Z);
            })
            .ToArray();
        var minimumRadius = radii.Min();
        var maximumRadius = radii.Max();
        var radiusThreshold = (minimumRadius + maximumRadius) * 0.5F;
        var centerTolerance = maximumRadius * 0.03F;

        if (dumpFirst && !dumpedFirst)
        {
            dumpedFirst = true;
            Console.WriteLine(
                $"First ScopeFade: {relative}; center={center}; " +
                $"minimumRadius={minimumRadius:F6}; maximumRadius={maximumRadius:F6}");
            for (var primitive = 0;
                 primitive < Math.Min(4, shape.Triangles.Count);
                 ++primitive)
            {
                var triangle = shape.Triangles[primitive];
                var indices = new[] {
                    (int)triangle.V1,
                    (int)triangle.V2,
                    (int)triangle.V3
                };
                Console.WriteLine(
                    $"  primitive {primitive}: [{string.Join(", ", indices)}]");
                foreach (var index in indices)
                {
                    Console.WriteLine(
                        $"    v{index}: {shape.VertexPositions[index]}, " +
                        $"radius={radii[index]:F6}");
                }
            }
        }

        for (var primitive = 0; primitive < shape.Triangles.Count; ++primitive)
        {
            var triangle = shape.Triangles[primitive];
            var indices = new[] {
                (int)triangle.V1,
                (int)triangle.V2,
                (int)triangle.V3
            };
            if (indices.Any(index =>
                    index < 0 || index >= shape.VertexPositions.Count))
            {
                failures.Add($"{relative}: primitive {primitive} has an invalid index");
                break;
            }

            var outerCount = indices.Count(index =>
                radii[index] > radiusThreshold);
            var expectedOuterCount = primitive % 2 == 0 ? 2 : 1;
            if (outerCount != expectedOuterCount)
            {
                failures.Add(
                    $"{relative}: primitive {primitive} has " +
                    $"{outerCount} outer vertices, expected {expectedOuterCount}");
                break;
            }

            if (primitive % 2 == 1)
            {
                var outerCurrent = shape.VertexPositions[indices[0]];
                var innerCurrent = shape.VertexPositions[indices[1]];
                var reconstructedCenter =
                    2.0F * innerCurrent - outerCurrent;
                var centerError = Vector3.Distance(
                    reconstructedCenter,
                    center);
                if (centerError > centerTolerance)
                {
                    failures.Add(
                        $"{relative}: primitive {primitive} center error " +
                        $"{centerError:F6} exceeds {centerTolerance:F6}");
                    break;
                }
            }
        }
    }
}

Console.WriteLine(
    $"Audited {nifCount} NIFs, {scopeFadeCount} ScopeFade shapes, " +
    $"{loadFailures} unrelated load failures.");
if (failures.Count != 0)
{
    foreach (var failure in failures.Take(25))
        Console.Error.WriteLine(failure);
    Console.Error.WriteLine(
        $"ScopeFade topology audit FAILED with {failures.Count} incompatible shapes.");
    return 1;
}

if (scopeFadeCount == 0)
{
    Console.Error.WriteLine("ScopeFade topology audit found no ScopeFade shapes.");
    return 3;
}

Console.WriteLine(
    "ScopeFade topology audit PASSED: every extracted ScopeFade uses the " +
    "48-triangle alternating annulus required by the geometry fill shader.");
return 0;
