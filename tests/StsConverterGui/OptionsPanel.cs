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
///
/// The panel is split into what most users need (the reticle route and the
/// overwrite switch) and an advanced block that starts collapsed. The
/// defaults are the configuration that was verified in game: keep the scope's
/// own reticle material, generate the canonical 24-segment ScopeFade, clone
/// the model into the hip branch.
/// </summary>
internal sealed class OptionsPanel : Panel
{
    private readonly ToolTip _tips = new()
    {
        AutoPopDelay = 30000,
        InitialDelay = 350,
        ReshowDelay = 100,
    };

    private readonly RadioButton _reticleKeep;
    private readonly RadioButton _reticlePreset;
    private readonly ComboBox _presetList;
    private readonly CheckBox _overwrite;
    private readonly CheckBox _showAdvanced;
    private readonly Panel _advanced;

    private readonly RadioButton _fadeGenerate;
    private readonly RadioButton _fadeRename;
    private readonly RadioButton _fadeNone;
    private readonly NumericUpDown _fadeScale;
    private readonly NumericUpDown _tolerance;
    private readonly TextBox _normalSuffix;
    private readonly CheckBox _duplicateHipModel;
    private readonly CheckBox _textureLoader;
    private readonly CheckBox _flatViewParts;
    private readonly CheckBox _force;

    /// <summary>Raised when the advanced block is shown or hidden.</summary>
    public event EventHandler? AdvancedVisibleChanged;

    public OptionsPanel()
    {
        Dock = DockStyle.Fill;
        AutoScroll = true;
        Padding = new Padding(10, 6, 10, 6);

        var layout = NewTable();

        // ---- Reticle -------------------------------------------------------
        layout.Controls.Add(Header("Reticle"));
        layout.SetColumnSpan(layout.Controls[^1], 2);

        _reticleKeep = new RadioButton
        {
            Text = "Keep the scope's own reticle (recommended)",
            Checked = true,
            AutoSize = true,
        };
        _tips.SetToolTip(_reticleKeep,
            "The reticle keeps the mod's own material and renders exactly as it " +
            "did before conversion. This is the route verified in game. STS's " +
            "built-in reticle swapping stays off for this scope; MagnaScope's " +
            "own reticle switching covers that.");
        AddSpan(layout, _reticleKeep);

        _reticlePreset = new RadioButton
        {
            Text = "Use an STS preset reticle instead",
            AutoSize = true,
        };
        _tips.SetToolTip(_reticlePreset,
            "Points Reticle:0 and Dot:0 at a reticle set that ships with See " +
            "Through Scopes (Materials\\Scope\\Defaults\\). The scope loses its " +
            "own reticle art and shows STS's crosshair, but STS reticle " +
            "customisation works. This route passes every structural check but " +
            "has NOT been confirmed in game.");
        AddSpan(layout, _reticlePreset);

        _presetList = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList,
            Enabled = false,
            Margin = new Padding(24, 3, 3, 8),
        };
        foreach (var preset in StsReticlePresets.Names)
            _presetList.Items.Add(preset);
        _presetList.SelectedIndex = Math.Max(0, StsReticlePresets.Names
            .ToList().IndexOf("HuntingRifle4x"));
        AddSpan(layout, _presetList);

        // ---- Output --------------------------------------------------------
        layout.Controls.Add(Header("Output"));
        layout.SetColumnSpan(layout.Controls[^1], 2);

        _overwrite = new CheckBox
        {
            Text = "Overwrite existing output files",
            AutoSize = true,
        };
        _tips.SetToolTip(_overwrite,
            "Off: a file that already exists in the output folder is reported " +
            "as failed and left alone. The input file is never touched either " +
            "way.");
        AddSpan(layout, _overwrite);

        var placementNote = new Label
        {
            Text =
                "Converted files are written with their original names. To use " +
                "one, place it at the same path as the original inside a mod " +
                "(meshes\\Weapons\\...) so it overrides it.",
            AutoSize = true,
            MaximumSize = new Size(300, 0),
            ForeColor = SystemColors.GrayText,
            Margin = new Padding(3, 0, 3, 8),
        };
        AddSpan(layout, placementNote);

        // ---- Advanced toggle ----------------------------------------------
        _showAdvanced = new CheckBox
        {
            Text = "Show advanced options",
            AutoSize = true,
            Margin = new Padding(3, 10, 3, 3),
        };
        AddSpan(layout, _showAdvanced);

        Controls.Add(layout);

