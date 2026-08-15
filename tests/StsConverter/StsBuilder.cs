using System.Numerics;
using NiflySharp;
using NiflySharp.Blocks;
using NiflySharp.Interfaces;
using NiflySharp.Structs;

namespace StsConverter;

public sealed class ConvertOptions
{
    public required string InputPath { get; init; }
    public required string OutputPath { get; init; }
    public required string GlassSelector { get; init; }
    public required string ReticleSelector { get; init; }
    public string? DotSelector { get; init; }
    public string? FadeFromSelector { get; init; }

    public string ScopeNormalName { get; init; } = "ScopeNormal";
    public string ScopeAimingName { get; init; } = "ScopeAiming";
    public string ScopeViewPartsName { get; init; } = "ScopeViewParts";
    public string ReticleHolderName { get; init; } = "Adjustments";
    public string FadeName { get; init; } = "ScopeFade:0";
    public string ReticleName { get; init; } = "Reticle:0";
    public string DotName { get; init; } = "Dot:0";
    public string TextureLoaderName { get; init; } = "TextureLoader:0";
    public string GlassName { get; init; } = "Glass:0";

    /// <summary>Suffix appended to the cloned hip-model shapes.</summary>
    public string NormalSuffix { get; init; } = "_full";

    public bool RenameGlass { get; init; }
    public bool RenameFade { get; init; }
    public bool NoScopeFade { get; init; }
    public bool NoDuplicate { get; init; }
    public bool FlatViewParts { get; init; }
    public bool KeepReticleMaterial { get; init; }
    public bool NoTextureLoader { get; init; }

    /// <summary>
    /// Reticle texture to write into Reticle:0 and TextureLoader:0, enabling
    /// the STS custom-material fallback.
    ///
    /// Supplying this is what switches the fallback on, and it has to be
    /// supplied by hand because the converter cannot derive it. In a non-STS
    /// scope the reticle's real texture is named inside its .BGEM material,
    /// which lives in a BA2 this tool does not read; the Source Texture field
    /// in the NIF is unused by the engine there and is routinely stale. One
    /// test mesh carried an MK18 material path and an RU556 texture path in a
    /// single M4A1 shader property. Repointing the material at the
    /// non-existent ReticleCrossCustom promotes that dead field to
    /// authoritative, and the reticle renders as a flat emissive quad.
    /// </summary>
    public string? ReticleTexture { get; init; }

    /// <summary>Dot texture, written to Dot:0 and TextureLoader:0's normal slot.</summary>
    public string? DotTexture { get; init; }

    /// <summary>
    /// An STS shipped reticle set from <c>Materials\Scope\Defaults\</c>, by
    /// stem (e.g. "HuntingRifle4x"). This is the documentation's "easiest way"
    /// and the route to prefer: the materials exist, so nothing rests on the
    /// missing-material fallback, and reticle customisation still works.
    /// Takes precedence over <see cref="ReticleTexture"/>.
    /// </summary>
    public string? ReticlePreset { get; init; }

    /// <summary>
    /// Folder holding the mesh's material files. When set, the reticle's and
    /// dot's real textures are read out of their .BGEM materials and retained,
    /// which is the only reliable way to keep a converted scope's own reticle:
    /// the material is where the engine actually gets the texture from.
    /// May differ per NIF, so it is a per-conversion option rather than global.
    /// </summary>
    public string? MaterialsRoot { get; init; }
    public bool Force { get; init; }

    public int Segments { get; init; } = ScopeFadeGeometry.CanonicalSegments;
    public float FadeScale { get; init; } = 0.94f;
    public float? FadeRadius { get; init; }
    public float SlabFraction { get; init; } = ApertureMeasurement.DefaultSlabFraction;
    public float Tolerance { get; init; } = 1e-4f;

    public List<string> HideWhenAiming { get; init; } = new();
}

public sealed record ConvertReport(
    Aperture? Aperture,
    float FadeWorldRadius,
    Transform FadeWorldTransform,
    Dictionary<string, string> Renames,
    HashSet<string> AddedShapeNames,
    HashSet<string> AimingOnlyShapeNames,
    List<string> Notes);

/// <summary>
/// Rewrites a scope NIF into the STS node layout derived from the reference
/// corpus, keeping every mesh at exactly its original world transform.
/// </summary>
public static class StsBuilder
{
    /// <summary>
    /// Local translation the reference scopes give <c>ScopeViewParts</c>. Five
    /// of the six carry this exact value with the reticle holder underneath
    /// carrying its exact negation, so the pair cancels out. It is reproduced
    /// to stay close to files that are known to work in game, and it also
    /// exercises the reparenting maths, since it means the new intermediate
    /// nodes are not identity.
    /// </summary>
    public static readonly Vector3 ViewPartsTemplateOffset =
        new(0.004677f, -18.882536f, 1.844049f);

