using System.Diagnostics;

namespace StsConverter.Gui;

internal sealed class MainForm : Form
{
    private const int ColumnFile = 0;
    private const int ColumnGlass = 1;
    private const int ColumnReticle = 2;
    private const int ColumnDot = 3;
    private const int ColumnStatus = 4;
    private const int ColumnDetail = 5;
    private const int ColumnReticleTexture = 6;
    private const int ColumnMaterials = 7;

    private const string NoneLabel = "(none)";

    private static readonly Color AccentColor = Color.FromArgb(0, 120, 212);
    private static readonly Color CancelColor = Color.FromArgb(196, 89, 17);

    private readonly DataGridView _grid = new();
    private readonly Label _emptyHint = new();
    private readonly TextBox _log = new();
    private readonly TextBox _outputFolder = new();
    private readonly OptionsPanel _options = new();
    private readonly ToolStripProgressBar _progress = new();
    private readonly ToolStripStatusLabel _statusLabel = new();
    private readonly ToolStripButton _removeButton = new("Remove");
    private readonly ToolStripButton _materialsButton = new("Materials…");
    private readonly ToolStripButton _shapesButton = new("Shapes…");
    private readonly Button _convertAll = new();
    private readonly Button _convertSelected = new();
    private readonly Button _openOutput = new();

    private readonly List<FileEntry> _entries = new();
    private CancellationTokenSource? _cancellation;
    private bool _busy;

    public MainForm()
    {
        Text = "STS Scope Converter";
        Font = new Font("Segoe UI", 9f);
        MinimumSize = new Size(980, 620);
        Size = new Size(1180, 760);
        StartPosition = FormStartPosition.CenterScreen;
        AllowDrop = true;

        var toolStrip = BuildToolStrip();
        var statusStrip = BuildStatusStrip();
        var actionBar = BuildActionBar();

        ConfigureGrid();
        ConfigureLog();

        _emptyHint.Text =
            "Drop scope .nif files here, or use Add Files / Add Folder. Each " +
            "file is analysed on arrival and its glass and reticle are guessed; " +
            "check them before converting.";
        _emptyHint.Dock = DockStyle.Top;
        _emptyHint.Height = 40;
        _emptyHint.TextAlign = ContentAlignment.MiddleLeft;
        _emptyHint.Padding = new Padding(8, 0, 8, 0);
        _emptyHint.ForeColor = SystemColors.GrayText;
        _emptyHint.BackColor = Color.FromArgb(247, 249, 252);

        var gridHost = new Panel { Dock = DockStyle.Fill };
        gridHost.Controls.Add(_grid);
        gridHost.Controls.Add(_emptyHint);

        // Size explicitly before touching SplitterDistance or Panel*MinSize. A
        // docked SplitContainer is still at its 150x100 default here, and both
        // of those properties throw when the value exceeds the current extent.
        var verticalSplit = new SplitContainer
        {
            Dock = DockStyle.Fill,
            Orientation = Orientation.Horizontal,
            Size = new Size(1180, 640),
        };
        verticalSplit.Panel1.Controls.Add(gridHost);
        verticalSplit.Panel2.Controls.Add(_log);
        verticalSplit.Panel1MinSize = 160;
        verticalSplit.Panel2MinSize = 70;

        var optionsGroup = new GroupBox
        {
            Text = "Options (apply to every file)",
            Dock = DockStyle.Fill,
            Padding = new Padding(4),
        };
        optionsGroup.Controls.Add(_options);

        var horizontalSplit = new SplitContainer
        {
            Dock = DockStyle.Fill,
            Orientation = Orientation.Vertical,
            Size = new Size(1180, 640),
            FixedPanel = FixedPanel.Panel2,
        };
        horizontalSplit.Panel1.Controls.Add(verticalSplit);
        horizontalSplit.Panel2.Controls.Add(optionsGroup);
        horizontalSplit.Panel1MinSize = 480;
        horizontalSplit.Panel2MinSize = 320;

        Controls.Add(horizontalSplit);
        Controls.Add(actionBar);
        Controls.Add(toolStrip);
        Controls.Add(statusStrip);

        // SplitterDistance must be set after the control has a real size, or
        // WinForms silently clamps it to whatever the design-time width was.
        Shown += (_, _) =>
        {
            SetSplitter(horizontalSplit, horizontalSplit.Width - 350);
            SetSplitter(verticalSplit, (int)(verticalSplit.Height * 0.66));
        };

        _options.AdvancedVisibleChanged += (_, _) => ApplyAdvancedVisibility();
        ApplyAdvancedVisibility();

        DragEnter += OnDragEnter;
        DragDrop += OnDragDrop;

        Log("Ready. The defaults are the configuration verified in game: the " +
            "scope keeps its own reticle, a canonical ScopeFade is generated, " +
            "and the full model is cloned into the hip branch.");
        UpdateButtons();
    }

