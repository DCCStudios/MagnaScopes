using StsConverter;

if (args.Length == 0 ||
    args[0] is "--help" or "-h" or "/?" or "help")
{
    Help.Print();
    return args.Length == 0 ? 2 : 0;
}

var verb = args[0].ToLowerInvariant();
var rest = args.Skip(1).ToArray();

try
{
    return verb switch
    {
        "dump" => DumpCommand.Run(rest),
        "inspect" => InspectCommand.Run(rest),
        "convert" => ConvertCommand.Run(rest),
        "batch" => BatchCommand.Run(rest),
        "presets" => ListPresets(),
        "verify" => VerifyCommand.Run(rest),
        "check" => CheckCommand.Run(rest),
        _ => Unknown(verb),
    };
}
catch (StsConversionException exception)
{
    Console.Error.WriteLine();
    Console.Error.WriteLine($"ERROR: {exception.Message}");
    return 1;
}

static int ListPresets()
{
    Console.WriteLine(
        "STS reticle presets (Materials\\Scope\\Defaults\\<name>Cross/Dot" +
        ".BGSM.BGEM).");
    Console.WriteLine(
        "Pass one to --reticle-preset. These materials ship with STS, so the " +
        "reticle works");
    Console.WriteLine(
        "without the missing-material trick and stays customisable in game.");
    Console.WriteLine();
    var column = 0;
    foreach (var name in StsReticlePresets.Names)
    {
        Console.Write($"  {name,-20}");
        if (++column % 3 == 0)
            Console.WriteLine();
    }

    if (column % 3 != 0)
        Console.WriteLine();
    Console.WriteLine();
    Console.WriteLine($"{StsReticlePresets.Names.Count} presets.");
    return 0;
}

static int Unknown(string verb)
{
    Console.Error.WriteLine($"Unknown command '{verb}'.");
    Console.Error.WriteLine();
    Help.Print();
    return 2;
}

