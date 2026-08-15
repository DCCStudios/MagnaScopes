using System.Reflection;
using NiflySharp;
using NiflySharp.Blocks;
using NiflySharp.Interfaces;

namespace StsConverter;

/// <summary>
/// Scene-graph navigation built from the authored <c>NiNode.Children</c>
/// arrays rather than from <see cref="NifFile.GetParentNode"/>, because child
/// ordering is part of the STS specification and has to be preserved exactly.
/// </summary>
public sealed class NifTree
{
    private readonly Dictionary<int, int> _parentByIndex = new();
    private readonly Dictionary<int, List<int>> _childrenByIndex = new();

    public NifTree(NifFile nif)
    {
        Nif = nif;
        RootIndex = -1;

        var root = nif.GetRootNode();
        if (root is not null && nif.GetBlockIndex(root, out var rootIndex))
            RootIndex = rootIndex;

        for (var index = 0; index < nif.Blocks.Count; ++index)
        {
            if (nif.Blocks[index] is not NiNode node)
                continue;

            var children = new List<int>();
            foreach (var childIndex in node.Children.Indices)
            {
                if (childIndex < 0 || childIndex >= nif.Blocks.Count)
                    continue;
                children.Add(childIndex);
                _parentByIndex[childIndex] = index;
            }

            _childrenByIndex[index] = children;
        }
    }

    public NifFile Nif { get; }

    public int RootIndex { get; }

    public IReadOnlyList<int> Children(int index) =>
        _childrenByIndex.TryGetValue(index, out var children)
            ? children
            : Array.Empty<int>();

    public int Parent(int index) =>
        _parentByIndex.TryGetValue(index, out var parent) ? parent : -1;

    public static string NameOf(NifFile nif, int index)
    {
        if (index < 0 || index >= nif.Blocks.Count)
            return "<none>";
        return (nif.Blocks[index] as INiNamed)?.Name?.String ?? string.Empty;
    }

    public string NameOf(int index) => NameOf(Nif, index);

    public string TypeOf(int index) =>
        index < 0 || index >= Nif.Blocks.Count
            ? "<none>"
            : Nif.Blocks[index].GetType().Name;

    /// <summary>
    /// Chain of block indices from the root down to <paramref name="index"/>,
    /// inclusive at both ends. Empty if the block is not reachable from root.
    /// </summary>
    public List<int> PathFromRoot(int index)
    {
        var path = new List<int>();
        var cursor = index;
        var guard = 0;
        while (cursor >= 0 && guard++ < 4096)
        {
            path.Add(cursor);
            if (cursor == RootIndex)
                break;
            cursor = Parent(cursor);
        }

        path.Reverse();
        return path.Count > 0 && path[0] == RootIndex ? path : new List<int>();
    }

    /// <summary>
    /// World (root-relative) transform of a block, composed from every
    /// transform on the path from the root down. The root's own transform is
    /// included, so "world" here means "the space the root node sits in".
    /// </summary>
    public Transform WorldTransform(int index)
    {
        var path = PathFromRoot(index);
        if (path.Count == 0)
        {
            throw new StsConversionException(
                $"Block {index} ('{NameOf(index)}') is not reachable from the " +
                "root node, so its world transform is undefined.");
        }

        var world = Transform.Identity;
        foreach (var step in path)
        {
            if (Nif.Blocks[step] is not NiAVObject obj)
            {
                throw new StsConversionException(
                    $"Block {step} on the path to '{NameOf(index)}' is a " +
                    $"{TypeOf(step)}, which carries no transform.");
            }

            world = Transform.Compose(world, Transform.FromObject(obj));
        }

        return world;
    }

    /// <summary>All BSTriShape/NiShape blocks reachable from the root.</summary>
    public List<int> ShapeIndices()
    {
        var result = new List<int>();
        Walk(RootIndex, index =>
        {
            if (Nif.Blocks[index] is INiShape)
                result.Add(index);
        });
        return result;
    }

    public void Walk(int index, Action<int> visit)
    {
        if (index < 0 || index >= Nif.Blocks.Count)
            return;
        visit(index);
        foreach (var child in Children(index))
            Walk(child, visit);
    }

    public int FindByName(string name)
    {
        var found = -1;
        Walk(RootIndex, index =>
        {
            if (found < 0 &&
                string.Equals(NameOf(index), name, StringComparison.Ordinal))
            {
                found = index;
            }
        });
        return found;
    }
}

/// <summary>
/// Reflection accessors for NiflySharp block members that the 1.0.0 package
/// keeps private with no public property. <c>BSEffectShaderProperty</c> in
/// particular parses its texture paths correctly but exposes none of them.
/// </summary>
public static class BlockReflection
{
    private const BindingFlags InstanceMembers =
        BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;

    public static object? GetField(object block, string fieldName)
    {
        var field = FindField(block.GetType(), fieldName);
        return field?.GetValue(block);
    }

    public static void SetField(object block, string fieldName, object? value)
    {
        var field = FindField(block.GetType(), fieldName)
            ?? throw new StsConversionException(
                $"NiflySharp type {block.GetType().Name} has no field " +
                $"'{fieldName}'. The Nifly package version may have changed.");
        field.SetValue(block, value);
    }

    private static FieldInfo? FindField(Type? type, string fieldName)
    {
        while (type is not null)
        {
            var field = type.GetField(fieldName, InstanceMembers);
            if (field is not null)
                return field;
            type = type.BaseType;
        }

        return null;
    }

    /// <summary>
    /// Sets a block's name. <c>INiNamed.Name</c> is read-only on the interface
    /// even though every concrete block declares it settable, so the backing
    /// field is written directly rather than casting to a dozen block types.
    /// </summary>
    public static void SetName(object block, string name)
    {
        var property = block.GetType().GetProperty("Name", InstanceMembers);
        if (property is not null && property.CanWrite)
        {
            property.SetValue(block, new NiStringRef(name));
            return;
        }

        SetField(block, "_name", new NiStringRef(name));
    }

    /// <summary>Reads an NiString4-typed private field as a plain string.</summary>
    public static string GetNiString4(object block, string fieldName)
    {
        var value = GetField(block, fieldName);
        if (value is null)
            return string.Empty;
        var content = value.GetType()
            .GetProperty("Content", InstanceMembers)?.GetValue(value);
        return content as string ?? string.Empty;
    }

    public static void SetNiString4(object block, string fieldName, string text)
    {
        var value = GetField(block, fieldName);
        if (value is null)
        {
            SetField(block, fieldName, new NiString4(text, false));
            return;
        }

        var property = value.GetType().GetProperty("Content", InstanceMembers)
            ?? throw new StsConversionException(
                $"NiString4 has no Content property (field '{fieldName}').");
        property.SetValue(value, text);
    }
}

/// <summary>A conversion failure that should be reported loudly, not swallowed.</summary>
public sealed class StsConversionException : Exception
{
    public StsConversionException(string message) : base(message)
    {
    }
}