    /// <summary>
    /// Sets a splitter position, clamped into the range the control will
    /// actually accept. SplitterDistance throws rather than clamping, and the
    /// legal range depends on the current extent and both panel minimums, so
    /// any unclamped assignment is a crash waiting for an unusual window size.
    /// </summary>
    private static void SetSplitter(SplitContainer split, int distance)
    {
        var extent = split.Orientation == Orientation.Vertical
            ? split.Width
            : split.Height;
        var minimum = split.Panel1MinSize;
        var maximum = extent - split.Panel2MinSize - split.SplitterWidth;
        if (maximum < minimum)
            return;
        split.SplitterDistance = Math.Clamp(distance, minimum, maximum);
    }

    private ToolStrip BuildToolStrip()
    {
        var addFiles = new ToolStripButton("Add Files…");
        addFiles.Click += (_, _) => AddFilesDialog();

        var addFolder = new ToolStripButton("Add Folder…");
        addFolder.Click += (_, _) => AddFolderDialog();

        _removeButton.Click += (_, _) => RemoveSelected();

        var clear = new ToolStripButton("Clear");
        clear.Click += (_, _) => ClearAll();

        _shapesButton.Click += (_, _) => ShowShapesForSelection();
        _shapesButton.ToolTipText =
            "Full per-shape report for the selected file, for when a guess " +
            "looks wrong. Double-clicking a row does the same.";

        _materialsButton.Click += (_, _) => SetMaterialsForSelection();
        _materialsButton.ToolTipText =
            "Advanced: set the materials folder on the selected rows (custom " +
            "reticle route only).";

        var strip = new ToolStrip
        {
            GripStyle = ToolStripGripStyle.Hidden,
            RenderMode = ToolStripRenderMode.System,
            Padding = new Padding(6, 3, 6, 3),
        };
        strip.Items.AddRange(new ToolStripItem[]
        {
            addFiles,
            addFolder,
            _removeButton,
            clear,
            new ToolStripSeparator(),
            _shapesButton,
            _materialsButton,
        });
        return strip;
    }

    private StatusStrip BuildStatusStrip()
    {
        _progress.Visible = false;
        _progress.Width = 220;
        _statusLabel.Spring = true;
        _statusLabel.TextAlign = ContentAlignment.MiddleLeft;

        var strip = new StatusStrip();
        strip.Items.Add(_progress);
        strip.Items.Add(_statusLabel);
        return strip;
    }

    private Control BuildActionBar()
    {
        var label = new Label
        {
            Text = "Output folder:",
            AutoSize = true,
            Anchor = AnchorStyles.Left,
            Margin = new Padding(3, 9, 3, 3),
        };

        _outputFolder.Dock = DockStyle.Fill;
        _outputFolder.Margin = new Padding(3, 6, 3, 3);

        var browse = new Button { Text = "Browse…", AutoSize = true, Margin = new Padding(3, 4, 3, 3) };
        browse.Click += (_, _) =>
        {
            using var dialog = new FolderBrowserDialog
            {
                Description = "Where converted NIFs are written",
                SelectedPath = _outputFolder.Text,
            };
            if (dialog.ShowDialog(this) == DialogResult.OK)
                _outputFolder.Text = dialog.SelectedPath;
        };

        _openOutput.Text = "Open";
        _openOutput.AutoSize = true;
        _openOutput.Margin = new Padding(3, 4, 12, 3);
        _openOutput.Click += (_, _) => OpenOutputFolder();

        StylePrimary(_convertAll, "Convert All");
        _convertAll.Click += (_, _) => StartConversion(convertAll: true);

        _convertSelected.Text = "Convert Selected";
        _convertSelected.AutoSize = true;
        _convertSelected.Height = 30;
        _convertSelected.Margin = new Padding(3, 4, 3, 3);
        _convertSelected.Click += (_, _) => StartConversion(convertAll: false);

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Bottom,
            ColumnCount = 6,
            Height = 42,
            Padding = new Padding(6, 0, 6, 0),
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.Controls.Add(label, 0, 0);
        layout.Controls.Add(_outputFolder, 1, 0);
        layout.Controls.Add(browse, 2, 0);
        layout.Controls.Add(_openOutput, 3, 0);
        layout.Controls.Add(_convertSelected, 4, 0);
        layout.Controls.Add(_convertAll, 5, 0);
        return layout;
    }

