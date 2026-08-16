using System.Numerics;
using System.Text;
using NiflySharp;
using NiflySharp.Blocks;
using NiflySharp.Interfaces;

namespace StsConverter;

/// <summary>
/// Step 1 instrument: prints everything about a NIF that the STS layout
/// depends on, so the target structure can be derived from real files instead
/// of from prose.
/// </summary>
public static class DumpCommand
{
    public static int Run(string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("Usage: StsConverter dump <nif> [<nif>...]");
            return 2;
        }

        foreach (var path in args)
        {
            var nif = NifIo.Load(path, allowUnknown: true);
            var tree = new NifTree(nif);

            Console.WriteLine(new string('=', 78));
            Console.WriteLine($"FILE: {path}");
            Console.WriteLine(
                $"version={nif.Header.Version.VersionString} " +
                $"user={nif.Header.Version.UserVersion} " +
                $"stream={nif.Header.Version.StreamVersion} " +
                $"blocks={nif.Blocks.Count} unknown={nif.HasUnknownBlocks} " +
                $"[{string.Join(", ", NifIo.UnknownBlockTypes(nif))}]");
            Console.WriteLine(new string('=', 78));

            Console.WriteLine("--- SCENE GRAPH (child order as authored) ---");
            PrintNode(nif, tree, tree.RootIndex, 0);

            Console.WriteLine();
            Console.WriteLine("--- SEQUENCES / CONTROLLER MANAGER ---");
            PrintControllers(nif, tree);

            Console.WriteLine();
            Console.WriteLine("--- OPTICAL GEOMETRY DETAIL ---");
            foreach (var shapeIndex in tree.ShapeIndices())
            {
                var name = tree.NameOf(shapeIndex);
                if (!IsOpticalName(name))
                    continue;
                PrintGeometryDetail(nif, tree, shapeIndex);
            }

            Console.WriteLine();
            Console.WriteLine("--- BLOCK TYPE HISTOGRAM ---");
            var histogram = new SortedDictionary<string, int>(StringComparer.Ordinal);
            foreach (var block in nif.Blocks)
            {
                var key = block.GetType().Name;
                histogram[key] = histogram.TryGetValue(key, out var count)
                    ? count + 1 : 1;
            }

            foreach (var entry in histogram)
                Console.WriteLine($"  {entry.Value,4} x {entry.Key}");

            Console.WriteLine();
        }

