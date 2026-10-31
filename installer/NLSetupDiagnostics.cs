// NLSetupDiagnostics.cs — read-only diagnostics shared by NLMatrixSetup.exe and
// NLMatrixVerify.exe.
//
// It answers, without changing anything:
//  * is the NL kernel driver loaded, and against which PnP device instance?
//  * what does the DriverStore hold, what version is bound, which .sys file is loaded?
//  * how many NL audio endpoints exist, and which of the eight are missing?
//  * can a test-signed driver load here (Secure Boot / TESTSIGNING / HVCI / blocklist)?
//  * did Windows Code Integrity reject the driver (SetupAPI problem code + CI events)?
//
// Only reads registry, files, services and the event log.  Nothing is written to
// the system by this file except the optional JSON report written by NlVerify.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

// --------------------------------------------------------------------- security
internal sealed class SecurityInfo
{
    public string Firmware = "unknown";          // bios | uefi | unknown
    public string SecureBoot = "unknown";        // enabled | disabled | unknown
    public string TestSigning = "unknown";       // enabled | disabled | unknown
    public string Hvci = "unknown";              // enabled | disabled | unknown
    public string Vbs = "unknown";               // enabled | disabled | unknown
    public string DriverBlocklist = "unknown";   // enabled | disabled | unknown
    public int WindowsBuild;
    public string Verdict = "unknown";           // ok | blocked | unknown
    public string BlockReason = "";
    public readonly List<string> Guidance = new List<string>();
    public readonly List<string> Notes = new List<string>();

    public bool Known
    {
        get { return Verdict != "unknown"; }
    }

    public bool TestSignedDriverAllowed
    {
        get { return Verdict == "ok"; }
    }
}

internal static class NlSecurity
{
    [DllImport("kernel32.dll")]
    private static extern bool GetFirmwareType(out uint firmwareType);

