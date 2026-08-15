using System.Runtime.InteropServices;

namespace StsConverter.Gui;

internal static class GuiProgram
{
    private const int AttachParentProcess = -1;

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AttachConsole(int processId);

    [STAThread]
    private static int Main(string[] args)
    {
        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        if (args.Length > 0 && args[0] == "--selftest")
        {
            // A WinExe has no console of its own, and .NET has already cached a
            // null writer by the time Main runs. Attach to whatever launched us
            // and rebuild the writer, or the self-test reports nothing.
            if (AttachConsole(AttachParentProcess))
            {
                Console.SetOut(new StreamWriter(Console.OpenStandardOutput())
                {
                    AutoFlush = true,
                });
                Console.SetError(new StreamWriter(Console.OpenStandardError())
                {
                    AutoFlush = true,
                });
            }

            return MainForm.SelfTest(args.Skip(1).ToArray());
        }

        Application.Run(new MainForm());
        return 0;
    }
}
