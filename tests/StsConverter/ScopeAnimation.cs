using NiflySharp;
using NiflySharp.Bitfields;
using NiflySharp.Blocks;
using NiflySharp.Enums;
using NiflySharp.Structs;

namespace StsConverter;

/// <summary>
/// Builds the STS ControlManager: one NiControllerManager on the root, one
/// NiVisController per model node, and the five animation sequences that
/// switch between the hip model and the see-through model.
/// </summary>
/// <remarks>
/// The scaffold is synthesised rather than copied from a reference file
/// because it has to point at the nodes of the file being converted. The
/// values below were read out of the reference scopes field by field; all six
/// carry an identical scaffold apart from block indices, so there is nothing
/// scope-specific to preserve. Note that the reference files contain
/// <c>scopeRechamber</c>, <c>scopeAiming</c> and <c>scopeFire</c> in addition
/// to <c>scopeInit</c> and <c>scopeStartAiming</c>, and that none of them
/// contains a <c>scopeStopAiming</c> sequence despite the documentation
/// mentioning one.
/// </remarks>
public static class ScopeAnimation
{
    private sealed record SequenceSpec(
        string Name,
        float StopTime,
        (float Time, byte Value)[] NormalKeys,
        (float Time, byte Value)[] AimingKeys);

    private static readonly SequenceSpec[] Sequences =
    {
        new("scopeInit", 1.0f,
            new[] { (0.0f, (byte)1), (1.0f, (byte)1) },
            new[] { (0.0f, (byte)0), (1.0f, (byte)0) }),
        new("scopeStartAiming", 0.15f,
            new[] { (0.0f, (byte)1), (0.13f, (byte)0), (0.15f, (byte)0) },
            new[] { (0.0f, (byte)0), (0.13f, (byte)1), (0.15f, (byte)1) }),
        new("scopeRechamber", 2.0f,
            new[] { (0.0f, (byte)0), (2.0f, (byte)0) },
            new[] { (0.0f, (byte)1), (2.0f, (byte)1) }),
        new("scopeAiming", 0.01f,
            new[] { (0.0f, (byte)0), (0.01f, (byte)0) },
            new[] { (0.0f, (byte)1), (0.01f, (byte)1) }),
        new("scopeFire", 0.15f,
            new[] { (0.0f, (byte)0), (0.15f, (byte)0) },
            new[] { (0.0f, (byte)1), (0.15f, (byte)1) }),
    };

    /// <summary>Names of the sequences this builder emits.</summary>
    public static IReadOnlyList<string> SequenceNames =>
        Sequences.Select(sequence => sequence.Name).ToList();

    public static void Build(
        NifFile nif,
        int rootIndex,
        string rootName,
        int scopeNormalIndex,
        string scopeNormalName,
        int scopeAimingIndex,
        string scopeAimingName)
    {
        var manager = new NiControllerManager
        {
            Cumulative = false,
            // 0x004C: cycle type CLAMP, active, compute scaled time.
            Flags = new TimeControllerFlags(0x004C),
            Frequency = 1.0f,
            Phase = 0.0f,
            StartTime = float.MaxValue,
            StopTime = float.MinValue,
            Target = new NiBlockPtr<NiObjectNET>(rootIndex),
            NextController = new NiBlockRef<NiTimeController>(-1),
        };
        var managerIndex = nif.AddBlock(manager);

        var normalController = CreateVisController(nif, scopeNormalIndex);
        var aimingController = CreateVisController(nif, scopeAimingIndex);

        var sequenceIndices = new List<int>();
        foreach (var spec in Sequences)
        {
            var textKeys = new NiTextKeyExtraData
            {
                TextKeys = new List<Key<NiStringRef>>
                {
                    new() { Time = 0.0f, Value = new NiStringRef("start") },
                    new() { Time = spec.StopTime, Value = new NiStringRef("end") },
                },
            };
            textKeys.NumTextKeys = (uint)textKeys.TextKeys.Count;
            var textKeysIndex = nif.AddBlock(textKeys);

            var normalInterp = CreateBoolInterpolator(nif, spec.NormalKeys);
            var aimingInterp = CreateBoolInterpolator(nif, spec.AimingKeys);

            var sequence = new NiControllerSequence
            {
                Name = new NiStringRef(spec.Name),
                Weight = 1.0f,
                TextKeys_NiBlockRef_NTKED =
                    new NiBlockRef<NiTextKeyExtraData>(textKeysIndex),
                CycleType = CycleType.CYCLE_CLAMP,
                Frequency = 1.0f,
                Phase = 0.0f,
                StartTime = 0.0f,
                StopTime = spec.StopTime,
                Manager = new NiBlockPtr<NiControllerManager>(managerIndex),
                AccumRootName_NSR = new NiStringRef(rootName),
                AccumFlags = AccumFlags.ACCUM_X_FRONT,
                ArrayGrowBy = 1,
                ControlledBlocks = new List<ControlledBlock>
                {
                    ControlledFor(scopeNormalName, normalInterp, normalController),
                    ControlledFor(scopeAimingName, aimingInterp, aimingController),
                },
            };
            sequence.NumControlledBlocks = (uint)sequence.ControlledBlocks.Count;
            sequenceIndices.Add(nif.AddBlock(sequence));
        }

        var palette = new NiDefaultAVObjectPalette
        {
            Scene = new NiBlockPtr<NiAVObject>(rootIndex),
            Objs = new List<AVObject>
            {
                ObjectEntry(rootName, rootIndex),
                ObjectEntry(scopeNormalName, scopeNormalIndex),
                ObjectEntry(scopeAimingName, scopeAimingIndex),
            },
        };
        palette.NumObjs = (uint)palette.Objs.Count;
        var paletteIndex = nif.AddBlock(palette);

        manager.ObjectPalette =
            new NiBlockRef<NiDefaultAVObjectPalette>(paletteIndex);
        manager.ControllerSequences =
            new NiBlockRefArray<NiControllerSequence>();
        manager.ControllerSequences.SetIndices(sequenceIndices);
        manager.NumControllerSequences = (uint)sequenceIndices.Count;

        // Attach the manager to the root, preserving any controller that was
        // already there by chaining it behind the manager.
        var root = (NiNode)nif.Blocks[rootIndex];
        var existing = root.Controller?.Index ?? -1;
        if (existing >= 0)
            manager.NextController = new NiBlockRef<NiTimeController>(existing);
        root.Controller = new NiBlockRef<NiTimeController>(managerIndex);
    }