    public static ConvertReport Convert(NifFile nif, ConvertOptions options)
    {
        var notes = new List<string>();
        var tree = new NifTree(nif);
        var rootIndex = tree.RootIndex;
        if (rootIndex < 0)
            throw new StsConversionException("Input has no root NiNode.");

        var root = (NiNode)nif.Blocks[rootIndex];
        var rootName = tree.NameOf(rootIndex);

        GuardAlreadyConverted(tree, options, notes);

        var glassIndex = Selection.Resolve(tree, options.GlassSelector, "glass");
        var reticleIndex = Selection.Resolve(tree, options.ReticleSelector, "reticle");
        var dotIndex = options.DotSelector is null
            ? -1
            : Selection.Resolve(tree, options.DotSelector, "dot");
        var fadeSourceIndex = options.FadeFromSelector is null
            ? glassIndex
            : Selection.Resolve(tree, options.FadeFromSelector, "fade source");

        if (reticleIndex == glassIndex)
        {
            throw new StsConversionException(
                "The glass and reticle selections resolve to the same shape " +
                $"('{tree.NameOf(glassIndex)}').");
        }

        // Snapshot every transform-bearing block's world transform before any
        // restructuring, so new local transforms can be solved for exactly.
        var worldBefore = new Dictionary<int, Transform>();
        tree.Walk(rootIndex, index =>
        {
            if (nif.Blocks[index] is NiAVObject)
                worldBefore[index] = tree.WorldTransform(index);
        });

        var originalRootChildren = tree.Children(rootIndex).ToList();
        var originalChildLists = new Dictionary<int, List<int>>();
        tree.Walk(rootIndex, index =>
        {
            if (nif.Blocks[index] is NiNode)
                originalChildLists[index] = tree.Children(index).ToList();
        });

        var renames = new Dictionary<string, string>(StringComparer.Ordinal);
        var added = new HashSet<string>(StringComparer.Ordinal);
        var fixedLocal = new Dictionary<int, Transform>();
        var desiredWorld = new Dictionary<int, Transform>(worldBefore);

        // --- hip model: clone the original tree ---------------------------
        // A subtree named by --hide-when-aiming belongs to the hip model only,
        // so its original is handed straight to ScopeNormal instead of being
        // cloned. That keeps its name and leaves no orphaned blocks behind.
        // The reticle and the dot are aiming-only. They are illuminated marks
        // projected inside the optic, not parts of the scope body, and the
        // reference STS scopes carry them solely under ScopeAiming (inside
        // ScopeViewParts\Adjustments). Cloning them into the hip branch leaves
        // a lit reticle floating on a lowered weapon.
        var aimingOnly = new HashSet<int> { reticleIndex };
        if (dotIndex >= 0)
            aimingOnly.Add(dotIndex);
        var aimingOnlyNames = aimingOnly
            .Select(tree.NameOf)
            .Where(name => !string.IsNullOrEmpty(name))
            .ToHashSet(StringComparer.Ordinal);

        var normalChildren = new List<int>();
        foreach (var child in originalRootChildren)
        {
            if (ShouldHide(tree, child, options))
            {
                normalChildren.Add(child);
                continue;
            }

            if (options.NoDuplicate)
                continue;

            var clone = CloneSubtree(
                nif, tree, child, options.NormalSuffix,
                worldBefore, desiredWorld, added, notes, aimingOnly);
            if (clone >= 0)
                normalChildren.Add(clone);
        }

        if (aimingOnlyNames.Count > 0 && !options.NoDuplicate)
        {
            notes.Add(
                "Kept out of the hip model (ScopeNormal), because a reticle is " +
                "only visible through the optic: " +
                string.Join(", ", aimingOnlyNames.Order(StringComparer.Ordinal)));
        }

        // Restore every original node's child list; CloneShape attaches its
        // result to the source shape's parent, which would otherwise leave
        // clones dangling inside the aiming branch.
        foreach (var entry in originalChildLists)
        {
            var node = (NiNode)nif.Blocks[entry.Key];
            SetChildren(node, entry.Value);
        }

        if (options.HideWhenAiming.Count > 0)
        {
            notes.Add(
                "--hide-when-aiming: these subtrees were placed under " +
                "ScopeNormal only, not duplicated into ScopeAiming: " +
                string.Join(", ", options.HideWhenAiming));
        }

        if (options.NoDuplicate)
        {
            notes.Add(
                "--no-duplicate: ScopeNormal holds only the --hide-when-aiming " +
                "subtrees. Without a hip model the scope will be invisible when " +
                "not aiming, so finish it by hand.");
        }

        // --- scaffold nodes ------------------------------------------------
        var scopeNormalIndex = AddNode(nif, options.ScopeNormalName, 0x0000000E);
        var scopeAimingIndex = AddNode(nif, options.ScopeAimingName, 0x0000000F);
        var viewPartsIndex = AddNode(nif, options.ScopeViewPartsName, 0x0000000E);
        var holderIndex = AddNode(nif, options.ReticleHolderName, 0x0008000E);

        fixedLocal[scopeNormalIndex] = Transform.Identity;
        fixedLocal[scopeAimingIndex] = Transform.Identity;

        var viewPartsLocal = options.FlatViewParts
            ? Transform.Identity
            : new Transform(ViewPartsTemplateOffset, Matrix4x4.Identity, 1.0f);
        fixedLocal[viewPartsIndex] = viewPartsLocal;
        fixedLocal[holderIndex] = viewPartsLocal.Inverse();

        // --- aiming model: the originals -----------------------------------
        var aimingChildren = originalRootChildren
            .Where(child => !ShouldHide(tree, child, options))
            .ToList();

        // --- ScopeFade -----------------------------------------------------
        Aperture? aperture = null;
        var fadeWorld = Transform.Identity;
        var fadeWorldRadius = 0.0f;
        var viewPartsChildren = new List<int>();

        if (options.RenameFade)
        {
            Rename(nif, glassIndex, options.FadeName, renames, tree);
            Detach(nif, tree, aimingChildren, glassIndex);
            viewPartsChildren.Add(glassIndex);
            notes.Add(
                "--rename-fade: the chosen glass mesh was renamed to " +
                $"'{options.FadeName}'. MagnaScope's exact geometry-replay path " +
                "expects a 24-segment 48-vertex annulus and will fall back to " +
                "its synthesised-aperture path for arbitrary lens geometry.");
        }
        else if (!options.NoScopeFade)
        {
            if (nif.Blocks[fadeSourceIndex] is not BSTriShape fadeSource)
            {
                throw new StsConversionException(
                    $"Shape '{tree.NameOf(fadeSourceIndex)}' is a " +
                    $"{tree.TypeOf(fadeSourceIndex)}, not a BSTriShape, so its " +
                    "aperture cannot be measured.");
            }

            aperture = ApertureMeasurement.Measure(
                fadeSource, worldBefore[fadeSourceIndex], options.SlabFraction);
            fadeWorldRadius = options.FadeRadius
                ?? aperture.Radius * options.FadeScale;

            var fadeIndex = ScopeFadeGeometry.Create(
                nif, options.FadeName, options.Segments);
            fadeWorld = new Transform(
                aperture.Centre,
                ApertureMeasurement.OrientationFor(aperture.RearwardNormal),
                fadeWorldRadius / ScopeFadeGeometry.CanonicalOuterRadius);
            desiredWorld[fadeIndex] = fadeWorld;
            added.Add(options.FadeName);
            viewPartsChildren.Add(fadeIndex);
            notes.Add(
                $"Generated {options.FadeName}: {options.Segments} segments, " +
                $"{options.Segments * 2} vertices, {options.Segments * 2} triangles, " +
                $"{options.Segments * 6} indices, inner:outer ratio " +
                $"{ScopeFadeGeometry.CanonicalRadiusRatio:F4}.");
        }

        // --- reticle subtree ------------------------------------------------
        var holderChildren = new List<int>();

        if (!options.NoTextureLoader)
        {
            var loaderIndex = CreateTextureLoader(
                nif, tree, reticleIndex, dotIndex, options, notes);
            if (loaderIndex >= 0)
            {
                // CloneShape parents its result to the source shape's parent,
                // so the loader has to be pulled back out before it is placed
                // under the reticle holder; otherwise it ends up listed twice.
                Detach(nif, tree, aimingChildren, loaderIndex);
                fixedLocal[loaderIndex] =
                    new Transform(Vector3.Zero, Matrix4x4.Identity, 0.0f);
                added.Add(options.TextureLoaderName);
                holderChildren.Add(loaderIndex);
            }
        }

        Detach(nif, tree, aimingChildren, reticleIndex);
        Rename(nif, reticleIndex, options.ReticleName, renames, tree);

        // Every reference STS reticle and dot carries node flags 0xE. Source
        // meshes commonly arrive with 0x8000E, and that extra bit is the only
        // remaining difference between a reticle that renders in game and one
        // that comes out as a flat quad. Normalise to what the working corpus
        // actually uses.
        if (nif.Blocks[reticleIndex] is BSTriShape reticleShapeFlags)
        {
            reticleShapeFlags.Flags_ui = 0x0000000E;
        }
        if (dotIndex >= 0 && nif.Blocks[dotIndex] is BSTriShape dotShapeFlags)
        {
            dotShapeFlags.Flags_ui = 0x0000000E;
        }
        holderChildren.Add(reticleIndex);

        if (dotIndex >= 0)
        {
            Detach(nif, tree, aimingChildren, dotIndex);
            Rename(nif, dotIndex, options.DotName, renames, tree);
            holderChildren.Add(dotIndex);
        }
        else
        {
            notes.Add(
                $"No --dot given, so no '{options.DotName}' was created. The " +
                "reference STS scopes all have one; STS's dot reticle swap has " +
                "nothing to act on without it.");
        }

        if (options.RenameGlass && !options.RenameFade)
            Rename(nif, glassIndex, options.GlassName, renames, tree);

        // The reticle holder is always the LAST child of ScopeViewParts, which
        // is what the reference scopes do and what the STS documentation asks
        // for when it says the reticle node must come last.
        viewPartsChildren.Add(holderIndex);

        // --- wire the tree together ------------------------------------------
        SetChildren((NiNode)nif.Blocks[holderIndex], holderChildren);
        SetChildren((NiNode)nif.Blocks[viewPartsIndex], viewPartsChildren);
        SetChildren((NiNode)nif.Blocks[scopeNormalIndex], normalChildren);

        var aimingFinal = new List<int> { viewPartsIndex };
        aimingFinal.AddRange(aimingChildren);
        SetChildren((NiNode)nif.Blocks[scopeAimingIndex], aimingFinal);

        SetChildren(root, new List<int> { scopeNormalIndex, scopeAimingIndex });

        ScopeAnimation.Build(
            nif, rootIndex, rootName,
            scopeNormalIndex, options.ScopeNormalName,
            scopeAimingIndex, options.ScopeAimingName);

        // --- solve local transforms top-down ---------------------------------
        var newTree = new NifTree(nif);
        SolveLocals(nif, newTree, rootIndex,
            Transform.FromObject(root), fixedLocal, desiredWorld);

        // NifSkope's Spells > Sanitize > Reorder Blocks, which the STS setup
        // documentation ends on and which is not cosmetic.
        //
        // Getting TextureLoader:0 to child#0 of the reticle holder is only half
        // the requirement: the docs are explicit that after reordering it must
        // also hold "the lowest number of all the items in that list". Without
        // this pass the loader was child#0 but block 67 while Reticle:0 was
        // block 19 -- correct in the child array, wrong in the block table, and
        // the block table is what NifSkope displays and what the engine reads in
        // order. The whole point of the loader is to make its texture resident
        // BEFORE the reticle resolves its missing material, so a loader that
        // comes later in the file cannot do its job.
        //
        // Must run last: it renumbers blocks, invalidating the indices the
        // transform solve above works in.
        //
        // PrettySortBlocks is nifly's version of the same spell, but it bails
        // out entirely when the file has unknown block types -- and every scope
        // in the test corpus has BSConnectPoint::Children, which NiflySharp
        // keeps as opaque bytes. It was therefore a silent no-op here. So this
        // does the one move the documentation actually requires rather than a
        // full sort: the loader ahead of the reticle, everything else left
        // exactly where it was. A minimal permutation is also the safest thing
        // to do around blocks nifly cannot see inside.
        MoveTextureLoaderBeforeReticle(nif, options, notes);

        return new ConvertReport(
            aperture, fadeWorldRadius, fadeWorld, renames, added,
            aimingOnlyNames, notes);
    }

