// NLMatrixInstaller.cs — NL Audio Matrix setup, repair, upgrade and uninstall.
//
// Starting the executable with no arguments opens the setup wizard (install,
// upgrade, repair, uninstall, read-only diagnostics).  The command line below is
// still supported for automation and for Add/Remove Programs.
//
// CLI
//   NLMatrixSetup.exe [--mode install|repair|upgrade|uninstall|diagnose|verify]
//                     [/silent] [/launch] [/dir=<path>] [/accept-testsigning]
//                     [/app-selftest] [/register-post-reboot-verify]
//
//   /silent                 -> no UI (for deployment automation)
//   /launch                 -> start NLMatrixUI.exe after a successful install.
//   /dir=<path>             -> installation folder (default C:\NLMatrix).
//   --mode install          -> install, or bring an existing installation to this
//                              package version (fresh create or devcon update).
//   --mode repair           -> restore missing/inconsistent app files, certificate,
//                              driver package, service, endpoints and shortcuts;
//                              requires an existing installation.
//   --mode upgrade          -> replace an older NL Audio Matrix installation with
//                              this package, keeping %APPDATA% presets/config and
//                              removing only obsolete NL driver packages.
//   --mode uninstall        -> remove devices, driver packages, app files and
//                              entries (exit 0 even when nothing is installed).
//   --mode diagnose         -> read-only report: driver, PnP identity, DriverStore,
//                              endpoints, Secure Boot/TESTSIGNING/HVCI, Code Integrity.
//   --mode verify           -> read-only health verification (exit 9 when not healthy);
//                              /app-selftest starts and stops the application.
//   Unknown options or modes print the usage text and exit with code 2.
//
// Exit code table (also printed by --help and written to the log)
//   0  Success (nothing further required)
//   1  Administrator privileges required
//   2  Invalid command line / unsupported Windows or architecture
//   3  Embedded payload missing, unreadable or incomplete
//   4  Payload integrity check failed (SHA256SUMS.txt)
//   5  Application files, Start Menu or Add/Remove entry could not be deployed
//   6  Test certificate trust operation failed
//   7  Driver package staging failed (pnputil)
//   8  Device install/update/remove failed (devcon / pnputil)
//   9  Driver service or NL audio endpoints not healthy
//  10  Unexpected error (see the log)
//  11  Mode precondition not met (upgrade/repair with nothing installed)
//  12  Test signing could not be enabled (Secure Boot / policy)
//  20  Success; reboot required to finalize (test signing or driver pending)
//
// Every run writes a structured log to %TEMP%\NLMatrixSetup-<mode>.log.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Windows.Forms;

[assembly: AssemblyTitle("NL Audio Matrix Setup")]
[assembly: AssemblyDescription("NL Audio Matrix installer, repair, upgrade and uninstall")]
[assembly: AssemblyCompany("Nicola Lanci")]
[assembly: AssemblyProduct("NL Audio Matrix")]
[assembly: AssemblyCopyright("Copyright (c) Nicola Lanci")]
[assembly: AssemblyVersion("0.6.0.0")]
[assembly: AssemblyFileVersion("0.6.0.0")]
[assembly: AssemblyInformationalVersion("0.6.0-setup")]

internal sealed class SetupOptions
{
    public string Mode = "install";
    public bool ModeSpecified;
    public bool Silent;
    public bool Launch;
    public bool AcceptTestSigning;
    public bool Help;
    public bool DirSpecified;
    public bool AppSelfTest;
    public bool ExpectAbsent;
    public bool RegisterPostRebootVerify;
    public bool Gui;
    public string AppDir = NlConst.DefaultAppDir;
}

internal static class Installer
{
    private const string AppName = NlConst.AppName;
    private static string tempDirectory = "";

    internal static string TemporaryDirectory { get { return tempDirectory; } }

    private sealed class InstallationState
    {
        public string AppDir = "";
        public string Devcon = "";
        public string PreviousAppVersion = "";
        public string PreviousBuildId = "";
        public string PreviousDriverVersion = "";
        public bool ArpEntry;
        public bool StartMenu;
        public bool AppPresent;
        public bool DevicePresent;
        public bool LegacyPresent;
        public bool ServiceRunning;
        public bool CertificateTrusted;
        public readonly List<string> AppProblems = new List<string>();
        public readonly List<DriverPackageInfo> DriverPackages = new List<DriverPackageInfo>();
        public EndpointReport Endpoints;

        public bool AnyInstallation
        {
            get
            {
                return ArpEntry || StartMenu || AppPresent || DriverPackages.Count > 0 ||
                       DevicePresent || LegacyPresent;
            }
        }
    }

    internal static string UsageText()
    {
        return
            "NL Audio Matrix Setup — usage\r\n" +
            "\r\n" +
            "  Start NLMatrixSetup.exe without arguments to open the setup wizard\r\n" +
            "  (install, upgrade, repair, uninstall and read-only diagnostics).\r\n" +
            "\r\n" +
            "  NLMatrixSetup.exe [--mode install|repair|upgrade|uninstall|diagnose|verify]\r\n" +
            "                    [/silent] [/launch] [/dir=<path>] [/accept-testsigning]\r\n" +
            "                    [/app-selftest] [/register-post-reboot-verify]\r\n" +
            "\r\n" +
            "Modes:\r\n" +
            "  --mode install    Install, or update an existing installation to this package (default)\r\n" +
            "  --mode repair     Restore missing/inconsistent files, driver, service and shortcuts\r\n" +
            "  --mode upgrade    Replace an older installation, keeping presets/config\r\n" +
            "  --mode uninstall  Remove NL Audio Matrix and its virtual audio driver\r\n" +
            "  --mode diagnose   Read-only report: driver, PnP identity, security, endpoints\r\n" +
            "  --mode verify     Read-only health check; /app-selftest also starts/stops the app\r\n" +
            "\r\n" +
            "Options:\r\n" +
            "  /silent           No UI (for deployment automation)\r\n" +
            "  /launch           Start NL Audio Matrix after a successful install/repair/upgrade\r\n" +
            "  /dir=<path>       Installation folder (default C:\\NLMatrix)\r\n" +
            "  /accept-testsigning  Allow setup to enable Windows test signing when the\r\n" +
            "                       test-signed NL driver needs it (never done silently)\r\n" +
            "  /register-post-reboot-verify  Ask the next logon to verify the installation\r\n" +
            "  /? --help         Show this help\r\n" +
            "\r\n" +
            "Exit codes:\r\n" +
            "   0 success            1 administrator required   2 invalid command line/OS\r\n" +
            "   3 payload missing    4 payload integrity failed 5 app files/entries failed\r\n" +
            "   6 certificate failed 7 pnputil staging failed   8 devcon device operation failed\r\n" +
            "   9 service/endpoints  10 unexpected error        11 mode precondition not met\r\n" +
            "  12 security config blocks test signing           20 success, reboot required\r\n";
    }

