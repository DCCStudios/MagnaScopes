using System.Diagnostics;
using System.Reflection;

namespace StsConverter.Gui;

/// <summary>
/// Version, credits and licence notes. The converter is built on libraries
/// and reference material by other people; this is where that is said in the
/// tool itself rather than only in a README nobody opens.
/// </summary>
internal sealed class AboutForm : Form
{
    public AboutForm()
    {
        var version = Assembly.GetExecutingAssembly()
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?
            .InformationalVersion ?? "0.1.0";
        // Strip the source-link hash the SDK appends ("0.1.0+abc123").
        var plus = version.IndexOf('+');
        if (plus > 0)
            version = version[..plus];

        Text = "About STS Scope Converter";
        Size = new Size(620, 560);
        MinimumSize = new Size(480, 400);
        StartPosition = FormStartPosition.CenterParent;
        MinimizeBox = false;
        MaximizeBox = false;
        Font = new Font("Segoe UI", 9f);

        var title = new Label
        {
            Text = $"STS Scope Converter {version}",
            Font = new Font("Segoe UI Semibold", 13f, FontStyle.Bold),
            AutoSize = true,
            Margin = new Padding(12, 12, 12, 0),
        };
        var subtitle = new Label
        {
            Text = "Converts Fallout 4 scope meshes to the See Through Scopes " +
                   "layout, for See Through Scopes and MagnaScope. By DCC Studios.",
            AutoSize = true,
            MaximumSize = new Size(580, 0),
            ForeColor = SystemColors.GrayText,
            Margin = new Padding(12, 2, 12, 8),
        };

        var credits = new TextBox
        {
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            WordWrap = true,
            BorderStyle = BorderStyle.None,
            BackColor = Color.FromArgb(250, 250, 250),
            Dock = DockStyle.Fill,
            Text = CreditsText,
        };

        var link = new LinkLabel
        {
            Text = "github.com/DCCStudios/MagnaScopes",
            AutoSize = true,
            Margin = new Padding(12, 8, 12, 8),
        };
        link.LinkClicked += (_, _) => Process.Start(new ProcessStartInfo(
            "https://github.com/DCCStudios/MagnaScopes")
        {
            UseShellExecute = true,
        });

        var close = new Button
        {
            Text = "Close",
            AutoSize = true,
            DialogResult = DialogResult.OK,
            Anchor = AnchorStyles.Right,
            Margin = new Padding(12, 6, 12, 8),
        };

        var footer = new TableLayoutPanel
        {
            Dock = DockStyle.Bottom,
            ColumnCount = 2,
            AutoSize = true,
        };
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        footer.Controls.Add(link, 0, 0);
        footer.Controls.Add(close, 1, 0);

        var header = new FlowLayoutPanel
        {
            Dock = DockStyle.Top,
            FlowDirection = FlowDirection.TopDown,
            AutoSize = true,
            WrapContents = false,
        };
        header.Controls.Add(title);
        header.Controls.Add(subtitle);

        var body = new Panel { Dock = DockStyle.Fill, Padding = new Padding(12, 0, 12, 0) };
        body.Controls.Add(credits);

        Controls.Add(body);
        Controls.Add(footer);
        Controls.Add(header);
        AcceptButton = close;
        CancelButton = close;

        // Keep the caret off the top so the text does not start selected.
        Shown += (_, _) => { credits.SelectionStart = 0; credits.SelectionLength = 0; close.Focus(); };
    }

    private const string CreditsText =
        "CREDITS\r\n" +
        "\r\n" +
        "NiflySharp by ousnius\r\n" +
        "    The NIF library this tool reads and writes meshes with (NuGet " +
        "package \"Nifly\"). A clean-room C# rewrite of ousnius's nifly, " +
        "generated from the niftools nifxml specification. Licensed under the " +
        "GNU General Public License v3.0; because it is compiled into this " +
        "program, the program is distributed under GPL-3.0 terms as well and " +
        "its source is published in the repository linked below.\r\n" +
        "    https://github.com/ousnius/NiflySharp\r\n" +
        "\r\n" +
        "Miniball (C# port by Lorenzo Delana / SearchAThing forks; original by Bernd Gaertner)\r\n" +
        "    Bounding-sphere computation, used by NiflySharp. Apache-2.0.\r\n" +
        "    https://github.com/SearchAThing-forks/miniball\r\n" +
        "\r\n" +
        "nifxml by the NifTools team\r\n" +
        "    The NIF format specification NiflySharp is generated from.\r\n" +
        "    https://github.com/niftools/nifxml\r\n" +
        "\r\n" +
        "See Through Scopes by henkspamadres\r\n" +
        "    The scope framework this tool converts meshes for. The STS node " +
        "layout, sequences, ScopeFade geometry and reticle conventions were " +
        "measured from STS's own shipped meshes, and the optional preset " +
        "reticles point at materials STS ships in Materials\\Scope\\Defaults.\r\n" +
        "\r\n" +
        "NifInspector\r\n" +
        "    The NIF reconnaissance tool in the same repository (also on " +
        "NiflySharp) whose dumps of the STS reference corpus this converter's " +
        "conventions were derived from.\r\n" +
        "\r\n" +
        "NifSkope by the NifTools team, and Outfit Studio by ousnius\r\n" +
        "    Block ordering follows NifSkope's Sanitize > Reorder Blocks; both " +
        "are the tools to finish a conversion in (cutting down the aiming " +
        "model, checking the ScopeFade).\r\n" +
        "\r\n" +
        "Test material\r\n" +
        "    The M4A1, MK18 and RU556 sights from the Haru's M4 weapon pack " +
        "were the in-game verification set for the keep-reticle route.\r\n" +
        "\r\n" +
        "Microsoft .NET 8 and Windows Forms\r\n" +
        "    The runtime bundled into this executable (MIT).\r\n" +
        "\r\n" +
        "MagnaScope by DCC Studios\r\n" +
        "    The F4SE plugin this converter was built alongside; converted " +
        "scopes are meant for it as much as for See Through Scopes.\r\n";
}
