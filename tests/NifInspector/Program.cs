using NiflySharp;
using NiflySharp.Blocks;
using NiflySharp.Interfaces;
using System.Collections;
using System.Reflection;

if (args.Length != 1)
{
    Console.Error.WriteLine("Usage: NifInspector <path-to-nif>");
    return 2;
}

var nif = new NifFile();
var result = nif.Load(args[0]);
if (result != 0)
{
    Console.Error.WriteLine($"NiflySharp failed to load '{args[0]}' (code {result}).");
    return result;
}

Console.WriteLine($"NIF: {args[0]}");
Console.WriteLine($"Blocks: {nif.Blocks.Count}; unknown blocks: {nif.HasUnknownBlocks}");
Console.WriteLine("index | type | parent | name | local translation | scale | bounds");

for (var index = 0; index < nif.Blocks.Count; ++index)
{
    if (nif.Blocks[index] is not NiAVObject obj)
        continue;

    var name = (obj as INiNamed)?.Name?.String ?? string.Empty;
    var parent = nif.GetParentNode(obj);
    var parentName = (parent as INiNamed)?.Name?.String ?? "<root>";
    var bounds = obj switch
    {
        BSTriShape tri => $"{tri.Bounds.Center} r={tri.Bounds.Radius:F4}",
        NiGeometry geometry => $"{geometry.Bounds.Center} r={geometry.Bounds.Radius:F4}",
        _ => "-"
    };

    Console.WriteLine(
        $"{index,5} | {obj.GetType().Name} | {parentName} | {name} | " +
        $"{obj.Translation} | {obj.Scale:F4} | {bounds}");

    if ((name == "ScopeFade:0" ||
         name == "Reticle:0" ||
         name.Contains("_STS", StringComparison.OrdinalIgnoreCase) ||
         name.Contains("Lens", StringComparison.OrdinalIgnoreCase) ||
         name.Contains("Glass", StringComparison.OrdinalIgnoreCase) ||
         name.Contains("ScreenWarp", StringComparison.OrdinalIgnoreCase) ||
         name.Contains("EdgeBlur", StringComparison.OrdinalIgnoreCase)) &&
        obj is BSTriShape opticalShape)
    {
        Console.WriteLine($"  {name} public geometry properties:");
        foreach (var property in opticalShape.GetType().GetProperties(
                     BindingFlags.Public | BindingFlags.Instance))
        {
            if (!property.Name.Contains("Vert", StringComparison.OrdinalIgnoreCase) &&
                !property.Name.Contains("Tri", StringComparison.OrdinalIgnoreCase) &&
                !property.Name.Contains("Position", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            object? value;
            try
            {
                value = property.GetValue(opticalShape);
            }
            catch (Exception exception)
            {
                Console.WriteLine(
                    $"    {property.Name} ({property.PropertyType.Name}): " +
                    $"<read failed: {exception.GetType().Name}>");
                continue;
            }

            var count = value is ICollection collection
                ? $", count={collection.Count}"
                : string.Empty;
            Console.WriteLine(
                $"    {property.Name} ({property.PropertyType.FullName}){count}");
        }

        if (opticalShape.VertexPositions.Count > 0)
        {
            var minimum = opticalShape.VertexPositions[0];
            var maximum = opticalShape.VertexPositions[0];
            foreach (var position in opticalShape.VertexPositions)
            {
                minimum = System.Numerics.Vector3.Min(minimum, position);
                maximum = System.Numerics.Vector3.Max(maximum, position);
            }

            Console.WriteLine(
                $"    local vertex min={minimum}, max={maximum}, " +
                $"extent={maximum - minimum}");
        }

        if (name == "ScopeFade:0" && opticalShape.Triangles.Count > 0)
        {
            var vertexDesc = opticalShape.VertexDesc;
            Console.WriteLine(
                $"    vertex descriptor value={vertexDesc}");
            foreach (var field in vertexDesc.GetType().GetFields(
                         BindingFlags.Public | BindingFlags.Instance))
            {
                Console.WriteLine(
                    $"      vertexDesc field {field.Name}=" +
                    $"{field.GetValue(vertexDesc)}");
            }
            if (opticalShape.VertexData.Count > 0)
            {
                var vertex = opticalShape.VertexData[0];
                Console.WriteLine(
                    $"    first packed vertex type={vertex.GetType().FullName}");
                foreach (var field in vertex.GetType().GetFields(
                             BindingFlags.Public | BindingFlags.Instance))
                {
                    var fieldValue = field.GetValue(vertex);
                    Console.WriteLine(
                        $"      vertex field {field.Name}=" +
                        $"{fieldValue}");
                    if (fieldValue is not null &&
                        field.FieldType.Namespace == "NiflySharp.Structs")
                    {
                        foreach (var component in field.FieldType.GetFields(
                                     BindingFlags.Public |
                                     BindingFlags.Instance))
                        {
                            Console.WriteLine(
                                $"        {field.Name}.{component.Name}=" +
                                $"{component.GetValue(fieldValue)}");
                        }
                    }
                }
                foreach (var property in vertex.GetType().GetProperties(
                             BindingFlags.Public | BindingFlags.Instance))
                {
                    if (property.GetIndexParameters().Length != 0)
                        continue;
                    object? value;
                    try
                    {
                        value = property.GetValue(vertex);
                    }
                    catch (Exception exception)
                    {
                        value = $"<read failed: {exception.GetType().Name}>";
                    }
                    Console.WriteLine(
                        $"      vertex property {property.Name}={value}");
                }
            }

            var triangle = opticalShape.Triangles[0];
            var triangleType = triangle.GetType();
            Console.WriteLine(
                $"    first triangle type={triangleType.FullName}, " +
                $"value={triangle}");
            foreach (var field in triangleType.GetFields(
                         BindingFlags.Public | BindingFlags.Instance))
            {
                Console.WriteLine(
                    $"      field {field.Name}={field.GetValue(triangle)}");
            }
            foreach (var property in triangleType.GetProperties(
                         BindingFlags.Public | BindingFlags.Instance))
            {
                object? value;
                try
                {
                    value = property.GetValue(triangle);
                }
                catch (Exception exception)
                {
                    value = $"<read failed: {exception.GetType().Name}>";
                }
                Console.WriteLine(
                    $"      property {property.Name}={value}");
            }

            var positiveWinding = 0;
            var negativeWinding = 0;
            var degenerateWinding = 0;
            foreach (var authoredTriangle in opticalShape.Triangles)
            {
                var first = opticalShape.VertexPositions[authoredTriangle.V1];
                var second = opticalShape.VertexPositions[authoredTriangle.V2];
                var third = opticalShape.VertexPositions[authoredTriangle.V3];
                var crossY =
                    (second.Z - first.Z) * (third.X - first.X) -
                    (second.X - first.X) * (third.Z - first.Z);
                if (crossY > 0.000001F)
                    ++positiveWinding;
                else if (crossY < -0.000001F)
                    ++negativeWinding;
                else
                    ++degenerateWinding;
            }

            var firstPosition =
                opticalShape.VertexPositions[triangle.V1];
            var secondPosition =
                opticalShape.VertexPositions[triangle.V2];
            var thirdPosition =
                opticalShape.VertexPositions[triangle.V3];
            Console.WriteLine(
                $"    first triangle positions={firstPosition}, " +
                $"{secondPosition}, {thirdPosition}");
            Console.WriteLine(
                $"    authored XZ winding: positive={positiveWinding}, " +
                $"negative={negativeWinding}, degenerate={degenerateWinding}");
        }
    }
}

Console.WriteLine("controller blocks");
for (var index = 0; index < nif.Blocks.Count; ++index)
{
    var block = nif.Blocks[index];
    var type = block.GetType();
    if (!type.Name.Contains("Controller", StringComparison.Ordinal) &&
        !type.Name.Contains("Interpolator", StringComparison.Ordinal) &&
        type.Name != "NiBoolData")
    {
        continue;
    }

    var name = (block as INiNamed)?.Name?.String ?? string.Empty;
    Console.WriteLine($"{index,5} | {type.Name} | {name}");
    foreach (var property in type.GetProperties(
                 BindingFlags.Public | BindingFlags.Instance))
    {
        if (property.GetIndexParameters().Length != 0)
            continue;

        object? value;
        try
        {
            value = property.GetValue(block);
        }
        catch (Exception exception)
        {
            value = $"<read failed: {exception.GetType().Name}>";
        }

        if (value is ICollection collection)
        {
            Console.WriteLine(
                $"    {property.Name} ({property.PropertyType.Name}), " +
                $"count={collection.Count}");
            var itemIndex = 0;
            foreach (var item in collection)
            {
                Console.WriteLine($"      [{itemIndex++}] {item}");
                if (item is null)
                    continue;
                foreach (var itemProperty in item.GetType().GetProperties(
                             BindingFlags.Public | BindingFlags.Instance))
                {
                    if (itemProperty.GetIndexParameters().Length != 0)
                        continue;
                    object? itemValue;
                    try
                    {
                        itemValue = itemProperty.GetValue(item);
                    }
                    catch
                    {
                        continue;
                    }
                    Console.WriteLine(
                        $"        {itemProperty.Name}={itemValue}");
                }
                foreach (var itemField in item.GetType().GetFields(
                             BindingFlags.Public | BindingFlags.Instance))
                {
                    Console.WriteLine(
                        $"        {itemField.Name}={itemField.GetValue(item)}");
                }

                // ControlledBlock.ToString() hides the strings that decide
                // which scene node a controller sequence animates. Print the
                // resolved string references explicitly so a scopeFire
                // visibility sequence can be audited without guessing from
                // block order.
                var stringRefsProperty = item.GetType().GetProperty(
                    "StringRefs",
                    BindingFlags.Public | BindingFlags.Instance);
                if (stringRefsProperty?.GetValue(item) is
                    IEnumerable<NiStringRef> stringRefs)
                {
                    Console.WriteLine(
                        "        resolved strings=" +
                        string.Join(
                            " | ",
                            stringRefs.Select(reference => reference.String)));
                }
            }
        }
        else
        {
            Console.WriteLine($"    {property.Name}={value}");

            // NiBoolData stores its keys inside a KeyGroup<T>. Reflect the
            // group one level deeper so the sequence audit includes the
            // authored visibility values and key times.
            if (value is not null &&
                value.GetType().Name.StartsWith(
                    "KeyGroup",
                    StringComparison.Ordinal))
            {
                foreach (var groupProperty in value.GetType().GetProperties(
                             BindingFlags.Public | BindingFlags.Instance))
                {
                    if (groupProperty.GetIndexParameters().Length != 0)
                        continue;

                    object? groupValue;
                    try
                    {
                        groupValue = groupProperty.GetValue(value);
                    }
                    catch
                    {
                        continue;
                    }

                    Console.WriteLine(
                        $"      {groupProperty.Name}={groupValue}");
                    if (groupValue is not IEnumerable enumerable ||
                        groupValue is string)
                    {
                        continue;
                    }

                    var keyIndex = 0;
                    foreach (var key in enumerable)
                    {
                        Console.WriteLine($"        [{keyIndex++}] {key}");
                        if (key is null)
                            continue;
                        foreach (var keyProperty in key.GetType().GetProperties(
                                     BindingFlags.Public |
                                     BindingFlags.Instance))
                        {
                            if (keyProperty.GetIndexParameters().Length != 0)
                                continue;
                            try
                            {
                                Console.WriteLine(
                                    $"          {keyProperty.Name}=" +
                                    $"{keyProperty.GetValue(key)}");
                            }
                            catch
                            {
                                // A diagnostic inspector should continue
                                // through partially implemented Nifly types.
                            }
                        }
                        foreach (var keyField in key.GetType().GetFields(
                                     BindingFlags.Public |
                                     BindingFlags.Instance))
                        {
                            Console.WriteLine(
                                $"          {keyField.Name}=" +
                                $"{keyField.GetValue(key)}");
                        }
                    }
                }
                foreach (var groupField in value.GetType().GetFields(
                             BindingFlags.Public | BindingFlags.Instance))
                {
                    var groupValue = groupField.GetValue(value);
                    Console.WriteLine(
                        $"      field {groupField.Name}={groupValue}");
                    if (groupValue is not IEnumerable enumerable ||
                        groupValue is string)
                    {
                        continue;
                    }

                    var keyIndex = 0;
                    foreach (var key in enumerable)
                    {
                        Console.WriteLine($"        [{keyIndex++}] {key}");
                        if (key is null)
                            continue;
                        foreach (var keyField in key.GetType().GetFields(
                                     BindingFlags.Public |
                                     BindingFlags.Instance))
                        {
                            Console.WriteLine(
                                $"          {keyField.Name}=" +
                                $"{keyField.GetValue(key)}");
                        }
                        foreach (var keyProperty in key.GetType().GetProperties(
                                     BindingFlags.Public |
                                     BindingFlags.Instance))
                        {
                            if (keyProperty.GetIndexParameters().Length != 0)
                                continue;
                            try
                            {
                                Console.WriteLine(
                                    $"          {keyProperty.Name}=" +
                                    $"{keyProperty.GetValue(key)}");
                            }
                            catch
                            {
                                // Continue through partially implemented
                                // diagnostic types.
                            }
                        }
                    }
                }
            }
        }
    }
}

return 0;