        return 0;
    }

    private static bool IsOpticalName(string name) =>
        name.Contains("ScopeFade", StringComparison.OrdinalIgnoreCase) ||
        name.Contains("Reticle", StringComparison.OrdinalIgnoreCase) ||
        name.Contains("Dot", StringComparison.OrdinalIgnoreCase) ||
        name.Contains("TextureLoader", StringComparison.OrdinalIgnoreCase) ||
        name.Contains("Glass", StringComparison.OrdinalIgnoreCase) ||
        name.Contains("Lens", StringComparison.OrdinalIgnoreCase);

    private static void PrintNode(NifFile nif, NifTree tree, int index, int depth)
    {
        if (index < 0 || index >= nif.Blocks.Count)
            return;

        var block = nif.Blocks[index];
        var indent = new string(' ', depth * 2);
        var name = tree.NameOf(index);
        var line = new StringBuilder();
        line.Append($"{indent}[{index}] {block.GetType().Name} \"{name}\"");

        if (block is NiAVObject obj)
        {
            var local = Transform.FromObject(obj);
            var identity =
                local.Translation == Vector3.Zero &&
                MathF.Abs(local.Scale - 1.0f) < 1e-6f &&
                Transform.MaxRotationDelta(local.Rotation, Matrix4x4.Identity) < 1e-6f;
            line.Append(identity ? "  local=IDENTITY" : $"  local={local}");
            line.Append($"  flags=0x{obj.Flags_ui:X}");
        }

        Console.WriteLine(line.ToString());

        if (block is NiObjectNET named)
        {
            var extras = named.ExtraDataList.Indices.ToList();
            foreach (var extraIndex in extras)
            {
                if (extraIndex < 0 || extraIndex >= nif.Blocks.Count)
                    continue;
                var extra = nif.Blocks[extraIndex];
                Console.WriteLine(
                    $"{indent}    extra[{extraIndex}] {extra.GetType().Name} " +
                    $"\"{(extra as INiNamed)?.Name?.String}\" " +
                    $"{DescribeExtraData(extra)}");
            }

            if (named.Controller?.Index >= 0)
            {
                var ctrlIndex = named.Controller.Index;
                Console.WriteLine(
                    $"{indent}    controller[{ctrlIndex}] " +
                    $"{nif.Blocks[ctrlIndex].GetType().Name}");
            }
        }

        if (block is INiShape shape)
        {
            var world = tree.WorldTransform(index);
            var boundsCenter = shape is BSTriShape triShape
                ? triShape.Bounds.Center
                : Vector3.Zero;
            var boundsRadius = shape is BSTriShape triShape2
                ? triShape2.Bounds.Radius
                : 0.0f;
            Console.WriteLine(
                $"{indent}    verts={shape.VertexCount} " +
                $"tris={shape.TriangleCount} " +
                $"boundsLocal=({boundsCenter.X:F4},{boundsCenter.Y:F4}," +
                $"{boundsCenter.Z:F4}) r={boundsRadius:F4}");
            Console.WriteLine($"{indent}    world={world}");

            if (block is BSTriShape bsTri)
            {
                Console.WriteLine(
                    $"{indent}    vertexSize={bsTri.VertexSize} " +
                    $"dataSize={bsTri.DataSize} " +
                    $"desc=0x{bsTri.VertexDesc.Value:X16} " +
                    $"attrs={bsTri.VertexDesc.VertexAttributes} " +
                    $"fullPrecision={bsTri.IsFullPrecision} " +
                    $"skinned={bsTri.IsSkinned}");
                if (bsTri.HasShaderProperty)
                    PrintShader(nif, bsTri.ShaderPropertyRef.Index, indent + "    ");
                if (bsTri.HasAlphaProperty)
                    PrintAlpha(nif, bsTri.AlphaPropertyRef.Index, indent + "    ");
                if (bsTri.HasSkinInstance)
                {
                    Console.WriteLine(
                        $"{indent}    skinInstance[{bsTri.SkinInstanceRef.Index}] " +
                        $"{nif.Blocks[bsTri.SkinInstanceRef.Index].GetType().Name}");
                }
            }
        }

        if (block is NiAVObject avObject && avObject.CollisionObject?.Index >= 0)
        {
            Console.WriteLine(
                $"{indent}    collision[{avObject.CollisionObject.Index}] " +
                $"{nif.Blocks[avObject.CollisionObject.Index].GetType().Name}");
        }

        var children = tree.Children(index);
        for (var order = 0; order < children.Count; ++order)
        {
            Console.WriteLine(
                $"{indent}  child#{order} -> [{children[order]}] " +
                $"\"{tree.NameOf(children[order])}\"");
            PrintNode(nif, tree, children[order], depth + 1);
        }
    }

    private static string DescribeExtraData(INiObject extra) => extra switch
    {
        NiStringExtraData stringExtra =>
            $"value=\"{BlockReflection.GetField(stringExtra, "_stringData")}\"",
        NiIntegerExtraData integerExtra =>
            $"value={BlockReflection.GetField(integerExtra, "_integerData")}",
        BSInvMarker => "inventory marker",
        _ => string.Empty,
    };

    private static void PrintShader(NifFile nif, int index, string indent)
    {
        if (index < 0 || index >= nif.Blocks.Count)
            return;
        var shader = nif.Blocks[index];
        Console.WriteLine(
            $"{indent}shader[{index}] {shader.GetType().Name} " +
            $"name=\"{(shader as INiNamed)?.Name?.String}\"");

        if (shader is BSEffectShaderProperty effect)
        {
            Console.WriteLine(
                $"{indent}  sourceTexture=\"" +
                $"{BlockReflection.GetNiString4(effect, "_sourceTexture")}\"");
            Console.WriteLine(
                $"{indent}  greyscaleTexture=\"" +
                $"{BlockReflection.GetNiString4(effect, "_greyscaleTexture")}\"");
            Console.WriteLine(
                $"{indent}  envMapTexture=\"" +
                $"{BlockReflection.GetNiString4(effect, "_envMapTexture")}\"");
            Console.WriteLine(
                $"{indent}  normalTexture=\"" +
                $"{BlockReflection.GetNiString4(effect, "_normalTexture")}\"");
            Console.WriteLine(
                $"{indent}  envMaskTexture=\"" +
                $"{BlockReflection.GetNiString4(effect, "_envMaskTexture")}\"");
            Console.WriteLine(
                $"{indent}  flags1=0x{Convert.ToUInt32(effect.ShaderFlags_F4SPF1):X8} " +
                $"flags2=0x{Convert.ToUInt32(effect.ShaderFlags_F4SPF2):X8}");
            var baseColor = BlockReflection.GetField(effect, "_baseColor");
            var colorText = baseColor is NiflySharp.Structs.Color4 rgba
                ? $"({rgba.R:F3},{rgba.G:F3},{rgba.B:F3},{rgba.A:F3})"
                : baseColor?.ToString();
            Console.WriteLine(
                $"{indent}  baseColor={colorText} " +
                $"baseColorScale={BlockReflection.GetField(effect, "_baseColorScale")} " +
                $"lightingInfluence={BlockReflection.GetField(effect, "_lightingInfluence")} " +
                $"textureClampMode={BlockReflection.GetField(effect, "_textureClampMode")} " +
                $"softFalloffDepth={BlockReflection.GetField(effect, "_softFalloffDepth")} " +
                $"falloffStartAngle={BlockReflection.GetField(effect, "_falloffStartAngle")} " +
                $"falloffStopAngle={BlockReflection.GetField(effect, "_falloffStopAngle")} " +
                $"falloffStartOpacity={BlockReflection.GetField(effect, "_falloffStartOpacity")} " +
                $"falloffStopOpacity={BlockReflection.GetField(effect, "_falloffStopOpacity")}");
        }
        else if (shader is BSLightingShaderProperty lighting)
        {
            Console.WriteLine(
                $"{indent}  shaderType={lighting.ShaderType_SK_FO4} " +
                $"flags1=0x{Convert.ToUInt32(lighting.ShaderFlags_F4SPF1):X8} " +
                $"flags2=0x{Convert.ToUInt32(lighting.ShaderFlags_F4SPF2):X8}");
            var textureSetRef = BlockReflection.GetField(lighting, "_textureSet");
            Console.WriteLine($"{indent}  textureSetRef={textureSetRef}");
        }
    }

    private static void PrintAlpha(NifFile nif, int index, string indent)
    {
        if (index < 0 || index >= nif.Blocks.Count)
            return;
        if (nif.Blocks[index] is not NiAlphaProperty alpha)
            return;
        // AlphaFlags is a bitfield struct; its default ToString is the type
        // name, which is how the alpha flags -- the difference between a
        // blended reticle and an opaque square -- went unprinted for a while.
        var rawFlags = alpha.Flags.GetType()
            .GetProperty("Value")?.GetValue(alpha.Flags) ?? alpha.Flags;
        Console.WriteLine(
            $"{indent}alpha[{index}] flags=0x{Convert.ToUInt32(rawFlags):X4} " +
            $"threshold={alpha.Threshold}");
    }

    private static void PrintControllers(NifFile nif, NifTree tree)
    {
        for (var index = 0; index < nif.Blocks.Count; ++index)
        {
            switch (nif.Blocks[index])
            {
                case NiControllerManager manager:
                    Console.WriteLine(
                        $"[{index}] NiControllerManager " +
                        $"target={manager.Target?.Index} " +
                        $"flags={manager.Flags} " +
                        $"frequency={manager.Frequency} " +
                        $"start={manager.StartTime} stop={manager.StopTime} " +
                        $"cumulative={manager.Cumulative} " +
                        $"objectPalette={manager.ObjectPalette?.Index} " +
                        $"sequences=[{string.Join(",", manager.ControllerSequences.Indices)}]");
                    break;

                case NiControllerSequence sequence:
                    Console.WriteLine(
                        $"[{index}] NiControllerSequence \"{sequence.Name?.String}\" " +
                        $"accumRoot=\"{sequence.AccumRootName?.String}\" " +
                        $"cycle={sequence.CycleType} " +
                        $"frequency={sequence.Frequency} " +
                        $"start={sequence.StartTime} stop={sequence.StopTime} " +
                        $"weight={sequence.Weight} " +
                        $"manager={sequence.Manager?.Index} " +
                        $"textKeys={sequence.TextKeys?.Index} " +
                        $"blocks={sequence.ControlledBlocks.Count}");
                    foreach (var controlled in sequence.ControlledBlocks)
                    {
                        Console.WriteLine(
                            $"      node=\"{controlled.NodeName?.String}\" " +
                            $"propertyType=\"{controlled.PropertyType?.String}\" " +
                            $"controllerType=\"{controlled.ControllerType?.String}\" " +
                            $"controllerID=\"{controlled.ControllerID?.String}\" " +
                            $"interpolatorID=\"{controlled.InterpolatorID?.String}\" " +
                            $"interp={controlled.Interpolator?.Index} " +
                            $"controller={controlled.Controller?.Index} " +
                            $"priority={controlled.Priority}");
                        DescribeInterpolator(nif, controlled.Interpolator?.Index ?? -1);
                    }

                    break;

                case NiDefaultAVObjectPalette palette:
                    Console.WriteLine(
                        $"[{index}] NiDefaultAVObjectPalette scene={palette.Scene?.Index} " +
                        $"objs={palette.Objs.Count}");
                    foreach (var entry in palette.Objs)
                    {
                        Console.WriteLine(
                            $"      \"{entry.Name?.Content}\" -> " +
                            $"[{entry.AV_Object?.Index}] " +
                            $"\"{NifTree.NameOf(nif, entry.AV_Object?.Index ?? -1)}\"");
                    }

                    break;

                case NiTextKeyExtraData textKeys:
                    Console.WriteLine($"[{index}] NiTextKeyExtraData");
                    if (BlockReflection.GetField(textKeys, "_textKeys") is
                        System.Collections.IEnumerable keys)
                    {
                        foreach (var key in keys)
                        {
                            var time = key?.GetType()
                                .GetField("Time")?.GetValue(key);
                            var value = key?.GetType()
                                .GetField("Value")?.GetValue(key);
                            var text = value?.GetType()
                                .GetProperty("String")?.GetValue(value);
                            Console.WriteLine($"      t={time} \"{text}\"");
                        }
                    }

                    break;

                case NiMultiTargetTransformController multi:
                    Console.WriteLine(
                        $"[{index}] NiMultiTargetTransformController " +
                        $"target={multi.Target?.Index} " +
                        $"extraTargets=[{string.Join(",", multi.ExtraTargets.Indices)}]");
                    break;

                case NiVisController visibility:
                    Console.WriteLine(
                        $"[{index}] NiVisController target={visibility.Target?.Index} " +
                        $"\"{NifTree.NameOf(nif, visibility.Target?.Index ?? -1)}\" " +
                        $"flags={visibility.Flags}");
                    break;
            }
        }
    }

    private static void DescribeInterpolator(NifFile nif, int index)
    {
        if (index < 0 || index >= nif.Blocks.Count)
            return;
        var block = nif.Blocks[index];
        switch (block)
        {
            case NiBoolInterpolator boolInterp:
            {
                var dataIndex = boolInterp.Data?.Index ?? -1;
                Console.WriteLine(
                    $"        interp[{index}] NiBoolInterpolator " +
                    $"value={BlockReflection.GetField(boolInterp, "_value")} " +
                    $"data={dataIndex}");
                if (dataIndex >= 0 && nif.Blocks[dataIndex] is NiBoolData boolData)
                {
                    var group = BlockReflection.GetField(boolData, "_data");
                    var keys = group?.GetType()
                        .GetProperty("Keys")?.GetValue(group)
                        ?? group?.GetType().GetField("Keys")?.GetValue(group);
                    var interpolation = group?.GetType()
                        .GetProperty("Interpolation")?.GetValue(group)
                        ?? group?.GetType().GetField("Interpolation")?.GetValue(group);
                    Console.WriteLine(
                        $"        boolData[{dataIndex}] interpolation={interpolation}");
                    if (keys is System.Collections.IEnumerable keyList)
                    {
                        foreach (var key in keyList)
                        {
                            var time = key?.GetType().GetField("Time")?.GetValue(key);
                            var value = key?.GetType().GetField("Value")?.GetValue(key);
                            Console.WriteLine($"          t={time} value={value}");
                        }
                    }
                }

                break;
            }

            case NiFloatInterpolator floatInterp:
                Console.WriteLine(
                    $"        interp[{index}] NiFloatInterpolator " +
                    $"value={BlockReflection.GetField(floatInterp, "_value")} " +
                    $"data={floatInterp.Data?.Index}");
                break;

            case NiTransformInterpolator transformInterp:
                Console.WriteLine(
                    $"        interp[{index}] NiTransformInterpolator " +
                    $"data={transformInterp.Data?.Index}");
                break;

            default:
                Console.WriteLine(
                    $"        interp[{index}] {block.GetType().Name}");
                break;
        }
    }

    private static void PrintGeometryDetail(NifFile nif, NifTree tree, int index)
    {
        if (nif.Blocks[index] is not BSTriShape shape)
            return;

        var name = tree.NameOf(index);
        Console.WriteLine($"[{index}] \"{name}\" parent=\"{tree.NameOf(tree.Parent(index))}\"");
        Console.WriteLine(
            $"    verts={shape.VertexCount} tris={shape.TriangleCount} " +
            $"indices={shape.TriangleCount * 3} " +
            $"vertexSize={shape.VertexSize} dataSize={shape.DataSize}");

        var positions = shape.VertexPositions;
        if (positions.Count == 0)
            return;

        var minimum = positions[0];
        var maximum = positions[0];
        foreach (var position in positions)
        {
            minimum = Vector3.Min(minimum, position);
            maximum = Vector3.Max(maximum, position);
        }

        Console.WriteLine(
            $"    localMin=({minimum.X:F5},{minimum.Y:F5},{minimum.Z:F5}) " +
            $"localMax=({maximum.X:F5},{maximum.Y:F5},{maximum.Z:F5}) " +
            $"extent=({maximum.X - minimum.X:F5}," +
            $"{maximum.Y - minimum.Y:F5},{maximum.Z - minimum.Z:F5})");

        var planeFit = PlaneFit.Fit(positions);
        Console.WriteLine(
            $"    planeFit normal=({planeFit.Normal.X:F5}," +
            $"{planeFit.Normal.Y:F5},{planeFit.Normal.Z:F5}) " +
            $"centroid=({planeFit.Centroid.X:F5},{planeFit.Centroid.Y:F5}," +
            $"{planeFit.Centroid.Z:F5}) " +
            $"maxDeviation={planeFit.MaxDeviation:F6} " +
            $"thicknessRatio={planeFit.ThicknessRatio:F6}");

        // Radial histogram: an annulus shows exactly two distinct radii.
        var radii = new List<float>();
        foreach (var position in positions)
        {
            var offset = position - planeFit.Centroid;
            var alongNormal = Vector3.Dot(offset, planeFit.Normal);
            radii.Add((offset - alongNormal * planeFit.Normal).Length());
        }

        radii.Sort();
        var buckets = new List<(float Radius, int Count)>();
        foreach (var radius in radii)
        {
            if (buckets.Count > 0 &&
                MathF.Abs(buckets[^1].Radius - radius) < 1e-3f)
            {
                buckets[^1] = (buckets[^1].Radius, buckets[^1].Count + 1);
            }
            else
            {
                buckets.Add((radius, 1));
            }
        }

        Console.WriteLine(
            $"    radius buckets ({buckets.Count}): " +
            string.Join(", ", buckets.Take(12)
                .Select(bucket => $"{bucket.Radius:F5}x{bucket.Count}")));
        if (buckets.Count == 2 && buckets[0].Radius > 1e-6f)
        {
            Console.WriteLine(
                $"    ANNULUS inner={buckets[0].Radius:F5} " +
                $"outer={buckets[1].Radius:F5} " +
                $"ratio={buckets[0].Radius / buckets[1].Radius:F6} " +
                $"segments={buckets[0].Count}");
        }

        if (shape.VertexColors.Count > 0)
        {
            var distinct = shape.VertexColors
                .Select(color => color.ToString())
                .Distinct()
                .Take(6)
                .ToList();
            Console.WriteLine(
                $"    vertexColors distinct(first 6)={string.Join(" | ", distinct)} " +
                $"total={shape.VertexColors.Count}");
        }

        if (shape.UVs.Count > 0)
        {
            Console.WriteLine(
                $"    uv[0]={shape.UVs[0]} uv[1]={shape.UVs[Math.Min(1, shape.UVs.Count - 1)]}");
        }

        if (shape.Normals.Count > 0)
            Console.WriteLine($"    normal[0]={shape.Normals[0]}");

        if (shape.Triangles.Count > 0)
        {
            var first = shape.Triangles.Take(6)
                .Select(triangle => $"({triangle.V1},{triangle.V2},{triangle.V3})");
            Console.WriteLine($"    firstTris={string.Join(" ", first)}");

            // Winding measured against the fitted plane normal.
            var positive = 0;
            var negative = 0;
            foreach (var triangle in shape.Triangles)
            {
                var a = positions[triangle.V1];
                var b = positions[triangle.V2];
                var c = positions[triangle.V3];
                var cross = Vector3.Cross(b - a, c - a);
                if (Vector3.Dot(cross, planeFit.Normal) > 0)
                    ++positive;
                else
                    ++negative;
            }

            Console.WriteLine(
                $"    winding vs planeNormal: positive={positive} negative={negative}");
        }
    }
}

