// NLMatrixUninstaller.cs — NL Audio Matrix uninstaller.
//
// CLI
//   NLMatrixUninstall.exe [/silent] [/dir=<path>]
//   --mode uninstall is accepted for symmetry with NLMatrixSetup.exe.
//
// The uninstaller removes only NL Audio Matrix objects:
//   * ROOT\NLMatrixVirtualAudio and the legacy ROOT\sysvad_ComponentizedAudioSample
//     device instances (via devcon when present, pnputil /remove-device otherwise);
//   * DriverStore packages whose INF contains the NLMatrix device description,
//     hardware ID or nlmatrixaudio.sys file name;
//   * the NLMatrix test certificate (matched by thumbprint, or by CN when the
//     certificate file has already been deleted);
//   * application files, Start Menu entries and the Add/Remove Programs entry.
// It never deletes generic audio registry keys, Windows audio defaults or other
// vendors' devices, and it never removes %APPDATA%\NL Audio Matrix presets.
// When nothing is installed it reports that and exits 0.
//
// Exit code table
//   0  Success (also when nothing was installed)
//   1  Administrator privileges required
//   2  Invalid command line / unsupported Windows or architecture
//  10  Unexpected error (see %TEMP%\NLMatrixSetup-uninstall.log)
//  20  Success; a reboot is required/recommended to finish driver removal
using System;
using System.Collections.Generic;
using System.IO;
using System.Windows.Forms;

[assembly: System.Reflection.AssemblyTitle("NL Audio Matrix Uninstall")]
[assembly: System.Reflection.AssemblyDescription("NL Audio Matrix uninstaller")]
[assembly: System.Reflection.AssemblyCompany("Nicola Lanci")]
[assembly: System.Reflection.AssemblyProduct("NL Audio Matrix")]
[assembly: System.Reflection.AssemblyCopyright("Copyright (c) Nicola Lanci")]
[assembly: System.Reflection.AssemblyVersion("0.6.0.0")]
[assembly: System.Reflection.AssemblyFileVersion("0.6.0.0")]
[assembly: System.Reflection.AssemblyInformationalVersion("0.6.0-setup")]

internal static class Uninstaller
{
    private const string AppName = NlConst.AppName;

    private sealed class Options
    {
        public bool Silent;
        public bool Help;
        public bool DirSpecified;
        public string AppDir = "";
    }

    private static string UsageText()
    {
        return
            "NL Audio Matrix Uninstall — usage\r\n" +
            "\r\n" +
            "  NLMatrixUninstall.exe [/silent] [/dir=<path>]\r\n" +
            "\r\n" +
            "Options:\r\n" +
            "  /silent           No UI (for deployment automation)\r\n" +
            "  /dir=<path>       Installation folder (default: read from Add/Remove Programs)\r\n" +
            "  /? --help         Show this help\r\n" +
            "\r\n" +
            "Exit codes:\r\n" +
            "   0 success (also when nothing is installed)\r\n" +
            "   1 administrator privileges required\r\n" +
            "   2 invalid command line / unsupported OS\r\n" +
            "  10 unexpected error\r\n" +
            "  20 success, reboot required to finish driver removal\r\n";
    }

