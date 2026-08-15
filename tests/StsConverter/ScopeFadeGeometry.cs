using System.Numerics;
using NiflySharp;
using NiflySharp.Bitfields;
using NiflySharp.Blocks;
using NiflySharp.Enums;
using NiflySharp.Structs;

namespace StsConverter;

/// <summary>
/// Builds the canonical STS <c>ScopeFade:0</c> annulus.
/// </summary>
/// <remarks>
/// Every one of the six reference STS scopes carries a bit-for-bit identical
/// ScopeFade mesh: 24 segments, 48 vertices, 48 triangles, 144 indices,
/// half-precision positions at a 20-byte vertex stride, outer radius 0.98730
/// and inner radius 0.49072 in the local XZ plane, facing local -Y. Only the
/// node's local translation and uniform scale differ between scopes. The
/// generator therefore reproduces the canonical asset exactly and adapts to a
/// given scope purely through the node transform, which is what MagnaScope's
/// exact geometry-replay path needs.
/// </remarks>
public static class ScopeFadeGeometry
{
    /// <summary>Segment count of the canonical annulus.</summary>
    public const int CanonicalSegments = 24;

    /// <summary>Outer radius of the canonical annulus in local units.</summary>
    public const float CanonicalOuterRadius = 0.98730f;

    /// <summary>Inner radius of the canonical annulus in local units.</summary>
    public const float CanonicalInnerRadius = 0.49072f;

    /// <summary>
    /// Inner:outer radius ratio, 0.49072 / 0.98730. MagnaScope measured ~0.497
    /// across its corpus, which this confirms.
    /// </summary>
    public const float CanonicalRadiusRatio =
        CanonicalInnerRadius / CanonicalOuterRadius;

    /// <summary>Material path the reference scopes put on ScopeFade:0.</summary>
    public const string FadeMaterial = @"Materials\Scope\LensFadeEffect.BGSM.BGEM";

    /// <summary>Source texture the reference scopes put on ScopeFade:0.</summary>
    public const string FadeTexture = @"textures\Scopes\ScopeRadiusCircle.dds";

    // UV parameterisation recovered from the reference mesh: the annulus maps
    // to a ring centred at (0.5012, 0.49898) in UV space, with the UV angle
    // running backwards from the geometric angle and offset by 97.5 degrees
    // (90 plus half a 15-degree segment). The UV inner:outer ratio is 0.6205,
    // deliberately different from the geometric 0.497 - the texture's own fade
    // gradient depends on it, so it is reproduced rather than recomputed.
    private const float UvCentreU = 0.5012f;
    private const float UvCentreV = 0.49898f;
    private const float UvOuterRadius = 0.44266f;
    private const float UvInnerRadius = 0.27471f;
    private const float UvAngleOffsetDegrees = 97.5f;