/// <summary>Least-squares plane fit used to characterise flat scope geometry.</summary>
public readonly struct PlaneFit
{
    public PlaneFit(Vector3 centroid, Vector3 normal, float maxDeviation,
        float thicknessRatio, float maxRadius)
    {
        Centroid = centroid;
        Normal = normal;
        MaxDeviation = maxDeviation;
        ThicknessRatio = thicknessRatio;
        MaxRadius = maxRadius;
    }

    public Vector3 Centroid { get; }

    /// <summary>Unit normal of the best-fit plane.</summary>
    public Vector3 Normal { get; }

    /// <summary>Largest absolute distance of any vertex from the plane.</summary>
    public float MaxDeviation { get; }

    /// <summary>
    /// <see cref="MaxDeviation"/> divided by <see cref="MaxRadius"/>. Small
    /// values mean the mesh really is a flat disc rather than a lens body.
    /// </summary>
    public float ThicknessRatio { get; }

    /// <summary>Largest in-plane distance from the centroid.</summary>
    public float MaxRadius { get; }

    /// <summary>
    /// Fits a plane by taking the eigenvector of the covariance matrix with the
    /// smallest eigenvalue, found by inverse power iteration on the covariance
    /// of the vertex cloud.
    /// </summary>
    public static PlaneFit Fit(IReadOnlyList<Vector3> points)
    {
        if (points.Count == 0)
            return new PlaneFit(Vector3.Zero, Vector3.UnitZ, 0, 0, 0);

        var centroid = Vector3.Zero;
        foreach (var point in points)
            centroid += point;
        centroid /= points.Count;

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

        // Direct determinant method: pick the axis with the largest cofactor
        // determinant, which is numerically the most stable of the three.
        var detX = yy * zz - yz * yz;
        var detY = xx * zz - xz * xz;
        var detZ = xx * yy - xy * xy;
        var maxDet = Math.Max(detX, Math.Max(detY, detZ));

        Vector3 normal;
        if (maxDet <= 0)
        {
            normal = Vector3.UnitZ;
        }
        else if (maxDet == detX)
        {
            normal = new Vector3(
                1.0f,
                (float)((xz * yz - xy * zz) / detX),
                (float)((xy * yz - xz * yy) / detX));
        }
        else if (maxDet == detY)
        {
            normal = new Vector3(
                (float)((yz * xz - xy * zz) / detY),
                1.0f,
                (float)((xy * xz - yz * xx) / detY));
        }
        else
        {
            normal = new Vector3(
                (float)((yz * xy - xz * yy) / detZ),
                (float)((xz * xy - yz * xx) / detZ),
                1.0f);
        }

        normal = Vector3.Normalize(normal);

        var maxDeviation = 0.0f;
        var maxRadius = 0.0f;
        foreach (var point in points)
        {
            var offset = point - centroid;
            var alongNormal = Vector3.Dot(offset, normal);
            maxDeviation = MathF.Max(maxDeviation, MathF.Abs(alongNormal));
            maxRadius = MathF.Max(
                maxRadius, (offset - alongNormal * normal).Length());
        }

        var ratio = maxRadius > 1e-6f ? maxDeviation / maxRadius : 0.0f;
        return new PlaneFit(centroid, normal, maxDeviation, ratio, maxRadius);
    }
}