    /// <summary>
    /// Renumbers <c>TextureLoader:0</c> to sit immediately before the reticle
    /// in the block table.
    ///
    /// The STS documentation ends on Spells &gt; Sanitize &gt; Reorder Blocks and
    /// is explicit that afterwards the loader must hold "the lowest number of
    /// all the items in that list". That is not cosmetic: the loader exists to
    /// make its texture resident before the reticle resolves its deliberately
    /// missing material, and a loader that appears later in the file cannot do
    /// that.
    ///
    /// Getting the loader to child#0 of the holder is only half of it; the
    /// child array and the block table are separate orderings, and NifSkope
    /// shows the latter.
    /// </summary>
    private static void MoveTextureLoaderBeforeReticle(
        NifFile nif,
        ConvertOptions options,
        List<string> notes)
    {
        var tree = new NifTree(nif);
        var loaderIndex = tree.FindByName(options.TextureLoaderName);
        var reticleIndex = tree.FindByName(options.ReticleName);
        if (loaderIndex < 0 || reticleIndex < 0 || loaderIndex < reticleIndex)
        {
            return;
        }

        var order = new List<int>(nif.Blocks.Count);
        for (var index = 0; index < nif.Blocks.Count; ++index)
        {
            if (index == loaderIndex)
            {
                continue;
            }

            if (index == reticleIndex)
            {
                order.Add(loaderIndex);
            }

            order.Add(index);
        }

        if (order.Count != nif.Blocks.Count)
        {
            // Never hand SetBlockOrder a permutation that is not one.
            throw new StsConversionException(
                "Internal error building the block order permutation " +
                $"({order.Count} entries for {nif.Blocks.Count} blocks).");
        }

        nif.Header.SetBlockOrder(nif.Blocks, order);
        notes.Add(
            $"Reordered blocks so {options.TextureLoaderName} precedes " +
            $"{options.ReticleName}, per the STS setup documentation's final " +
            "Sanitize step. nifly's own PrettySortBlocks refuses to run on " +
            "these files because they contain block types it cannot parse " +
            "(BSConnectPoint::Children), so this is a targeted move rather " +
            "than a full sort.");
    }

