namespace StsConverter.Gui;

internal enum FadeMode
{
    Generate,
    RenameGlass,
    None,
}

/// <summary>
/// The batch-wide conversion options. Every control here applies to every file
/// in the list; per-file choices (glass, reticle, dot) live in the grid.
/// </summary>
internal sealed class OptionsPanel : Panel
{
    private readonly ToolTip _tips = new()
    {
        AutoPopDelay = 30000,
        InitialDelay = 350,
        ReshowDelay = 100,
    };

    private readonly RadioButton _fadeGenerate;
    private readonly RadioButton _fadeRename;
    private readonly RadioButton _fadeNone;
    private readonly NumericUpDown _fadeScale;
    private readonly NumericUpDown _segments;
    private readonly NumericUpDown _tolerance;
    private readonly TextBox _normalSuffix;
    private readonly ComboBox _reticlePreset;
    private readonly CheckBox _duplicateHipModel;
    private readonly CheckBox _textureLoader;
    private readonly CheckBox _keepReticleMaterial;
    private readonly CheckBox _flatViewParts;
    private readonly CheckBox _force;
    private readonly CheckBox _overwrite;

    public OptionsPanel()
    {
        Dock = DockStyle.Fill;
        AutoScroll = true;
        Padding = new Padding(8);

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            ColumnCount = 2,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            GrowStyle = TableLayoutPanelGrowStyle.AddRows,
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 55));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 45));

        void AddSpan(Control control)
        {
            layout.Controls.Add(control);
            layout.SetColumnSpan(control, 2);
            control.Dock = DockStyle.Fill;
        }

        void AddPair(string label, Control control, string tip)
        {
            var text = new Label
            {
                Text = label,
                AutoSize = true,
                Anchor = AnchorStyles.Left,
                Margin = new Padding(3, 6, 3, 3),
            };
            _tips.SetToolTip(text, tip);
            _tips.SetToolTip(control, tip);
            layout.Controls.Add(text);
            layout.Controls.Add(control);
            control.Dock = DockStyle.Fill;
        }

        AddSpan(Header("ScopeFade aperture"));

        _fadeGenerate = new RadioButton
        {
            Text = "Generate 24-segment annulus (recommended)",
            Checked = true,
            AutoSize = true,
        };
        _tips.SetToolTip(_fadeGenerate,
            "Builds the exact 48-vertex / 48-triangle ScopeFade:0 that every " +
            "reference STS scope shares, and that MagnaScope's exact " +
            "geometry-replay path requires. Your chosen glass mesh is kept " +
            "separately as Glass:0.");
        AddSpan(_fadeGenerate);

        _fadeRename = new RadioButton
        {
            Text = "Rename the chosen glass to ScopeFade:0",
            AutoSize = true,
        };
        _tips.SetToolTip(_fadeRename,
            "Fallback. The glass mesh becomes the aperture. MagnaScope falls " +
            "back to its synthesized-aperture path because an arbitrary lens " +
            "disc is not the annulus topology the exact replay needs.");
        AddSpan(_fadeRename);

        _fadeNone = new RadioButton
        {
            Text = "Do not create a ScopeFade at all",
            AutoSize = true,
        };
        AddSpan(_fadeNone);

        _fadeScale = MakeNumeric(0.94m, 0.10m, 3.00m, 0.01m, 3);
        AddPair("Fade scale", _fadeScale,
            "Multiplies the aperture radius measured from the glass mesh. " +
            "Across the reference STS corpus the fade sits at 0.88x-0.98x the " +
            "ocular lens radius with no consistent rule, so this is a starting " +
            "guess you should eyeball. 0.94 is the corpus midpoint.");

        var ratioLabel = new Label
        {
            Text = "0.4970 (fixed)",
            AutoSize = true,
            ForeColor = SystemColors.GrayText,
            Margin = new Padding(3, 6, 3, 3),
        };
        AddPair("Inner:outer ratio", ratioLabel,
            "Measured at 0.4970 from the shared STS ScopeFade asset, which is " +
            "byte-identical across every reference scope. Not adjustable: a " +
            "different ratio would disagree with every other STS scope and " +
            "with the ratio MagnaScope measures at runtime.");

        _segments = MakeNumeric(24m, 3m, 128m, 1m, 0);
        AddPair("Segments", _segments,
            "24 gives the canonical 48 vertices / 48 triangles / 144 indices. " +
            "Any other value leaves MagnaScope's exact replay path unusable.");

        AddSpan(Header("Structure"));

        _duplicateHipModel = new CheckBox
        {
            Text = "Duplicate full model into hip branch",
            Checked = true,
            AutoSize = true,
        };
        _tips.SetToolTip(_duplicateHipModel,
            "The aiming branch starts as a full copy of the model. You still " +
            "have to delete the occluding rear geometry from ScopeAiming by " +
            "hand — that part is modelling work and is not automated.");
        AddSpan(_duplicateHipModel);

        _textureLoader = new CheckBox
        {
            Text = "Create TextureLoader:0",
            Checked = true,
            AutoSize = true,
        };
        _tips.SetToolTip(_textureLoader,
            "The zero-scale node that forces the reticle and dot textures " +
            "resident, so the deliberately non-existent reticle material falls " +
            "back to them. STS reticle swapping does not work without it.");
        AddSpan(_textureLoader);

        AddSpan(Header("Reticle"));

        _reticlePreset = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList,
        };
        _reticlePreset.Items.Add(KeepMaterialLabel);
        foreach (var preset in StsReticlePresets.Names)
            _reticlePreset.Items.Add(preset);
        _reticlePreset.SelectedIndex = 0;
        AddPair("STS preset", _reticlePreset,
            "The documented easiest way, and the one to prefer. Points " +
            "Reticle:0 and Dot:0 at a reticle set STS actually ships in " +
            "Materials\\Scope\\Defaults\\. Because the material exists, the " +
            "reticle does not depend on the missing-material trick, no texture " +
            "path has to be known, and in-game reticle customisation still " +
            "works.\r\n\r\n" +
            "Leave on 'Keep original material' to have the reticle render " +
            "exactly as it did before conversion, with STS customisation off.");

        _keepReticleMaterial = new CheckBox
        {
            Text = "Always keep the original reticle material",
            AutoSize = true,
        };
        _tips.SetToolTip(_keepReticleMaterial,
            "Forces the original material to be kept even when a reticle " +
            "texture has been entered in the grid. STS reticle swapping will " +
            "be off, but the reticle renders exactly as it did before " +
            "conversion.");
        AddSpan(_keepReticleMaterial);

        var reticleNote = new Label
        {
            Text =
                "The per-file 'Reticle texture' column is the custom route, " +
                "used only when no preset is chosen. It deletes the reticle's " +
                "material so the engine falls back to that texture — and the " +
                "path stored in a non-STS mesh is usually stale, since the " +
                "engine ignores it while a real material exists. An unverified " +
                "path renders the reticle as a flat coloured quad.",
            AutoSize = true,
            MaximumSize = new Size(300, 0),
            ForeColor = SystemColors.GrayText,
            Margin = new Padding(3, 2, 3, 6),
        };
        AddSpan(reticleNote);

        _flatViewParts = new CheckBox
        {
            Text = "Flat ScopeViewParts (no template offset)",
            AutoSize = true,
        };
        _tips.SetToolTip(_flatViewParts,
            "The reference scopes put a shared (0.0047, -18.8825, 1.8440) " +
            "offset on ScopeViewParts and its exact negation on Adjustments, " +
            "so the two cancel. It is template residue, not an engine " +
            "requirement. Tick this to omit both.");
        AddSpan(_flatViewParts);

        _normalSuffix = new TextBox { Text = "_full" };
        AddPair("Hip clone suffix", _normalSuffix,
            "Appended to the cloned hip-model shape names. They must differ " +
            "from the aiming copies, which means name-targeted material swaps " +
            "will not hit the hip copy — the reference scopes have the same " +
            "limitation.");

        AddSpan(Header("Safety"));

        _tolerance = MakeNumeric(0.0001m, 0.0000001m, 1m, 0.0001m, 7);
        AddPair("Error tolerance", _tolerance,
            "Maximum acceptable world-space translation error, in game units. " +
            "A conversion above this is reported as failed. Real conversions " +
            "land near 1e-13.");

        _force = new CheckBox
        {
            Text = "Force (convert even if already STS)",
            AutoSize = true,
        };
        AddSpan(_force);

        _overwrite = new CheckBox
        {
            Text = "Overwrite existing output files",
            AutoSize = true,
        };
        AddSpan(_overwrite);

        Controls.Add(layout);

        _fadeGenerate.CheckedChanged += (_, _) => UpdateEnabledState();
        _fadeRename.CheckedChanged += (_, _) => UpdateEnabledState();
        _fadeNone.CheckedChanged += (_, _) => UpdateEnabledState();
        UpdateEnabledState();
    }

    private const string KeepMaterialLabel = "(keep original material)";

    public bool Overwrite => _overwrite.Checked;

    /// <summary>Selected preset stem, or null when keeping the material.</summary>
    public string? ReticlePreset =>
        _reticlePreset.SelectedIndex <= 0
            ? null
            : _reticlePreset.SelectedItem as string;

    public FadeMode Fade =>
        _fadeRename.Checked ? FadeMode.RenameGlass :
        _fadeNone.Checked ? FadeMode.None :
        FadeMode.Generate;

    private static Label Header(string text) => new()
    {
        Text = text,
        AutoSize = true,
        Font = new Font(SystemFonts.DefaultFont, FontStyle.Bold),
        Margin = new Padding(3, 12, 3, 3),
    };

    private static NumericUpDown MakeNumeric(
        decimal value, decimal minimum, decimal maximum, decimal step, int decimals) =>
        new()
        {
            Value = value,
            Minimum = minimum,
            Maximum = maximum,
            Increment = step,
            DecimalPlaces = decimals,
        };

    private void UpdateEnabledState()
    {
        var generating = _fadeGenerate.Checked;
        _fadeScale.Enabled = generating;
        _segments.Enabled = generating;
    }

    /// <summary>
    /// Builds the options for one file. Per-file selections come from the
    /// entry; everything else from this panel.
    /// </summary>
    public ConvertOptions BuildOptions(FileEntry entry, string outputPath)
    {
        var options = new ConvertOptions
        {
            InputPath = entry.InputPath,
            OutputPath = outputPath,
            GlassSelector = $"#{entry.GlassIndex}",
            ReticleSelector = $"#{entry.ReticleIndex}",
            DotSelector = entry.DotIndex >= 0 ? $"#{entry.DotIndex}" : null,
            ReticlePreset = ReticlePreset,
            MaterialsRoot = string.IsNullOrWhiteSpace(entry.MaterialsRoot)
                ? null
                : entry.MaterialsRoot.Trim(),
            ReticleTexture = string.IsNullOrWhiteSpace(entry.ReticleTexture)
                ? null
                : entry.ReticleTexture.Trim(),
            DotTexture = string.IsNullOrWhiteSpace(entry.DotTexture)
                ? null
                : entry.DotTexture.Trim(),
            NormalSuffix = string.IsNullOrWhiteSpace(_normalSuffix.Text)
                ? "_full"
                : _normalSuffix.Text.Trim(),
            RenameFade = Fade == FadeMode.RenameGlass,
            NoScopeFade = Fade == FadeMode.None,
            NoDuplicate = !_duplicateHipModel.Checked,
            FlatViewParts = _flatViewParts.Checked,
            KeepReticleMaterial = _keepReticleMaterial.Checked,
            NoTextureLoader = !_textureLoader.Checked,
            Force = _force.Checked,
            Segments = (int)_segments.Value,
            FadeScale = (float)_fadeScale.Value,
            Tolerance = (float)_tolerance.Value,
        };

        return options;
    }
}
