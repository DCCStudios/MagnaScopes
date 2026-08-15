using System.Numerics;
using NiflySharp.Blocks;
using NiflySharp.Structs;

namespace StsConverter;

/// <summary>
/// A NIF NiAVObject transform: translation, 3x3 rotation, uniform scale.
/// </summary>
/// <remarks>
/// The composition convention was determined empirically rather than assumed.
/// <c>NiflySharp.Structs.Matrix33</c> documents its fields as
/// "Member &lt;row&gt;,&lt;column&gt;" and a runtime probe confirmed that
/// NiflySharp copies <c>Matrix33.M11/M12/M13</c> into
/// <c>MatTransform.Rotation.Rows[0]</c>, i.e. <c>Rows[i]</c> really is matrix
/// row <c>i</c>. nifly (the C++ library NiflySharp ports) applies a
/// <c>Matrix3</c> to a vector as <c>(rows[0]·v, rows[1]·v, rows[2]·v)</c>, so a
/// NIF local-to-parent transform is the textbook column-vector form:
/// <code>p_parent = translation + scale * (rotation * p_local)</code>
/// which gives the composition and inverse implemented below.
/// </remarks>
public readonly struct Transform
{
    public Transform(Vector3 translation, Matrix4x4 rotation, float scale)
    {
        Translation = translation;
        Rotation = rotation;
        Scale = scale;
    }

    /// <summary>Translation in parent space.</summary>
    public Vector3 Translation { get; }

    /// <summary>
    /// Rotation as a <see cref="Matrix4x4"/> whose upper-left 3x3 block holds
    /// the NIF rotation with <c>M{row}{column}</c> matching NIF's naming.
    /// Only the 3x3 block is ever populated or read.
    /// </summary>
    public Matrix4x4 Rotation { get; }

    /// <summary>Uniform scale factor.</summary>
    public float Scale { get; }

    public static Transform Identity { get; } =
        new(Vector3.Zero, Matrix4x4.Identity, 1.0f);

    /// <summary>Reads the local transform of a scene-graph object.</summary>
    public static Transform FromObject(NiAVObject obj) =>
        new(obj.Translation, ToMatrix(obj.Rotation), obj.Scale);

    /// <summary>Writes this transform back onto a scene-graph object.</summary>
    public void ApplyTo(NiAVObject obj)
    {
        obj.Translation = Translation;
        obj.Rotation = ToMatrix33(Rotation);
        obj.Scale = Scale;
    }

    /// <summary>
    /// Composes <paramref name="parent"/> with <paramref name="child"/> so the
    /// result maps child-local space straight into the parent's parent space.
    /// </summary>
    public static Transform Compose(Transform parent, Transform child)
    {
        var rotation = Multiply(parent.Rotation, child.Rotation);
        var translation = parent.Translation +
            parent.Scale * TransformVector(parent.Rotation, child.Translation);
        return new Transform(translation, rotation, parent.Scale * child.Scale);
    }

    /// <summary>Inverts a translation/rotation/uniform-scale transform.</summary>
    public Transform Inverse()
    {
        var inverseRotation = Transpose(Rotation);
        var inverseScale = 1.0f / Scale;
        var translation =
            -inverseScale * TransformVector(inverseRotation, Translation);
        return new Transform(translation, inverseRotation, inverseScale);
    }

    /// <summary>Maps a point from this transform's local space into its parent.</summary>
    public Vector3 Apply(Vector3 point) =>
        Translation + Scale * TransformVector(Rotation, point);

    /// <summary>Rotates a direction without translating or scaling it.</summary>
    public Vector3 ApplyRotation(Vector3 direction) =>
        TransformVector(Rotation, direction);

    public static Vector3 TransformVector(Matrix4x4 rotation, Vector3 v) =>
        new(
            rotation.M11 * v.X + rotation.M12 * v.Y + rotation.M13 * v.Z,
            rotation.M21 * v.X + rotation.M22 * v.Y + rotation.M23 * v.Z,
            rotation.M31 * v.X + rotation.M32 * v.Y + rotation.M33 * v.Z);

    public static Matrix4x4 Multiply(Matrix4x4 a, Matrix4x4 b)
    {
        var result = Matrix4x4.Identity;
        result.M11 = a.M11 * b.M11 + a.M12 * b.M21 + a.M13 * b.M31;
        result.M12 = a.M11 * b.M12 + a.M12 * b.M22 + a.M13 * b.M32;
        result.M13 = a.M11 * b.M13 + a.M12 * b.M23 + a.M13 * b.M33;
        result.M21 = a.M21 * b.M11 + a.M22 * b.M21 + a.M23 * b.M31;
        result.M22 = a.M21 * b.M12 + a.M22 * b.M22 + a.M23 * b.M32;
        result.M23 = a.M21 * b.M13 + a.M22 * b.M23 + a.M23 * b.M33;
        result.M31 = a.M31 * b.M11 + a.M32 * b.M21 + a.M33 * b.M31;
        result.M32 = a.M31 * b.M12 + a.M32 * b.M22 + a.M33 * b.M32;
        result.M33 = a.M31 * b.M13 + a.M32 * b.M23 + a.M33 * b.M33;
        return result;
    }

    public static Matrix4x4 Transpose(Matrix4x4 m)
    {
        var result = Matrix4x4.Identity;
        result.M11 = m.M11; result.M12 = m.M21; result.M13 = m.M31;
        result.M21 = m.M12; result.M22 = m.M22; result.M23 = m.M32;
        result.M31 = m.M13; result.M32 = m.M23; result.M33 = m.M33;
        return result;
    }

    public static Matrix4x4 ToMatrix(Matrix33 m)
    {
        var result = Matrix4x4.Identity;
        result.M11 = m.M11; result.M12 = m.M12; result.M13 = m.M13;
        result.M21 = m.M21; result.M22 = m.M22; result.M23 = m.M23;
        result.M31 = m.M31; result.M32 = m.M32; result.M33 = m.M33;
        return result;
    }

    public static Matrix33 ToMatrix33(Matrix4x4 m) => new()
    {
        M11 = m.M11, M12 = m.M12, M13 = m.M13,
        M21 = m.M21, M22 = m.M22, M23 = m.M23,
        M31 = m.M31, M32 = m.M32, M33 = m.M33,
    };

    /// <summary>
    /// Largest absolute difference between two rotation matrices, element-wise.
    /// </summary>
    public static float MaxRotationDelta(Matrix4x4 a, Matrix4x4 b)
    {
        var worst = 0.0f;
        worst = MathF.Max(worst, MathF.Abs(a.M11 - b.M11));
        worst = MathF.Max(worst, MathF.Abs(a.M12 - b.M12));
        worst = MathF.Max(worst, MathF.Abs(a.M13 - b.M13));
        worst = MathF.Max(worst, MathF.Abs(a.M21 - b.M21));
        worst = MathF.Max(worst, MathF.Abs(a.M22 - b.M22));
        worst = MathF.Max(worst, MathF.Abs(a.M23 - b.M23));
        worst = MathF.Max(worst, MathF.Abs(a.M31 - b.M31));
        worst = MathF.Max(worst, MathF.Abs(a.M32 - b.M32));
        worst = MathF.Max(worst, MathF.Abs(a.M33 - b.M33));
        return worst;
    }

    /// <summary>
    /// Rotation error expressed as an angle in degrees, computed from the
    /// relative rotation <c>aᵀ·b</c>. This is the number that actually matters
    /// for a reticle: an element-wise matrix delta is not an aiming error.
    /// </summary>
    public static float RotationAngleDegrees(Matrix4x4 a, Matrix4x4 b)
    {
        var relative = Multiply(Transpose(a), b);
        var trace = relative.M11 + relative.M22 + relative.M33;
        var cosine = Math.Clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f);
        return MathF.Acos(cosine) * (180.0f / MathF.PI);
    }

    public override string ToString() =>
        $"t=({Translation.X:F6}, {Translation.Y:F6}, {Translation.Z:F6}) " +
        $"s={Scale:F6} " +
        $"R=[{Rotation.M11:F4} {Rotation.M12:F4} {Rotation.M13:F4}; " +
        $"{Rotation.M21:F4} {Rotation.M22:F4} {Rotation.M23:F4}; " +
        $"{Rotation.M31:F4} {Rotation.M32:F4} {Rotation.M33:F4}]";
}