    private static void StylePrimary(Button button, string text)
    {
        button.Text = text;
        button.AutoSize = true;
        button.Height = 30;
        button.MinimumSize = new Size(130, 30);
        button.Margin = new Padding(3, 4, 3, 3);
        button.FlatStyle = FlatStyle.Flat;
        button.FlatAppearance.BorderSize = 0;
        button.BackColor = AccentColor;
        button.ForeColor = Color.White;
        button.Font = new Font("Segoe UI Semibold", 9f, FontStyle.Bold);
        button.UseVisualStyleBackColor = false;
    }

    private void ConfigureGrid()
    {
        _grid.Dock = DockStyle.Fill;
        _grid.AllowUserToAddRows = false;
        _grid.AllowUserToDeleteRows = false;
        _grid.AllowUserToResizeRows = false;
        _grid.RowHeadersVisible = false;
        _grid.SelectionMode = DataGridViewSelectionMode.FullRowSelect;
        _grid.MultiSelect = true;
        _grid.EditMode = DataGridViewEditMode.EditOnEnter;
        _grid.AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill;
        _grid.BackgroundColor = SystemColors.Window;
        _grid.BorderStyle = BorderStyle.None;
        _grid.CellBorderStyle = DataGridViewCellBorderStyle.SingleHorizontal;
        _grid.GridColor = Color.FromArgb(232, 232, 232);
        _grid.EnableHeadersVisualStyles = false;
        _grid.ColumnHeadersDefaultCellStyle.BackColor = Color.FromArgb(243, 243, 243);
        _grid.ColumnHeadersDefaultCellStyle.Font = new Font("Segoe UI Semibold", 9f, FontStyle.Bold);
        _grid.ColumnHeadersHeightSizeMode = DataGridViewColumnHeadersHeightSizeMode.AutoSize;
        _grid.RowTemplate.Height = 26;

        _grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            HeaderText = "File",
            ReadOnly = true,
            FillWeight = 140,
        });
        _grid.Columns.Add(new DataGridViewComboBoxColumn
        {
            HeaderText = "Glass / lens",
            FillWeight = 150,
            DisplayStyle = DataGridViewComboBoxDisplayStyle.DropDownButton,
            FlatStyle = FlatStyle.Flat,
        });
        _grid.Columns.Add(new DataGridViewComboBoxColumn
        {
            HeaderText = "Reticle",
            FillWeight = 150,
            DisplayStyle = DataGridViewComboBoxDisplayStyle.DropDownButton,
            FlatStyle = FlatStyle.Flat,
        });
        _grid.Columns.Add(new DataGridViewComboBoxColumn
        {
            HeaderText = "Dot (optional)",
            FillWeight = 130,
            DisplayStyle = DataGridViewComboBoxDisplayStyle.DropDownButton,
            FlatStyle = FlatStyle.Flat,
        });
        _grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            HeaderText = "Status",
            ReadOnly = true,
            FillWeight = 85,
        });
        _grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            HeaderText = "Result",
            ReadOnly = true,
            FillWeight = 220,
        });
        _grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            HeaderText = "Reticle texture (advanced)",
            FillWeight = 170,
            Visible = false,
            ToolTipText =
                "Custom route only. A verified texture path switches this file " +
                "to the missing-material route, which rendered black in game on " +
                "the test scopes. Ignored while the scope keeps its own reticle.",
        });
        _grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            HeaderText = "Materials folder (advanced)",
            FillWeight = 170,
            Visible = false,
            ToolTipText =
                "Custom route only. Where this mesh's .BGEM materials live, so " +
                "the reticle's real texture can be read for the missing-material " +
                "route. Ignored while the scope keeps its own reticle.",
        });

        _grid.CellValueChanged += OnCellValueChanged;
        _grid.CurrentCellDirtyStateChanged += (_, _) =>
        {
            // Combo edits do not commit until focus leaves the cell otherwise,
            // so a user who picks a shape and immediately hits Convert would
            // convert with the previous selection.
            if (_grid.IsCurrentCellDirty)
                _grid.CommitEdit(DataGridViewDataErrorContexts.Commit);
        };
        _grid.CellDoubleClick += OnCellDoubleClick;
        _grid.SelectionChanged += (_, _) => UpdateButtons();

        // A combo cell whose value is not in its item list raises DataError and
        // pops a modal dialog per repaint. Swallow it: the value is rebuilt
        // from the entry whenever the analysis changes.
        _grid.DataError += (_, e) => e.ThrowException = false;
    }

    private void ConfigureLog()
    {
        _log.Dock = DockStyle.Fill;
        _log.Multiline = true;
        _log.ReadOnly = true;
        _log.ScrollBars = ScrollBars.Vertical;
        _log.WordWrap = true;
        _log.Font = new Font("Consolas", 9f);
        _log.BackColor = Color.FromArgb(250, 250, 250);
        _log.BorderStyle = BorderStyle.None;
    }

    private void ApplyAdvancedVisibility()
    {
        var advanced = _options.AdvancedVisible;
        _grid.Columns[ColumnReticleTexture].Visible = advanced;
        _grid.Columns[ColumnMaterials].Visible = advanced;
        _materialsButton.Visible = advanced;
    }

    private void OnDragEnter(object? sender, DragEventArgs e)
    {
        if (_busy || e.Data is null)
            return;
        if (e.Data.GetDataPresent(DataFormats.FileDrop))
            e.Effect = DragDropEffects.Copy;
    }

    private void OnDragDrop(object? sender, DragEventArgs e)
    {
        if (_busy || e.Data?.GetData(DataFormats.FileDrop) is not string[] paths)
            return;

        var files = new List<string>();
        foreach (var path in paths)
        {
            if (Directory.Exists(path))
                files.AddRange(EnumerateNifs(path));
            else if (IsNif(path))
                files.Add(path);
        }

        AddFiles(files);
    }

    private static bool IsNif(string path) =>
        Path.GetExtension(path).Equals(".nif", StringComparison.OrdinalIgnoreCase);

    private static IEnumerable<string> EnumerateNifs(string folder) =>
        Directory.EnumerateFiles(folder, "*.nif", SearchOption.AllDirectories);

    private void AddFilesDialog()
    {
        using var dialog = new OpenFileDialog
        {
            Title = "Add scope NIFs",
            Filter = "NetImmerse/Gamebryo meshes (*.nif)|*.nif|All files (*.*)|*.*",
            Multiselect = true,
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
            AddFiles(dialog.FileNames);
    }

    private void AddFolderDialog()
    {
        using var dialog = new FolderBrowserDialog
        {
            Description = "Add every .nif under a folder (recursive)",
        };
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;

        var files = EnumerateNifs(dialog.SelectedPath).ToList();
        if (files.Count == 0)
        {
            Log($"No .nif files under {dialog.SelectedPath}.");
            return;
        }

        AddFiles(files);
    }

    /// <summary>
    /// Headless smoke test: build the window, load real files through the same
    /// path the UI uses, and report what the grid ended up holding. WinForms
    /// failures are overwhelmingly construction- and population-time, and this
    /// reaches both without a desktop session.
    /// </summary>
    internal static int SelfTest(IReadOnlyList<string> paths)
    {
        using var form = new MainForm();
        form.CreateControl();
        _ = form.Handle;

        if (paths.Count == 0)
        {
            Console.WriteLine("GUI self-test: window constructed, no files given.");
            return 0;
        }

        form.AddFiles(paths);

        var deadline = DateTime.UtcNow.AddSeconds(120);
        while (DateTime.UtcNow < deadline &&
               form._entries.Any(entry => entry.Status == EntryStatus.Analysing))
        {
            Application.DoEvents();
            Thread.Sleep(25);
        }

        var failures = 0;
        foreach (var entry in form._entries)
        {
            var glass = entry.ShapeByIndex(entry.GlassIndex)?.Name ?? "<none>";
            var reticle = entry.ShapeByIndex(entry.ReticleIndex)?.Name ?? "<none>";
            var row = form.RowFor(entry);
            var glassCell = row?.Cells[ColumnGlass] as DataGridViewComboBoxCell;

            Console.WriteLine(
                $"  {entry.FileName,-38} {entry.StatusText,-16} " +
                $"glass={glass,-14} reticle={reticle,-14} " +
                $"items={glassCell?.Items.Count ?? -1}");

            if (entry.Status is EntryStatus.Analysing or EntryStatus.Failed)
                ++failures;
            // A combo whose value is absent from its own item list is the
            // classic DataGridView defect: it renders blank and silently
            // reverts the user's pick.
            if (glassCell is not null &&
                glassCell.Value is string value &&
                !glassCell.Items.Contains(value))
            {
                Console.WriteLine($"    ! glass cell value '{value}' is not in its item list");
                ++failures;
            }
        }

        // The defaults are the contract this release makes with users: the
        // verified keep-material route, canonical fade, hip clone. Assert them
        // so a stray edit cannot ship the black-square route as the default.
        if (form._entries.Count > 0)
        {
            var probe = form._options.BuildOptions(form._entries[0], Path.GetTempFileName());
            var defaultsOk =
                probe.KeepReticleMaterial &&
                probe.ReticlePreset is null &&
                probe.ReticleTexture is null &&
                probe.MaterialsRoot is null &&
                !probe.RenameFade && !probe.NoScopeFade && !probe.NoDuplicate &&
                probe.Segments == ScopeFadeGeometry.CanonicalSegments;
            Console.WriteLine(defaultsOk
                ? "  defaults: keep-material route, canonical fade, hip clone (OK)"
                : "  ! defaults deviate from the verified configuration");
            if (!defaultsOk)
                ++failures;
        }

        Console.WriteLine(
            failures == 0
                ? "GUI self-test: PASSED"
                : $"GUI self-test: FAILED ({failures} problem(s))");
        return failures == 0 ? 0 : 1;
    }

    private void AddFiles(IEnumerable<string> paths)
    {
        var added = new List<FileEntry>();
        foreach (var path in paths)
        {
            var full = Path.GetFullPath(path);
            if (_entries.Any(entry =>
                    string.Equals(entry.InputPath, full, StringComparison.OrdinalIgnoreCase)))
            {
                continue;
            }

            var entry = new FileEntry(full)
            {
                MaterialsRoot = FileEntry.GuessMaterialsRoot(full),
            };
            _entries.Add(entry);
            added.Add(entry);

            var row = _grid.Rows[_grid.Rows.Add()];
            row.Tag = entry;
            row.Cells[ColumnFile].Value = entry.FileName;
            row.Cells[ColumnFile].ToolTipText = entry.InputPath;
            RefreshRow(entry);
        }

        if (added.Count == 0)
            return;

        if (string.IsNullOrWhiteSpace(_outputFolder.Text))
        {
            var first = Path.GetDirectoryName(added[0].InputPath);
            if (!string.IsNullOrEmpty(first))
                _outputFolder.Text = Path.Combine(first, "StsOutput");
        }

        Log($"Added {added.Count} file(s); analysing…");
        _ = AnalyseAsync(added);
    }

    private async Task AnalyseAsync(IReadOnlyList<FileEntry> entries)
    {
        UpdateButtons();

        foreach (var entry in entries)
        {
            try
            {
                var analysis = await Task.Run(() => ScopeAnalysis.Load(entry.InputPath));
                entry.Analysis = analysis;
                entry.ApplyGuesses();

                var glass = entry.ShapeByIndex(entry.GlassIndex);
                var reticle = entry.ShapeByIndex(entry.ReticleIndex);
                Log($"{entry.FileName}: {analysis.Shapes.Count} shapes; " +
                    $"glass = {glass?.Name ?? "?"}, reticle = {reticle?.Name ?? "?"}" +
                    (entry.DotIndex >= 0
                        ? $", dot = {entry.ShapeByIndex(entry.DotIndex)?.Name}"
                        : string.Empty));

                if (analysis.AlreadyConverted)
                {
                    Log($"  ! {entry.FileName} already contains " +
                        $"{string.Join(", ", analysis.ExistingStsNodes)}. Tick " +
                        "Force under advanced options to convert it anyway.");
                }
            }
            catch (Exception exception)
            {
                entry.AnalysisError = exception.Message;
                entry.Status = EntryStatus.Failed;
                entry.Detail = exception.Message;
                Log($"{entry.FileName}: FAILED to analyse: {exception.Message}");
            }

            RefreshRow(entry);
        }

        UpdateButtons();
    }

    private void RefreshRow(FileEntry entry)
    {
        var row = RowFor(entry);
        if (row is null)
            return;

        PopulateShapeCell(row, ColumnGlass, entry, entry.GlassIndex, allowNone: false);
        PopulateShapeCell(row, ColumnReticle, entry, entry.ReticleIndex, allowNone: false);
        PopulateShapeCell(row, ColumnDot, entry, entry.DotIndex, allowNone: true);

        var textureCell = row.Cells[ColumnReticleTexture];
        textureCell.Value = entry.ReticleTexture;
        textureCell.ToolTipText = string.IsNullOrEmpty(entry.ReticleTexture)
            ? $"Blank. The mesh names '{entry.SuggestedReticleTexture}', but " +
              "that field is unused while a real material exists, so it is " +
              "not trustworthy."
            : entry.ReticleTexture;

        var materialsCell = row.Cells[ColumnMaterials];
        materialsCell.Value = entry.MaterialsRoot;
        materialsCell.ToolTipText = string.IsNullOrEmpty(entry.MaterialsRoot)
            ? "Not set."
            : entry.MaterialsRoot;

        row.Cells[ColumnStatus].Value = entry.StatusText;
        row.Cells[ColumnDetail].Value = entry.Detail;
        row.Cells[ColumnDetail].ToolTipText = entry.Detail;

        row.DefaultCellStyle.BackColor = entry.Status switch
        {
            EntryStatus.Succeeded => Color.FromArgb(226, 245, 226),
            EntryStatus.Failed => Color.FromArgb(250, 226, 226),
            EntryStatus.NeedsSelection => Color.FromArgb(252, 246, 217),
            _ => SystemColors.Window,
        };
    }

    private void PopulateShapeCell(
        DataGridViewRow row, int column, FileEntry entry, int selectedIndex, bool allowNone)
    {
        var cell = (DataGridViewComboBoxCell)row.Cells[column];
        cell.Items.Clear();

        if (entry.Analysis is null)
        {
            cell.Value = null;
            cell.ReadOnly = true;
            return;
        }

        cell.ReadOnly = false;
        if (allowNone)
            cell.Items.Add(NoneLabel);
        foreach (var shape in entry.Analysis.Shapes)
            cell.Items.Add(shape.Label);

        var selected = entry.ShapeByIndex(selectedIndex);
        cell.Value = selected?.Label ?? (allowNone ? NoneLabel : null);
    }

    private void OnCellValueChanged(object? sender, DataGridViewCellEventArgs e)
    {
        if (e.RowIndex < 0 || e.ColumnIndex < 0)
            return;
        if (_grid.Rows[e.RowIndex].Tag is not FileEntry entry || entry.Analysis is null)
            return;

        if (e.ColumnIndex == ColumnMaterials)
        {
            entry.MaterialsRoot =
                (_grid.Rows[e.RowIndex].Cells[ColumnMaterials].Value as string
                 ?? string.Empty).Trim();
            return;
        }

        if (e.ColumnIndex == ColumnReticleTexture)
        {
            entry.ReticleTexture =
                (_grid.Rows[e.RowIndex].Cells[ColumnReticleTexture].Value as string
                 ?? string.Empty).Trim();
            return;
        }

        if (e.ColumnIndex is not (ColumnGlass or ColumnReticle or ColumnDot))
            return;

        var value = _grid.Rows[e.RowIndex].Cells[e.ColumnIndex].Value as string;
        var index = value == NoneLabel || string.IsNullOrEmpty(value)
            ? -1
            : entry.Analysis.Shapes
                .FirstOrDefault(shape => shape.Label == value)?.Index ?? -1;

        switch (e.ColumnIndex)
        {
            case ColumnGlass: entry.GlassIndex = index; break;
            case ColumnReticle: entry.ReticleIndex = index; break;
            case ColumnDot: entry.DotIndex = index; break;
        }

        // A hand-picked selection clears a previous run's verdict; leaving a
        // green "Converted" beside changed inputs would be a lie.
        if (entry.Status is EntryStatus.Succeeded or EntryStatus.Failed)
        {
            entry.Outcome = null;
            entry.Detail = string.Empty;
        }

        entry.Status = entry.GlassIndex >= 0 && entry.ReticleIndex >= 0
            ? EntryStatus.Ready
            : EntryStatus.NeedsSelection;

        RefreshRow(entry);
        UpdateButtons();
    }

    private void OnCellDoubleClick(object? sender, DataGridViewCellEventArgs e)
    {
        if (e.RowIndex < 0)
            return;
        if (_grid.Rows[e.RowIndex].Tag is not FileEntry entry)
            return;
        if (e.ColumnIndex is ColumnGlass or ColumnReticle or ColumnDot
            or ColumnReticleTexture or ColumnMaterials)
        {
            return;
        }

        using var detail = new ShapeDetailForm(entry);
        detail.ShowDialog(this);
    }

    private void ShowShapesForSelection()
    {
        var entry = _grid.SelectedRows.Cast<DataGridViewRow>()
            .Select(row => row.Tag)
            .OfType<FileEntry>()
            .FirstOrDefault();
        if (entry is null)
            return;
        using var detail = new ShapeDetailForm(entry);
        detail.ShowDialog(this);
    }

    private DataGridViewRow? RowFor(FileEntry entry) =>
        _grid.Rows.Cast<DataGridViewRow>()
            .FirstOrDefault(row => ReferenceEquals(row.Tag, entry));

    /// <summary>
    /// Applies one materials folder to every selected row, or to all rows when
    /// nothing is selected. A batch is usually one mod, but not always, which
    /// is why the value still lives per row.
    /// </summary>
    private void SetMaterialsForSelection()
    {
        if (_busy || _entries.Count == 0)
            return;

        var targets = _grid.SelectedRows.Cast<DataGridViewRow>()
            .Select(row => row.Tag)
            .OfType<FileEntry>()
            .ToList();
        if (targets.Count == 0)
            targets = _entries.ToList();

        using var dialog = new FolderBrowserDialog
        {
            Description =
                $"Materials folder for {targets.Count} file(s). Custom reticle " +
                "route only; ignored while the scope keeps its own reticle.",
            SelectedPath = targets[0].MaterialsRoot,
        };
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;

        foreach (var entry in targets)
        {
            entry.MaterialsRoot = dialog.SelectedPath;
            RefreshRow(entry);
        }

        Log($"Materials folder set on {targets.Count} file(s): {dialog.SelectedPath}");
    }

    private void RemoveSelected()
    {
        if (_busy)
            return;

        foreach (var row in _grid.SelectedRows.Cast<DataGridViewRow>().ToList())
        {
            if (row.Tag is FileEntry entry)
                _entries.Remove(entry);
            _grid.Rows.Remove(row);
        }

        UpdateButtons();
    }

    private void ClearAll()
    {
        if (_busy)
            return;
        _entries.Clear();
        _grid.Rows.Clear();
        UpdateButtons();
    }

    private void OpenOutputFolder()
    {
        var folder = _outputFolder.Text.Trim();
        if (string.IsNullOrWhiteSpace(folder) || !Directory.Exists(folder))
        {
            Log("The output folder does not exist yet; it is created on the first conversion.");
            return;
        }

        Process.Start(new ProcessStartInfo("explorer.exe", $"\"{folder}\"")
        {
            UseShellExecute = true,
        });
    }

    private void StartConversion(bool convertAll)
    {
        if (_busy)
        {
            _cancellation?.Cancel();
            return;
        }

        var targets = (convertAll
                ? _entries
                : _grid.SelectedRows.Cast<DataGridViewRow>()
                    .Select(row => row.Tag)
                    .OfType<FileEntry>())
            .Where(entry => entry.IsConvertible)
            .ToList();

        if (targets.Count == 0)
        {
            MessageBox.Show(
                this,
                "Nothing to convert. Each file needs a glass and a reticle " +
                "selected.",
                "STS Scope Converter",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
            return;
        }

        var outputFolder = _outputFolder.Text.Trim();
        if (string.IsNullOrWhiteSpace(outputFolder))
        {
            MessageBox.Show(
                this,
                "Choose an output folder first.",
                "STS Scope Converter",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
            return;
        }

        _ = ConvertAsync(targets, outputFolder);
    }

    private async Task ConvertAsync(IReadOnlyList<FileEntry> targets, string outputFolder)
    {
        _busy = true;
        _cancellation = new CancellationTokenSource();
        var token = _cancellation.Token;
        _progress.Visible = true;
        _progress.Minimum = 0;
        _progress.Maximum = targets.Count;
        _progress.Value = 0;
        UpdateButtons();

        var succeeded = 0;
        var failed = 0;

        Log(string.Empty);
        Log($"=== Converting {targets.Count} file(s) into {outputFolder} ===");

        for (var i = 0; i < targets.Count; ++i)
        {
            if (token.IsCancellationRequested)
            {
                Log("Cancelled.");
                break;
            }

            var entry = targets[i];
            entry.Status = EntryStatus.Converting;
            entry.Detail = string.Empty;
            RefreshRow(entry);
            _statusLabel.Text = $"Converting {entry.FileName} ({i + 1} of {targets.Count})…";

            var outputPath = Path.Combine(outputFolder, entry.FileName);

            try
            {
                if (string.Equals(
                        Path.GetFullPath(outputPath),
                        entry.InputPath,
                        StringComparison.OrdinalIgnoreCase))
                {
                    throw new StsConversionException(
                        "The output folder is the input folder, which would " +
                        "overwrite the source mesh. Choose a different folder.");
                }

                if (File.Exists(outputPath) && !_options.Overwrite)
                {
                    throw new StsConversionException(
                        $"{Path.GetFileName(outputPath)} already exists. Tick " +
                        "'Overwrite existing output files' to replace it.");
                }

                var options = _options.BuildOptions(entry, outputPath);
                var outcome = await Task.Run(() => ConversionRunner.Run(options), token);

                entry.Outcome = outcome;
                entry.Status = outcome.Passed ? EntryStatus.Succeeded : EntryStatus.Failed;
                entry.Detail = outcome.Passed
                    ? $"{outcome.ShapesCompared} shapes, max error " +
                      $"{outcome.MaxTranslationError:E2} units"
                    : $"VERIFICATION FAILED: max error " +
                      $"{outcome.MaxTranslationError:E2} units";

                if (outcome.Passed)
                    ++succeeded;
                else
                    ++failed;

                LogOutcome(entry, outcome);
            }
            catch (OperationCanceledException)
            {
                entry.Status = EntryStatus.Ready;
                entry.Detail = "Cancelled";
                RefreshRow(entry);
                break;
            }
            catch (Exception exception)
            {
                ++failed;
                entry.Status = EntryStatus.Failed;
                entry.Detail = exception.Message;
                Log($"{entry.FileName}: FAILED: {exception.Message}");
            }

            RefreshRow(entry);
            _progress.Value = i + 1;
        }

        Log($"=== Done: {succeeded} converted, {failed} failed ===");
        if (succeeded > 0)
        {
            Log("Install a converted file at the same path as the original " +
                "inside a mod (meshes\\Weapons\\...) so it overrides it, then " +
                "check the scope in game: the ScopeFade size is a heuristic and " +
                "the aiming model still contains the rear geometry.");
        }
        if (failed > 0)
        {
            Log("A failed verification means a mesh moved. Do not ship those " +
                "files; report the numbers above.");
        }

        _statusLabel.Text = $"{succeeded} converted, {failed} failed.";
        _progress.Visible = false;
        _cancellation.Dispose();
        _cancellation = null;
        _busy = false;
        UpdateButtons();
    }

    private void LogOutcome(FileEntry entry, ConversionOutcome outcome)
    {
        Log($"{entry.FileName}: {(outcome.Passed ? "OK" : "VERIFICATION FAILED")} " +
            $"-> {outcome.OutputPath}");
        Log($"    shapes compared {outcome.ShapesCompared}, " +
            $"max translation {outcome.MaxTranslationError:E6} units, " +
            $"max rotation {outcome.MaxRotationDegrees:E6} deg, " +
            $"max scale {outcome.MaxScaleError:E6}");

        foreach (var note in outcome.Report.Notes)
            Log($"    - {note}");

        if (outcome.OriginalCheck.Missing.Count > 0)
        {
            Log($"    MISSING in output: " +
                string.Join(", ", outcome.OriginalCheck.Missing));
        }

        if (outcome.CloneCheck?.Missing.Count > 0)
        {
            Log($"    MISSING hip clones: " +
                string.Join(", ", outcome.CloneCheck.Missing));
        }
    }

    private void UpdateButtons()
    {
        var convertible = _entries.Count(entry => entry.IsConvertible);
        var selectedConvertible = _grid.SelectedRows.Cast<DataGridViewRow>()
            .Select(row => row.Tag)
            .OfType<FileEntry>()
            .Count(entry => entry.IsConvertible);

        _convertAll.Enabled = _busy || convertible > 0;
        _convertSelected.Enabled = !_busy && selectedConvertible > 0;
        _removeButton.Enabled = !_busy && _grid.SelectedRows.Count > 0;
        _shapesButton.Enabled = _grid.SelectedRows.Count > 0;
        _grid.Enabled = !_busy;
        _options.Enabled = !_busy;
        _outputFolder.Enabled = !_busy;
        _emptyHint.Visible = _entries.Count == 0;

        _convertAll.Text = _busy ? "Cancel" : "Convert All";
        _convertAll.BackColor = _busy ? CancelColor : AccentColor;

        if (!_busy)
        {
            _statusLabel.Text =
                $"{_entries.Count} file(s), {convertible} ready to convert.";
        }
    }

    private void Log(string message)
    {
        if (InvokeRequired)
        {
            BeginInvoke(() => Log(message));
            return;
        }

        if (_log.TextLength > 0)
            _log.AppendText(Environment.NewLine);
        _log.AppendText(message);
        _log.SelectionStart = _log.TextLength;
        _log.ScrollToCaret();
    }
}
