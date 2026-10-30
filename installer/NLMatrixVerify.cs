// NLMatrixVerify.cs — post-reboot installer verification.
//
// Compiled twice:
//   * into NLMatrixVerify.exe (no administrator manifest): the small helper the
//     installer registers in RunOnce after a reboot, so the next logon confirms
//     that the driver, the eight endpoints and the application are healthy
//     without a UAC prompt;
//   * into NLMatrixSetup.exe, where "--mode verify" uses the same checks for the
//     automated coordinator (with an optional application start/stop self-test).
//
// It never changes drivers or system settings: it reads the state, writes a JSON
// report under ProgramData (or LocalAppData) and reports the exit code.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Windows.Forms;

internal sealed class VerifyOutcome
{
    public bool Healthy;
    public string Text = "";
    public string ReportPath = "";
    public int ExitCode = 9;

    public string OneLine
    {
        get { return Text.Replace("\r", " ").Replace("\n", " | "); }
    }
}

internal static class NlVerify
{
    // Runs the read-only verification.  endpointWaitMs lets a post-reboot run wait
    // for the driver to finish initialising before judging.
    public static VerifyOutcome Run(SetupLog log, string appDir, string kind, int endpointWaitMs,
                                    bool expectAbsent, bool appSelfTest)
    {
        var outcome = new VerifyOutcome();
        log.Step("Verifying " + (expectAbsent ? "removal" : "installation") + " in " + appDir);

        string packageVersion = NlDiagnostics.ReadVersionValue(appDir, "version");
        string packageBuildId = NlDiagnostics.ReadVersionValue(appDir, "buildId");
        string driverVersion = NlDrivers.ParseDriverVer(System.IO.Path.Combine(appDir, "driver", NlConst.InfName));
        string signature = DescribeSignature(appDir);

        if (!expectAbsent && endpointWaitMs > 0)
        {
            log.Step("Waiting up to " + (endpointWaitMs / 1000) + "s for the NL audio endpoints");
            var report = NlEndpoints.WaitFor(log, endpointWaitMs);
            log.Info("Endpoint wait finished: " + report.Summary);
        }

        var device = NlPnp.Inspect(NlConst.RootHwid);

        if (expectAbsent)
        {
            VerifyAbsence(log, outcome, appDir, packageVersion, packageBuildId, driverVersion, signature, device);
            return outcome;
        }

        var diagnostics = NlDiagnostics.Collect(log, kind, appDir, packageVersion, packageBuildId,
                                                driverVersion, signature, device);

        if (appSelfTest)
        {
            RunApplicationSelfTest(log, diagnostics);
            diagnostics.AppSelfTestRan = true;
            diagnostics.AppSelfTestOk = selfTestOk;
            diagnostics.AppSelfTestSummary = selfTestSummary;
        }

        outcome.ReportPath = NlDiagnostics.WriteReport(log, diagnostics);
        outcome.Healthy = diagnostics.Healthy && (appSelfTest ? selfTestOk : selfTestSkippedOrOk());
        outcome.ExitCode = outcome.Healthy ? 0 : 9;

        var sb = new StringBuilder();
        sb.AppendLine("NL Audio Matrix verification — " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture));
        sb.AppendLine("  Package     : " + Safe(diagnostics.PackageVersion, "unknown") +
                      " (driver " + Safe(diagnostics.PackageDriverVersion, "unknown") + ")");
        sb.AppendLine("  Application : " + Safe(diagnostics.InstalledAppVersion, "not installed") +
                      ", " + diagnostics.AppFileProblems + " file problem(s)");
        sb.AppendLine("  Driver file : " + (diagnostics.Runtime != null && diagnostics.Runtime.LoadedFilePresent
            ? Safe(diagnostics.Runtime.LoadedFileVersion, "unknown version") + " (" +
              diagnostics.Runtime.LoadedFileBytes + " bytes)"
            : "missing"));
        sb.AppendLine("  Service     : " + (diagnostics.Runtime != null && diagnostics.Runtime.ServiceRunning ? "running" : "stopped"));
        sb.AppendLine("  PnP device  : " + (diagnostics.Device != null && diagnostics.Device.Present
            ? Safe(diagnostics.Device.InstanceId, "present") + ", problem=" +
              (diagnostics.Device.ProblemText.Length > 0 ? diagnostics.Device.ProblemText : "none")
            : "absent"));
        sb.AppendLine("  Endpoints   : " + (diagnostics.Endpoints != null
            ? diagnostics.Endpoints.ActiveCount + " active of " + diagnostics.Endpoints.TotalCount +
              " (capture " + diagnostics.Endpoints.ActiveCapture + ", render " + diagnostics.Endpoints.ActiveRender + ")"
            : "unknown"));
        sb.AppendLine("  Security    : test signing " + Safe(diagnostics.Security.TestSigning, "unknown") +
                      ", Secure Boot " + Safe(diagnostics.Security.SecureBoot, "unknown") +
                      ", HVCI " + Safe(diagnostics.Security.Hvci, "unknown"));
        if (diagnostics.CodeIntegrity != null && diagnostics.CodeIntegrity.DriverEvents > 0)
            sb.AppendLine("  CodeIntegrity: " + diagnostics.CodeIntegrity.DriverEvents + " rejection event(s) for the NL driver");
        if (appSelfTest)
            sb.AppendLine("  App start/stop: " + selfTestSummary);
        sb.AppendLine("  Result      : " + (outcome.Healthy ? "READY" : "NOT READY"));
        if (!outcome.Healthy)
        {
            sb.AppendLine("  Problems:");
            foreach (var problem in diagnostics.Problems) sb.AppendLine("    - " + problem);
        }
        if (outcome.ReportPath.Length > 0) sb.AppendLine("  Report      : " + outcome.ReportPath);
        sb.AppendLine("  Log         : " + log.Path);
        outcome.Text = sb.ToString();
        return outcome;
    }

    // ------------------------------------------------------- removal verification
    private static void VerifyAbsence(SetupLog log, VerifyOutcome outcome, string appDir, string packageVersion,
                                      string packageBuildId, string driverVersion, string signature,
                                      DeviceHealth device)
    {
        var endpoints = NlEndpoints.Collect(log);
        var packages = NlDrivers.FindInstalled(log);
        bool certTrusted = NlCert.IsAnyTrusted(log);
        bool service = NlDevices.ServiceExists(log);
        bool arp = NlReg.HasUninstallEntry();
        bool startMenu = Directory.Exists(NlShell.StartMenuFolder());
        bool appPresent = Directory.Exists(appDir) && File.Exists(System.IO.Path.Combine(appDir, NlConst.UiExeName));

        var problems = new List<string>();
        if (device.Present)
            problems.Add("the NLMatrix device is still present (" + device.InstanceCount + " instance(s))");
        if (packages.Count > 0)
            problems.Add(packages.Count + " NLMatrix driver package(s) are still staged");
        if (certTrusted) problems.Add("the NL test certificate is still trusted");
        if (service) problems.Add("the NLMatrix driver service still exists");
        if (arp) problems.Add("the Add/Remove Programs entry still exists");
        if (startMenu) problems.Add("the Start Menu entries still exist");
        if (appPresent) problems.Add("the application files still exist");
        if (endpoints.ActiveCount > 0)
            problems.Add(endpoints.ActiveCount + " NL audio endpoint(s) are still active");
        else if (endpoints.TotalCount > 0)
            log.Info(endpoints.TotalCount + " inactive endpoint registry entry(ies) remain; Windows removes them " +
                     "on its own and they cannot be used by applications.");

        var report = new SetupDiagnosticsReport
        {
            Kind = "verify-uninstall",
            Utc = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture),
            AppDir = appDir,
            PackageVersion = packageVersion,
            PackageBuildId = packageBuildId,
            PackageDriverVersion = driverVersion,
            PackageSignature = signature,
            Runtime = new DriverRuntimeInfo { ServicePresent = service },
            Device = device,
            Endpoints = endpoints,
            Pending = NlPendingReboot.Read()
        };
        report.Packages.AddRange(packages);
        report.CertificateTrusted = certTrusted;
        report.ArpEntry = arp;
        report.StartMenu = startMenu;
        report.Problems.AddRange(problems);
        report.Healthy = problems.Count == 0;
        report.ExitCode = report.Healthy ? 0 : 9;

        outcome.ReportPath = NlDiagnostics.WriteReport(log, report);
        outcome.Healthy = report.Healthy;
        outcome.ExitCode = report.ExitCode;
        var sb = new StringBuilder();
        sb.AppendLine("NL Audio Matrix removal verification — " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture));
        sb.AppendLine("  Devices removed   : " + (!device.Present));
        sb.AppendLine("  Driver packages   : " + packages.Count);
        sb.AppendLine("  Test certificate  : " + (certTrusted ? "still trusted" : "removed"));
        sb.AppendLine("  Driver service    : " + (service ? "still present" : "removed"));
        sb.AppendLine("  Endpoints         : " + endpoints.TotalCount);
        sb.AppendLine("  Application files : " + (appPresent ? "still present" : "removed"));
        sb.AppendLine("  Result            : " + (outcome.Healthy ? "CLEAN" : "NOT CLEAN"));
        foreach (var problem in problems) sb.AppendLine("    - " + problem);
        if (outcome.ReportPath.Length > 0) sb.AppendLine("  Report            : " + outcome.ReportPath);
        sb.AppendLine("  Log               : " + log.Path);
        outcome.Text = sb.ToString();
    }

    // ------------------------------------------------------ application self-test
    private static bool selfTestOk;
    private static string selfTestSummary = "not run";

    private static bool selfTestSkippedOrOk()
    {
        return selfTestOk || selfTestSummary == "not run";
    }

    private static void RunApplicationSelfTest(SetupLog log, SetupDiagnosticsReport report)
    {
        selfTestOk = false;
        selfTestSummary = "not run";
        string ui = System.IO.Path.Combine(report.AppDir, NlConst.UiExeName);
        if (!File.Exists(ui))
        {
            selfTestSummary = "NLMatrixUI.exe missing";
            report.Problems.Add("cannot run the application start/stop test: NLMatrixUI.exe is missing");
            report.Healthy = false;
            report.ExitCode = 9;
            return;
        }
        log.Step("Application start/stop self-test: " + ui);
        var started = DateTime.UtcNow;
        Process process = null;
        try
        {
            // --exit-on-close makes the window X a real exit for this run, so the
            // test exercises a normal start/stop without changing the operator's
            // saved "minimise to tray" preference.
            process = Process.Start(new ProcessStartInfo(ui, "--exit-on-close")
            {
                WorkingDirectory = report.AppDir,
                UseShellExecute = true
            });
        }
        catch (Exception ex)
        {
            selfTestSummary = "could not start: " + ex.Message;
            report.Problems.Add("the application could not be started: " + ex.Message);
            report.Healthy = false;
            report.ExitCode = 9;
            return;
        }
        if (process == null)
        {
            selfTestSummary = "process did not start";
            report.Problems.Add("the application process did not start");
            report.Healthy = false;
            report.ExitCode = 9;
            return;
        }

        bool windowSeen = false;
        var deadline = DateTime.UtcNow.AddSeconds(45);
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                if (process.HasExited) break;
                process.Refresh();
                if (process.MainWindowHandle != IntPtr.Zero) { windowSeen = true; break; }
            }
            catch { }
            System.Threading.Thread.Sleep(500);
        }
        if (process.HasExited)
        {
            selfTestSummary = "exited during startup (code " + SafeExitCode(process) + ")";
            report.Problems.Add("the application exited during startup");
            report.Healthy = false;
            report.ExitCode = 9;
            return;
        }
        if (!windowSeen)
        {
            try { process.Kill(); } catch { }
            selfTestSummary = "window did not appear within 45 s";
            report.Problems.Add("the application window did not appear");
            report.Healthy = false;
            report.ExitCode = 9;
            return;
        }

        bool requested = false;
        string windowTitle = "";
        try { windowTitle = process.MainWindowTitle; } catch { }
        // Keep the window visible for a moment so the check exercises a real
        // startup (device/profile loading), not just a created handle.
        var visibleFor = DateTime.UtcNow - started;
        if (visibleFor.TotalMilliseconds < 1500)
            System.Threading.Thread.Sleep(1500 - (int)visibleFor.TotalMilliseconds);
        try { requested = process.CloseMainWindow(); } catch { }
        bool exited = false;
        var closeDeadline = DateTime.UtcNow.AddSeconds(20);
        while (DateTime.UtcNow < closeDeadline)
        {
            try { if (process.HasExited) { exited = true; break; } } catch { }
            System.Threading.Thread.Sleep(250);
        }
        if (!exited)
        {
            try { process.Kill(); } catch { }
            selfTestSummary = "window opened but did not close cleanly" + (requested ? "" : " (CloseMainWindow failed)");
            report.Problems.Add("the application did not stop normally after its window was closed");
            report.Healthy = false;
            report.ExitCode = 9;
            return;
        }
        int code = SafeExitCode(process);
        double seconds = (DateTime.UtcNow - started).TotalSeconds;
        selfTestOk = code == 0;
        selfTestSummary = "window \"" + windowTitle + "\" shown and closed in " +
                          seconds.ToString("F1", CultureInfo.InvariantCulture) + " s (exit " + code + ")";
        if (!selfTestOk)
        {
            report.Problems.Add("the application did not report a clean exit (code " + code + ")");
            report.Healthy = false;
            report.ExitCode = 9;
        }
        log.Info("Application self-test: " + selfTestSummary);
    }

    private static int SafeExitCode(Process process)
    {
        try { return process.ExitCode; } catch { return -1; }
    }

    private static string Safe(string value, string fallback)
    {
        return string.IsNullOrEmpty(value) ? fallback : value;
    }

    private static string DescribeSignature(string appDir)
    {
        return NlSignature.Describe(System.IO.Path.Combine(appDir, "driver"));
    }

    // ------------------------------------------------------------- entry point
    public static int RunFromCommandLine(string[] args)
    {
        bool silent = false, postReboot = false, appSelfTest = false, installDirGiven = false;
        string installDir = "";
        int waitSeconds = 0;
        for (int i = 0; i < args.Length; i++)
        {
            string arg = (args[i] ?? "").Trim();
            string lower = arg.ToLowerInvariant();
            if (lower == "/silent" || lower == "--silent") silent = true;
            else if (lower == "--post-reboot") postReboot = true;
            else if (lower == "--app-selftest") appSelfTest = true;
            else if (lower == "--install-dir" && i + 1 < args.Length) { installDir = args[++i].Trim().Trim('"'); installDirGiven = true; }
            else if (lower.StartsWith("--install-dir=")) { installDir = arg.Substring(arg.IndexOf('=') + 1).Trim().Trim('"'); installDirGiven = true; }
            else if (lower == "--wait-endpoints" && i + 1 < args.Length)
                int.TryParse(args[++i], NumberStyles.Integer, CultureInfo.InvariantCulture, out waitSeconds);
            else if (lower.StartsWith("--wait-endpoints="))
                int.TryParse(arg.Substring(arg.IndexOf('=') + 1), NumberStyles.Integer, CultureInfo.InvariantCulture, out waitSeconds);
            // Unknown options are ignored: this helper is also launched from RunOnce.
        }

        if (!installDirGiven) installDir = NlReg.ReadInstallLocation();
        if (string.IsNullOrEmpty(installDir)) installDir = NlConst.DefaultAppDir;

        string logPath = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "NLMatrixVerify-" +
            DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture) + ".log");
        var log = new SetupLog(logPath);
        var outcome = new VerifyOutcome { Healthy = false, ExitCode = 9, Text = "verification did not run" };
        try
        {
            log.Header("NL Audio Matrix Verify", postReboot ? "post-reboot" : "verify", args,
                       "Install folder: " + installDir);
            var pending = NlPendingReboot.Read();
            bool expectAbsent = pending.Pending &&
                string.Equals(pending.Phase, "uninstall", StringComparison.OrdinalIgnoreCase);
            if (pending.Pending && !expectAbsent && pending.Phase.Length > 0)
                log.Info("Pending post-reboot phase: " + pending.Phase);

            outcome = Run(log, installDir, postReboot ? "verify-post-reboot" : "verify",
                          postReboot && waitSeconds == 0 ? 90000 : waitSeconds * 1000,
                          expectAbsent, appSelfTest);
            log.Info("Verification result: " + (outcome.Healthy ? "READY" : "NOT READY"));
            if (outcome.Healthy && pending.Pending) NlPendingReboot.Clear(log);
        }
        catch (Exception ex)
        {
            log.Error("Verification failed: " + ex);
            outcome = new VerifyOutcome
            {
                Healthy = false,
                ExitCode = 9,
                Text = "Verification failed: " + ex.Message + "\r\nLog: " + log.Path
            };
        }
        finally
        {
            log.Result(outcome.ExitCode, outcome.OneLine);
            log.Dispose();
        }

        if (!silent)
        {
            MessageBox.Show(outcome.Text, NlConst.AppName + " — verifica installazione", MessageBoxButtons.OK,
                            outcome.Healthy ? MessageBoxIcon.Information : MessageBoxIcon.Warning);
        }
        else
        {
            Console.WriteLine(outcome.Text);
        }
        return outcome.ExitCode;
    }
}

internal static class VerifyProgram
{
    [STAThread]
    private static int Main(string[] args)
    {
        return NlVerify.RunFromCommandLine(args);
    }
}
