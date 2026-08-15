using System.Text;

namespace StsConverter.Gui;

internal sealed class MainForm : Form
{
    private const int ColumnFile = 0;
    private const int ColumnGlass = 1;
    private const int ColumnReticle = 2;
    private const int ColumnDot = 3;
    private const int ColumnReticleTexture = 4;
    private const int ColumnMaterials = 5;
    private const int ColumnStatus = 6;
    private const int ColumnDetail = 7;

    private const string NoneLabel = "(none)";

    private readonly DataGridView _grid = new();
    private readonly TextBox _log = new();
    private readonly TextBox _outputFolder = new();
    private readonly OptionsPanel _options = new();
    private readonly ToolStripProgressBar _progress = new();
    private readonly ToolStripStatusLabel _statusLabel = new();
    private readonly ToolStripButton _convertAll = new("Convert All");
    private readonly ToolStripButton _convertSelected = new("Convert Selected");
    private readonly ToolStripButton _removeButton = new("Remove");

    private readonly List<FileEntry> _entries = new();
    private CancellationTokenSource? _cancellation;
    private bool _busy;

    public MainForm()
    {
        Text = "STS Scope Converter";
        MinimumSize = new Size(1100, 620);
        Size = new Size(1400, 820);
        StartPosition = FormStartPosition.CenterScreen;
        AllowDrop = true;

        var toolStrip = BuildToolStrip();
        var statusStrip = BuildStatusStrip();

        ConfigureGrid();
        ConfigureLog();

        var outputRow = BuildOutputRow();

        // Size explicitly before touching SplitterDistance or Panel*MinSize. A
        // docked SplitContainer is still at its 150x100 default here, and both
        // of those properties throw when the value exceeds the current extent.
        var verticalSplit = new SplitContainer
        {
            Dock = DockStyle.Fill,
            Orientation = Orientation.Horizontal,
            Size = new Size(1400, 700),
        };
        verticalSplit.Panel1.Controls.Add(_grid);
        verticalSplit.Panel2.Controls.Add(_log);
        verticalSplit.Panel1MinSize = 160;
        verticalSplit.Panel2MinSize = 80;

        var optionsGroup = new GroupBox
        {
            Text = "Conversion options (apply to every file)",
            Dock = DockStyle.Fill,
            Padding = new Padding(4),
        };
        optionsGroup.Controls.Add(_options);

        var horizontalSplit = new SplitContainer
        {
            Dock = DockStyle.Fill,
            Orientation = Orientation.Vertical,
            Size = new Size(1400, 700),
        };
        horizontalSplit.Panel1.Controls.Add(verticalSplit);
        horizontalSplit.Panel2.Controls.Add(optionsGroup);
        horizontalSplit.Panel1MinSize = 420;
        horizontalSplit.Panel2MinSize = 300;

        Controls.Add(horizontalSplit);
        Controls.Add(outputRow);
        Controls.Add(toolStrip);
        Controls.Add(statusStrip);

        // SplitterDistance must be set after the control has a real size, or
        // WinForms silently clamps it to whatever the design-time width was.
        Shown += (_, _) =>
        {
            SetSplitter(
                horizontalSplit, horizontalSplit.Width - 420);
            SetSplitter(
                verticalSplit, (int)(verticalSplit.Height * 0.62));
        };

        DragEnter += OnDragEnter;
        DragDrop += OnDragDrop;

        Log("Drop scope NIFs here, or use Add Files / Add Folder.");
        Log("Each file is analysed on arrival and the glass and reticle are " +
            "guessed. Check them before converting — the guesses are " +
            "heuristics, not detection.");
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

        var materials = new ToolStripButton("Materials…");
        materials.Click += (_, _) => SetMaterialsForSelection();

        var clear = new ToolStripButton("Clear");
        clear.Click += (_, _) => ClearAll();

        _convertAll.Click += (_, _) => StartConversion(convertAll: true);
        _convertSelected.Click += (_, _) => StartConversion(convertAll: false);

        var strip = new ToolStrip
        {
            GripStyle = ToolStripGripStyle.Hidden,
            Padding = new Padding(4),
        };
        strip.Items.AddRange(new ToolStripItem[]
        {
            addFiles,
            addFolder,
            _removeButton,
            clear,
            new ToolStripSeparator(),
            materials,
            new ToolStripSeparator(),
            _convertAll,
            _convertSelected,
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

    private Control BuildOutputRow()
    {
        var label = new Label
        {
            Text = "Output folder:",
            AutoSize = true,
            Anchor = AnchorStyles.Left,
            Margin = new Padding(3, 8, 3, 3),
        };

        _outputFolder.Dock = DockStyle.Fill;
        var browse = new Button { Text = "Browse…", AutoSize = true };
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

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            ColumnCount = 3,
            Height = 34,
            Padding = new Padding(6, 4, 6, 4),
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.Controls.Add(label, 0, 0);
        layout.Controls.Add(_outputFolder, 1, 0);
        layout.Controls.Add(browse, 2, 0);
        return layout;
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

        _grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            HeaderText = "File",
            ReadOnly = true,
            FillWeight = 130,
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
            HeaderText = "Reticle texture (blank = keep material)",
            FillWeight = 200,
            ToolTipText =
                "Leave blank and the reticle keeps its own material and renders " +
                "exactly as before, but STS reticle swapping is off. Enter a " +
                "verified texture path to turn swapping on. The path shown in " +
                "the mesh is NOT trustworthy: while a real material exists the " +
                "engine ignores this field, so it is routinely stale.",
        });
        _grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            HeaderText = "Materials folder",
            FillWeight = 200,
            ToolTipText =
                "Where this mesh's .BGEM materials live. The reticle's real " +
                "texture is read from there and retained, which is the only " +
                "reliable way to keep the scope's own reticle -- the material " +
                "is where the engine actually gets the texture, and the path " +
                "stored in the mesh is ignored while a material exists. " +
                "Guessed from a Materials folder near the mesh. Per-file, " +
                "because a batch can span mods. Use the Materials... button to " +
                "set it on several rows at once.",
        });
        _grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            HeaderText = "Status",
            ReadOnly = true,
            FillWeight = 80,
        });
        _grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            HeaderText = "Result",
            ReadOnly = true,
            FillWeight = 220,
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
        _log.Font = new Font(FontFamily.GenericMonospace, 8.5f);
        _log.BackColor = SystemColors.Window;
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
                        $"{string.Join(", ", analysis.ExistingStsNodes)} — " +
                        "tick Force to convert it anyway.");
                }
            }
            catch (Exception exception)
            {
                entry.AnalysisError = exception.Message;
                entry.Status = EntryStatus.Failed;
                entry.Detail = exception.Message;
                Log($"{entry.FileName}: FAILED to analyse — {exception.Message}");
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
            ? $"Blank: keeping the original material. The mesh names " +
              $"'{entry.SuggestedReticleTexture}', but that field is unused " +
              "while a real material exists, so verify it before using it."
            : entry.ReticleTexture;

        var materialsCell = row.Cells[ColumnMaterials];
        materialsCell.Value = entry.MaterialsRoot;
        materialsCell.ToolTipText = string.IsNullOrEmpty(entry.MaterialsRoot)
            ? "Not set: the reticle texture cannot be read from its material, " +
              "so the mesh's own (often stale) path is all there is."
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
                $"Materials folder for {targets.Count} file(s). The reticle's " +
                "real texture is read from the .BGEM materials inside it.",
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
                    : $"VERIFICATION FAILED — max error " +
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
                Log($"{entry.FileName}: FAILED — {exception.Message}");
            }

            RefreshRow(entry);
            _progress.Value = i + 1;
        }

        Log($"=== Done: {succeeded} converted, {failed} failed ===");
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

        _convertAll.Enabled = !_busy && convertible > 0;
        _convertSelected.Enabled = !_busy && selectedConvertible > 0;
        _removeButton.Enabled = !_busy && _grid.SelectedRows.Count > 0;
        _grid.Enabled = !_busy;
        _options.Enabled = !_busy;
        _outputFolder.Enabled = !_busy;

        _convertAll.Text = _busy ? "Cancel" : "Convert All";

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

        var builder = new StringBuilder(_log.Text);
        if (builder.Length > 0)
            builder.AppendLine();
        builder.Append(message);
        _log.Text = builder.ToString();
        _log.SelectionStart = _log.TextLength;
        _log.ScrollToCaret();
    }
}