    [STAThread]
    private static int Main(string[] args)
    {
        string logPath = Path.Combine(Path.GetTempPath(), "NLMatrixSetup-uninstall.log");
        var log = new SetupLog(logPath);
        int exitCode = 10;
        string summary = "";
        Options options;
        try
        {
            options = ParseOptions(args);
        }
        catch (SetupException ex)
        {
            log.Header("NL Audio Matrix Uninstall", "uninstall", args, null);
            log.Error(ex.Message);
            Console.WriteLine("ERROR: " + ex.Message);
            Console.WriteLine(UsageText());
            if (!Array.Exists(args, a => string.Equals(a, "/silent", StringComparison.OrdinalIgnoreCase) ||
                                          string.Equals(a, "--silent", StringComparison.OrdinalIgnoreCase)))
                MessageBox.Show(ex.Message + "\r\n\r\n" + UsageText(), AppName + " Uninstall",
                                MessageBoxButtons.OK, MessageBoxIcon.Error);
            log.Result(ex.ExitCode, ex.Message);
            log.Dispose();
            return ex.ExitCode;
        }

        if (options.Help)
        {
            Console.WriteLine(UsageText());
            if (!options.Silent)
                MessageBox.Show(UsageText(), AppName + " Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return 0;
        }

        try
        {
            log.Header("NL Audio Matrix Uninstall", "uninstall", args,
                       "Install folder override: " + (options.DirSpecified ? options.AppDir : "<from registry>"));
            Console.WriteLine("NL Audio Matrix Uninstall: log=" + log.Path);

            if (!Sys.IsAdmin())
                throw new SetupException(1,
                    "The NL Audio Matrix uninstaller requires administrator privileges.\r\n" +
                    "Right-click NLMatrixUninstall.exe and choose 'Run as administrator'.");
            if (!Environment.Is64BitOperatingSystem || !Environment.Is64BitProcess)
                throw new SetupException(2,
                    "NL Audio Matrix requires 64-bit Windows and the 64-bit uninstaller.");

            if (!options.Silent)
            {
                var answer = MessageBox.Show(
                    "Remove " + AppName + " and its virtual audio driver from this computer?\r\n\r\n" +
                    "User presets and configuration in %APPDATA%\\" + AppName + " will be kept.",
                    AppName + " - Uninstall", MessageBoxButtons.YesNo, MessageBoxIcon.Question,
                    MessageBoxDefaultButton.Button2);
                if (answer != DialogResult.Yes)
                {
                    log.Info("Uninstall cancelled by the user.");
                    summary = "Uninstall cancelled; nothing was changed.";
                    exitCode = 0;
                    return exitCode;
                }
            }

            UninstallResult result = NlUninstall.Run(log, options.DirSpecified ? options.AppDir : null, out exitCode);
            summary = BuildSummary(result);
            if (!options.Silent)
            {
                MessageBox.Show(summary, AppName + " - Uninstall", MessageBoxButtons.OK,
                                result.RebootRecommended ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
            }
            else
            {
                Console.WriteLine(summary);
            }
            return exitCode;
        }
        catch (SetupException ex)
        {
            log.Error("Uninstall failed: " + ex.Message);
            summary = "Uninstall failed:\r\n" + ex.Message;
            if (!options.Silent)
                MessageBox.Show(summary + "\r\n\r\nSee the log:\r\n" + log.Path,
                                AppName + " Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Error);
            else
                Console.WriteLine("ERROR: " + summary);
            exitCode = ex.ExitCode;
            return exitCode;
        }
        catch (Exception ex)
        {
            log.Error("Unexpected error: " + ex);
            summary = "Uninstall failed with an unexpected error:\r\n" + ex.Message;
            if (!options.Silent)
                MessageBox.Show(summary + "\r\n\r\nSee the log:\r\n" + log.Path,
                                AppName + " Uninstall", MessageBoxButtons.OK, MessageBoxIcon.Error);
            else
                Console.WriteLine("ERROR: " + summary);
            exitCode = 10;
            return exitCode;
        }
        finally
        {
            log.Result(exitCode, summary);
            log.Dispose();
        }
    }

    private static Options ParseOptions(string[] args)
    {
        var options = new Options();
        for (int i = 0; i < args.Length; i++)
        {
            string arg = args[i] ?? "";
            string lower = arg.ToLowerInvariant();
            if (lower == "/silent" || lower == "--silent")
            {
                options.Silent = true;
            }
            else if (lower == "/?" || lower == "/help" || lower == "--help" || lower == "-h")
            {
                options.Help = true;
            }
            else if (lower.StartsWith("/dir=") || lower.StartsWith("--dir="))
            {
                string value = arg.Substring(arg.IndexOf('=') + 1).Trim().Trim('"');
                if (value.Length == 0) throw new SetupException(2, "The /dir option requires a folder path.");
                options.AppDir = value;
                options.DirSpecified = true;
            }
            else if (lower == "/dir" || lower == "--dir")
            {
                if (i + 1 >= args.Length) throw new SetupException(2, "The --dir option requires a folder path.");
                options.AppDir = args[++i].Trim().Trim('"');
                if (options.AppDir.Length == 0) throw new SetupException(2, "The --dir option requires a folder path.");
                options.DirSpecified = true;
            }
            else if (lower == "--mode=uninstall" || lower == "/mode=uninstall" || lower == "--mode" || lower == "/mode")
            {
                // Accepted for symmetry with NLMatrixSetup.exe --mode uninstall.
                if ((lower == "--mode" || lower == "/mode") && i + 1 < args.Length)
                {
                    string value = args[++i].Trim().ToLowerInvariant();
                    if (value != "uninstall")
                        throw new SetupException(2, "The uninstaller only supports '--mode uninstall'.");
                }
                else if ((lower == "--mode" || lower == "/mode") && i + 1 >= args.Length)
                {
                    throw new SetupException(2, "The --mode option requires a value.");
                }
            }
            else
            {
                throw new SetupException(2, "Unknown command-line option: " + arg);
            }
        }
        return options;
    }

    private static string BuildSummary(UninstallResult result)
    {
        var sb = new System.Text.StringBuilder();
        if (result.NothingInstalled)
        {
            sb.AppendLine(AppName + " is not installed on this computer; nothing had to be removed.");
            sb.AppendLine("Log: " + Path.Combine(Path.GetTempPath(), "NLMatrixSetup-uninstall.log"));
            return sb.ToString();
        }
        sb.AppendLine(AppName + " was uninstalled.");
        sb.AppendLine("  Audio devices removed          : " + result.RemovedDevices);
        sb.AppendLine("  Driver packages removed/queued : " + result.RemovedPackages);
        sb.AppendLine("  Test certificates removed      : " + result.RemovedCertificates);
        sb.AppendLine("  Application files              : " +
                      (result.AppFilesScheduled ? "scheduled for deletion after exit" :
                       (result.AppFilesRemoved ? "removed" : "not removed")));
        sb.AppendLine();
        sb.AppendLine("Windows audio defaults and other vendors' audio devices were not changed.");
        sb.AppendLine("User presets/config in %APPDATA%\\" + AppName + " were kept.");
        foreach (var warning in result.Warnings) sb.AppendLine("  Warning: " + warning);
        if (result.RebootRecommended)
            sb.AppendLine("\r\nA reboot is required to finish removing the driver or its endpoints.");
        sb.AppendLine("Log: " + Path.Combine(Path.GetTempPath(), "NLMatrixSetup-uninstall.log"));
        return sb.ToString();
    }
}