    [STAThread]
    private static int Main(string[] args)
    {
        SetupOptions options;
        try
        {
            options = ParseOptions(args);
        }
        catch (SetupException ex)
        {
            var usageLog = new SetupLog(Path.Combine(Path.GetTempPath(), "NLMatrixSetup-usage.log"));
            usageLog.Header("NL Audio Matrix Setup", "usage", args, null);
            usageLog.Error(ex.Message);
            PrintUsage(ex.Message);
            bool silentArgument = Array.Exists(args, a =>
                string.Equals(a, "/silent", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(a, "--silent", StringComparison.OrdinalIgnoreCase));
            if (!silentArgument)
                ShowError(ex.Message + "\r\n\r\nLog: " + usageLog.Path, false);
            usageLog.Result(ex.ExitCode, "usage error");
            usageLog.Dispose();
            return ex.ExitCode;
        }

        if (options.Help)
        {
            PrintUsage("");
            if (!options.Silent)
                MessageBox.Show(UsageText(), AppName + " Setup", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return 0;
        }

        // Double-clicking the setup (or passing only cosmetic switches) opens the
        // wizard.  An explicit --mode keeps the scriptable command-line behaviour.
        if (options.Gui) return NlSetupUi.Run(options, args);
        return RunCommandLine(options, args);
    }

    internal static int RunCommandLine(SetupOptions options, string[] args)
    {
        var logPath = Path.Combine(Path.GetTempPath(), "NLMatrixSetup-" + options.Mode + ".log");
        var log = new SetupLog(logPath);
        int exitCode = 10;
        string summary = "";
        bool rebootRequired = false;
        try
        {
            log.Header("NL Audio Matrix Setup", options.Mode, args,
                       "Install folder: " + options.AppDir + " (from command line: " + options.DirSpecified + ")");
            Console.WriteLine("NL Audio Matrix Setup: mode=" + options.Mode + " log=" + log.Path);

            ValidateEnvironment();
            exitCode = Execute(options, log, out summary, out rebootRequired);
        }
        catch (SetupException ex)
        {
            log.Error("Setup failed: " + ex.Message);
            summary = "Setup failed:\r\n" + ex.Message;
            ShowError(summary + "\r\n\r\nSee the log for details:\r\n" + log.Path, options.Silent);
            exitCode = ex.ExitCode;
        }
        catch (Exception ex)
        {
            log.Error("Unexpected error: " + ex);
            summary = "Setup failed with an unexpected error:\r\n" + ex.Message;
            ShowError(summary + "\r\n\r\nSee the log for details:\r\n" + log.Path, options.Silent);
            exitCode = 10;
        }
        finally
        {
            RecordPendingReboot(options, log, exitCode, rebootRequired);
            log.Result(exitCode, summary);
            CleanupPayload();
            log.Dispose();
        }
        return exitCode;
    }

    // Shared by the command line and the wizard: validates the host, then runs the
    // requested mode.  rebootRequired is true when a reboot is needed to finish.
    internal static void ValidateEnvironment()
    {
        if (!Sys.IsAdmin())
            throw new SetupException(1,
                "NL Audio Matrix Setup requires administrator privileges.\r\n" +
                "Right-click NLMatrixSetup.exe and choose 'Run as administrator'.");
        if (!Environment.Is64BitOperatingSystem || !Environment.Is64BitProcess)
            throw new SetupException(2,
                "NL Audio Matrix requires 64-bit Windows and the 64-bit setup executable.");
    }

    internal static int Execute(SetupOptions options, SetupLog log, out string summary, out bool rebootRequired)
    {
        summary = "";
        rebootRequired = false;
        if (options.Mode == "diagnose") return RunDiagnoseMode(options, log, out summary);
        if (options.Mode == "verify") return RunVerifyMode(options, log, out summary);
        if (options.Mode == "uninstall") return RunUninstallMode(options, log, out summary);
        return RunSetup(options, log, out summary, out rebootRequired);
    }

    internal static void RecordPendingReboot(SetupOptions options, SetupLog log, int exitCode, bool rebootRequired)
    {
        if (!rebootRequired) return;
        if (exitCode != 20) return;
        string phase = options.Mode == "uninstall" ? "uninstall"
                     : options.Mode == "repair" ? "repair"
                     : options.Mode == "upgrade" ? "upgrade" : "install";
        string version = NlDiagnostics.ReadVersionValue(options.AppDir, "version");
        NlPendingReboot.Write(log, phase, options.AppDir, version);
        if (options.RegisterPostRebootVerify && options.Mode != "uninstall")
            NlPendingReboot.RegisterRunOnce(log, options.AppDir);
    }

    internal static void CleanupPayload()
    {
        if (!string.IsNullOrEmpty(tempDirectory))
        {
            try { Directory.Delete(tempDirectory, true); } catch { }
            tempDirectory = "";
        }
    }

    private static SetupOptions ParseOptions(string[] args)
    {
        var options = new SetupOptions();
        bool modeSpecified = false;
        for (int i = 0; i < args.Length; i++)
        {
            string arg = args[i] ?? "";
            string lower = arg.ToLowerInvariant();
            if (lower == "/silent" || lower == "--silent")
            {
                options.Silent = true;
            }
            else if (lower == "/launch" || lower == "--launch")
            {
                options.Launch = true;
            }
            else if (lower == "/accept-testsigning" || lower == "--accept-testsigning")
            {
                options.AcceptTestSigning = true;
            }
            else if (lower == "/app-selftest" || lower == "--app-selftest")
            {
                options.AppSelfTest = true;
            }
            else if (lower == "/register-post-reboot-verify" || lower == "--register-post-reboot-verify")
            {
                options.RegisterPostRebootVerify = true;
            }
            else if (lower == "/expect-absent" || lower == "--expect-absent")
            {
                options.ExpectAbsent = true;
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
            else if (lower == "--dir" || lower == "/dir")
            {
                if (i + 1 >= args.Length) throw new SetupException(2, "The --dir option requires a folder path.");
                options.AppDir = args[++i].Trim().Trim('"');
                if (options.AppDir.Length == 0) throw new SetupException(2, "The --dir option requires a folder path.");
                options.DirSpecified = true;
            }
            else if (lower.StartsWith("--mode=") || lower.StartsWith("/mode="))
            {
                if (modeSpecified) throw new SetupException(2, "The --mode option was specified more than once.");
                options.Mode = ParseMode(arg.Substring(arg.IndexOf('=') + 1));
                modeSpecified = true;
            }
            else if (lower == "--mode" || lower == "/mode")
            {
                if (modeSpecified) throw new SetupException(2, "The --mode option was specified more than once.");
                if (i + 1 >= args.Length) throw new SetupException(2, "The --mode option requires a value.");
                options.Mode = ParseMode(args[++i]);
                modeSpecified = true;
            }
            else
            {
                throw new SetupException(2, "Unknown command-line option: " + arg);
            }
        }
        options.ModeSpecified = modeSpecified;
        options.Gui = !options.ModeSpecified && !options.Silent && !options.Help;
        return options;
    }

    private static string ParseMode(string value)
    {
        string mode = (value ?? "").Trim().ToLowerInvariant();
        if (mode == "install" || mode == "repair" || mode == "upgrade" || mode == "uninstall"
            || mode == "diagnose" || mode == "verify")
            return mode;
        throw new SetupException(2,
            "Unknown mode '" + value + "'. Valid modes are: install, repair, upgrade, uninstall, diagnose, verify.");
    }

    private static void PrintUsage(string error)
    {
        if (!string.IsNullOrEmpty(error)) Console.WriteLine("ERROR: " + error);
        Console.WriteLine(UsageText());
    }

    private static void ShowInfo(string text, bool warning)
    {
        if (!string.IsNullOrEmpty(text)) Console.WriteLine(text);
        MessageBox.Show(text, AppName + " Setup", MessageBoxButtons.OK,
                        warning ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
    }

    private static void ShowError(string text, bool silent)
    {
        if (!string.IsNullOrEmpty(text)) Console.WriteLine("ERROR: " + text);
        if (!silent)
            MessageBox.Show(text, AppName + " Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
    }

    // ---------------------------------------------------------------- uninstall
    private static int RunUninstallMode(SetupOptions options, SetupLog log, out string summary)
    {
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
                return 0;
            }
        }

        int exitCode;
        var result = NlUninstall.Run(log, options.DirSpecified ? options.AppDir : null, out exitCode);
        summary = BuildUninstallSummary(result);
        if (!options.Silent) ShowInfo(summary, result.RebootRecommended);
        return exitCode;
    }

    private static string BuildUninstallSummary(UninstallResult result)
    {
        var sb = new StringBuilder();
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

    // ------------------------------------------------------------- diagnose
    // Read-only report used before installing: what is present, whether the
    // test-signed driver can load on this Windows host (Secure Boot/TESTSIGNING/
    // HVCI), the exact PnP identity, Code Integrity evidence, and which setup
    // action the state selects.  It never changes drivers, certificates, bcdedit
    // or Secure Boot.
    private static int RunDiagnoseMode(SetupOptions options, SetupLog log, out string summary)
    {
        summary = "";
        string payloadRoot = ExtractPayload(log);
        try
        {
            ValidatePayload(payloadRoot, log);
            string payloadVersion = ReadVersionValue(payloadRoot, "version");
            string payloadBuildId = ReadVersionValue(payloadRoot, "buildId");
            string payloadDriverVersion = NlDrivers.ParseDriverVer(
                Path.Combine(payloadRoot, "driver", NlConst.InfName));
            var report = BuildDiagnostics(options, log, payloadRoot, payloadVersion, payloadBuildId,
                                          payloadDriverVersion);
            report.Decision = DescribeDecision(report, payloadDriverVersion);
            var sb = new StringBuilder();
            report.ToText(sb);
            summary = sb.ToString();
            Console.WriteLine(summary);
            log.Info("Diagnostics: " + summary.Replace("\r\n", " | "));
            NlDiagnostics.WriteReport(log, report);
            return 0;
        }
        finally
        {
            CleanupPayload();
        }
    }

    internal static SetupDiagnosticsReport BuildDiagnostics(SetupOptions options, SetupLog log, string payloadRoot,
                                                            string packageVersion, string buildId,
                                                            string driverVersion)
    {
        string signature = DescribePackageSignature(payloadRoot);
        var device = NlPnp.Inspect(NlConst.RootHwid);
        var report = NlDiagnostics.Collect(log, "diagnose", options.AppDir, packageVersion, buildId,
                                            driverVersion, signature, device);
        report.LegacyDevicePresent = NlPnp.Inspect(NlConst.LegacyHwid).Present;
        return report;
    }

    // Read-only state scan used by the wizard before the user chooses an action.
    internal static SetupDiagnosticsReport ScanOnce(SetupOptions options, SetupLog log)
    {
        string payloadRoot = ExtractPayload(log);
        try
        {
            ValidatePayload(payloadRoot, log);
            string payloadVersion = ReadVersionValue(payloadRoot, "version");
            string payloadBuildId = ReadVersionValue(payloadRoot, "buildId");
            string payloadDriverVersion = NlDrivers.ParseDriverVer(
                Path.Combine(payloadRoot, "driver", NlConst.InfName));
            var report = BuildDiagnostics(options, log, payloadRoot, payloadVersion, payloadBuildId,
                                          payloadDriverVersion);
            report.Decision = DescribeDecision(report, payloadDriverVersion);
            return report;
        }
        finally
        {
            CleanupPayload();
        }
    }

    // --------------------------------------------------------------- verify
    private static int RunVerifyMode(SetupOptions options, SetupLog log, out string summary)
    {
        var outcome = NlVerify.Run(log, options.AppDir, "verify", 0, options.ExpectAbsent,
                                   options.AppSelfTest);
        summary = outcome.Text;
        Console.WriteLine(summary);
        log.Info("Verify: " + outcome.OneLine);
        return outcome.ExitCode;
    }

    internal static string DescribePackageSignature(string payloadRoot)
    {
        return NlSignature.Describe(Path.Combine(payloadRoot, "driver"));
    }

    private static string DescribeDecision(SetupDiagnosticsReport report, string payloadDriverVersion)
    {
        string securitySuffix = report.Security != null && report.Security.Verdict == "blocked"
            ? " [driver blocked by the Windows security configuration: follow the guidance]"
            : "";
        if (!report.Device.Present && report.LegacyDevicePresent)
            return "legacy NLMatrix device only -> install (legacy device replaced)" + securitySuffix;
        if (!report.Device.Present)
        {
            if (report.Packages.Count > 0)
                return "driver package staged but device absent/damaged -> install/repair" + securitySuffix;
            return "driver absent -> install" + securitySuffix;
        }

        bool versionOk = NlText.SameVersion(report.BoundDriverVersion, payloadDriverVersion)
                         || (report.BoundDriverVersion.Length == 0
                             && NlDrivers.HasVersion(report.Packages, payloadDriverVersion));
        if (!versionOk)
        {
            return "driver outdated (" + NlText.DisplayVersion(report.BoundDriverVersion) + " vs "
                 + NlText.DisplayVersion(payloadDriverVersion) + ") -> upgrade" + securitySuffix;
        }
        if (report.Packages.Count > 1)
            return "obsolete NL driver packages in the DriverStore (" + report.Packages.Count +
                   ") -> upgrade removes the outdated ones, keeps the current one" + securitySuffix;
        if (report.Device.InstanceCount > 1)
            return "duplicate NLMatrix device instances (" + report.Device.InstanceCount + ") -> repair removes the extras";
        if (report.Runtime == null || !report.Runtime.ServiceRunning)
            return "driver present but service not running -> repair";
        if (report.AppFileProblems > 0)
            return "application files missing or modified (" + report.AppFileProblems + ") -> repair";
        if (report.Endpoints == null || report.Endpoints.MissingExpected.Count > 0)
            return "user mode endpoints incomplete -> repair";
        if (report.Security != null && report.Security.Verdict == "blocked")
            return "installation healthy but the driver is blocked (" + report.Security.BlockReason + ")" + securitySuffix;
        return "installation current and healthy -> keep";
    }

    // ------------------------------------------------------------ install flow
    private static int RunSetup(SetupOptions options, SetupLog log, out string summary, out bool rebootRequired)
    {
        summary = "";
        rebootRequired = false;
        string payloadRoot = ExtractPayload(log);
        try
        {
            var manifest = ValidatePayload(payloadRoot, log);
            string payloadVersion = ReadVersionValue(payloadRoot, "version");
            string payloadBuildId = ReadVersionValue(payloadRoot, "buildId");
            string payloadDriverVersion = NlDrivers.ParseDriverVer(
                Path.Combine(payloadRoot, "driver", NlConst.InfName));
            if (payloadVersion.Length == 0 || payloadDriverVersion.Length == 0)
                throw new SetupException(3, "The embedded payload is incomplete (VERSION.txt or driver INF DriverVer missing).");
            log.Info("Payload: app " + payloadVersion + " build " + payloadBuildId +
                     ", driver " + NlText.DisplayVersion(payloadDriverVersion));

            var state = GatherState(options, payloadRoot, manifest, log);
            bool repair = options.Mode == "repair";
            bool upgrade = options.Mode == "upgrade";

            if ((repair || upgrade) && !state.AnyInstallation)
            {
                throw new SetupException(11,
                    (upgrade
                        ? "No existing NL Audio Matrix installation was found, so there is nothing to upgrade."
                        : "No existing NL Audio Matrix installation was found, so there is nothing to repair.") +
                    "\r\nRun with --mode install to perform a fresh installation.");
            }

            bool fresh = !state.AnyInstallation;
            var repairs = new List<string>();

            log.Step("Stopping NL Audio Matrix processes");
            NlAppFiles.StopKnownProcesses(log, options.AppDir);

            bool appDeployed = EnsureApplicationFiles(options, payloadRoot, state, manifest, log, repairs, fresh);
            NlAppFiles.CopySelf(log, options.AppDir);

            EnsureDriver(options, payloadRoot, payloadDriverVersion, state, log, repairs, ref rebootRequired);

            RemoveDuplicateInstances(options, payloadRoot, payloadDriverVersion, state, log, repairs, ref rebootRequired);

            if (!rebootRequired && state.DriverPackages.Count > 0)
            {
                int obsolete = NlDrivers.RemoveObsolete(log, payloadDriverVersion);
                if (obsolete > 0) repairs.Add(obsolete + " obsolete NL driver package(s) removed");
            }

            EnsureEntries(options, payloadVersion, state, log, repairs);

            log.Step("Verifying the eight NL audio endpoints");
            var endpoints = NlEndpoints.WaitFor(log, rebootRequired ? 5000 : 25000);
            if (endpoints.ActiveCount == 0)
            {
                if (rebootRequired)
                    log.Warn("No NL audio endpoints are active yet; this is expected until the required reboot.");
                else
                    throw new SetupException(9,
                        "The NLMatrix device is installed and the driver service is running, but no NL Mic/NL Return " +
                        "audio endpoints were enumerated. See the log for the devcon/pnputil output.");
            }
            else if (!rebootRequired && (endpoints.ActiveCapture < 4 || endpoints.ActiveRender < 4))
            {
                throw new SetupException(9,
                    "Only " + endpoints.ActiveCapture + " NL capture and " + endpoints.ActiveRender +
                    " NL render endpoints are active; 4 + 4 are required. See the log for the endpoint list.");
            }
            else if (endpoints.MissingExpected.Count > 0)
            {
                log.Warn("Only " + (NlConst.ExpectedEndpointNames.Length - endpoints.MissingExpected.Count) +
                         " of " + NlConst.ExpectedEndpointNames.Length + " expected NL endpoint names are present " +
                         "(endpoints may have been renamed). Missing: " +
                         string.Join(", ", endpoints.MissingExpected.ToArray()));
            }
            log.Info("Endpoint result: " + endpoints.Summary);

            if (options.Launch && !rebootRequired)
            {
                string ui = Path.Combine(options.AppDir, NlConst.UiExeName);
                if (File.Exists(ui))
                {
                    log.Info("Launching " + ui);
                    Process.Start(new ProcessStartInfo(ui) { WorkingDirectory = options.AppDir, UseShellExecute = true });
                }
                else log.Warn("Cannot launch: " + ui + " not found.");
            }

            summary = BuildSetupSummary(options, state, payloadVersion, payloadBuildId,
                                        payloadDriverVersion, fresh, appDeployed, repairs,
                                        rebootRequired, endpoints, log.Path, repair, upgrade);
            if (!options.Silent) ShowInfo(summary, rebootRequired);
            return rebootRequired ? 20 : 0;
        }
        finally
        {
            CleanupPayload();
        }
    }

    // A repeated install must never leave more than one root device instance.
    // Only the NLMatrix hardware ID is touched; nothing else is removed.
    private static void RemoveDuplicateInstances(SetupOptions options, string payloadRoot, string payloadDriverVersion,
                                                 InstallationState state, SetupLog log, List<string> repairs,
                                                 ref bool rebootRequired)
    {
        if (!state.DevicePresent) return;
        int instances = NlPnp.Inspect(NlConst.RootHwid).InstanceCount;
        if (instances <= 1) return;

        log.Warn("Found " + instances + " NLMatrix device instances; removing them and reinstalling one clean instance.");
        bool duplicateReboot;
        NlDevices.RemoveDevice(log, state.Devcon, NlConst.RootHwid, out duplicateReboot);
        rebootRequired |= duplicateReboot;
        state.DevicePresent = NlDevices.Exists(log, state.Devcon, NlConst.RootHwid);
        if (!state.DevicePresent && !rebootRequired)
        {
            EnsureDriver(options, payloadRoot, payloadDriverVersion, state, log, repairs, ref rebootRequired);
            repairs.Add("duplicate device instances removed");
        }
        else
        {
            repairs.Add("duplicate device instances queued for removal (reboot required)");
        }
    }

    private static InstallationState GatherState(SetupOptions options, string payloadRoot,
                                                 List<HashEntry> manifest, SetupLog log)
    {
        var state = new InstallationState { AppDir = options.AppDir };
        state.PreviousAppVersion = ReadVersionValue(options.AppDir, "version");
        state.PreviousBuildId = ReadVersionValue(options.AppDir, "buildId");
        state.ArpEntry = NlReg.HasUninstallEntry();
        state.StartMenu = Directory.Exists(NlShell.StartMenuFolder());
        state.AppPresent = File.Exists(Path.Combine(options.AppDir, NlConst.UiExeName)) ||
                           File.Exists(Path.Combine(options.AppDir, NlConst.RouterExeName)) ||
                           File.Exists(Path.Combine(options.AppDir, NlConst.UninstallerExeName));
        state.AppProblems.AddRange(NlManifest.VerifyInstall(options.AppDir, manifest, log));
        state.DriverPackages.AddRange(NlDrivers.FindInstalled(log));

        state.Devcon = NlDevices.FindDevcon(log, options.AppDir, payloadRoot);
        state.DevicePresent = NlDevices.Exists(log, state.Devcon, NlConst.RootHwid);
        state.LegacyPresent = NlDevices.Exists(log, state.Devcon, NlConst.LegacyHwid);
        state.ServiceRunning = NlDevices.ServiceRunning(log);

        state.PreviousDriverVersion = NlDevices.BoundDriverVersion(log, state.DriverPackages, NlConst.RootHwid);
        if (state.PreviousDriverVersion.Length == 0 && state.DriverPackages.Count > 0)
            state.PreviousDriverVersion = state.DriverPackages[0].DriverVer;

        string certificatePath = Path.Combine(payloadRoot, "driver", NlConst.CerName);
        X509Certificate2 certificate = NlCert.Load(certificatePath, log);
        try
        {
            state.CertificateTrusted =
                NlCert.IsTrusted(certificate, StoreName.Root) &&
                NlCert.IsTrusted(certificate, StoreName.TrustedPublisher);
        }
        finally
        {
            certificate.Dispose();
        }

        state.Endpoints = NlEndpoints.Collect(log);

        log.Info("Installation state: installed=" + state.AnyInstallation +
                 " app=" + state.PreviousAppVersion + " build=" + state.PreviousBuildId +
                 " driver=" + NlText.DisplayVersion(state.PreviousDriverVersion) +
                 " device=" + state.DevicePresent + " legacy=" + state.LegacyPresent +
                 " serviceRunning=" + state.ServiceRunning + " certTrusted=" + state.CertificateTrusted +
                 " arp=" + state.ArpEntry + " startMenu=" + state.StartMenu +
                 " appProblems=" + state.AppProblems.Count);
        return state;
    }

    private static bool EnsureApplicationFiles(SetupOptions options, string payloadRoot, InstallationState state,
                                               List<HashEntry> manifest, SetupLog log, List<string> repairs,
                                               bool fresh)
    {
        bool present = File.Exists(Path.Combine(options.AppDir, NlConst.UiExeName)) &&
                       File.Exists(Path.Combine(options.AppDir, NlConst.RouterExeName)) &&
                       File.Exists(Path.Combine(options.AppDir, NlConst.UninstallerExeName));
        int problems = state.AppProblems.Count;
        if (present && problems == 0)
        {
            log.Info("Application files are current (" + manifest.Count + " files verified against SHA256SUMS.txt).");
            return false;
        }

        log.Step("Deploying application files to " + options.AppDir);
        if (!present)
            log.Info("Application binaries are missing from the installation folder.");
        foreach (var problem in state.AppProblems) log.Info("  " + problem);

        NlAppFiles.Deploy(payloadRoot, options.AppDir, log);

        var after = NlManifest.VerifyInstall(options.AppDir, manifest, log);
        if (after.Count > 0)
            throw new SetupException(5,
                "Application files failed verification after deployment (" + after.Count + " problem(s)): " +
                string.Join("; ", after.ToArray()));
        repairs.Add(appProblemsDescription(present, problems, fresh));
        return true;
    }

    private static string appProblemsDescription(bool present, int problems, bool fresh)
    {
        if (fresh) return "application files deployed";
        if (!present && problems == 0) return "application files restored";
        if (!present) return "application files restored (" + problems + " missing/modified)";
        return "application files repaired (" + problems + " missing/modified)";
    }

    private static void EnsureDriver(SetupOptions options, string payloadRoot, string payloadDriverVersion,
                                     InstallationState state, SetupLog log, List<string> repairs,
                                     ref bool rebootRequired)
    {
        string inf = Path.Combine(payloadRoot, "driver", NlConst.InfName);
        string boundVersion = NlDevices.BoundDriverVersion(log, state.DriverPackages, NlConst.RootHwid);
        bool versionOk = NlText.SameVersion(boundVersion, payloadDriverVersion) ||
                         (boundVersion.Length == 0 && NlDrivers.HasVersion(state.DriverPackages, payloadDriverVersion));
        bool healthy = state.DevicePresent && state.ServiceRunning;

        log.Info("Driver check: devicePresent=" + state.DevicePresent +
                 " serviceRunning=" + state.ServiceRunning +
                 " bound=" + NlText.DisplayVersion(boundVersion) +
                 " staged=[" + NlDrivers.DescribeInstalled(state.DriverPackages) + "]" +
                 " payload=" + NlText.DisplayVersion(payloadDriverVersion) +
                 " versionOk=" + versionOk + " healthy=" + healthy);

        log.Step("Verifying test certificate trust");
        var certificate = NlCert.Load(Path.Combine(payloadRoot, "driver", NlConst.CerName), log);
        try
        {
            if (NlCert.EnsureTrusted(certificate, log))
                repairs.Add("test certificate trust restored");
        }
        finally
        {
            certificate.Dispose();
        }

        bool signingEnabled = NlDevices.IsTestSigningEnabled(log);
        if (!signingEnabled && state.DevicePresent && state.ServiceRunning)
        {
            // Some localizations render the bcdedit boolean differently.  A loaded
            // NLMatrix driver already proves that an active boot accepts the
            // test-signed package, so never force a spurious reboot in that case.
            log.Warn("The test-signing state could not be confirmed from bcdedit, but the NLMatrix driver is " +
                     "already loaded; assuming test signing is active.");
            signingEnabled = true;
        }
        if (!signingEnabled)
        {
            if (!options.AcceptTestSigning)
                throw new SetupException(12,
                    "Windows test signing is not enabled, so the test-signed NLMatrix driver cannot load.\r\n\r\n" +
                    "Nothing was changed. Either enable test signing yourself (bcdedit /set testsigning on, then reboot) " +
                    "or re-run setup with /accept-testsigning if you accept that Windows change.");
            log.Step("Enabling Windows test signing (explicitly accepted with /accept-testsigning)");
            NlDevices.EnableTestSigning(log);
            rebootRequired = true;
            repairs.Add("test signing enabled (reboot required)");
        }

        if (state.LegacyPresent)
        {
            log.Step("Removing the legacy NLMatrix device");
            bool legacyReboot;
            bool removed = NlDevices.RemoveDevice(log, state.Devcon, NlConst.LegacyHwid, out legacyReboot);
            rebootRequired |= legacyReboot;
            if (removed) repairs.Add("legacy device removed");
            else
            {
                log.Warn("The legacy device could not be removed before the required reboot.");
                rebootRequired = true;
            }
        }

        if (versionOk && healthy)
        {
            log.Info("Driver package is current and the NLMatrix service is running; no driver change needed.");
            return;
        }

        if (versionOk && state.DevicePresent && !state.ServiceRunning)
        {
            string status = NlDevices.StatusText(log, state.Devcon, NlConst.RootHwid);
            if (status.IndexOf("disabled", StringComparison.OrdinalIgnoreCase) >= 0 &&
                !string.IsNullOrEmpty(state.Devcon))
            {
                log.Step("Re-enabling the disabled NLMatrix device (devcon enable)");
                var enable = Sys.Run(state.Devcon, "enable " + NlConst.RootHwid, 120000);
                log.Command(enable);
                if (enable.ExitCode == 1) rebootRequired = true;
                else if (enable.ExitCode == 0) repairs.Add("device re-enabled");
                else log.Warn("devcon enable returned exit " + enable.ExitCode + ".");
            }

            log.Step("Starting the NLMatrix driver service");
            if (NlDevices.StartService(log, 20))
            {
                repairs.Add("driver service started");
                return;
            }
            log.Warn("The driver service did not start; reinstalling the device driver.");
        }

        if (string.IsNullOrEmpty(state.Devcon) || !File.Exists(state.Devcon))
            throw new SetupException(8,
                "devcon.exe is missing from the installation folder and the payload; cannot install or update the driver.");

        if (!NlDrivers.HasVersion(state.DriverPackages, payloadDriverVersion))
        {
            log.Step("Staging NLMatrix driver package (pnputil /add-driver /install)");
            bool packageReboot;
            NlDrivers.Stage(log, inf, out packageReboot);
            rebootRequired |= packageReboot;
            repairs.Add("driver package staged (" + NlText.DisplayVersion(payloadDriverVersion) + ")");
        }
        else
        {
            log.Info("The payload driver package version is already staged in the DriverStore.");
        }

        bool deviceReboot = false;
        if (state.DevicePresent)
        {
            log.Step("Updating the NLMatrix device driver (devcon update)");
            var result = Sys.Run(state.Devcon, "update " + Sys.Quote(inf) + " " + NlConst.RootHwid, 600000);
            log.Command(result);
            if (!DevconReportedFailure(result))
            {
                if (result.ExitCode == 1)
                {
                    deviceReboot = true;
                    log.Warn("devcon update reports a reboot is required (exit 1).");
                }
                else repairs.Add("driver updated to " + NlText.DisplayVersion(payloadDriverVersion));
            }
            else if (rebootRequired)
            {
                log.Warn("devcon update failed before the required reboot (exit " + result.ExitCode +
                         "); it will be retried by --mode repair after the reboot.");
                deviceReboot = true;
            }
            else
            {
                throw new SetupException(8,
                    "devcon update failed for " + NlConst.RootHwid + " (exit " + result.ExitCode + "). " +
                    NlText.Clip(result.Output, 1500));
            }
        }
        else
        {
            log.Step("Installing the NLMatrix root device (devcon install)");
            var result = Sys.Run(state.Devcon, "install " + Sys.Quote(inf) + " " + NlConst.RootHwid, 600000);
            log.Command(result);
            if (!DevconReportedFailure(result)
                || (result.ExitCode == 2
                    && result.OutputOrEmpty.IndexOf("Device node created", StringComparison.OrdinalIgnoreCase) >= 0))
            {
                if (result.ExitCode == 1)
                {
                    deviceReboot = true;
                    log.Warn("devcon install reports a reboot is required (exit 1).");
                }
                else if (result.ExitCode == 2)
                {
                    // Current devcon builds create the device node and report
                    // "Install is complete when drivers are installed..." with exit
                    // 2; that is a reboot-required install, not a failure.
                    deviceReboot = true;
                    log.Info("devcon install created the device node; the install completes after the reboot.");
                }
                else repairs.Add("device installed");
            }
            else if (rebootRequired)
            {
                log.Warn("devcon install failed before the required reboot (exit " + result.ExitCode +
                         "); it will be retried by --mode repair after the reboot.");
                deviceReboot = true;
            }
            else
            {
                throw new SetupException(8,
                    "devcon install failed for " + NlConst.RootHwid + " (exit " + result.ExitCode + "). " +
                    NlText.Clip(result.Output, 1500));
            }
        }
        rebootRequired |= deviceReboot;

        log.Step("Starting the NLMatrix driver service");
        bool started = NlDevices.StartService(log, 20);
        if (!started && !rebootRequired)
        {
            log.Warn("The driver service did not start; trying devcon restart.");
            bool restartReboot;
            NlDevices.RestartDevice(log, state.Devcon, NlConst.RootHwid, out restartReboot);
            rebootRequired |= restartReboot;
            started = NlDevices.StartService(log, 20);
        }
        if (started)
        {
            repairs.Add("driver service running");
        }
        else if (!rebootRequired)
        {
            throw new SetupException(9,
                "The NLMatrix driver service (" + NlConst.ServiceName + ") did not start. " +
                "See the log for sc/devcon output.");
        }
        else
        {
            log.Warn("The driver service is not running yet; a reboot is required to complete the driver installation.");
        }
    }

    // devcon.exe return codes (Microsoft docs): 0 success, 1 reboot required,
    // 2 failure, 3 syntax error.  Some devcon builds print a failure message while
    // returning 1, so an exit code of 1 counts as success only without failure text.
    private static bool DevconReportedFailure(ExecResult result)
    {
        if (!result.Started || result.TimedOut) return true;
        if (result.ExitCode == 2 || result.ExitCode == 3) return true;
        if (result.ExitCode != 0 && result.ExitCode != 1) return true;
        if (result.ExitCode == 1)
        {
            string text = result.OutputOrEmpty;
            if (text.IndexOf("failed", StringComparison.OrdinalIgnoreCase) >= 0 ||
                text.IndexOf("error", StringComparison.OrdinalIgnoreCase) >= 0)
                return true;
        }
        return false;
    }

    private static void EnsureEntries(SetupOptions options, string payloadVersion, InstallationState state,
                                      SetupLog log, List<string> repairs)
    {
        log.Step("Writing Start Menu and Add/Remove Programs entries");
        bool setupCopy = File.Exists(Path.Combine(options.AppDir, NlConst.SetupExeName));
        NlReg.WriteUninstallEntry(options.AppDir, payloadVersion,
            Path.Combine(options.AppDir, NlConst.UninstallerExeName),
            Path.Combine(options.AppDir, NlConst.UiExeName),
            NlAppFiles.DirectorySizeKb(options.AppDir), setupCopy, log);

        if (!NlShell.WriteStartMenu(log, options.AppDir))
            throw new SetupException(5, "The Start Menu entries could not be created; see the log for details.");

        if (!state.ArpEntry) repairs.Add("Add/Remove Programs entry created");
        if (!state.StartMenu) repairs.Add("Start Menu shortcuts created");
    }

    private static string BuildSetupSummary(SetupOptions options, InstallationState state,
                                            string appVersion, string buildId, string driverVersion,
                                            bool fresh, bool appDeployed, List<string> repairs,
                                            bool rebootRequired, EndpointReport endpoints, string logPath,
                                            bool repair, bool upgrade)
    {
        var sb = new StringBuilder();
        string previousApp = state.PreviousAppVersion.Length > 0 ? state.PreviousAppVersion : "not installed";
        string previousDriver = state.PreviousDriverVersion.Length > 0
            ? NlText.DisplayVersion(state.PreviousDriverVersion) : "not installed";

        if (repair)
        {
            if (repairs.Count == 0 && !appDeployed)
            {
                sb.AppendLine(AppName + " " + appVersion + ": no repairs were needed; the installation is healthy.");
            }
            else
            {
                sb.AppendLine(AppName + " " + appVersion + ": repair completed. Repaired " + repairs.Count + " item(s):");
                foreach (var item in repairs) sb.AppendLine("  - " + item);
            }
        }
        else if (upgrade)
        {
            if (repairs.Count == 0 && !appDeployed)
            {
                sb.AppendLine(AppName + " " + appVersion + " is already installed; nothing to upgrade.");
            }
            else
            {
                sb.AppendLine(AppName + " upgraded in " + options.AppDir + ".");
                sb.AppendLine("  Application: " + previousApp + " -> " + appVersion + " (build " + buildId + ")");
                sb.AppendLine("  Driver     : " + previousDriver + " -> " + NlText.DisplayVersion(driverVersion));
            }
        }
        else
        {
            if (repairs.Count == 0 && !appDeployed && !rebootRequired)
            {
                sb.AppendLine(AppName + " " + appVersion + " is already installed and up to date; nothing to change.");
            }
            else
            {
                sb.AppendLine((fresh ? "Installed " : "Updated ") + AppName + " " + appVersion + " in " + options.AppDir + ".");
                sb.AppendLine("  Build ID : " + buildId);
                if (!fresh)
                {
                    sb.AppendLine("  Application: " + previousApp + " -> " + appVersion);
                    sb.AppendLine("  Driver     : " + previousDriver + " -> " + NlText.DisplayVersion(driverVersion));
                }
                else
                {
                    sb.AppendLine("  Driver     : " + NlText.DisplayVersion(driverVersion));
                }
            }
        }

        if (repairs.Count > 0 && !repair)
        {
            sb.AppendLine("  Changes:");
            foreach (var item in repairs) sb.AppendLine("    - " + item);
        }

        if (endpoints != null)
            sb.AppendLine("  Audio endpoints: " + endpoints.ActiveCount + " active of " + endpoints.TotalCount +
                          (endpoints.MissingExpected.Count > 0
                              ? " (missing: " + string.Join(", ", endpoints.MissingExpected.ToArray()) + ")"
                              : ""));

        if (rebootRequired)
            sb.AppendLine("\r\nA reboot is required to finish setup. Run --mode repair after the reboot to complete the driver installation.");

        sb.AppendLine("Log: " + logPath);
        return sb.ToString();
    }

    // ------------------------------------------------------------- payload
    private static string ExtractPayload(SetupLog log)
    {
        tempDirectory = Path.Combine(Path.GetTempPath(), "NLMatrixSetup-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(tempDirectory);
        string zipPath = Path.Combine(tempDirectory, "payload.zip");
        using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
        {
            if (stream == null)
                throw new SetupException(3, "The embedded payload.zip resource is missing from the setup executable.");
            using (var file = File.Create(zipPath)) stream.CopyTo(file);
        }
        long zipSize = new FileInfo(zipPath).Length;
        log.Info("Embedded payload extracted to " + tempDirectory + " (" + zipSize + " bytes).");
        try
        {
            ZipFile.ExtractToDirectory(zipPath, tempDirectory);
        }
        catch (Exception ex)
        {
            throw new SetupException(3, "The embedded payload.zip could not be extracted: " + ex.Message);
        }

        string root = Path.Combine(tempDirectory, "payload");
        if (!Directory.Exists(root))
        {
            // Tolerate a zip built without the "payload/" top-level folder.
            root = tempDirectory;
        }
        log.Info("Payload root: " + root);
        return root;
    }

    private static List<HashEntry> ValidatePayload(string payloadRoot, SetupLog log)
    {
        var manifest = NlManifest.Parse(Path.Combine(payloadRoot, "SHA256SUMS.txt"));
        if (manifest.Count == 0)
            throw new SetupException(4, "SHA256SUMS.txt in the payload contains no file entries.");

        var problems = NlManifest.VerifyPayload(payloadRoot, manifest, log);
        log.Info("Payload integrity: " + manifest.Count + " files hashed, " + problems.Count + " problem(s).");
        if (problems.Count > 0)
        {
            int shown = Math.Min(5, problems.Count);
            throw new SetupException(4,
                "Payload integrity check failed (" + problems.Count + " problem(s)). " +
                string.Join("; ", problems.GetRange(0, shown).ToArray()) +
                (problems.Count > shown ? "; ..." : ""));
        }

        foreach (var relative in new[]
                 {
                     Path.Combine("app", NlConst.UiExeName),
                     Path.Combine("app", NlConst.RouterExeName),
                     Path.Combine("app", NlConst.UninstallerExeName),
                     Path.Combine("driver", NlConst.SysName),
                     Path.Combine("driver", NlConst.InfName),
                     Path.Combine("driver", NlConst.CatName),
                     Path.Combine("driver", NlConst.CerName),
                     Path.Combine("driver", NlConst.DevconName),
                     "VERSION.txt"
                 })
        {
            if (!File.Exists(Path.Combine(payloadRoot, relative)))
                throw new SetupException(3, "The embedded payload is incomplete: missing " + relative);
        }
        return manifest;
    }

    private static string ReadVersionValue(string directory, string key)
    {
        try
        {
            string file = Path.Combine(directory, "VERSION.txt");
            if (!File.Exists(file)) return "";
            foreach (var raw in File.ReadAllLines(file))
            {
                int index = raw.IndexOf('=');
                if (index <= 0) continue;
                if (!raw.Substring(0, index).Trim().Equals(key, StringComparison.OrdinalIgnoreCase)) continue;
                return raw.Substring(index + 1).Trim();
            }
        }
        catch { }
        return "";
    }
}