    private static AVObject ObjectEntry(string name, int index) => new()
    {
        Name = new NiString4(name, false),
        AV_Object = new NiBlockPtr<NiAVObject>(index),
    };

    private static ControlledBlock ControlledFor(
        string nodeName, int interpolatorIndex, int controllerIndex) => new()
    {
        Interpolator = new NiBlockRef<NiInterpolator>(interpolatorIndex),
        Controller = new NiBlockRef<NiTimeController>(controllerIndex),
        Priority = 0,
        NodeName = new NiStringRef(nodeName),
        PropertyType = new NiStringRef(string.Empty),
        ControllerType = new NiStringRef("NiVisController"),
        ControllerID = new NiStringRef(string.Empty),
        InterpolatorID = new NiStringRef(string.Empty),
    };

    private static int CreateVisController(NifFile nif, int targetIndex)
    {
        // The reference scopes give each NiVisController a NiBlendBoolInterpolator
        // in manager-controlled mode; the sequences then supply the real keys.
        var blend = new NiBlendBoolInterpolator
        {
            Value = 0,
            Flags = InterpBlendFlags.ManagerControlled,
            ArraySize_by = 2,
            WeightThreshold = 0.0f,
            InterpCount_by = 0,
            SingleIndex_by = 255,
            HighPriority_sb = -128,
            NextHighPriority_sb = -128,
            SingleTime = float.MinValue,
            HighWeightsSum = float.MinValue,
            NextHighWeightsSum = float.MinValue,
            HighEaseSpinner = float.MinValue,
        };
        var blendIndex = nif.AddBlock(blend);

        var controller = new NiVisController
        {
            // 0x006C: cycle type CLAMP, active, manager controlled, scaled time.
            Flags = new TimeControllerFlags(0x006C),
            Frequency = 1.0f,
            Phase = 0.0f,
            StartTime = float.MaxValue,
            StopTime = float.MinValue,
            Target = new NiBlockPtr<NiObjectNET>(targetIndex),
            NextController = new NiBlockRef<NiTimeController>(-1),
            Interpolator = new NiBlockRef<NiInterpolator>(blendIndex),
        };
        var controllerIndex = nif.AddBlock(controller);

        var target = (NiAVObject)nif.Blocks[targetIndex];
        target.Controller = new NiBlockRef<NiTimeController>(controllerIndex);
        return controllerIndex;
    }

    private static int CreateBoolInterpolator(
        NifFile nif, (float Time, byte Value)[] keys)
    {
        var data = new NiBoolData
        {
            Data = new KeyGroup<byte>
            {
                Interpolation = KeyType.CONST_KEY,
                NumKeys = (uint)keys.Length,
                Keys = keys
                    .Select(key => new Key<byte>
                    {
                        Time = key.Time,
                        Value = key.Value,
                    })
                    .ToList(),
            },
        };
        var dataIndex = nif.AddBlock(data);

        var interpolator = new NiBoolInterpolator
        {
            Value = true,
            Data = new NiBlockRef<NiBoolData>(dataIndex),
        };
        return nif.AddBlock(interpolator);
    }
}