    /// <summary>
    /// Creates the ScopeFade:0 BSTriShape plus its shader and alpha property,
    /// adds them to <paramref name="nif"/>, and returns the shape's block index.
    /// The shape is created with an identity transform; the caller positions it.
    /// </summary>
    public static int Create(NifFile nif, string name, int segments)
    {
        if (segments < 3)
            throw new StsConversionException($"Annulus segment count {segments} is too low.");

        var vertices = new List<Vector3>(segments * 2);
        var uvs = new List<TexCoord>(segments * 2);
        var normals = new List<Vector3>(segments * 2);
        for (var i = 0; i < segments * 2; ++i)
        {
            vertices.Add(Vector3.Zero);
            uvs.Add(default);
            normals.Add(-Vector3.UnitY);
        }

        var step = 360.0f / segments;
        for (var k = 0; k < segments; ++k)
        {
            // Angles run clockwise from +Z, matching the reference mesh where
            // vertex 0 sits on +Z and successive rings step by -15 degrees.
            var degrees = 90.0f - step * k;
            var radians = degrees * (MathF.PI / 180.0f);
            var cos = MathF.Cos(radians);
            var sin = MathF.Sin(radians);

            var uvRadians =
                (UvAngleOffsetDegrees - degrees) * (MathF.PI / 180.0f);
            var uvCos = MathF.Cos(uvRadians);
            var uvSin = MathF.Sin(uvRadians);

            var innerIndex = InnerIndexFor(k);
            var outerIndex = OuterIndexFor(k);

            vertices[innerIndex] = new Vector3(
                CanonicalInnerRadius * cos, 0.0f, CanonicalInnerRadius * sin);
            vertices[outerIndex] = new Vector3(
                CanonicalOuterRadius * cos, 0.0f, CanonicalOuterRadius * sin);
            uvs[innerIndex] = new TexCoord(
                UvCentreU + UvInnerRadius * uvCos,
                UvCentreV + UvInnerRadius * uvSin);
            uvs[outerIndex] = new TexCoord(
                UvCentreU + UvOuterRadius * uvCos,
                UvCentreV + UvOuterRadius * uvSin);
        }

        // Two triangles per segment, wound so the face normal points along
        // local -Y, matching the reference mesh's authored winding.
        var triangles = new List<Triangle>(segments * 2);
        for (var k = 1; k <= segments; ++k)
        {
            var previous = k - 1;
            var current = k % segments;
            triangles.Add(new Triangle(
                (ushort)OuterIndexFor(previous),
                (ushort)InnerIndexFor(current),
                (ushort)OuterIndexFor(current)));
            triangles.Add(new Triangle(
                (ushort)OuterIndexFor(previous),
                (ushort)InnerIndexFor(previous),
                (ushort)InnerIndexFor(current)));
        }

        var shape = new BSTriShape(
            nif.Header.Version, vertices, triangles, uvs, normals);
        BlockReflection.SetName(shape, name);
        shape.HasNormals = true;
        shape.HasUVs = true;
        shape.HasTangents = true;
        shape.HasVertexColors = false;
        shape.IsFullPrecision = false;
        shape.Flags_ui = 0x0000000E;
        shape.Scale = 1.0f;
        shape.Translation = Vector3.Zero;
        shape.Rotation = Transform.ToMatrix33(Matrix4x4.Identity);
        // The BSTriShape constructor leaves the Skinned attribute set, which
        // pushes the vertex stride to 32. The reference ScopeFade meshes carry
        // exactly Vertex|UVs|Normals|Tangents, descriptor 0x0001B00000430205,
        // stride 20.
        shape.VertexDesc.VertexAttributes =
            VertexAttribute.Vertex | VertexAttribute.UVs |
            VertexAttribute.Normals | VertexAttribute.Tangents;

        shape.CalcTangentSpace();
        shape.UpdateBounds();
        shape.CalcDataSizes(nif.Header.Version);

        // NiflySharp 1.0.0's CalcDataSizes computes dataSize as
        // vertexSize * vertexCount + triangleCount, but the format (and nifly
        // itself) uses vertexSize * vertexCount + 6 * triangleCount, which is
        // what every shape in the reference files actually stores. Writing the
        // library's value would under-report the index block.
        BlockReflection.SetField(
            shape,
            "_dataSize",
            (uint)(shape.VertexSize * shape.VertexCount +
                   6 * shape.TriangleCount));

        var shapeIndex = nif.AddBlock(shape);

        var shader = new BSEffectShaderProperty();
        BlockReflection.SetName(shader, FadeMaterial);
        shader.ShaderFlags_F4SPF1 =
            Fallout4ShaderPropertyFlags1.ZBuffer_Test |
            Fallout4ShaderPropertyFlags1.External_Emittance;
        shader.ShaderFlags_F4SPF2 = 0;
        shader.UVOffset = new TexCoord(0.0f, 0.0f);
        shader.UVScale = new TexCoord(1.0f, 1.0f);
        BlockReflection.SetNiString4(shader, "_sourceTexture", FadeTexture);
        BlockReflection.SetNiString4(shader, "_greyscaleTexture", string.Empty);
        BlockReflection.SetNiString4(shader, "_envMapTexture", string.Empty);
        BlockReflection.SetNiString4(shader, "_normalTexture", string.Empty);
        BlockReflection.SetNiString4(shader, "_envMaskTexture", string.Empty);
        BlockReflection.SetField(shader, "_textureClampMode", (byte)3);
        BlockReflection.SetField(shader, "_lightingInfluence", (byte)128);
        BlockReflection.SetField(shader, "_baseColor", new Color4(1, 1, 1, 1));
        BlockReflection.SetField(shader, "_baseColorScale", 1.0f);
        BlockReflection.SetField(shader, "_softFalloffDepth", 100.0f);
        BlockReflection.SetField(shader, "_falloffStartAngle", 0.0f);
        BlockReflection.SetField(shader, "_falloffStopAngle", 0.0f);
        BlockReflection.SetField(shader, "_falloffStartOpacity", 0.0f);
        BlockReflection.SetField(shader, "_falloffStopOpacity", 0.0f);
        BlockReflection.SetField(shader, "_environmentMapScale_fl", 1.0f);
        var shaderIndex = nif.AddBlock(shader);

        // Alpha flags 0x10ED, threshold 64: exactly what the reference scopes
        // put on ScopeFade:0.
        var alpha = new NiAlphaProperty
        {
            Flags = new AlphaFlags(0x10ED),
            Threshold = 64,
        };
        var alphaIndex = nif.AddBlock(alpha);

        shape.ShaderPropertyRef = new NiBlockRef<BSShaderProperty>(shaderIndex);
        shape.AlphaPropertyRef = new NiBlockRef<NiAlphaProperty>(alphaIndex);

        VerifyCanonical(shape, segments);
        return shapeIndex;
    }