    public static SecurityInfo Collect(SetupLog log)
    {
        var info = new SecurityInfo();
        info.WindowsBuild = Environment.OSVersion.Version.Build;

        // Firmware type (UEFI or legacy BIOS) decides whether Secure Boot can exist.
        try
        {
            uint type;
            if (GetFirmwareType(out type))
            {
                if (type == 1) info.Firmware = "bios";
                else if (type == 2) info.Firmware = "uefi";
            }
            if (info.Firmware == "unknown")
            {
                // Fallback: the EFI variables directory only exists on UEFI systems.
                string efi = System.IO.Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.Windows), "System32", "config", "systemprofile");
                if (Directory.Exists(efi))
                {
                    var boot = ReadRegDword(@"SYSTEM\CurrentControlSet\Control", "PEFirmwareType");
                    if (boot == 2) info.Firmware = "uefi";
                    else if (boot == 1) info.Firmware = "bios";
                }
            }
        }
        catch (Exception ex) { info.Notes.Add("firmware type: " + ex.Message); }

        // Secure Boot: the OS exposes the state only when the firmware supports it.
        try
        {
            object value = ReadRegValue(@"SYSTEM\CurrentControlSet\Control\SecureBoot\State", "UEFISecureBootEnabled");
            if (value == null) info.SecureBoot = info.Firmware == "bios" ? "disabled" : "unknown";
            else info.SecureBoot = Convert.ToInt32(value, CultureInfo.InvariantCulture) == 1 ? "enabled" : "disabled";
        }
        catch (Exception ex) { info.Notes.Add("secure boot: " + ex.Message); }

        // Test signing has two independent states: what the boot entry is
        // configured to do (bcdedit) and what the running kernel actually
        // applied (Code Integrity).  A configured-but-not-booted state is the
        // classic reason a correctly signed test driver fails with 577.
        bool testSigningKnown = false;
        bool testSigningConfigured = false;
        try
        {
            var result = Sys.Run("bcdedit.exe", "/enum {current}", 60000);
            if (result.Started && result.ExitCode == 0)
            {
                testSigningKnown = true;
                foreach (var raw in result.OutputOrEmpty.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n'))
                {
                    string line = raw.Trim();
                    if (!line.StartsWith("testsigning", StringComparison.OrdinalIgnoreCase)) continue;
                    testSigningConfigured = line.IndexOf("Yes", StringComparison.OrdinalIgnoreCase) >= 0
                           || line.IndexOf("On", StringComparison.OrdinalIgnoreCase) >= 0
                           || line.IndexOf("Si", StringComparison.OrdinalIgnoreCase) >= 0;
                }
            }
            else
            {
                info.Notes.Add("test signing: bcdedit could not be read (exit " + result.ExitCode +
                               ", administrator rights are required)");
            }
        }
        catch (Exception ex) { info.Notes.Add("test signing: " + ex.Message); }

        uint ciOptions;
        bool liveKnown = NlDevices.TryGetCodeIntegrityOptions(out ciOptions);
        bool liveActive = liveKnown && (ciOptions & 0x2) != 0;
        if (liveKnown)
        {
            info.Notes.Add("kernel Code Integrity options=0x" + ciOptions.ToString("X8", CultureInfo.InvariantCulture) +
                           (liveActive ? " (test signing active)" : " (test signing not active)"));
            if (liveActive) info.TestSigning = "enabled";
            else if (testSigningConfigured) info.TestSigning = "reboot-required";
            else info.TestSigning = "disabled";
        }
        else
        {
            info.TestSigning = testSigningConfigured ? "unknown" : (testSigningKnown ? "disabled" : "unknown");
            info.Notes.Add("test signing: the live Code Integrity state could not be read");
        }

        // HVCI (Memory integrity) refuses unsigned/test-signed kernel images even
        // when test signing is on, so it is a hard blocker for this driver.
        try
        {
            object hvci = ReadRegValue(
                @"SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity", "Enabled");
            if (hvci != null)
                info.Hvci = Convert.ToInt32(hvci, CultureInfo.InvariantCulture) == 1 ? "enabled" : "disabled";
            else
                info.Hvci = "disabled";
        }
        catch (Exception ex) { info.Notes.Add("HVCI: " + ex.Message); }

        try
        {
            object vbs = ReadRegValue(@"SYSTEM\CurrentControlSet\Control\DeviceGuard", "EnableVirtualizationBasedSecurity");
            if (vbs != null)
                info.Vbs = Convert.ToInt32(vbs, CultureInfo.InvariantCulture) == 1 ? "enabled" : "disabled";
            else
                info.Vbs = "disabled";
        }
        catch (Exception ex) { info.Notes.Add("VBS: " + ex.Message); }

        try
        {
            object blocklist = ReadRegValue(@"SYSTEM\CurrentControlSet\Control\CI\Config", "VulnerableDriverBlocklistEnable");
            if (blocklist != null)
                info.DriverBlocklist = Convert.ToInt32(blocklist, CultureInfo.InvariantCulture) == 1 ? "enabled" : "disabled";
        }
        catch (Exception ex) { info.Notes.Add("driver blocklist: " + ex.Message); }

        Decide(info, testSigningKnown);
        log.Info("Security: firmware=" + info.Firmware + " secureBoot=" + info.SecureBoot +
                 " testSigning=" + info.TestSigning + " hvci=" + info.Hvci + " vbs=" + info.Vbs +
                 " blocklist=" + info.DriverBlocklist + " verdict=" + info.Verdict);
        foreach (var note in info.Notes) log.Info("  security note: " + note);
        return info;
    }

    private static void Decide(SecurityInfo info, bool testSigningKnown)
    {
        // A test-signed kernel driver loads only with test signing active and with
        // Secure Boot/HVCI not preventing it.  Setup never changes those settings.
        if (info.Hvci == "enabled")
        {
            info.Verdict = "blocked";
            info.BlockReason = "HVCI (Memory integrity) is enabled and rejects test-signed kernel drivers.";
            info.Guidance.Add("Open Windows Security > Device security > Core isolation details and turn Memory integrity off.");
            info.Guidance.Add("Reboot Windows, then run setup again. Setup never changes this setting itself.");
            return;
        }
        if (info.TestSigning == "reboot-required")
        {
            info.Verdict = "blocked";
            info.BlockReason = "Windows test signing is configured in the boot entry but this boot did not start with it, " +
                               "so a test-signed kernel driver cannot load until Windows restarts.";
            info.Guidance.Add("Restart Windows, then run setup again: nothing has to be reinstalled and no setting is changed.");
            info.Guidance.Add("If the kernel still reports test signing off after the restart, verify that the PC boots the " +
                              "entry you edited (bcdedit /enum {current}) and that Memory integrity (HVCI) is off.");
            return;
        }
        if (info.SecureBoot == "enabled" && info.TestSigning == "enabled")
        {
            info.Verdict = "ok";
            info.BlockReason = "";
            info.Guidance.Add("Secure Boot is on but test signing is active: keep Secure Boot disabled in the firmware until a production-signed driver exists.");
            return;
        }
        if (info.SecureBoot == "enabled" && info.TestSigning == "disabled")
        {
            info.Verdict = "blocked";
            info.BlockReason = "Secure Boot is enabled and Windows test signing is off, so the test-signed NL driver cannot load.";
            info.Guidance.Add("Factory-built (OEM) and self-built PCs with Secure Boot on must disable Secure Boot in the firmware setup (or install the public UEFI test key) before test signing can be enabled; this reduces boot protection on that PC.");
            info.Guidance.Add("Disable Secure Boot manually in the firmware (BIOS/UEFI) settings. Setup cannot and will not do it.");
            info.Guidance.Add("Then enable test signing (bcdedit /set testsigning on) and reboot, or let setup do it with /accept-testsigning.");
            return;
        }
        if (info.SecureBoot == "enabled" && info.TestSigning == "unknown")
        {
            info.Verdict = "unknown";
            info.BlockReason = "Secure Boot is on and the test-signing state could not be read (run setup as administrator).";
            return;
        }
        if (info.TestSigning == "enabled")
        {
            info.Verdict = "ok";
            return;
        }
        if (info.TestSigning == "disabled")
        {
            info.Verdict = "blocked";
            info.BlockReason = "Windows test signing is off, so the test-signed NL driver cannot load.";
            info.Guidance.Add("Enable test signing yourself (bcdedit /set testsigning on, reboot) or let setup do it with /accept-testsigning.");
            return;
        }
        info.Verdict = "unknown";
        info.BlockReason = testSigningKnown ? "" : "The test-signing state could not be read (run setup as administrator).";
    }

    private static object ReadRegValue(string path, string name)
    {
        using (var baseKey = NlReg.OpenBase())
        using (var key = baseKey.OpenSubKey(path))
            return key == null ? null : key.GetValue(name);
    }

    private static int ReadRegDword(string path, string name)
    {
        try
        {
            object value = ReadRegValue(path, name);
            if (value == null) return -1;
            return Convert.ToInt32(value, CultureInfo.InvariantCulture);
        }
        catch { return -1; }
    }

    public static string Describe(SecurityInfo info)
    {
        var sb = new StringBuilder();
        sb.AppendLine("  Firmware            : " + info.Firmware);
        sb.AppendLine("  Secure Boot         : " + info.SecureBoot);
        sb.AppendLine("  Test signing        : " + info.TestSigning);
        sb.AppendLine("  HVCI (Memory integ.): " + info.Hvci);
        sb.AppendLine("  VBS                 : " + info.Vbs);
        sb.AppendLine("  Driver blocklist    : " + info.DriverBlocklist);
        sb.AppendLine("  Windows build       : " + info.WindowsBuild);
        sb.AppendLine("  Driver verdict      : " + info.Verdict +
                      (info.BlockReason.Length > 0 ? " — " + info.BlockReason : ""));
        foreach (var line in info.Guidance) sb.AppendLine("    * " + line);
        return sb.ToString();
    }
}