    private static bool ShouldHide(NifTree tree, int index, ConvertOptions options)
    {
        if (options.HideWhenAiming.Count == 0)
            return false;
        var name = tree.NameOf(index);
        return options.HideWhenAiming.Any(
            hidden => string.Equals(hidden, name, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>
    /// Walks the rebuilt tree from the root, giving every node the local
    /// transform that reproduces its recorded world transform under its new
    /// parent. Scaffold nodes instead keep the fixed local transform they were
    /// created with, and their world transform follows from it.
    /// </summary>
    private static void SolveLocals(
        NifFile nif,
        NifTree tree,
        int index,
        Transform world,
        IReadOnlyDictionary<int, Transform> fixedLocal,
        IReadOnlyDictionary<int, Transform> desiredWorld)
    {
        foreach (var child in tree.Children(index))
        {
            if (nif.Blocks[child] is not NiAVObject obj)
            {
                throw new StsConversionException(
                    $"Block {child} ({nif.Blocks[child].GetType().Name}) is a " +
                    "child of a NiNode but carries no transform.");
            }

            Transform local;
            if (fixedLocal.TryGetValue(child, out var pinned))
            {
                local = pinned;
            }
            else if (desiredWorld.TryGetValue(child, out var target))
            {
                if (MathF.Abs(world.Scale) < 1e-9f)
                {
                    throw new StsConversionException(
                        $"Parent of '{NifTree.NameOf(nif, child)}' has zero " +
                        "scale, so its local transform cannot be solved.");
                }

                local = Transform.Compose(world.Inverse(), target);
            }
            else
            {
                local = Transform.FromObject(obj);
            }

            local.ApplyTo(obj);
            SolveLocals(
                nif, tree, child, Transform.Compose(world, local),
                fixedLocal, desiredWorld);
        }
    }

    private static void GuardAlreadyConverted(
        NifTree tree, ConvertOptions options, List<string> notes)
    {
        var markers = new[]
        {
            options.ScopeNormalName, options.ScopeAimingName,
            options.ScopeViewPartsName, options.FadeName,
        };
        var found = markers.Where(name => tree.FindByName(name) >= 0).ToList();
        if (found.Count == 0)
            return;

        if (!options.Force)
        {
            throw new StsConversionException(
                "This NIF already contains STS nodes (" +
                string.Join(", ", found) +
                "). Converting it again would nest a second STS tree inside the " +
                "first. Aborting; pass --force if you really mean to do this.");
        }

        notes.Add(
            "--force: converted despite existing STS nodes (" +
            string.Join(", ", found) + ").");
    }

    private static int AddNode(NifFile nif, string name, uint flags)
    {
        var node = new NiNode
        {
            Name = new NiStringRef(name),
            Flags_ui = flags,
            Translation = Vector3.Zero,
            Rotation = Transform.ToMatrix33(Matrix4x4.Identity),
            Scale = 1.0f,
            Children = new NiBlockRefArray<NiAVObject>(),
        };
        return nif.AddBlock(node);
    }

    private static void SetChildren(NiNode node, List<int> children)
    {
        node.Children ??= new NiBlockRefArray<NiAVObject>();
        node.Children.SetIndices(children);
        node.NumChildren = (uint)children.Count;
    }

    /// <summary>
    /// Removes a block from every NiNode that currently lists it as a child,
    /// and from <paramref name="siblingList"/>. The live child arrays are read
    /// rather than the snapshot tree, because two Detach calls against the same
    /// parent would otherwise undo each other and leave the block attached in
    /// two places at once.
    /// </summary>
    private static void Detach(
        NifFile nif, NifTree tree, List<int> siblingList, int index)
    {
        siblingList.Remove(index);
        foreach (var block in nif.Blocks)
        {
            if (block is not NiNode node || node.Children is null)
                continue;
            var children = node.Children.Indices.ToList();
            if (!children.Remove(index))
                continue;
            SetChildren(node, children);
        }
    }

    private static void Rename(
        NifFile nif, int index, string newName,
        Dictionary<string, string> renames, NifTree tree)
    {
        var oldName = tree.NameOf(index);
        if (string.Equals(oldName, newName, StringComparison.Ordinal))
            return;
        if (nif.Blocks[index] is not INiNamed named)
            return;
        BlockReflection.SetName(named, newName);
        renames[oldName] = newName;
    }

    /// <summary>
    /// Deep-copies a subtree. Shapes go through <c>NifFile.CloneShape</c>,
    /// which duplicates the shader, texture set and alpha property; NiNodes are
    /// recreated with their flags and transforms and their children recursed.
    /// Any other block type in the subtree is a hard error rather than a silent
    /// drop.
    /// </summary>
    private static int CloneSubtree(
        NifFile nif,
        NifTree tree,
        int index,
        string suffix,
        IReadOnlyDictionary<int, Transform> worldBefore,
        Dictionary<int, Transform> desiredWorld,
        HashSet<string> added,
        List<string> notes,
        IReadOnlySet<int> aimingOnly)
    {
        // Aiming-only shapes have no hip counterpart at all.
        if (aimingOnly.Contains(index))
            return -1;

        var name = tree.NameOf(index);
        var cloneName = name + suffix;

        switch (nif.Blocks[index])
        {
            case NiNode node:
            {
                var cloneIndex = AddNode(nif, cloneName, node.Flags_ui);
                var clone = (NiNode)nif.Blocks[cloneIndex];
                Transform.FromObject(node).ApplyTo(clone);
                if (worldBefore.TryGetValue(index, out var world))
                    desiredWorld[cloneIndex] = world;
                added.Add(cloneName);

                var children = new List<int>();
                foreach (var child in tree.Children(index))
                {
                    var cloneChild = CloneSubtree(
                        nif, tree, child, suffix, worldBefore, desiredWorld,
                        added, notes, aimingOnly);
                    if (cloneChild >= 0)
                        children.Add(cloneChild);
                }

                SetChildren(clone, children);
                return cloneIndex;
            }

            case INiShape shape:
            {
                var clone = nif.CloneShape(shape, cloneName, nif);
                if (clone is null || !nif.GetBlockIndex((NiObject)clone, out var cloneIndex))
                {
                    throw new StsConversionException(
                        $"NiflySharp could not clone shape '{name}'.");
                }

                if (worldBefore.TryGetValue(index, out var world))
                    desiredWorld[cloneIndex] = world;
                added.Add(cloneName);
                return cloneIndex;
            }

            default:
                throw new StsConversionException(
                    $"Block {index} ('{name}') is a " +
                    $"{nif.Blocks[index].GetType().Name}, which this converter " +
                    "does not know how to duplicate into the hip model. Pass " +
                    "--no-duplicate to skip building ScopeNormal.");
        }
    }

    /// <summary>
    /// Builds the STS <c>TextureLoader:0</c>: a zero-scaled copy of the reticle
    /// whose BSEffectShaderProperty carries the reticle texture as Source and
    /// the dot texture as Normal, purely so the engine has them resident. That
    /// is what lets the deliberately non-existent
    /// <c>ReticleCrossCustom.BGSM.BGEM</c> material fall back to its texture.
    /// </summary>
    private static int CreateTextureLoader(
        NifFile nif,
        NifTree tree,
        int reticleIndex,
        int dotIndex,
        ConvertOptions options,
        List<string> notes)
    {
        // The preset route needs no texture, no fallback and no loader: it
        // points the reticle at a material that actually exists.
        if (!string.IsNullOrWhiteSpace(options.ReticlePreset) &&
            !options.KeepReticleMaterial)
        {
            var preset = StsReticlePresets.Normalise(
                options.ReticlePreset!, out var known);

            SetMaterialName(nif, reticleIndex, StsReticlePresets.CrossMaterial(preset));
            if (dotIndex >= 0)
                SetMaterialName(nif, dotIndex, StsReticlePresets.DotMaterial(preset));

            notes.Add(
                $"Reticle set to the shipped STS preset '{preset}' " +
                $"({StsReticlePresets.CrossMaterial(preset)}" +
                (dotIndex >= 0
                    ? $", {StsReticlePresets.DotMaterial(preset)}"
                    : ", no Dot:0") +
                "). These materials exist, so the reticle does not depend on " +
                "the missing-material fallback, and in-game reticle " +
                "customisation still works." +
                (known
                    ? string.Empty
                    : " NOTE: this preset name is not in this build's known " +
                      "list, so it may not exist in your STS install."));

            return -1;
        }

        // Preference order: what the user typed, then what the material
        // actually uses, then the NIF's own field -- which is last precisely
        // because it is the one the engine never reads.
        var reticleFromMaterial = TextureFromMaterial(
            nif, reticleIndex, options, notes, "reticle");
        var dotFromMaterial = dotIndex >= 0
            ? TextureFromMaterial(nif, dotIndex, options, notes, "dot")
            : null;

        var explicitReticleTexture =
            !string.IsNullOrWhiteSpace(options.ReticleTexture) ||
            !string.IsNullOrEmpty(reticleFromMaterial);

        var reticleTexture = !string.IsNullOrWhiteSpace(options.ReticleTexture)
            ? options.ReticleTexture!.Trim()
            : !string.IsNullOrEmpty(reticleFromMaterial)
                ? reticleFromMaterial!
                : SourceTextureOf(nif, reticleIndex);

        var dotTexture = !string.IsNullOrWhiteSpace(options.DotTexture)
            ? options.DotTexture!.Trim()
            : !string.IsNullOrEmpty(dotFromMaterial)
                ? dotFromMaterial!
                : dotIndex >= 0
                    ? SourceTextureOf(nif, dotIndex)
                    : string.Empty;

        if (explicitReticleTexture)
        {
            SetSourceTexture(nif, reticleIndex, reticleTexture);
            if (dotIndex >= 0 && !string.IsNullOrEmpty(dotTexture))
                SetSourceTexture(nif, dotIndex, dotTexture);
        }

        if (string.IsNullOrEmpty(reticleTexture))
        {
            notes.Add(
                $"'{tree.NameOf(reticleIndex)}' has no BSEffectShaderProperty " +
                "source texture, so no TextureLoader:0 was created and the " +
                "reticle keeps its own material. Pass --reticle-texture to " +
                "enable STS reticle customisation on this scope.");
            return -1;
        }

        if (nif.Blocks[reticleIndex] is not INiShape reticleShape)
            return -1;

        var loader = nif.CloneShape(
            reticleShape, options.TextureLoaderName, nif);
        if (loader is null ||
            !nif.GetBlockIndex((NiObject)loader, out var loaderIndex))
        {
            throw new StsConversionException(
                "NiflySharp could not clone the reticle to build TextureLoader:0.");
        }

        if (nif.Blocks[loaderIndex] is BSTriShape loaderShape)
        {
            loaderShape.Flags_ui = 0x0000000E;
            var shaderRef = loaderShape.ShaderPropertyRef?.Index ?? -1;
            if (shaderRef >= 0 &&
                nif.Blocks[shaderRef] is BSEffectShaderProperty loaderShader)
            {
                BlockReflection.SetName(loaderShader, string.Empty);
                BlockReflection.SetNiString4(
                    loaderShader, "_sourceTexture", reticleTexture);
                BlockReflection.SetNiString4(
                    loaderShader, "_normalTexture", dotTexture);
                BlockReflection.SetField(loaderShader, "_baseColorScale", 0.0f);
                BlockReflection.SetField(loaderShader, "_lightingInfluence", (byte)255);
                // Match the reference loaders exactly. Every working STS scope
                // carries shaderFlags2 = 0 here; the clone inherits the
                // reticle's ZBuffer_Write, and a zero-scale node that still
                // asks to write depth is not what the reference does. These
                // flags were the last unmatched field between a loader that
                // works in game and one that does not.
                // Set through the typed property, not BlockReflection: a
                // reflection setter with a wrong field name fails silently, and
                // an earlier attempt at exactly this line did nothing at all
                // while reporting success.
                loaderShader.ShaderFlags_F4SPF2 = default;
            }
            else
            {
                notes.Add(
                    "TextureLoader:0 was cloned but its shader is not a " +
                    "BSEffectShaderProperty, so the textures could not be set.");
            }
        }

        // Repointing the material at a non-existent path is what enables STS
        // reticle swapping, and it is also what destroys the reticle when the
        // fallback texture is wrong. The engine reads the real texture out of
        // the .BGEM; delete that and the NIF's own Source Texture field becomes
        // authoritative for the first time in its life. In practice that field
        // is stale in non-STS scopes, so this only happens when the user has
        // named a texture they have verified.
        if (!options.KeepReticleMaterial && explicitReticleTexture)
        {
            SetMaterialName(
                nif, reticleIndex, @"Materials\Scope\ReticleCrossCustom.BGSM.BGEM");
            if (dotIndex >= 0 && !string.IsNullOrEmpty(dotTexture))
            {
                SetMaterialName(
                    nif, dotIndex, @"Materials\Scope\ReticleDotCustom.BGSM.BGEM");
            }

            notes.Add(
                "Reticle material set to the deliberately non-existent " +
                "Materials\\Scope\\ReticleCrossCustom.BGSM.BGEM so STS reticle " +
                $"swaps apply; the reticle renders from '{reticleTexture}' via " +
                "TextureLoader:0.");
        }
        else if (!options.KeepReticleMaterial)
        {
            notes.Add(
                "Reticle keeps its original material, so it renders exactly as " +
                "it did before conversion, but STS reticle swapping is OFF. " +
                "Enabling it means pointing the material at a non-existent path " +
                "so the engine falls back to a texture -- and the texture named " +
                $"in this mesh ('{reticleTexture}') comes from a field the " +
                "engine ignores while a real material exists, so it is often " +
                "stale. Verify it, then pass --reticle-texture (and " +
                "--dot-texture) to switch the fallback on.");
        }

        return loaderIndex;
    }

    /// <summary>
    /// Reads a shape's real texture out of the material it names. Returns null
    /// when no materials folder was given; empty string when one was given but
    /// the material could not be read, which is worth a note either way.
    /// </summary>
    private static string? TextureFromMaterial(
        NifFile nif,
        int shapeIndex,
        ConvertOptions options,
        List<string> notes,
        string role)
    {
        if (string.IsNullOrWhiteSpace(options.MaterialsRoot))
            return null;

        var materialName = MaterialNameOf(nif, shapeIndex);
        if (string.IsNullOrEmpty(materialName))
        {
            notes.Add($"The {role} names no material, so no texture could be read.");
            return string.Empty;
        }

        var resolved = BgemReader.Resolve(materialName, options.MaterialsRoot!);
        if (resolved is null)
        {
            notes.Add(
                $"Could not find the {role} material '{materialName}' under " +
                $"'{options.MaterialsRoot}'. Its texture could not be read, so " +
                "the reticle keeps whatever the mesh already named.");
            return string.Empty;
        }

        if (!BgemReader.TryReadDiffuseTexture(resolved, out var texture))
        {
            notes.Add(
                $"Read '{resolved}' for the {role} but found no diffuse texture " +
                "in it.");
            return string.Empty;
        }

        var shaderPath = BgemReader.ToShaderTexturePath(texture);
        var stale = SourceTextureOf(nif, shapeIndex);
        notes.Add(
            $"Retained the {role} texture from its material: '{shaderPath}' " +
            $"(from {Path.GetFileName(resolved)})." +
            (!string.IsNullOrEmpty(stale) &&
             !string.Equals(stale, shaderPath, StringComparison.OrdinalIgnoreCase)
                ? $" The mesh's own field said '{stale}', which the engine was " +
                  "ignoring; that is the value that would have been used had " +
                  "the material not been read."
                : string.Empty));

        return shaderPath;
    }

    private static string MaterialNameOf(NifFile nif, int shapeIndex)
    {
        if (nif.Blocks[shapeIndex] is not BSTriShape shape)
            return string.Empty;
        var shaderRef = shape.ShaderPropertyRef?.Index ?? -1;
        if (shaderRef < 0)
            return string.Empty;
        return (nif.Blocks[shaderRef] as NiflySharp.Interfaces.INiNamed)
            ?.Name?.String ?? string.Empty;
    }

    private static void SetSourceTexture(NifFile nif, int shapeIndex, string texture)
    {
        if (nif.Blocks[shapeIndex] is not BSTriShape shape)
            return;
        var shaderRef = shape.ShaderPropertyRef?.Index ?? -1;
        if (shaderRef < 0 || nif.Blocks[shaderRef] is not BSEffectShaderProperty shader)
            return;
        BlockReflection.SetNiString4(shader, "_sourceTexture", texture);
    }

    private static string SourceTextureOf(NifFile nif, int shapeIndex)
    {
        if (nif.Blocks[shapeIndex] is not BSTriShape shape)
            return string.Empty;
        var shaderRef = shape.ShaderPropertyRef?.Index ?? -1;
        if (shaderRef < 0 || nif.Blocks[shaderRef] is not BSEffectShaderProperty shader)
            return string.Empty;
        return BlockReflection.GetNiString4(shader, "_sourceTexture");
    }

    private static void SetMaterialName(NifFile nif, int shapeIndex, string material)
    {
        if (nif.Blocks[shapeIndex] is not BSTriShape shape)
            return;
        var shaderRef = shape.ShaderPropertyRef?.Index ?? -1;
        if (shaderRef < 0 || nif.Blocks[shaderRef] is not INiNamed shader)
            return;
        BlockReflection.SetName(shader, material);
    }
}

/// <summary>Resolves a shape given either its name or a printed index.</summary>
public static class Selection
{
    public static int Resolve(NifTree tree, string selector, string role)
    {
        if (string.IsNullOrWhiteSpace(selector))
            throw new StsConversionException($"No {role} shape was given.");

        var text = selector.Trim();
        if (text.StartsWith('#'))
            text = text[1..];

        if (int.TryParse(text, out var index))
        {
            if (index < 0 || index >= tree.Nif.Blocks.Count)
            {
                throw new StsConversionException(
                    $"The {role} selection '#{index}' is not a block index in " +
                    $"this file (0..{tree.Nif.Blocks.Count - 1}).");
            }

            if (tree.Nif.Blocks[index] is not INiShape)
            {
                throw new StsConversionException(
                    $"The {role} selection '#{index}' is a " +
                    $"{tree.Nif.Blocks[index].GetType().Name}, not a shape.");
            }

            return index;
        }

        var byName = tree.FindByName(selector.Trim());
        if (byName < 0)
        {
            var available = string.Join(
                ", ", tree.ShapeIndices().Select(i => $"#{i} {tree.NameOf(i)}"));
            throw new StsConversionException(
                $"No shape named '{selector}' for the {role} selection. " +
                $"Available shapes: {available}");
        }

        if (tree.Nif.Blocks[byName] is not INiShape)
        {
            throw new StsConversionException(
                $"'{selector}' names a {tree.TypeOf(byName)}, not a shape.");
        }

        return byName;
    }
}
