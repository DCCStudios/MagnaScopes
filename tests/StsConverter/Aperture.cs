using System.Numerics;
using NiflySharp.Blocks;

namespace StsConverter;

/// <summary>
/// The rear (eye-side) opening of a lens mesh, measured in world space.
/// </summary>
public sealed record Aperture(
    Vector3 Centre,
    Vector3 RearwardNormal,
    Vector3 Axis,
    float Radius,
    int SlabVertexCount,
    int TotalVertexCount,
    float AxisExtent,
    float Flatness,
    string Method);

/// <summary>
/// Measures where and how big the see-through opening of a lens mesh is, so a
/// generated ScopeFade annulus can be placed on it.
/// </summary>
/// <remarks>
/// This is a heuristic, and the README says so plainly. The reference STS
/// scopes place ScopeFade on the ocular (rear) lens at roughly 0.88 to 0.98 of
/// that lens's radius, and the exact figure is author-tuned per scope, not
/// derivable. What the measurement does give reliably is the correct plane,
/// the correct facing and the right order of magnitude.
/// </remarks>
public static class ApertureMeasurement
{
    /// <summary>
    /// Fraction of the mesh's axial extent treated as the rear slab. The lens
    /// meshes in the test corpus are two rings roughly nine units apart, so a
    /// slab of 12% isolates the rear ring cleanly.
    /// </summary>
    public const float DefaultSlabFraction = 0.12f;

    /// <summary>
    /// Above this thickness-to-radius ratio a mesh is treated as a lens body
    /// rather than a single disc. The lens bodies in the test corpus sit at
    /// 0.26 to 0.61; the flat rear elements sit at 0.00 to 0.12.
    /// </summary>
    public const float DiscThicknessRatio = 0.2f;

    public static Aperture Measure(
        BSTriShape shape, Transform world, float slabFraction)
    {
        var positions = shape.VertexPositions;
        if (positions.Count < 3)
        {
            throw new StsConversionException(
                $"Shape '{(shape as NiflySharp.Interfaces.INiNamed).Name?.String}' " +
                $"has only {positions.Count} vertices; it cannot be measured " +
                "as a lens aperture.");
        }

        var worldPositions = new List<Vector3>(positions.Count);
        foreach (var position in positions)
            worldPositions.Add(world.Apply(position));

        var centroid = Vector3.Zero;
        foreach (var position in worldPositions)
            centroid += position;
        centroid /= worldPositions.Count;

        // A mesh that is already a flat disc facing along the sight axis IS the
        // aperture; measuring a "rear slab" of it would carve off a chord and
        // put the centre in the wrong place. A thick lens body, which is what
        // most of these scopes ship, gets the axial-slab treatment below.
        var fit = PlaneFit.Fit(worldPositions);
        var facesSightAxis =
            MathF.Abs(Vector3.Dot(fit.Normal, Vector3.UnitY)) > 0.9f;
        if (facesSightAxis && fit.ThicknessRatio < DiscThicknessRatio)
        {
            var discNormal = Vector3.Dot(fit.Normal, -Vector3.UnitY) < 0.0f
                ? -fit.Normal
                : fit.Normal;
            return new Aperture(
                fit.Centroid, discNormal, discNormal, fit.MaxRadius,
                worldPositions.Count, worldPositions.Count,
                fit.MaxDeviation, fit.ThicknessRatio, "flat disc");
        }

        var axis = PrincipalAxis(worldPositions, centroid);

        // Orient the axis so it points rearward, i.e. toward the shooter. In
        // Fallout 4 first-person weapon space the muzzle points along +Y, so
        // rearward is the -Y half-space. This is the same convention the
        // reference ScopeFade meshes use: their vertex normals are (0,-1,0).
        if (Vector3.Dot(axis, -Vector3.UnitY) < 0.0f)
            axis = -axis;

        var minimum = float.MaxValue;
        var maximum = float.MinValue;
        foreach (var position in worldPositions)
        {
            var along = Vector3.Dot(position - centroid, axis);
            minimum = MathF.Min(minimum, along);
            maximum = MathF.Max(maximum, along);
        }

        var extent = maximum - minimum;
        var slabDepth = MathF.Max(extent * slabFraction, 1e-4f);

        // The rear extreme is the largest coordinate along the rearward axis.
        var slab = new List<Vector3>();
        foreach (var position in worldPositions)
        {
            if (Vector3.Dot(position - centroid, axis) >= maximum - slabDepth)
                slab.Add(position);
        }

        if (slab.Count < 3)
            slab = worldPositions;

        var slabCentre = Vector3.Zero;
        foreach (var position in slab)
            slabCentre += position;
        slabCentre /= slab.Count;

        var radius = 0.0f;
        var flatness = 0.0f;
        foreach (var position in slab)
        {
            var offset = position - slabCentre;
            var along = Vector3.Dot(offset, axis);
            radius = MathF.Max(radius, (offset - along * axis).Length());
            flatness = MathF.Max(flatness, MathF.Abs(along));
        }

        if (radius <= 1e-5f)
        {
            throw new StsConversionException(
                "Measured lens aperture radius is zero; the chosen glass shape " +
                "is degenerate. Pass --fade-radius to size ScopeFade manually.");
        }

        return new Aperture(
            slabCentre, axis, axis, radius, slab.Count, worldPositions.Count,
            extent, radius > 0 ? flatness / radius : 0.0f, "rear slab");
    }