internal static class Help
{
    public static void Print()
    {
        Console.WriteLine(
"""
StsConverter - convert a non-STS Fallout 4 scope NIF into the See Through
Scopes node layout, usable by STS and by the MagnaScope F4SE plugin.

USAGE
  StsConverter inspect <input.nif>
  StsConverter convert <input.nif> --glass <name|#index> --reticle <name|#index>
                       --out <output.nif> [options]
  StsConverter batch <file-or-folder>... --out <folder>
                     [--recurse] [--force] [--overwrite]
  StsConverter verify <input.nif> <output.nif>
  StsConverter dump <nif> [<nif>...]

  For a GUI with the same batch flow, build and run StsConverterGui.

COMMANDS
  inspect   List every shape with index, name, parent, world bounds centre and
            radius, triangle count, and a heuristic guess at which shape is the
            glass/lens and which is the reticle. Use the printed indices or
            names with 'convert'.

  convert   Build the STS tree (ScopeNormal / ScopeAiming / ScopeViewParts /
            Adjustments / TextureLoader:0 plus the ControlManager and the
            scopeInit, scopeStartAiming, scopeAiming, scopeRechamber and
            scopeFire sequences), reparent the existing meshes into it, and
            write a new NIF. Every mesh keeps its exact world transform; this is
            verified numerically before the file is written.

  batch     Convert many scopes at once, taking the glass and reticle from the
            same heuristics 'inspect' prints. Files where either cannot be
            guessed are skipped rather than converted wrongly. This is the
            command-line form of what the GUI does.

  verify    Recompute every shape's world transform in both files and report the
            maximum translation and rotation error. Run automatically at the end
            of 'convert'; exposed separately so a converted file can be
            re-checked later.

  dump      Print the full node tree, transforms, child ordering, shader and
            extra data, controller sequences and optical geometry statistics.
            This is the reconnaissance tool the STS layout was derived from.

CONVERT OPTIONS
  --glass <name|#index>    Required. The lens/window mesh. Keeps its own name by
                           default, matching the reference corpus, where lens
                           meshes are never called Glass:0.
  --reticle <name|#index>  Required. The reticle mesh. Renamed to Reticle:0.
  --out <path>             Required. Output NIF path. Never overwrites input.
  --dot <name|#index>      Optional. A second reticle element, renamed to Dot:0.
                           Nothing is invented if this is omitted, but every
                           reference STS scope has one.
  --hide-when-aiming <..>  Comma-separated shapes that exist only in the hip
                           model (ScopeNormal) and are dropped from ScopeAiming.
                           Default: none, i.e. the aiming model is the full model
                           minus nothing; see README for why.
  --rename-glass           Rename the chosen glass to Glass:0.
  --rename-fade            Fallback: rename the chosen glass to ScopeFade:0
                           instead of generating a dedicated annulus. Mutually
                           exclusive with --rename-glass and --no-scope-fade.
  --no-scope-fade          Do not create a ScopeFade:0 at all.
  --fade-from <name|#i>    Measure the aperture from this shape instead of the
                           glass.
  --fade-scale <s>         Multiplies the outer radius measured from the glass
                           mesh. Default 0.94, the midpoint of the 0.88-0.98
                           range seen across the reference corpus. Author-tuned
                           there, so this is a starting guess, not a derivation.
  --fade-radius <r>        Set the outer radius outright, bypassing --fade-scale
                           and the measurement.
  --slab-fraction <f>      Fraction of a thick lens body's axial extent treated
                           as its rear slab when measuring the aperture.
  --segments <n>           Annulus segment count. Default 24 (48 verts, 48 tris,
                           144 indices) which is what MagnaScope's exact
                           geometry-replay path requires. The inner:outer ratio
                           is fixed at the 0.4970 measured from the shared STS
                           ScopeFade asset and is deliberately not adjustable.
  --no-duplicate           Do not clone the model into the hip branch.
  --normal-suffix <s>      Suffix for the cloned hip-model shapes. Default _full.
  --flat-viewparts         Omit the shared ScopeViewParts/Adjustments offset pair
                           that the reference scopes carry and that cancels out.
  --materials <folder>     PREFERRED. The mesh's material folder. The reticle's
                           and dot's real textures are read out of their .BGEM
                           materials and retained, which is the only reliable
                           way to keep a converted scope's own reticle: the
                           material is where the engine actually reads the
                           texture from, and the Source Texture field in the NIF
                           is ignored while that material exists (the M4A1 ACOG
                           names an RU556 path in the mesh and the correct MK18
                           one in its material).
                           Each NIF may need its own folder. Point it at the
                           mod's Materials folder, or the Data folder above it.

  --reticle-preset <name>  Point Reticle:0 and Dot:0 at one of the
                           reticle sets STS ships in Materials\Scope\Defaults\.
                           These materials exist, so nothing depends on the
                           missing-material fallback and no texture path has to
                           be known; in-game reticle customisation still works.
                           Run 'StsConverter presets' for the list.

  --reticle-texture <p>    The custom route. Turns STS reticle customisation ON
                           by the missing-material trick. Writes this
                           texture to Reticle:0 and TextureLoader:0 and repoints
                           the material at the non-existent ReticleCrossCustom
                           so the engine falls back to it.
                           You must supply this by hand and it must be right.
                           The converter cannot derive it: in a non-STS scope
                           the real texture is named inside the .BGEM material,
                           which lives in a BA2 this tool never reads, and the
                           NIF's own Source Texture field is ignored by the
                           engine while that material exists -- so it is
                           routinely stale. One test mesh had an MK18 material
                           and an RU556 texture in one M4A1 shader property.
  --dot-texture <p>        The same, for Dot:0. Needs --dot.
  --keep-reticle-material  Force-keep the original material even if
                           --reticle-texture was given.
  --no-texture-loader      Do not create TextureLoader:0.
  --force                  Convert even if the input already looks like an STS
                           scope. Without this, such an input is refused.
  --tolerance <units>      Max acceptable world translation error. Default 1e-4.

EXAMPLES
  StsConverter inspect meshes\Conversion\M4A1_Sight_ACOG.nif
  StsConverter convert meshes\Conversion\M4A1_Sight_ACOG.nif ^
      --glass "#12" --reticle "ACOGReticle" --out out\M4A1_Sight_ACOG.nif
""");
    }
}