// ------------------------------------------------------------------- signature
// The NL driver is catalog-signed (sysvad.cat), so its .sys file usually carries
// no embedded signature.  Describe both cases instead of reporting a fake error.
internal static class NlSignature
{
    public static string Describe(string driverDirectory)
    {
        string sys = System.IO.Path.Combine(driverDirectory, NlConst.SysName);
        string cer = System.IO.Path.Combine(driverDirectory, NlConst.CerName);
        string cat = System.IO.Path.Combine(driverDirectory, NlConst.CatName);
        if (!File.Exists(sys) || !File.Exists(cer)) return "unknown (driver files are missing)";

        System.Security.Cryptography.X509Certificates.X509Certificate2 expected = null;
        try { expected = new System.Security.Cryptography.X509Certificates.X509Certificate2(cer); }
        catch { }

        try
        {
            var signer = System.Security.Cryptography.X509Certificates.X509Certificate.CreateFromSignedFile(sys);
            bool match = expected != null && string.Equals(signer.GetCertHashString(), expected.Thumbprint,
                                                           StringComparison.OrdinalIgnoreCase);
            return "embedded test signature by " + signer.Subject +
                   (match ? " (matches the embedded certificate)" : " (does NOT match the embedded certificate)");
        }
        catch
        {
            // Expected for a catalog-signed driver; fall through to the catalog.
        }

        if (!File.Exists(cat)) return "no signature found (missing " + NlConst.CatName + ")";
        try
        {
            var cms = new System.Security.Cryptography.Pkcs.SignedCms();
            cms.Decode(File.ReadAllBytes(cat));
            var subjects = new List<string>();
            bool match = false;
            foreach (var certificate in cms.Certificates)
            {
                if (!string.IsNullOrEmpty(certificate.Subject) && subjects.Count < 3) subjects.Add(certificate.Subject);
                if (expected != null && string.Equals(certificate.Thumbprint, expected.Thumbprint,
                                                      StringComparison.OrdinalIgnoreCase)) match = true;
            }
            string who = subjects.Count > 0 ? string.Join("; ", subjects.ToArray()) : "catalog signer unreadable";
            return "catalog-signed (" + NlConst.CatName + "): " + who +
                   (match ? " (matches the embedded certificate)" : "");
        }
        catch (Exception ex)
        {
            return "catalog present (" + NlConst.CatName + "), signature unreadable: " + ex.Message;
        }
    }
}

// --------------------------------------------------------------------- PnP state
internal sealed class DeviceHealth
{
    public bool Present;
    public int InstanceCount;
    public string InstanceId = "";
    public string HardwareId = "";
    public string ServiceName = "";
    public string DriverInf = "";
    public uint StatusFlags;
    public uint ProblemCode;
    public bool HasProblem;
    public bool DriverLoaded;
    public bool Started;
    public string ProblemText = "";

    public bool Healthy
    {
        get { return Present && Started && !HasProblem; }
    }
}

internal static class NlPnp
{
    private const int CM_LOCATE_DEVNODE_NORMAL = 0;
    private const uint DN_HAS_PROBLEM = 0x00000400;
    private const uint DN_DRIVER_LOADED = 0x00000800;
    private const uint DN_STARTED = 0x00000008;

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    private static extern uint CM_Locate_DevNodeW(out uint devInst, string deviceId, int flags);

    [DllImport("cfgmgr32.dll")]
    private static extern uint CM_Get_DevNode_Status(out uint status, out uint problem, uint devInst, int flags);

    public static DeviceHealth Inspect(string hwid)
    {
        var health = new DeviceHealth { HardwareId = hwid };
        var instances = NlDevices.RootInstanceIds(hwid);
        health.InstanceCount = instances.Count;
        if (instances.Count == 0) return health;
        health.InstanceId = instances[0];

        ReadEnumValues(health, instances[0]);

        uint devInst;
        uint locate = CM_Locate_DevNodeW(out devInst, instances[0], CM_LOCATE_DEVNODE_NORMAL);
        if (locate != 0)
        {
            health.Present = false;
            health.ProblemText = "device instance exists in the registry but is not enumerated (CM_Locate_DevNode status " + locate + ")";
            return health;
        }
        health.Present = true;
        uint status, problem;
        if (CM_Get_DevNode_Status(out status, out problem, devInst, 0) == 0)
        {
            health.StatusFlags = status;
            health.ProblemCode = problem;
            health.HasProblem = (status & DN_HAS_PROBLEM) != 0 || problem != 0;
            health.DriverLoaded = (status & DN_DRIVER_LOADED) != 0;
            health.Started = (status & DN_STARTED) != 0;
            health.ProblemText = DescribeProblem(problem);
        }
        else
        {
            health.ProblemText = "CM_Get_DevNode_Status failed";
        }
        return health;
    }