    /// <summary>
    /// Direction of largest variance, found by power iteration on the vertex
    /// covariance matrix. For a lens body this is the optical axis.
    /// </summary>
    private static Vector3 PrincipalAxis(
        IReadOnlyList<Vector3> points, Vector3 centroid)
    {
        double xx = 0, xy = 0, xz = 0, yy = 0, yz = 0, zz = 0;
        foreach (var point in points)
        {
            var offset = point - centroid;
            xx += offset.X * offset.X;
            xy += offset.X * offset.Y;
            xz += offset.X * offset.Z;
            yy += offset.Y * offset.Y;
            yz += offset.Y * offset.Z;
            zz += offset.Z * offset.Z;
        }

        var vector = new Vector3(0.3f, 0.9f, 0.31f);
        for (var iteration = 0; iteration < 64; ++iteration)
        {
            var next = new Vector3(
                (float)(xx * vector.X + xy * vector.Y + xz * vector.Z),
                (float)(xy * vector.X + yy * vector.Y + yz * vector.Z),
                (float)(xz * vector.X + yz * vector.Y + zz * vector.Z));
            if (next.LengthSquared() <= 1e-20f)
                return Vector3.UnitY;
            next = Vector3.Normalize(next);
            if ((next - vector).LengthSquared() < 1e-16f)
                return next;
            vector = next;
        }

        return Vector3.Normalize(vector);
    }

    /// <summary>
    /// Rotation that maps the annulus's local -Y face normal onto
    /// <paramref name="rearward"/>, with the roll chosen so local +Z stays as
    /// close as possible to world +Z. For the axis-aligned scopes in the test
    /// corpus this returns the identity, matching the reference files.
    /// </summary>
    public static Matrix4x4 OrientationFor(Vector3 rearward)
    {
        var forward = -Vector3.Normalize(rearward);
        var reference = MathF.Abs(Vector3.Dot(forward, Vector3.UnitZ)) > 0.999f
            ? Vector3.UnitX
            : Vector3.UnitZ;
        var up = reference - Vector3.Dot(reference, forward) * forward;
        up = Vector3.Normalize(up);
        var right = Vector3.Cross(forward, up);

        var rotation = Matrix4x4.Identity;
        rotation.M11 = right.X; rotation.M12 = forward.X; rotation.M13 = up.X;
        rotation.M21 = right.Y; rotation.M22 = forward.Y; rotation.M23 = up.Y;
        rotation.M31 = right.Z; rotation.M32 = forward.Z; rotation.M33 = up.Z;
        return rotation;
    }
}