    /// <summary>
    /// Index of the inner-ring vertex for segment <paramref name="k"/>. The
    /// reference mesh interleaves inner and outer vertices as (inner, outer)
    /// pairs from segment 2 onward but starts with outer(0), inner(1),
    /// outer(1), inner(0); that ordering is reproduced because MagnaScope's
    /// exact-replay path keys on the vertex layout, not just the shape.
    /// </summary>
    private static int InnerIndexFor(int k) => k switch
    {
        0 => 3,
        1 => 1,
        _ => k * 2,
    };

    private static int OuterIndexFor(int k) => k switch
    {
        0 => 0,
        1 => 2,
        _ => k * 2 + 1,
    };

    /// <summary>
    /// Fails loudly if the generated mesh does not match what MagnaScope's
    /// exact geometry-replay path requires.
    /// </summary>
    private static void VerifyCanonical(BSTriShape shape, int segments)
    {
        var expectedVertices = segments * 2;
        var expectedTriangles = segments * 2;
        if (shape.VertexCount != expectedVertices ||
            shape.TriangleCount != expectedTriangles)
        {
            throw new StsConversionException(
                $"Generated ScopeFade has {shape.VertexCount} vertices and " +
                $"{shape.TriangleCount} triangles; expected " +
                $"{expectedVertices} and {expectedTriangles}.");
        }

        if (shape.VertexSize != 20)
        {
            throw new StsConversionException(
                $"Generated ScopeFade has vertex stride {shape.VertexSize}, " +
                "expected 20. MagnaScope's exact replay path requires stride 20.");
        }

        var attributes = shape.VertexDesc.VertexAttributes;
        const VertexAttribute required =
            VertexAttribute.Vertex | VertexAttribute.UVs |
            VertexAttribute.Normals | VertexAttribute.Tangents;
        if (attributes != required)
        {
            throw new StsConversionException(
                $"Generated ScopeFade vertex attributes are {attributes}, " +
                $"expected {required}.");
        }

        var expectedDataSize =
            shape.VertexSize * shape.VertexCount + 6 * shape.TriangleCount;
        if (shape.DataSize != expectedDataSize)
        {
            throw new StsConversionException(
                $"Generated ScopeFade dataSize is {shape.DataSize}, expected " +
                $"{expectedDataSize}.");
        }
    }
}