        // ---- Advanced block -----------------------------------------------
        _advanced = new Panel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Visible = false,
        };
        var advanced = NewTable();

        advanced.Controls.Add(Header("ScopeFade aperture"));
        advanced.SetColumnSpan(advanced.Controls[^1], 2);

        _fadeGenerate = new RadioButton
        {
            Text = "Generate the 24-segment annulus (recommended)",
            Checked = true,
            AutoSize = true,
        };
        _tips.SetToolTip(_fadeGenerate,
            "Builds the exact 48-vertex / 48-triangle ScopeFade:0 that every " +
            "reference STS scope shares, and that MagnaScope's exact " +
            "geometry-replay path requires. Your chosen glass mesh is kept " +
            "separately.");
        AddSpan(advanced, _fadeGenerate);

        _fadeRename = new RadioButton
        {
            Text = "Rename the chosen glass to ScopeFade:0",
            AutoSize = true,
        };
        _tips.SetToolTip(_fadeRename,
            "Fallback. The glass mesh becomes the aperture. MagnaScope falls " +
            "back to its synthesized-aperture path because an arbitrary lens " +
            "disc is not the annulus topology the exact replay needs.");
        AddSpan(advanced, _fadeRename);

        _fadeNone = new RadioButton
        {
            Text = "Do not create a ScopeFade at all",
            AutoSize = true,
        };
        AddSpan(advanced, _fadeNone);

        _fadeScale = MakeNumeric(0.94m, 0.10m, 3.00m, 0.01m, 3);
        AddPair(advanced, "Fade scale", _fadeScale,
            "Multiplies the aperture radius measured from the glass mesh. " +
            "Across the reference STS corpus the fade sits at 0.88x-0.98x the " +
            "ocular lens radius with no consistent rule, so this is a starting " +
            "guess you should eyeball in NifSkope. 0.94 is the corpus midpoint.");

        var geometryNote = new Label
        {
            Text = "24 segments, inner:outer 0.4970 (fixed)",
            AutoSize = true,
            ForeColor = SystemColors.GrayText,
            Margin = new Padding(3, 6, 3, 3),
        };
        AddPair(advanced, "Geometry", geometryNote,
            "The segment count and ratio are measured from the shared STS " +
            "ScopeFade asset, which is byte-identical across every reference " +
            "scope, and MagnaScope's exact replay path depends on them. They " +
            "are deliberately not adjustable here.");

        advanced.Controls.Add(Header("Structure"));
        advanced.SetColumnSpan(advanced.Controls[^1], 2);

        _duplicateHipModel = new CheckBox
        {
            Text = "Duplicate full model into hip branch",
            Checked = true,
            AutoSize = true,
        };
        _tips.SetToolTip(_duplicateHipModel,
            "The aiming branch starts as a full copy of the model. Deleting " +
            "the occluding rear geometry from ScopeAiming is modelling work " +
            "that is not automated.");
        AddSpan(advanced, _duplicateHipModel);

        _textureLoader = new CheckBox
        {
            Text = "Create TextureLoader:0",
            Checked = true,
            AutoSize = true,
        };
        _tips.SetToolTip(_textureLoader,
            "The zero-scale node that keeps the reticle and dot textures " +
            "resident. Only the STS swap routes depend on it; with the scope's " +
            "own reticle kept it is inert, and it is left on to match the " +
            "configuration that was verified in game.");
        AddSpan(advanced, _textureLoader);

        _flatViewParts = new CheckBox
        {
            Text = "Flat ScopeViewParts (no template offset)",
            AutoSize = true,
        };
        _tips.SetToolTip(_flatViewParts,
            "The reference scopes put a shared (0.0047, -18.8825, 1.8440) " +
            "offset on ScopeViewParts and its exact negation on the reticle " +
            "holder, so the two cancel. It is template residue, not an engine " +
            "requirement. Tick this to omit both.");
        AddSpan(advanced, _flatViewParts);

        _normalSuffix = new TextBox { Text = "_full" };
        AddPair(advanced, "Hip clone suffix", _normalSuffix,
            "Appended to the cloned hip-model shape names. They must differ " +
            "from the aiming copies, which means name-targeted material swaps " +
            "will not hit the hip copy; the reference scopes have the same " +
            "limitation.");

        advanced.Controls.Add(Header("Custom reticle route (not recommended)"));
        advanced.SetColumnSpan(advanced.Controls[^1], 2);

        var customNote = new Label
        {
            Text =
                "With advanced options shown, the grid gains 'Reticle texture' " +
                "and 'Materials folder' columns and a Materials... button. " +
                "Filling either switches that file to the missing-material " +
                "route: the reticle's material is repointed at a non-existent " +
                "path so the engine falls back to a texture. In game this " +
                "rendered a solid black square on the test scopes, because the " +
                "texture was authored against the mod's own material blend " +
                "settings. Leave both blank unless you are debugging that " +
                "route. They are ignored while 'Keep the scope's own reticle' " +
                "is selected.",
            AutoSize = true,
            MaximumSize = new Size(300, 0),
            ForeColor = SystemColors.GrayText,
            Margin = new Padding(3, 2, 3, 6),
        };
        AddSpan(advanced, customNote);

        advanced.Controls.Add(Header("Safety"));
        advanced.SetColumnSpan(advanced.Controls[^1], 2);

        _tolerance = MakeNumeric(0.0001m, 0.0000001m, 1m, 0.0001m, 7);
        AddPair(advanced, "Error tolerance", _tolerance,
            "Maximum acceptable world-space translation error, in game units. " +
            "A conversion above this is reported as failed. Real conversions " +
            "land near 1e-13.");

        _force = new CheckBox
        {
            Text = "Force (convert even if already STS)",
            AutoSize = true,
        };
        AddSpan(advanced, _force);

        _advanced.Controls.Add(advanced);
        Controls.Add(_advanced);
        _advanced.BringToFront();
        layout.SendToBack();

        _reticleKeep.CheckedChanged += (_, _) => UpdateEnabledState();
        _reticlePreset.CheckedChanged += (_, _) => UpdateEnabledState();
        _fadeGenerate.CheckedChanged += (_, _) => UpdateEnabledState();
        _fadeRename.CheckedChanged += (_, _) => UpdateEnabledState();
        _fadeNone.CheckedChanged += (_, _) => UpdateEnabledState();
        _showAdvanced.CheckedChanged += (_, _) =>
        {
            _advanced.Visible = _showAdvanced.Checked;
            AdvancedVisibleChanged?.Invoke(this, EventArgs.Empty);
        };
        UpdateEnabledState();
    }

    public bool Overwrite => _overwrite.Checked;

    public bool AdvancedVisible => _showAdvanced.Checked;

    /// <summary>Selected preset stem, or null when keeping the material.</summary>
    public string? ReticlePreset =>
        _reticlePreset.Checked ? _presetList.SelectedItem as string : null;

    public bool KeepReticleMaterial => _reticleKeep.Checked;

    public FadeMode Fade =>
        _fadeRename.Checked ? FadeMode.RenameGlass :
        _fadeNone.Checked ? FadeMode.None :
        FadeMode.Generate;

    private static TableLayoutPanel NewTable()
    {
        var table = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            ColumnCount = 2,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            GrowStyle = TableLayoutPanelGrowStyle.AddRows,
        };
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        return table;
    }

    private static void AddSpan(TableLayoutPanel table, Control control)
    {
        table.Controls.Add(control);
        table.SetColumnSpan(control, 2);
        if (control is not RadioButton and not CheckBox and not Label)
            control.Dock = DockStyle.Fill;
    }

    private void AddPair(TableLayoutPanel table, string label, Control control, string tip)
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
        table.Controls.Add(text);
        table.Controls.Add(control);
        if (control is not Label)
            control.Dock = DockStyle.Fill;
    }

    private static Label Header(string text) => new()
    {
        Text = text,
        AutoSize = true,
        Font = new Font("Segoe UI Semibold", 9.5f, FontStyle.Bold),
        Margin = new Padding(3, 10, 3, 4),
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
        _presetList.Enabled = _reticlePreset.Checked;
        var generating = _fadeGenerate.Checked;
        _fadeScale.Enabled = generating;
    }

    /// <summary>
    /// Builds the options for one file. Per-file selections come from the
    /// entry; everything else from this panel.
    /// </summary>
    public ConvertOptions BuildOptions(FileEntry entry, string outputPath)
    {
        // The custom missing-material inputs only reach the builder when the
        // advanced block is open AND the keep route is not selected; otherwise
        // a materials folder guessed from the mesh's neighbourhood would
        // silently flip a file onto the route that renders black in game.
        var customRoute = AdvancedVisible && !KeepReticleMaterial;

        return new ConvertOptions
        {
            InputPath = entry.InputPath,
            OutputPath = outputPath,
            GlassSelector = $"#{entry.GlassIndex}",
            ReticleSelector = $"#{entry.ReticleIndex}",
            DotSelector = entry.DotIndex >= 0 ? $"#{entry.DotIndex}" : null,
            ReticlePreset = ReticlePreset,
            MaterialsRoot = customRoute && !string.IsNullOrWhiteSpace(entry.MaterialsRoot)
                ? entry.MaterialsRoot.Trim()
                : null,
            ReticleTexture = customRoute && !string.IsNullOrWhiteSpace(entry.ReticleTexture)
                ? entry.ReticleTexture.Trim()
                : null,
            DotTexture = null,
            NormalSuffix = string.IsNullOrWhiteSpace(_normalSuffix.Text)
                ? "_full"
                : _normalSuffix.Text.Trim(),
            RenameFade = Fade == FadeMode.RenameGlass,
            NoScopeFade = Fade == FadeMode.None,
            NoDuplicate = !_duplicateHipModel.Checked,
            FlatViewParts = _flatViewParts.Checked,
            KeepReticleMaterial = KeepReticleMaterial,
            NoTextureLoader = !_textureLoader.Checked,
            Force = _force.Checked,
            Segments = ScopeFadeGeometry.CanonicalSegments,
            FadeScale = (float)_fadeScale.Value,
            Tolerance = (float)_tolerance.Value,
        };
    }
}
