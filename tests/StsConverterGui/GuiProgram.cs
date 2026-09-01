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

        // A WinExe that throws outside a handled path dies silently: no console,
        // no dialog, the window just vanishes. Route both the UI-thread and the
        // background-thread channels to a dialog so a failure is reportable.
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
        Application.ThreadException += (_, e) => ReportCrash(e.Exception);
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            ReportCrash(e.ExceptionObject as Exception);

        Application.Run(new MainForm());
        return 0;
    }

    private static void ReportCrash(Exception? exception)
    {
        var text = exception is null
            ? "An unknown error occurred."
            : $"{exception.GetType().Name}: {exception.Message}\r\n\r\n{exception.StackTrace}";
        MessageBox.Show(
            text,
            "STS Scope Converter has hit an error",
            MessageBoxButtons.OK,
            MessageBoxIcon.Error);
    }
}