    private static void ReadEnumValues(DeviceHealth health, string instanceId)
    {
        try
        {
            using (var baseKey = NlReg.OpenBase())
            using (var key = baseKey.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\" + instanceId))
            {
                if (key == null) return;
                health.ServiceName = (key.GetValue("Service") as string) ?? "";
                string driver = (key.GetValue("Driver") as string) ?? "";
                if (driver.Length > 0) health.DriverInf = System.IO.Path.GetFileName(driver.Replace('/', '\\'));
                if (!health.DriverInf.EndsWith(".inf", StringComparison.OrdinalIgnoreCase))
                {
                    // For a root device the driver value is a class subkey such as
                    // {4d36e96c-...}\0001; the class key carries the INF name.
                    string inf = "";
                    if (driver.StartsWith("{", StringComparison.Ordinal))
                    {
                        try
                        {
                            using (var classBase = NlReg.OpenBase())
                            using (var classKey = classBase.OpenSubKey(
                                       @"SYSTEM\CurrentControlSet\Control\Class\" + driver.Replace('/', '\\')))
                                if (classKey != null) inf = (classKey.GetValue("InfPath") as string) ?? "";
                        }
                        catch { }
                    }
                    if (inf.Length == 0) inf = NlDevices.InfPathForInstance(instanceId);
                    if (inf.Length > 0)
                        health.DriverInf = System.IO.Path.GetFileName(inf.Replace('/', '\\'));
                    else
                        health.DriverInf = "";
                }
            }
        }
        catch { }
    }

    public static string DescribeProblem(uint code)
    {
        // CM_PROB_* values from cfg.h; unknown values are reported numerically.
        switch (code)
        {
            case 0: return "";
            case 1: return "CM_PROB_NOT_CONFIGURED";
            case 3: return "CM_PROB_OUT_OF_MEMORY";
            case 4: return "CM_PROB_VOLTAGE";
            case 5: return "CM_PROB_REINSTALL";
            case 6: return "CM_PROB_FAILED_NO_CONFIG";
            case 10: return "CM_PROB_FAILED_START (the driver failed to start)";
            case 12: return "CM_PROB_NOT_ENOUGH_RESOURCES";
            case 14: return "CM_PROB_NEED_RESTART (a reboot is required)";
            case 18: return "CM_PROB_REINSTALL_FAILED";
            case 19: return "CM_PROB_REGISTRY (the device registry entry is invalid)";
            case 22: return "CM_PROB_DISABLED (the device is disabled)";
            case 24: return "CM_PROB_NOT_VERIFIED";
            case 28: return "CM_PROB_FAILED_INSTALL (drivers for this device are not installed)";
            case 29: return "CM_PROB_HARDWARE_DISABLED";
            case 30: return "CM_PROB_FIRMWARE_DISABLED";
            case 31: return "CM_PROB_DEVICE_IS_HUNG";
            case 32: return "CM_PROB_DISABLED_SERVICE (the driver service is disabled)";
            case 38: return "CM_PROB_DRIVER_FAILED_PRIOR_UNLOAD";
            case 39: return "CM_PROB_DRIVER_FAILED_LOAD (Windows rejected loading the driver; usually signing or corruption)";
            case 42: return "CM_PROB_DRIVER_FAILED_SLEEP";
            case 43: return "CM_PROB_DRIVER_PNP_WATCHDOG";
            case 44: return "CM_PROB_DRIVER_FAILED_NP";
            default: return "CM_PROB code " + code;
        }
    }
}

// ------------------------------------------------------------- code integrity
internal sealed class CodeIntegrityInfo
{
    public bool ChannelAvailable;
    public int Scanned;
    public int DriverEvents;
    public readonly List<string> Events = new List<string>();
    public string Error = "";
}

internal static class NlCodeIntegrity
{
    private const string Channel = "Microsoft-Windows-CodeIntegrity/Operational";

    public static CodeIntegrityInfo Collect(SetupLog log)
    {
        var info = new CodeIntegrityInfo();
        try
        {
            var query = new System.Diagnostics.Eventing.Reader.EventLogQuery(
                Channel, System.Diagnostics.Eventing.Reader.PathType.LogName,
                "*[System[TimeCreated[timediff(@SystemTime) <= 604800000]]]");
            using (var reader = new System.Diagnostics.Eventing.Reader.EventLogReader(query))
            {
                info.ChannelAvailable = true;
                System.Diagnostics.Eventing.Reader.EventRecord record;
                while ((record = reader.ReadEvent()) != null && info.Scanned < 400)
                {
                    info.Scanned++;
                    using (record)
                    {
                        string message = "";
                        try { message = record.FormatDescription() ?? ""; } catch { }
                        bool relevant = message.IndexOf("NLMatrix", StringComparison.OrdinalIgnoreCase) >= 0
                                     || message.IndexOf("nlmatrixaudio", StringComparison.OrdinalIgnoreCase) >= 0;
                        if (!relevant) continue;
                        info.DriverEvents++;
                        if (info.Events.Count < 8)
                        {
                            string when = record.TimeCreated.HasValue
                                ? record.TimeCreated.Value.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture)
                                : "unknown time";
                            string level = "";
                            try { level = record.LevelDisplayName; } catch { }
                            info.Events.Add(when + " id=" + record.Id + " " + level + ": " +
                                            NlText.Clip(message.Replace("\r", " ").Replace("\n", " "), 240));
                        }
                    }
                }
            }
        }
        catch (Exception ex)
        {
            info.Error = ex.GetType().Name + ": " + ex.Message;
        }
        log.Info("Code Integrity: channel=" + info.ChannelAvailable + " scanned=" + info.Scanned +
                 " driverEvents=" + info.DriverEvents +
                 (info.Error.Length > 0 ? " error=" + info.Error : ""));
        foreach (var line in info.Events) log.Info("  CI: " + line);
        return info;
    }
}

// ------------------------------------------------------------ loaded driver
internal sealed class DriverRuntimeInfo
{
    public bool ServicePresent;
    public bool ServiceRunning;
    public string ServicePath = "";
    public string LoadedFile = "";
    public bool LoadedFilePresent;
    public long LoadedFileBytes;
    public string LoadedFileVersion = "";
    public string LoadedFileModified = "";
}

internal static class NlDriverRuntime
{
    public static DriverRuntimeInfo Collect(SetupLog log)
    {
        var info = new DriverRuntimeInfo();
        info.ServicePresent = NlDevices.ServiceExists(log);
        info.ServiceRunning = info.ServicePresent && NlDevices.ServiceRunning(log);

        try
        {
            using (var baseKey = NlReg.OpenBase())
            using (var key = baseKey.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\" + NlConst.ServiceName))
            {
                if (key != null)
                {
                    info.ServicePath = (key.GetValue("ImagePath") as string) ?? "";
                    object start = key.GetValue("Start");
                    if (start != null)
                        log.Info("Service start type: " + Convert.ToInt32(start, CultureInfo.InvariantCulture));
                }
            }
        }
        catch (Exception ex) { log.Warn("Could not read the driver service key: " + ex.Message); }

        string systemRoot = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        string defaultPath = System.IO.Path.Combine(systemRoot, "System32", "drivers", NlConst.SysName);
        string candidate = defaultPath;
        string configured = info.ServicePath.Trim().Trim('"');
        if (configured.Length > 0)
        {
            string expanded = configured;
            if (expanded.StartsWith(@"\SystemRoot\", StringComparison.OrdinalIgnoreCase))
                expanded = System.IO.Path.Combine(systemRoot, expanded.Substring(@"\SystemRoot\".Length));
            else if (expanded.StartsWith(@"System32\", StringComparison.OrdinalIgnoreCase))
                expanded = System.IO.Path.Combine(systemRoot, expanded);
            if (File.Exists(expanded)) candidate = expanded;
        }
        info.LoadedFile = candidate;
        info.LoadedFilePresent = File.Exists(candidate);
        if (info.LoadedFilePresent)
        {
            try
            {
                var file = new FileInfo(candidate);
                info.LoadedFileBytes = file.Length;
                info.LoadedFileModified = file.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture);
                info.LoadedFileVersion = FileVersionInfo.GetVersionInfo(candidate).FileVersion ?? "";
            }
            catch (Exception ex) { log.Warn("Could not read the loaded driver file: " + ex.Message); }
        }
        log.Info("Driver runtime: service=" + (info.ServiceRunning ? "running" : "stopped") +
                 " image=" + info.LoadedFile + " present=" + info.LoadedFilePresent +
                 " fileVersion=" + info.LoadedFileVersion + " modified=" + info.LoadedFileModified);
        return info;
    }
}

// ------------------------------------------------------------- pending reboot
internal sealed class PendingRebootInfo
{
    public bool Pending;
    public string Phase = "";
    public string AppDir = "";
    public string Version = "";
    public string StartedUtc = "";
    public string Path = "";
}

internal static class NlPendingReboot
{
    public static string MarkerPath
    {
        get
        {
            return System.IO.Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                NlConst.AppName, "pending-reboot.txt");
        }
    }

    public static string RunOnceKeyPath
    {
        get { return @"SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce"; }
    }

    public static string RunOnceValueName
    {
        get { return "NLMatrixVerifyAfterReboot"; }
    }

    public static bool Write(SetupLog log, string phase, string appDir, string version)
    {
        try
        {
            string path = MarkerPath;
            string directory = System.IO.Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
            var lines = new[]
            {
                "phase=" + phase,
                "appDir=" + appDir,
                "version=" + version,
                "startedUtc=" + DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture)
            };
            File.WriteAllLines(path, lines, new UTF8Encoding(false));
            log.Info("Pending-reboot marker written: " + path);
            return true;
        }
        catch (Exception ex)
        {
            log.Warn("Could not write the pending-reboot marker: " + ex.Message);
            return false;
        }
    }

    public static PendingRebootInfo Read()
    {
        var info = new PendingRebootInfo { Path = MarkerPath };
        try
        {
            if (!File.Exists(MarkerPath)) return info;
            info.Pending = true;
            foreach (var raw in File.ReadAllLines(MarkerPath))
            {
                int index = raw.IndexOf('=');
                if (index <= 0) continue;
                string key = raw.Substring(0, index).Trim();
                string value = raw.Substring(index + 1).Trim();
                if (string.Equals(key, "phase", StringComparison.OrdinalIgnoreCase)) info.Phase = value;
                else if (string.Equals(key, "appDir", StringComparison.OrdinalIgnoreCase)) info.AppDir = value;
                else if (string.Equals(key, "version", StringComparison.OrdinalIgnoreCase)) info.Version = value;
                else if (string.Equals(key, "startedUtc", StringComparison.OrdinalIgnoreCase)) info.StartedUtc = value;
            }
        }
        catch { }
        return info;
    }

    public static void Clear(SetupLog log)
    {
        try
        {
            if (File.Exists(MarkerPath)) File.Delete(MarkerPath);
            using (var baseKey = NlReg.OpenBase())
            using (var key = baseKey.OpenSubKey(RunOnceKeyPath, true))
            {
                if (key != null && Array.IndexOf(key.GetValueNames(), RunOnceValueName) >= 0)
                    key.DeleteValue(RunOnceValueName, false);
            }
            log.Info("Pending-reboot marker cleared.");
        }
        catch (Exception ex) { log.Warn("Could not clear the pending-reboot marker: " + ex.Message); }
    }

    public static bool RegisterRunOnce(SetupLog log, string appDir)
    {
        string verify = System.IO.Path.Combine(appDir, "NLMatrixVerify.exe");
        if (!File.Exists(verify))
        {
            log.Warn("Cannot register post-reboot verification: " + verify + " is missing.");
            return false;
        }
        return RegisterRunOnce(log, verify, "--post-reboot");
    }

    public static bool RegisterRunOnce(SetupLog log, string executable, string arguments)
    {
        try
        {
            if (!File.Exists(executable))
            {
                log.Warn("Cannot register post-reboot verification: " + executable + " is missing.");
                return false;
            }
            using (var baseKey = NlReg.OpenBase())
            using (var key = baseKey.CreateSubKey(RunOnceKeyPath))
            {
                if (key == null) return false;
                key.SetValue(RunOnceValueName, Sys.Quote(executable) + " " + arguments, RegistryValueKind.String);
            }
            log.Info("Registered post-reboot verification (RunOnce " + RunOnceValueName + " -> " + executable + ").");
            return true;
        }
        catch (Exception ex)
        {
            log.Warn("Could not register post-reboot verification: " + ex.Message);
            return false;
        }
    }

    public static void UnregisterRunOnce(SetupLog log)
    {
        try
        {
            using (var baseKey = NlReg.OpenBase())
            using (var key = baseKey.OpenSubKey(RunOnceKeyPath, true))
            {
                if (key != null && Array.IndexOf(key.GetValueNames(), RunOnceValueName) >= 0)
                {
                    key.DeleteValue(RunOnceValueName, false);
                    log.Info("Post-reboot verification cancelled by the user.");
                }
            }
        }
        catch (Exception ex) { log.Warn("Could not cancel post-reboot verification: " + ex.Message); }
    }
}

// ------------------------------------------------------------------ report
internal sealed class SetupDiagnosticsReport
{
    public string Kind = "diagnose";
    public string Utc = "";
    public string AppDir = "";
    public string PackageVersion = "";
    public string PackageBuildId = "";
    public string PackageDriverVersion = "";
    public string PackageSignature = "";
    public string InstalledAppVersion = "";
    public bool ArpEntry;
    public bool StartMenu;
    public int AppFileProblems;
    public readonly List<string> AppProblemList = new List<string>();
    public readonly List<DriverPackageInfo> Packages = new List<DriverPackageInfo>();
    public string BoundDriverVersion = "";
    public DriverRuntimeInfo Runtime;
    public DeviceHealth Device;
    public EndpointReport Endpoints;
    public SecurityInfo Security;
    public CodeIntegrityInfo CodeIntegrity;
    public PendingRebootInfo Pending;
    public bool CertificateTrusted;
    public bool LegacyDevicePresent;
    public string Decision = "";
    public bool AppSelfTestRan;
    public bool AppSelfTestOk;
    public string AppSelfTestSummary = "";
    public bool Healthy;
    public readonly List<string> Problems = new List<string>();
    public int ExitCode = 0;

    public void ToText(StringBuilder sb)
    {
        sb.AppendLine("NL Audio Matrix diagnostics — read-only, nothing was installed or changed");
        sb.AppendLine("  When                : " + Utc);
        sb.AppendLine("  Install folder      : " + AppDir);
        sb.AppendLine("  Package             : " + PackageVersion + " (build " + PackageBuildId + ")");
        sb.AppendLine("  Package driver      : " + PackageDriverVersion);
        sb.AppendLine("  Driver signature    : " + PackageSignature);
        sb.AppendLine("  App installed       : " + (InstalledAppVersion.Length > 0 ? InstalledAppVersion : "no"));
        sb.AppendLine("  Add/Remove entry    : " + ArpEntry);
        sb.AppendLine("  Start Menu          : " + StartMenu);
        sb.AppendLine("  App file problems   : " + AppFileProblems);
        foreach (var problem in AppProblemList) sb.AppendLine("      - " + problem);
        sb.AppendLine("  Certificate trusted : " + CertificateTrusted);
        sb.AppendLine("  Staged packages     : " + NlDrivers.DescribeInstalled(Packages));
        sb.AppendLine("  Bound driver (PnP)  : " + (BoundDriverVersion.Length > 0 ? BoundDriverVersion : "not bound"));
        if (Runtime != null)
        {
            sb.AppendLine("  Service             : " + (Runtime.ServiceRunning ? "running" : (Runtime.ServicePresent ? "stopped" : "absent")));
            sb.AppendLine("  Loaded driver file  : " + (Runtime.LoadedFilePresent
                ? Runtime.LoadedFile + " (version " + Runtime.LoadedFileVersion + ", " + Runtime.LoadedFileBytes +
                  " bytes, modified " + Runtime.LoadedFileModified + ")"
                : "missing (" + Runtime.LoadedFile + ")"));
        }
        if (Device != null)
        {
            sb.AppendLine("  PnP device          : " + (Device.Present ? "present" : "absent") +
                          " instances=" + Device.InstanceCount);
            sb.AppendLine("  PnP instance        : " + (Device.InstanceId.Length > 0 ? Device.InstanceId : "none"));
            sb.AppendLine("  PnP hardware id     : " + Device.HardwareId);
            sb.AppendLine("  PnP service         : " + (Device.ServiceName.Length > 0 ? Device.ServiceName : "none") +
                          " driver=" + (Device.DriverInf.Length > 0 ? Device.DriverInf : "none"));
            sb.AppendLine("  PnP status          : driverLoaded=" + Device.DriverLoaded + " started=" + Device.Started +
                          " problem=" + Device.ProblemCode +
                          (Device.ProblemText.Length > 0 ? " (" + Device.ProblemText + ")" : ""));
        }
        if (Endpoints != null)
        {
            sb.AppendLine("  Audio endpoints     : " + Endpoints.ActiveCount + " active of " + Endpoints.TotalCount +
                          " (capture " + Endpoints.ActiveCapture + ", render " + Endpoints.ActiveRender + ")");
            if (Endpoints.MissingExpected.Count > 0)
                sb.AppendLine("  Missing endpoints   : " + string.Join(", ", Endpoints.MissingExpected.ToArray()));
            foreach (var name in Endpoints.Names) sb.AppendLine("      " + name);
        }
        if (Security != null) { sb.AppendLine("  Security:"); sb.Append(NlSecurity.Describe(Security)); }
        if (CodeIntegrity != null)
        {
            sb.AppendLine("  Code Integrity      : " + (CodeIntegrity.ChannelAvailable ? "channel available" : "channel unavailable") +
                          ", driver-related rejections=" + CodeIntegrity.DriverEvents);
            foreach (var line in CodeIntegrity.Events) sb.AppendLine("      " + line);
        }
        if (Pending != null)
            sb.AppendLine("  Pending reboot      : " + (Pending.Pending
                ? "yes (phase " + Pending.Phase + ", since " + Pending.StartedUtc + ")"
                : "no"));
        sb.AppendLine("  Setup decision      : " + (Decision.Length > 0 ? Decision : "n/a"));
        if (AppSelfTestRan)
            sb.AppendLine("  App start/stop test : " + (AppSelfTestOk ? "PASS" : "FAIL") + " - " + AppSelfTestSummary);
        sb.AppendLine("  Healthy             : " + Healthy);
        if (Problems.Count > 0)
        {
            sb.AppendLine("  Problems:");
            foreach (var problem in Problems) sb.AppendLine("      - " + problem);
        }
        sb.AppendLine("  Result              : " + (Healthy ? "READY" : "NOT READY") +
                      " (exit " + ExitCode + ")");
    }

    public string ToJson()
    {
        var sb = new StringBuilder();
        sb.AppendLine("{");
        sb.AppendLine("  \"schema\": 1,");
        sb.AppendLine("  \"kind\": " + Json.Str(Kind) + ",");
        sb.AppendLine("  \"utc\": " + Json.Str(Utc) + ",");
        sb.AppendLine("  \"appDir\": " + Json.Str(AppDir) + ",");
        sb.AppendLine("  \"package\": {");
        sb.AppendLine("    \"version\": " + Json.Str(PackageVersion) + ",");
        sb.AppendLine("    \"buildId\": " + Json.Str(PackageBuildId) + ",");
        sb.AppendLine("    \"driverVersion\": " + Json.Str(PackageDriverVersion) + ",");
        sb.AppendLine("    \"signature\": " + Json.Str(PackageSignature));
        sb.AppendLine("  },");
        sb.AppendLine("  \"installed\": {");
        sb.AppendLine("    \"appVersion\": " + Json.Str(InstalledAppVersion) + ",");
        sb.AppendLine("    \"arpEntry\": " + Json.Bool(ArpEntry) + ",");
        sb.AppendLine("    \"startMenu\": " + Json.Bool(StartMenu) + ",");
        sb.AppendLine("    \"appFileProblems\": " + AppFileProblems + ",");
        sb.AppendLine("    \"certificateTrusted\": " + Json.Bool(CertificateTrusted));
        sb.AppendLine("  },");
        sb.AppendLine("  \"driver\": {");
        sb.Append("    \"packages\": [");
        for (int i = 0; i < Packages.Count; i++)
            sb.Append((i > 0 ? ", " : "") + "{\"name\": " + Json.Str(Packages[i].PublishedName) +
                      ", \"driverVer\": " + Json.Str(Packages[i].DriverVer) + "}");
        sb.AppendLine("],");
        sb.AppendLine("    \"boundVersion\": " + Json.Str(BoundDriverVersion) + ",");
        if (Runtime != null)
        {
            sb.AppendLine("    \"servicePresent\": " + Json.Bool(Runtime.ServicePresent) + ",");
            sb.AppendLine("    \"serviceRunning\": " + Json.Bool(Runtime.ServiceRunning) + ",");
            sb.AppendLine("    \"loadedFile\": " + Json.Str(Runtime.LoadedFile) + ",");
            sb.AppendLine("    \"loadedFilePresent\": " + Json.Bool(Runtime.LoadedFilePresent) + ",");
            sb.AppendLine("    \"loadedFileVersion\": " + Json.Str(Runtime.LoadedFileVersion) + ",");
            sb.AppendLine("    \"loadedFileBytes\": " + Runtime.LoadedFileBytes + ",");
            sb.AppendLine("    \"loadedFileModified\": " + Json.Str(Runtime.LoadedFileModified));
        }
        sb.AppendLine("  },");
        if (Device != null)
        {
            sb.AppendLine("  \"device\": {");
            sb.AppendLine("    \"present\": " + Json.Bool(Device.Present) + ",");
            sb.AppendLine("    \"instanceCount\": " + Device.InstanceCount + ",");
            sb.AppendLine("    \"instanceId\": " + Json.Str(Device.InstanceId) + ",");
            sb.AppendLine("    \"hardwareId\": " + Json.Str(Device.HardwareId) + ",");
            sb.AppendLine("    \"service\": " + Json.Str(Device.ServiceName) + ",");
            sb.AppendLine("    \"driverInf\": " + Json.Str(Device.DriverInf) + ",");
            sb.AppendLine("    \"statusFlags\": " + Device.StatusFlags + ",");
            sb.AppendLine("    \"problemCode\": " + Device.ProblemCode + ",");
            sb.AppendLine("    \"driverLoaded\": " + Json.Bool(Device.DriverLoaded) + ",");
            sb.AppendLine("    \"started\": " + Json.Bool(Device.Started) + ",");
            sb.AppendLine("    \"problemText\": " + Json.Str(Device.ProblemText));
            sb.AppendLine("  },");
        }
        if (Endpoints != null)
        {
            sb.AppendLine("  \"endpoints\": {");
            sb.AppendLine("    \"total\": " + Endpoints.TotalCount + ",");
            sb.AppendLine("    \"active\": " + Endpoints.ActiveCount + ",");
            sb.AppendLine("    \"activeCapture\": " + Endpoints.ActiveCapture + ",");
            sb.AppendLine("    \"activeRender\": " + Endpoints.ActiveRender + ",");
            sb.AppendLine("    \"names\": [" + Json.Array(Endpoints.Names) + "],");
            sb.AppendLine("    \"missing\": [" + Json.Array(Endpoints.MissingExpected) + "]");
            sb.AppendLine("  },");
        }
        if (Security != null)
        {
            sb.AppendLine("  \"security\": {");
            sb.AppendLine("    \"firmware\": " + Json.Str(Security.Firmware) + ",");
            sb.AppendLine("    \"secureBoot\": " + Json.Str(Security.SecureBoot) + ",");
            sb.AppendLine("    \"testSigning\": " + Json.Str(Security.TestSigning) + ",");
            sb.AppendLine("    \"hvci\": " + Json.Str(Security.Hvci) + ",");
            sb.AppendLine("    \"vbs\": " + Json.Str(Security.Vbs) + ",");
            sb.AppendLine("    \"driverBlocklist\": " + Json.Str(Security.DriverBlocklist) + ",");
            sb.AppendLine("    \"windowsBuild\": " + Security.WindowsBuild + ",");
            sb.AppendLine("    \"verdict\": " + Json.Str(Security.Verdict) + ",");
            sb.AppendLine("    \"blockReason\": " + Json.Str(Security.BlockReason) + ",");
            sb.AppendLine("    \"guidance\": [" + Json.Array(Security.Guidance) + "]");
            sb.AppendLine("  },");
        }
        if (CodeIntegrity != null)
        {
            sb.AppendLine("  \"codeIntegrity\": {");
            sb.AppendLine("    \"channelAvailable\": " + Json.Bool(CodeIntegrity.ChannelAvailable) + ",");
            sb.AppendLine("    \"scanned\": " + CodeIntegrity.Scanned + ",");
            sb.AppendLine("    \"driverEvents\": " + CodeIntegrity.DriverEvents + ",");
            sb.AppendLine("    \"events\": [" + Json.Array(CodeIntegrity.Events) + "]");
            sb.AppendLine("  },");
        }
        if (Pending != null)
        {
            sb.AppendLine("  \"pendingReboot\": {");
            sb.AppendLine("    \"pending\": " + Json.Bool(Pending.Pending) + ",");
            sb.AppendLine("    \"phase\": " + Json.Str(Pending.Phase) + ",");
            sb.AppendLine("    \"startedUtc\": " + Json.Str(Pending.StartedUtc));
            sb.AppendLine("  },");
        }
        sb.AppendLine("  \"appSelfTest\": {");
        sb.AppendLine("    \"ran\": " + Json.Bool(AppSelfTestRan) + ",");
        sb.AppendLine("    \"ok\": " + Json.Bool(AppSelfTestOk) + ",");
        sb.AppendLine("    \"summary\": " + Json.Str(AppSelfTestSummary));
        sb.AppendLine("  },");
        sb.AppendLine("  \"decision\": " + Json.Str(Decision) + ",");
        sb.AppendLine("  \"healthy\": " + Json.Bool(Healthy) + ",");
        sb.AppendLine("  \"problems\": [" + Json.Array(Problems) + "],");
        sb.AppendLine("  \"exitCode\": " + ExitCode);
        sb.AppendLine("}");
        return sb.ToString();
    }
}

internal static class Json
{
    public static string Str(string value)
    {
        if (value == null) return "null";
        var sb = new StringBuilder("\"");
        foreach (char c in value)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 32) sb.Append("\\u" + ((int)c).ToString("x4", CultureInfo.InvariantCulture));
                    else sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
        return sb.ToString();
    }

    public static string Bool(bool value) { return value ? "true" : "false"; }

    public static string Array(IEnumerable<string> values)
    {
        var sb = new StringBuilder();
        bool first = true;
        foreach (var value in values)
        {
            if (!first) sb.Append(", ");
            sb.Append(Str(value));
            first = false;
        }
        return sb.ToString();
    }
}

// ------------------------------------------------------------------ collect
internal static class NlDiagnostics
{
    public static SetupDiagnosticsReport Collect(SetupLog log, string kind, string appDir,
                                                 string packageVersion, string packageBuildId,
                                                 string packageDriverVersion, string packageSignature,
                                                 DeviceHealth device)
    {
        var report = new SetupDiagnosticsReport
        {
            Kind = kind,
            Utc = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture),
            AppDir = appDir,
            PackageVersion = packageVersion ?? "",
            PackageBuildId = packageBuildId ?? "",
            PackageDriverVersion = packageDriverVersion ?? "",
            PackageSignature = packageSignature ?? ""
        };

        if (!string.IsNullOrEmpty(appDir))
        {
            report.InstalledAppVersion = ReadVersionValue(appDir, "version");
            string manifestPath = System.IO.Path.Combine(appDir, "SHA256SUMS.txt");
            if (File.Exists(manifestPath))
            {
                try
                {
                    var entries = NlManifest.Parse(manifestPath);
                    var problems = NlManifest.VerifyInstall(appDir, entries, log);
                    report.AppFileProblems = problems.Count;
                    report.AppProblemList.AddRange(problems);
                }
                catch (Exception ex)
                {
                    report.AppFileProblems = -1;
                    report.AppProblemList.Add("cannot verify installed files: " + ex.Message);
                }
            }
            else
            {
                report.AppProblemList.Add("SHA256SUMS.txt is missing from the installation folder");
            }
        }
        report.ArpEntry = NlReg.HasUninstallEntry();
        report.StartMenu = Directory.Exists(NlShell.StartMenuFolder());
        report.Packages.AddRange(NlDrivers.FindInstalled(log));
        report.Runtime = NlDriverRuntime.Collect(log);
        report.Device = device ?? NlPnp.Inspect(NlConst.RootHwid);
        report.BoundDriverVersion = NlDevices.BoundDriverVersion(log, report.Packages, NlConst.RootHwid);
        if (report.BoundDriverVersion.Length == 0 && report.Device != null && report.Device.DriverInf.Length > 0)
        {
            foreach (var package in report.Packages)
            {
                if (!string.Equals(package.PublishedName, report.Device.DriverInf, StringComparison.OrdinalIgnoreCase)) continue;
                report.BoundDriverVersion = package.DriverVer;
                break;
            }
        }
        report.CertificateTrusted = NlCert.IsAnyTrusted(log);
        report.Endpoints = NlEndpoints.Collect(log);
        report.Security = NlSecurity.Collect(log);
        report.CodeIntegrity = NlCodeIntegrity.Collect(log);
        report.Pending = NlPendingReboot.Read();
        Evaluate(report, log);
        return report;
    }

    private static void Evaluate(SetupDiagnosticsReport report, SetupLog log)
    {
        var problems = report.Problems;
        if (report.AppFileProblems > 0)
            problems.Add(report.AppFileProblems + " application file(s) are missing or modified");
        if (report.AppFileProblems < 0)
            problems.Add("the installation folder could not be verified");
        if (report.InstalledAppVersion.Length == 0)
            problems.Add("the application is not installed");
        if (!report.ArpEntry)
            problems.Add("the Add/Remove Programs entry is missing");
        if (!report.CertificateTrusted)
            problems.Add("the NL test certificate is not trusted");
        if (report.Packages.Count == 0)
            problems.Add("no NLMatrix driver package is staged in the DriverStore");
        if (report.Device == null || !report.Device.Present)
            problems.Add("the NLMatrix PnP device is not present");
        else if (report.Device.HasProblem)
            problems.Add("the NLMatrix PnP device reports " + (report.Device.ProblemText.Length > 0
                ? report.Device.ProblemText : "problem code " + report.Device.ProblemCode));
        if (report.Device != null && report.Device.InstanceCount > 1)
            problems.Add("duplicate NLMatrix device instances: " + report.Device.InstanceCount);
        if (report.Runtime == null || !report.Runtime.ServiceRunning)
            problems.Add("the NLMatrix driver service is not running");
        if (report.Runtime != null && !report.Runtime.LoadedFilePresent)
            problems.Add("the loaded driver file is missing: " + report.Runtime.LoadedFile);
        if (report.Endpoints == null || report.Endpoints.ActiveCapture < 4 || report.Endpoints.ActiveRender < 4)
            problems.Add("NL audio endpoints are incomplete (" + (report.Endpoints == null ? "none"
                : report.Endpoints.ActiveCount + " active of " + report.Endpoints.TotalCount) + ")");
        if (report.Security != null && report.Security.Verdict == "blocked")
            problems.Add("this Windows configuration blocks test-signed drivers: " + report.Security.BlockReason);
        if (DriverBlockedByCodeIntegrity(report))
            problems.Add("Windows Code Integrity rejected the NL driver (" + report.CodeIntegrity.DriverEvents + " event(s))");

        report.Healthy = problems.Count == 0;
        report.ExitCode = report.Healthy ? 0 : 9;
        foreach (var problem in problems) log.Warn("Health problem: " + problem);
        log.Info("Health: " + (report.Healthy ? "READY" : "NOT READY (" + problems.Count + " problem(s))"));
    }

    private static bool DriverBlockedByCodeIntegrity(SetupDiagnosticsReport report)
    {
        return report.CodeIntegrity != null && report.CodeIntegrity.DriverEvents > 0;
    }

    public static string ReadVersionValue(string directory, string key)
    {
        try
        {
            string file = System.IO.Path.Combine(directory, "VERSION.txt");
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

    public static string ReportsDirectory
    {
        get
        {
            string programData = Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData);
            string path = System.IO.Path.Combine(programData, NlConst.AppName, "reports");
            try
            {
                Directory.CreateDirectory(path);
                string probe = System.IO.Path.Combine(path, ".write-test");
                File.WriteAllText(probe, "ok");
                File.Delete(probe);
                return path;
            }
            catch
            {
                string local = System.IO.Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    NlConst.AppName, "reports");
                Directory.CreateDirectory(local);
                return local;
            }
        }
    }

    public static string WriteReport(SetupLog log, SetupDiagnosticsReport report)
    {
        try
        {
            string directory = ReportsDirectory;
            string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture);
            string path = System.IO.Path.Combine(directory, report.Kind + "-" + stamp + ".json");
            File.WriteAllText(path, report.ToJson(), new UTF8Encoding(false));
            // All verify variants share last-verify.* so a post-reboot caller can
            // always find the latest result under one stable name.
            string baseName = report.Kind.StartsWith("verify", StringComparison.OrdinalIgnoreCase)
                ? "verify" : report.Kind;
            string latest = System.IO.Path.Combine(directory, "last-" + baseName + ".json");
            File.WriteAllText(latest, report.ToJson(), new UTF8Encoding(false));
            File.WriteAllText(System.IO.Path.Combine(directory, "last-" + baseName + ".txt"),
                              TextOf(report), new UTF8Encoding(false));
            log.Info("Diagnostics report written: " + path);
            return path;
        }
        catch (Exception ex)
        {
            log.Warn("Could not write the diagnostics report: " + ex.Message);
            return "";
        }
    }

    public static string TextOf(SetupDiagnosticsReport report)
    {
        var sb = new StringBuilder();
        report.ToText(sb);
        return sb.ToString();
    }
}
