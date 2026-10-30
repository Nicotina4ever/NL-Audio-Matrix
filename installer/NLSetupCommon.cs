// NLSetupCommon.cs — shared implementation for NLMatrixSetup.exe and
// NLMatrixUninstall.exe.  Compiled into both executables; it contains no Main
// and no assembly attributes.
//
// Design rules:
//  * every external command goes through Sys.Run so exit code + output are logged;
//  * driver/device/certificate operations are scoped to NLMatrix names/IDs only,
//    never to generic audio classes, other vendors' devices or Windows defaults;
//  * paths are validated before anything is deleted;
//  * Registry and driver-store access always uses the 64-bit view explicitly.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Security.Principal;
using Microsoft.Win32;

internal static class NlConst
{
    public const string AppName = "NL Audio Matrix";
    public const string Publisher = "Nicola Lanci";
    public const string RootHwid = @"Root\NLMatrixVirtualAudio";
    public const string LegacyHwid = @"Root\sysvad_ComponentizedAudioSample";
    public const string ServiceName = "sysvad_componentizedaudiosample";
    public const string UninstallKeyPath = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NLMatrix";
    public const string CertCommonName = "NLMatrix Test Driver";
    public const string InfName = "ComponentizedAudioSample.inf";
    public const string SysName = "NLMatrixAudio.sys";
    public const string CatName = "sysvad.cat";
    public const string CerName = "NLMatrixTest.cer";
    public const string DevconName = "devcon.exe";
    public const string DefaultAppDir = @"C:\NLMatrix";
    public const string FriendlyNameValueName = "{a45c254e-df1c-4efd-8020-67d146a850e0},2";
    public const string SetupExeName = "NLMatrixSetup.exe";
    public const string UninstallerExeName = "NLMatrixUninstall.exe";
    public const string UiExeName = "NLMatrixUI.exe";
    public const string RouterExeName = "NLMatrixRouter.exe";

    public static readonly string[] ExpectedEndpointNames =
    {
        "NL Mic 1", "NL Mic 2", "NL Mic 3", "NL Mic 4",
        "NL Return 1", "NL Return 2", "NL Return 3", "NL Return 4"
    };
}

internal sealed class SetupException : Exception
{
    public int ExitCode { get; private set; }

    public SetupException(int exitCode, string message)
        : base(message)
    {
        ExitCode = exitCode;
    }
}

internal sealed class ExecResult
{
    public string Exe = "";
    public string Args = "";
    public int ExitCode = -1;
    public bool Started;
    public bool TimedOut;
    public string Output = "";

    public string OutputOrEmpty
    {
        get { return Output == null ? "" : Output; }
    }
}

internal static class NlText
{
    public static string Clip(string text, int max)
    {
        if (string.IsNullOrEmpty(text)) return "";
        text = text.Trim();
        if (text.Length <= max) return text;
        return text.Substring(0, max) + "... (truncated, " + text.Length + " chars total)";
    }

    public static string NormalizeVersion(string value)
    {
        if (value == null) return "";
        return Regex.Replace(value.Trim(), "[\\s,]+", "").ToLowerInvariant();
    }

    public static string DisplayVersion(string value)
    {
        if (value == null) return "";
        return Regex.Replace(value.Trim(), "\\s*,\\s*", " ");
    }

    public static bool SameVersion(string a, string b)
    {
        return NormalizeVersion(a).Length > 0 && NormalizeVersion(a) == NormalizeVersion(b);
    }
}

internal static class Sys
{
    public static bool IsAdmin()
    {
        try
        {
            using (var identity = WindowsIdentity.GetCurrent())
                return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
        }
        catch { return false; }
    }

    public static ExecResult Run(string exe, string arguments, int timeoutMs)
    {
        var result = new ExecResult { Exe = exe, Args = arguments };
        try
        {
            var psi = new ProcessStartInfo(exe, arguments)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            using (var process = new Process())
            {
                process.StartInfo = psi;
                if (!process.Start())
                {
                    result.Output = "process did not start";
                    return result;
                }
                result.Started = true;
                var stdout = process.StandardOutput.ReadToEndAsync();
                var stderr = process.StandardError.ReadToEndAsync();
                if (!process.WaitForExit(timeoutMs))
                {
                    result.TimedOut = true;
                    try { if (!process.HasExited) process.Kill(); } catch { }
                    try { process.WaitForExit(5000); } catch { }
                }
                string outText = "";
                string errText = "";
                try { outText = stdout.Result; } catch { }
                try { errText = stderr.Result; } catch { }
                result.Output = outText + errText;
                try { result.ExitCode = process.HasExited ? process.ExitCode : -1; } catch { result.ExitCode = -1; }
            }
        }
        catch (Exception ex)
        {
            result.Started = false;
            result.ExitCode = -1;
            result.Output = ex.GetType().Name + ": " + ex.Message;
        }
        return result;
    }

    public static ExecResult Run(string exe, IEnumerable<string> arguments, int timeoutMs)
    {
        var sb = new StringBuilder();
        foreach (var argument in arguments)
        {
            if (sb.Length > 0) sb.Append(' ');
            sb.Append(argument);
        }
        return Run(exe, sb.ToString(), timeoutMs);
    }

    public static void StartDetached(string exe, string arguments)
    {
        try
        {
            var psi = new ProcessStartInfo(exe, arguments)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden
            };
            Process.Start(psi);
        }
        catch { }
    }

    // CommandLineToArgvW-compatible quoting for a single argument.
    public static string Quote(string value)
    {
        if (value == null) return "\"\"";
        if (value.Length > 0 && value.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return value;
        var sb = new StringBuilder();
        sb.Append('"');
        int backslashes = 0;
        foreach (char c in value)
        {
            if (c == '\\') { backslashes++; continue; }
            if (c == '"')
            {
                sb.Append('\\', backslashes * 2 + 1);
                sb.Append('"');
                backslashes = 0;
                continue;
            }
            sb.Append('\\', backslashes);
            backslashes = 0;
            sb.Append(c);
        }
        sb.Append('\\', backslashes * 2);
        sb.Append('"');
        return sb.ToString();
    }
}

internal sealed class SetupLog : IDisposable
{
    private StreamWriter writer;

    // Optional live mirror of the log lines, used by the setup wizard to show
    // progress.  It is a plain static field so a closed wizard leaves nothing
    // subscribed; the setter is cleared when the operation ends.
    public static Action<string, string> LineWritten;

    public string Path { get; private set; }
    public bool Enabled { get { return writer != null; } }

    public SetupLog(string path)
    {
        Path = path;
        Open(path, false);
        if (writer == null)
        {
            string fallback = System.IO.Path.Combine(
                System.IO.Path.GetTempPath(),
                System.IO.Path.GetFileNameWithoutExtension(path) + "-fallback.log");
            if (!string.Equals(fallback, path, StringComparison.OrdinalIgnoreCase)) Open(fallback, false);
        }
    }

    private void Open(string path, bool append)
    {
        try
        {
            string directory = System.IO.Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
            writer = new StreamWriter(path, append, new UTF8Encoding(false));
            writer.AutoFlush = true;
            Path = path;
        }
        catch
        {
            writer = null;
        }
    }

    public void Header(string title, string mode, string[] args, string extra)
    {
        Line("INFO", "===== " + title + " =====");
        Line("INFO", "Mode: " + mode);
        Line("INFO", "Started local: " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss zzz", CultureInfo.InvariantCulture));
        Line("INFO", "Started UTC: " + DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture));
        Line("INFO", "Command line: " + string.Join(" ", args ?? new string[0]));
        Line("INFO", "Log file: " + Path);
        Line("INFO", "User: " + Safe(() => Environment.UserDomainName + "\\" + Environment.UserName));
        Line("INFO", "Administrator: " + Sys.IsAdmin());
        Line("INFO", "OS: " + Safe(() => Environment.OSVersion.VersionString) +
                      " 64-bitOS=" + Safe(() => Environment.Is64BitOperatingSystem.ToString()) +
                      " 64-bitProcess=" + Safe(() => Environment.Is64BitProcess.ToString()));
        Line("INFO", "Windows folder: " + Safe(() => Environment.GetFolderPath(Environment.SpecialFolder.Windows)));
        if (!string.IsNullOrEmpty(extra)) Line("INFO", extra);
    }

    private static string Safe(Func<string> accessor)
    {
        try { return accessor(); } catch { return "unknown"; }
    }

    private void WriteRaw(string level, string text)
    {
        if (writer == null) return;
        try
        {
            writer.WriteLine("[" + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture) + "] " +
                             level.PadRight(5) + " " + (text == null ? "" : text));
        }
        catch { }
        var mirror = LineWritten;
        if (mirror != null)
        {
            try { mirror(level, text == null ? "" : text); } catch { }
        }
    }

    public void Line(string level, string text)
    {
        WriteRaw(level, text);
    }

    public void Info(string text) { WriteRaw("INFO", text); }
    public void Step(string text) { WriteRaw("STEP", text); }
    public void Warn(string text) { WriteRaw("WARN", text); }
    public void Error(string text) { WriteRaw("ERROR", text); }

    public void Block(string level, string text)
    {
        if (string.IsNullOrEmpty(text)) return;
        string normalized = text.Replace("\r\n", "\n").Replace('\r', '\n');
        foreach (var line in normalized.Split('\n'))
            WriteRaw(level, "  " + line.TrimEnd());
    }

    public void Command(ExecResult result)
    {
        string suffix = result.TimedOut ? " (TIMEOUT)" : (result.Started ? "" : " (NOT STARTED)");
        WriteRaw(result.TimedOut ? "WARN" : "INFO",
                 "RUN " + result.Exe + " " + result.Args + " -> exit " + result.ExitCode + suffix);
        if (!string.IsNullOrEmpty(result.Output))
        {
            WriteRaw("INFO", "  output:");
            Block("INFO", NlText.Clip(result.Output, 20000));
        }
    }

    public void Result(int exitCode, string summary)
    {
        WriteRaw("RESULT", "exit=" + exitCode + " " + (summary == null ? "" : summary.Replace("\r", " ").Replace("\n", " ")));
        if (writer != null) { try { writer.Flush(); } catch { } }
    }

    public void Dispose()
    {
        if (writer != null)
        {
            try { writer.Flush(); writer.Dispose(); } catch { }
            writer = null;
        }
    }
}

internal static class NlPaths
{
    public static string Normalize(string path)
    {
        return System.IO.Path.GetFullPath(path).TrimEnd('\\');
    }

    public static bool IsSafeDirectory(string fullPath, out string reason)
    {
        reason = "";
        if (string.IsNullOrWhiteSpace(fullPath)) { reason = "empty path"; return false; }
        string normalized;
        try { normalized = Normalize(fullPath); }
        catch { reason = "invalid path"; return false; }
        string root = System.IO.Path.GetPathRoot(normalized);
        if (string.IsNullOrEmpty(root)) { reason = "path has no root"; return false; }
        if (normalized.Length <= root.TrimEnd('\\').Length + 1) { reason = "path is a drive root"; return false; }

        string[] reserved =
        {
            Environment.GetFolderPath(Environment.SpecialFolder.Windows),
            Environment.GetFolderPath(Environment.SpecialFolder.System),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            System.IO.Path.GetTempPath()
        };
        foreach (var item in reserved)
        {
            if (string.IsNullOrWhiteSpace(item)) continue;
            string candidate;
            try { candidate = Normalize(item); } catch { continue; }
            if (string.Equals(normalized, candidate, StringComparison.OrdinalIgnoreCase))
            {
                reason = "refusing to use reserved system folder " + candidate;
                return false;
            }
        }

        string windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        if (!string.IsNullOrWhiteSpace(windows))
        {
            try
            {
                string win = Normalize(windows);
                if (normalized.StartsWith(win + "\\", StringComparison.OrdinalIgnoreCase))
                {
                    reason = "refusing to use a path inside " + win;
                    return false;
                }
            }
            catch { }
        }
        return true;
    }

    public static bool IsEmptyDirectory(string path)
    {
        try { return Directory.Exists(path) && Directory.GetFileSystemEntries(path).Length == 0; }
        catch { return false; }
    }

    public static bool LooksLikeOurDirectory(string path)
    {
        try
        {
            if (!Directory.Exists(path)) return true;
            if (IsEmptyDirectory(path)) return true;
            if (File.Exists(System.IO.Path.Combine(path, NlConst.UiExeName))) return true;
            if (File.Exists(System.IO.Path.Combine(path, NlConst.RouterExeName))) return true;
            if (File.Exists(System.IO.Path.Combine(path, NlConst.UninstallerExeName))) return true;
            if (File.Exists(System.IO.Path.Combine(path, NlConst.SetupExeName))) return true;
            if (File.Exists(System.IO.Path.Combine(path, "SHA256SUMS.txt"))) return true;
            if (File.Exists(System.IO.Path.Combine(path, "VERSION.txt")) &&
                File.Exists(System.IO.Path.Combine(path, "BUILD_INFO.json"))) return true;
            // Partially deleted installation: only the driver subfolder may remain.
            if (File.Exists(System.IO.Path.Combine(path, "driver", NlConst.SysName))) return true;
            if (File.Exists(System.IO.Path.Combine(path, "driver", NlConst.CerName))) return true;
            if (File.Exists(System.IO.Path.Combine(path, "driver", NlConst.InfName))) return true;
            return false;
        }
        catch { return false; }
    }

    public static string EscapeBatch(string text)
    {
        return (text ?? "").Replace("%", "%%");
    }

    public static void ScheduleDelete(string path, SetupLog log)
    {
        try
        {
            string cmdPath = System.IO.Path.Combine(
                System.IO.Path.GetTempPath(), "nlmatrix-cleanup-" + Guid.NewGuid().ToString("N") + ".cmd");
            var sb = new StringBuilder();
            sb.AppendLine("@echo off");
            sb.AppendLine("timeout /t 2 /nobreak >nul");
            sb.AppendLine("rmdir /s /q \"" + EscapeBatch(path) + "\" >nul 2>&1");
            sb.AppendLine("del /q \"%~f0\" >nul 2>&1");
            File.WriteAllText(cmdPath, sb.ToString(), Encoding.ASCII);
            Sys.StartDetached("cmd.exe", "/c " + Sys.Quote(cmdPath));
            log.Info("Scheduled delayed deletion of " + path);
        }
        catch (Exception ex)
        {
            log.Warn("Could not schedule deletion of " + path + ": " + ex.Message);
        }
    }

    public static void CleanupStaleSiblings(string appDir, SetupLog log)
    {
        try
        {
            string full = Normalize(appDir);
            string parent = System.IO.Path.GetDirectoryName(full);
            string leaf = System.IO.Path.GetFileName(full);
            if (string.IsNullOrEmpty(parent) || string.IsNullOrEmpty(leaf)) return;
            foreach (var dir in Directory.GetDirectories(parent, leaf + ".old-*"))
            {
                try { Directory.Delete(dir, true); log.Info("Removed stale folder " + dir); }
                catch { ScheduleDelete(dir, log); }
            }
        }
        catch (Exception ex)
        {
            log.Warn("Could not check for stale .old-* folders: " + ex.Message);
        }
    }
}

internal static class NlReg
{
    public static RegistryKey OpenBase()
    {
        return RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
    }

    public static bool HasUninstallEntry()
    {
        foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            try
            {
                using (var baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view))
                using (var key = baseKey.OpenSubKey(NlConst.UninstallKeyPath))
                    if (key != null) return true;
            }
            catch { }
        }
        return false;
    }

    public static string ReadInstallLocation()
    {
        foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            try
            {
                using (var baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view))
                using (var key = baseKey.OpenSubKey(NlConst.UninstallKeyPath))
                {
                    if (key == null) continue;
                    var value = key.GetValue("InstallLocation") as string;
                    if (!string.IsNullOrWhiteSpace(value)) return value;
                }
            }
            catch { }
        }
        return "";
    }

    public static void WriteUninstallEntry(string appDir, string version, string uninstallerPath,
                                           string uiPath, long estimatedSizeKb, bool setupCopyPresent, SetupLog log)
    {
        try
        {
            using (var baseKey = OpenBase())
            using (var key = baseKey.CreateSubKey(NlConst.UninstallKeyPath))
            {
                if (key == null) throw new IOException("cannot create " + NlConst.UninstallKeyPath);
                key.SetValue("DisplayName", NlConst.AppName, RegistryValueKind.String);
                key.SetValue("DisplayVersion", version ?? "", RegistryValueKind.String);
                key.SetValue("Publisher", NlConst.Publisher, RegistryValueKind.String);
                key.SetValue("InstallLocation", appDir, RegistryValueKind.String);
                key.SetValue("InstallDate", DateTime.Now.ToString("yyyyMMdd", CultureInfo.InvariantCulture), RegistryValueKind.String);
                key.SetValue("DisplayIcon", uiPath + ",0", RegistryValueKind.String);
                key.SetValue("UninstallString", Sys.Quote(uninstallerPath), RegistryValueKind.String);
                key.SetValue("QuietUninstallString", Sys.Quote(uninstallerPath) + " /silent", RegistryValueKind.String);
                if (setupCopyPresent)
                    key.SetValue("ModifyPath", Sys.Quote(System.IO.Path.Combine(appDir, NlConst.SetupExeName)) + " --mode repair",
                                 RegistryValueKind.String);
                key.SetValue("NoModify", 0, RegistryValueKind.DWord);
                key.SetValue("NoRepair", 0, RegistryValueKind.DWord);
                key.SetValue("EstimatedSize", (int)Math.Max(0, Math.Min(int.MaxValue, estimatedSizeKb)), RegistryValueKind.DWord);
                int major = 0, minor = 0;
                var parts = (version ?? "").Split('.');
                if (parts.Length > 0) int.TryParse(parts[0], NumberStyles.Integer, CultureInfo.InvariantCulture, out major);
                if (parts.Length > 1) int.TryParse(parts[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out minor);
                key.SetValue("VersionMajor", major, RegistryValueKind.DWord);
                key.SetValue("VersionMinor", minor, RegistryValueKind.DWord);
            }
            log.Info("Add/Remove entry written: " + NlConst.UninstallKeyPath);

            // A pre-0.6 installer may have created a 32-bit-view entry; remove it so
            // Windows never shows two NL Audio Matrix entries.
            try
            {
                using (var base32 = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32))
                using (var parent = base32.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall", true))
                    if (parent != null) parent.DeleteSubKeyTree("NLMatrix", false);
            }
            catch { }
        }
        catch (Exception ex)
        {
            throw new SetupException(5, "Cannot write the Add/Remove Programs entry: " + ex.Message);
        }
    }

    public static void DeleteUninstallEntries(SetupLog log)
    {
        foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            try
            {
                using (var baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view))
                using (var parent = baseKey.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall", true))
                {
                    if (parent == null) continue;
                    if (Array.IndexOf(parent.GetSubKeyNames(), "NLMatrix") >= 0)
                    {
                        parent.DeleteSubKeyTree("NLMatrix", false);
                        log.Info("Removed Add/Remove entry (" + view + ").");
                    }
                }
            }
            catch (Exception ex)
            {
                log.Warn("Could not remove Add/Remove entry (" + view + "): " + ex.Message);
            }
        }
    }
}

internal static class NlCert
{
    public static X509Certificate2 Load(string cerPath, SetupLog log)
    {
        if (!File.Exists(cerPath))
            throw new SetupException(6, "Test certificate is missing from the payload: " + cerPath);
        try
        {
            var certificate = new X509Certificate2(cerPath);
            log.Info("Loaded test certificate " + cerPath + " (subject " + certificate.Subject +
                     ", thumbprint " + certificate.Thumbprint + ", not after " + certificate.NotAfter.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) + ").");
            if (DateTime.Now > certificate.NotAfter)
                log.Warn("The test certificate expired on " + certificate.NotAfter.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) + ".");
            return certificate;
        }
        catch (Exception ex)
        {
            throw new SetupException(6, "Cannot read the test certificate " + cerPath + ": " + ex.Message);
        }
    }

    public static bool IsTrusted(X509Certificate2 certificate, StoreName storeName)
    {
        try
        {
            using (var store = new X509Store(storeName, StoreLocation.LocalMachine))
            {
                store.Open(OpenFlags.ReadOnly);
                foreach (var candidate in store.Certificates)
                {
                    if (string.Equals(candidate.Thumbprint, certificate.Thumbprint, StringComparison.OrdinalIgnoreCase))
                        return true;
                }
            }
        }
        catch { }
        return false;
    }

    public static bool EnsureTrusted(X509Certificate2 certificate, SetupLog log)
    {
        bool changed = false;
        foreach (var storeName in new[] { StoreName.Root, StoreName.TrustedPublisher })
        {
            if (IsTrusted(certificate, storeName))
            {
                log.Info("Certificate already trusted in " + storeName + ".");
                continue;
            }
            try
            {
                using (var store = new X509Store(storeName, StoreLocation.LocalMachine))
                {
                    store.Open(OpenFlags.ReadWrite);
                    store.Add(certificate);
                }
                changed = true;
                log.Info("Test certificate imported into " + storeName + ".");
            }
            catch (Exception ex)
            {
                throw new SetupException(6, "Cannot import the test certificate into " + storeName + ": " + ex.Message);
            }
        }
        return changed;
    }

    public static bool IsAnyTrusted(SetupLog log)
    {
        foreach (var storeName in new[] { StoreName.Root, StoreName.TrustedPublisher })
        {
            try
            {
                using (var store = new X509Store(storeName, StoreLocation.LocalMachine))
                {
                    store.Open(OpenFlags.ReadOnly);
                    foreach (var certificate in store.Certificates)
                    {
                        if (MatchesOurName(certificate)) return true;
                    }
                }
            }
            catch { }
        }
        return false;
    }

    private static bool MatchesOurName(X509Certificate2 certificate)
    {
        try
        {
            if (string.Equals(certificate.GetNameInfo(X509NameType.SimpleName, false),
                              NlConst.CertCommonName, StringComparison.OrdinalIgnoreCase)) return true;
        }
        catch { }
        return (certificate.Subject ?? "").IndexOf("CN=" + NlConst.CertCommonName, StringComparison.OrdinalIgnoreCase) >= 0;
    }

    public static int RemoveTrusted(string cerPath, SetupLog log)
    {
        string thumbprint = null;
        if (!string.IsNullOrEmpty(cerPath) && File.Exists(cerPath))
        {
            try
            {
                using (var certificate = new X509Certificate2(cerPath))
                    thumbprint = certificate.Thumbprint;
            }
            catch { }
        }

        int removed = 0;
        foreach (var storeName in new[] { StoreName.Root, StoreName.TrustedPublisher })
        {
            try
            {
                using (var store = new X509Store(storeName, StoreLocation.LocalMachine))
                {
                    store.Open(OpenFlags.ReadWrite);
                    var matches = new List<X509Certificate2>();
                    foreach (var certificate in store.Certificates)
                    {
                        // Remove the exact payload certificate and any older NLMatrix
                        // test certificate that shares our CN (a re-generated test
                        // certificate has a different thumbprint but the same CN).
                        bool match = MatchesOurName(certificate) ||
                                     (thumbprint != null && string.Equals(certificate.Thumbprint,
                                                                          thumbprint, StringComparison.OrdinalIgnoreCase));
                        if (match) matches.Add(certificate);
                    }
                    foreach (var certificate in matches)
                    {
                        store.Remove(certificate);
                        removed++;
                        log.Info("Removed certificate " + certificate.Thumbprint + " (" + certificate.Subject + ") from " + storeName + ".");
                    }
                }
            }
            catch (Exception ex)
            {
                log.Warn("Could not inspect the " + storeName + " certificate store: " + ex.Message);
            }
        }
        return removed;
    }
}

internal sealed class DriverPackageInfo
{
    public string InfPath = "";
    public string PublishedName = "";
    public string DriverVer = "";
}

internal static class NlDrivers
{
    public static bool IsOurInfText(string text)
    {
        if (string.IsNullOrEmpty(text)) return false;
        return text.IndexOf("NLMatrix Virtual Audio Device", StringComparison.OrdinalIgnoreCase) >= 0
            || text.IndexOf("Root\\NLMatrixVirtualAudio", StringComparison.OrdinalIgnoreCase) >= 0
            || text.IndexOf("nlmatrixaudio.sys", StringComparison.OrdinalIgnoreCase) >= 0;
    }

    public static string ParseDriverVerText(string text)
    {
        if (string.IsNullOrEmpty(text)) return "";
        foreach (var raw in text.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n'))
        {
            string line = raw.Trim();
            if (!line.StartsWith("DriverVer", StringComparison.OrdinalIgnoreCase)) continue;
            int index = line.IndexOf('=');
            if (index <= 0) continue;
            string value = line.Substring(index + 1).Trim();
            if (value.Length > 0) return value;
        }
        return "";
    }

    public static string ParseDriverVer(string infPath)
    {
        try
        {
            if (!File.Exists(infPath)) return "";
            return ParseDriverVerText(File.ReadAllText(infPath));
        }
        catch { return ""; }
    }

    public static List<DriverPackageInfo> FindInstalled(SetupLog log)
    {
        var packages = new List<DriverPackageInfo>();
        string infDirectory = System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.Windows), "INF");
        try
        {
            if (!Directory.Exists(infDirectory))
            {
                log.Warn("Windows INF directory not found: " + infDirectory);
                return packages;
            }
            foreach (var infPath in Directory.GetFiles(infDirectory, "oem*.inf"))
            {
                string text;
                try { text = File.ReadAllText(infPath); }
                catch { continue; }
                if (!IsOurInfText(text)) continue;
                packages.Add(new DriverPackageInfo
                {
                    InfPath = infPath,
                    PublishedName = System.IO.Path.GetFileName(infPath),
                    DriverVer = ParseDriverVerText(text)
                });
            }
        }
        catch (Exception ex)
        {
            log.Warn("Could not enumerate the driver store: " + ex.Message);
        }
        return packages;
    }

    public static bool HasVersion(List<DriverPackageInfo> packages, string payloadVersion)
    {
        if (string.IsNullOrEmpty(payloadVersion)) return false;
        foreach (var package in packages)
            if (NlText.SameVersion(package.DriverVer, payloadVersion)) return true;
        return false;
    }

    public static string DescribeInstalled(List<DriverPackageInfo> packages)
    {
        if (packages.Count == 0) return "none";
        var parts = new List<string>();
        foreach (var package in packages)
            parts.Add(package.PublishedName + " [" + NlText.DisplayVersion(package.DriverVer) + "]");
        return string.Join(", ", parts.ToArray());
    }

    public static bool Stage(SetupLog log, string infPath, out bool rebootRequired)
    {
        rebootRequired = false;
        if (!File.Exists(infPath))
            throw new SetupException(7, "Driver INF is missing from the payload: " + infPath);
        var result = Sys.Run("pnputil.exe",
            "/add-driver " + Sys.Quote(infPath) + " /install", 600000);
        log.Command(result);
        if (!result.Started)
            throw new SetupException(7, "pnputil.exe could not be started: " + result.Output);
        switch (result.ExitCode)
        {
            case 0:
                return true;
            case 259:
                // ERROR_NO_MORE_ITEMS: package already staged / device already has an
                // equal or better driver.  Staging itself succeeded.
                log.Info("pnputil reports the package is already present or a newer driver is in use (exit 259).");
                return true;
            case 3010:
            case 1641:
                rebootRequired = true;
                log.Warn("pnputil reports a reboot is required to finalize the driver package (exit " + result.ExitCode + ").");
                return true;
            default:
                throw new SetupException(7,
                    "pnputil failed to stage the NLMatrix driver package (exit " + result.ExitCode + "). " +
                    NlText.Clip(result.Output, 1200));
        }
    }

    // Removes only NLMatrix driver packages whose DriverVer differs from the
    // version that is now installed.  Older versions are kept when their version
    // cannot be read, and no other vendor's package is ever touched.
    public static int RemoveObsolete(SetupLog log, string keepVersion)
    {
        if (string.IsNullOrEmpty(keepVersion)) return 0;
        int removed = 0;
        foreach (var package in FindInstalled(log))
        {
            if (NlText.SameVersion(package.DriverVer, keepVersion))
            {
                log.Info("Keeping current NL driver package " + package.PublishedName +
                         " [" + NlText.DisplayVersion(package.DriverVer) + "].");
                continue;
            }
            if (NlText.NormalizeVersion(package.DriverVer).Length == 0)
            {
                log.Warn("Keeping driver package " + package.PublishedName + ": its DriverVer could not be read.");
                continue;
            }
            var result = Sys.Run("pnputil.exe",
                "/delete-driver " + Sys.Quote(package.PublishedName) + " /uninstall", 180000);
            log.Command(result);
            if (result.ExitCode == 0 || result.ExitCode == 3010 || result.ExitCode == 1641)
            {
                removed++;
                log.Info("Removed obsolete driver package " + package.PublishedName +
                         " [" + NlText.DisplayVersion(package.DriverVer) + "].");
            }
            else
            {
                log.Warn("Could not remove the obsolete driver package " + package.PublishedName +
                         " (exit " + result.ExitCode + "); it stays in the DriverStore.");
            }
        }
        return removed;
    }

    public static bool DeleteInstalled(SetupLog log, out int removed, out bool rebootRequired)
    {
        removed = 0;
        rebootRequired = false;
        bool allOk = true;
        foreach (var package in FindInstalled(log))
        {
            var result = Sys.Run("pnputil.exe",
                "/delete-driver " + Sys.Quote(package.PublishedName) + " /uninstall /force", 180000);
            log.Command(result);
            switch (result.ExitCode)
            {
                case 0:
                    removed++;
                    log.Info("Removed driver package " + package.PublishedName + ".");
                    break;
                case 3010:
                case 1641:
                    removed++;
                    rebootRequired = true;
                    log.Warn("Driver package " + package.PublishedName + " removed; reboot required (exit " + result.ExitCode + ").");
                    break;
                case 1168:
                case 2:
                    log.Info("Driver package " + package.PublishedName + " is already absent.");
                    break;
                default:
                    allOk = false;
                    rebootRequired = true;
                    log.Warn("Could not remove driver package " + package.PublishedName +
                             " (exit " + result.ExitCode + "): " + NlText.Clip(result.Output, 800));
                    break;
            }
        }
        return allOk;
    }
}

internal static class NlDevices
{
    public static string FindDevcon(SetupLog log, string appDir, string payloadRoot)
    {
        var candidates = new List<string>();
        if (!string.IsNullOrEmpty(appDir))
        {
            candidates.Add(System.IO.Path.Combine(appDir, "driver", NlConst.DevconName));
            candidates.Add(System.IO.Path.Combine(appDir, NlConst.DevconName));
        }
        if (!string.IsNullOrEmpty(payloadRoot))
            candidates.Add(System.IO.Path.Combine(payloadRoot, "driver", NlConst.DevconName));
        foreach (var candidate in candidates)
        {
            if (File.Exists(candidate))
            {
                log.Info("Using devcon: " + candidate);
                return candidate;
            }
        }
        log.Warn("devcon.exe was not found in the installation or payload; pnputil fallback will be used.");
        return "";
    }

    // Returns the device instance IDs that carry this hardware ID.  Root-enumerated
    // devices do not necessarily live under Enum\<hwid> (the ROOT enumerator may
    // use its own instance key, e.g. ROOT\MEDIA\0000), so the authoritative answer
    // comes from SetupAPI: every present device whose SPDRP_HARDWAREID matches.
    public static List<string> RootInstanceIds(string hwid)
    {
        var byHardware = PresentInstanceIds(hwid);
        if (byHardware.Count > 0) return byHardware;

        // Fallback for a device that is registered but not present right now.
        var instances = new List<string>();
        if (string.IsNullOrEmpty(hwid)) return instances;
        var parts = hwid.Split('\\');
        if (parts.Length < 2) return instances;
        string enumPath = @"SYSTEM\CurrentControlSet\Enum\" + parts[0] + "\\" + parts[1];
        try
        {
            using (var baseKey = NlReg.OpenBase())
            using (var key = baseKey.OpenSubKey(enumPath))
            {
                if (key == null) return instances;
                foreach (var name in key.GetSubKeyNames())
                    instances.Add(parts[0] + "\\" + parts[1] + "\\" + name);
            }
        }
        catch { }
        return instances;
    }

    private const int DIGCF_PRESENT = 0x00000002;
    private const int DIGCF_ALLCLASSES = 0x00000004;
    private const int SPDRP_HARDWAREID = 0x00000001;

    [StructLayout(LayoutKind.Sequential)]
    private struct SP_DEVINFO_DATA
    {
        public int cbSize;
        public Guid ClassGuid;
        public int DevInst;
        public IntPtr Reserved;
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SetupDiGetClassDevsW(IntPtr classGuid, string enumerator, IntPtr hwndParent, int flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiEnumDeviceInfo(IntPtr deviceInfoSet, int memberIndex, ref SP_DEVINFO_DATA deviceInfoData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetupDiGetDeviceInstanceIdW(IntPtr deviceInfoSet, ref SP_DEVINFO_DATA deviceInfoData,
                                                           StringBuilder deviceInstanceId, int deviceInstanceIdSize,
                                                           out int requiredSize);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr deviceInfoSet, ref SP_DEVINFO_DATA deviceInfoData,
                                                                 int property, out int propertyRegDataType,
                                                                 byte[] propertyBuffer, int propertyBufferSize,
                                                                 out int requiredSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

    public static List<string> PresentInstanceIds(string hwid)
    {
        var matches = new List<string>();
        if (string.IsNullOrEmpty(hwid)) return matches;
        IntPtr set = IntPtr.Zero;
        try
        {
            set = SetupDiGetClassDevsW(IntPtr.Zero, null, IntPtr.Zero, DIGCF_PRESENT | DIGCF_ALLCLASSES);
            if (set == IntPtr.Zero || set == new IntPtr(-1)) return matches;
            var data = new SP_DEVINFO_DATA();
            data.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            for (int index = 0; SetupDiEnumDeviceInfo(set, index, ref data); index++)
            {
                var buffer = new StringBuilder(512);
                int required;
                if (!SetupDiGetDeviceInstanceIdW(set, ref data, buffer, buffer.Capacity, out required)) continue;
                string instanceId = buffer.ToString();
                if (!HardwareIdMatches(set, ref data, hwid)) continue;
                matches.Add(instanceId);
            }
        }
        catch { }
        finally
        {
            if (set != IntPtr.Zero && set != new IntPtr(-1))
            {
                try { SetupDiDestroyDeviceInfoList(set); } catch { }
            }
        }
        return matches;
    }

    private static bool HardwareIdMatches(IntPtr set, ref SP_DEVINFO_DATA data, string hwid)
    {
        try
        {
            int type, required;
            var buffer = new byte[4096];
            if (!SetupDiGetDeviceRegistryPropertyW(set, ref data, SPDRP_HARDWAREID, out type, buffer, buffer.Length, out required))
                return false;
            string multi = Encoding.Unicode.GetString(buffer, 0, Math.Min(required, buffer.Length));
            foreach (var entry in multi.Split('\0'))
            {
                if (string.Equals(entry.Trim(), hwid, StringComparison.OrdinalIgnoreCase)) return true;
            }
        }
        catch { }
        return false;
    }

    // The INF that installed a device instance (SPDRP_INF_PATH).  The Enum "Driver"
    // value of a root device points at the class key instead, so this is the
    // reliable way to name the driver package that is actually bound.
    public static string InfPathForInstance(string instanceId)
    {
        if (string.IsNullOrEmpty(instanceId)) return "";
        IntPtr set = IntPtr.Zero;
        try
        {
            set = SetupDiGetClassDevsW(IntPtr.Zero, null, IntPtr.Zero, DIGCF_PRESENT | DIGCF_ALLCLASSES);
            if (set == IntPtr.Zero || set == new IntPtr(-1)) return "";
            var data = new SP_DEVINFO_DATA();
            data.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            for (int index = 0; SetupDiEnumDeviceInfo(set, index, ref data); index++)
            {
                var buffer = new StringBuilder(512);
                int required;
                if (!SetupDiGetDeviceInstanceIdW(set, ref data, buffer, buffer.Capacity, out required)) continue;
                if (!string.Equals(buffer.ToString(), instanceId, StringComparison.OrdinalIgnoreCase)) continue;

                int type, size;
                var bytes = new byte[1024];
                // SPDRP_INF_PATH
                if (!SetupDiGetDeviceRegistryPropertyW(set, ref data, 0x0000000A, out type, bytes, bytes.Length, out size))
                    return "";
                return Encoding.Unicode.GetString(bytes, 0, Math.Min(size, bytes.Length)).TrimEnd('\0').Trim();
            }
        }
        catch { }
        finally
        {
            if (set != IntPtr.Zero && set != new IntPtr(-1))
            {
                try { SetupDiDestroyDeviceInfoList(set); } catch { }
            }
        }
        return "";
    }

    public static bool Exists(SetupLog log, string devcon, string hwid)
    {
        if (!string.IsNullOrEmpty(devcon) && File.Exists(devcon))
        {
            var result = Sys.Run(devcon, "status " + hwid, 60000);
            log.Command(result);
            if (result.Started)
            {
                string text = result.OutputOrEmpty;
                if (text.IndexOf("No matching devices", StringComparison.OrdinalIgnoreCase) >= 0 ||
                    text.IndexOf("No devices", StringComparison.OrdinalIgnoreCase) >= 0)
                    return false;
                if (text.IndexOf("matching device", StringComparison.OrdinalIgnoreCase) >= 0 ||
                    text.IndexOf(hwid, StringComparison.OrdinalIgnoreCase) >= 0)
                    return true;
                log.Warn("devcon status output was not recognised; falling back to the device registry.");
            }
            else
            {
                log.Warn("devcon could not be started; falling back to the device registry.");
            }
        }
        return RootInstanceIds(hwid).Count > 0;
    }

    public static string StatusText(SetupLog log, string devcon, string hwid)
    {
        if (string.IsNullOrEmpty(devcon) || !File.Exists(devcon)) return "";
        var result = Sys.Run(devcon, "status " + hwid, 60000);
        log.Command(result);
        return result.OutputOrEmpty;
    }

    public static bool ServiceExists(SetupLog log)
    {
        var result = Sys.Run("sc.exe", "query " + NlConst.ServiceName, 30000);
        return result.Started && result.ExitCode == 0;
    }

    public static bool ServiceRunning(SetupLog log)
    {
        var result = Sys.Run("sc.exe", "query " + NlConst.ServiceName, 30000);
        if (!result.Started || result.ExitCode != 0) return false;
        return Regex.IsMatch(result.OutputOrEmpty, "\\bRUNNING\\b");
    }

    public static bool StartService(SetupLog log, int attempts)
    {
        var result = Sys.Run("sc.exe", "start " + NlConst.ServiceName, 120000);
        log.Command(result);
        if (result.ExitCode != 0 && result.ExitCode != 1056)
            log.Warn("sc start " + NlConst.ServiceName + " returned exit " + result.ExitCode + ".");
        for (int i = 0; i < attempts; i++)
        {
            if (ServiceRunning(log)) return true;
            Thread.Sleep(750);
        }
        return ServiceRunning(log);
    }

    public static void DeleteService(SetupLog log)
    {
        if (!ServiceExists(log)) return;
        var result = Sys.Run("sc.exe", "delete " + NlConst.ServiceName, 60000);
        log.Command(result);
        if (result.ExitCode == 0)
            log.Info("Removed service " + NlConst.ServiceName + ".");
        else if (result.ExitCode == 1060 || result.ExitCode == 1072)
            log.Info("Service " + NlConst.ServiceName + " was already removed or is marked for deletion.");
        else
            log.Warn("Could not remove service " + NlConst.ServiceName + " (exit " + result.ExitCode + ").");
    }

    public static bool RemoveDevice(SetupLog log, string devcon, string hwid, out bool rebootRequired)
    {
        rebootRequired = false;
        bool present = Exists(log, devcon, hwid);
        if (!present)
        {
            log.Info("Device not present: " + hwid);
            return true;
        }

        if (!string.IsNullOrEmpty(devcon) && File.Exists(devcon))
        {
            var result = Sys.Run(devcon, "remove " + hwid, 120000);
            log.Command(result);
            if (result.ExitCode == 1) rebootRequired = true;
            else if (result.ExitCode != 0)
                log.Warn("devcon remove " + hwid + " returned exit " + result.ExitCode + ".");
        }

        if (!Exists(log, "", hwid)) return true;

        // Robust fallback: remove the exact ROOT-enumerated instances with pnputil.
        var instances = RootInstanceIds(hwid);
        if (instances.Count == 0)
        {
            log.Warn("Device " + hwid + " appears present but no Enum\\ROOT instance was found.");
            return false;
        }
        foreach (var instance in instances)
        {
            var result = Sys.Run("pnputil.exe", "/remove-device " + Sys.Quote(instance), 120000);
            log.Command(result);
            if (result.ExitCode == 3010 || result.ExitCode == 1641) rebootRequired = true;
            else if (result.ExitCode == 0) { }
            else if (result.ExitCode == 1168 || result.ExitCode == 2)
                log.Info("Device " + instance + " is already absent.");
            else
                log.Warn("pnputil /remove-device " + instance + " returned exit " + result.ExitCode + ".");
        }
        return !Exists(log, "", hwid);
    }

    public static bool RestartDevice(SetupLog log, string devcon, string hwid, out bool rebootRequired)
    {
        rebootRequired = false;
        if (string.IsNullOrEmpty(devcon) || !File.Exists(devcon)) return false;
        var result = Sys.Run(devcon, "restart " + hwid, 120000);
        log.Command(result);
        if (result.ExitCode == 1) { rebootRequired = true; return true; }
        return result.ExitCode == 0;
    }

    public static bool IsTestSigningEnabled(SetupLog log)
    {
        var result = Sys.Run("bcdedit.exe", "/enum {current}", 60000);
        log.Command(result);
        if (!result.Started || result.ExitCode != 0) return false;
        foreach (var raw in result.OutputOrEmpty.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n'))
        {
            string line = raw.Trim();
            if (!line.StartsWith("testsigning", StringComparison.OrdinalIgnoreCase)) continue;
            // bcdedit uses a boolean element; accept English and common localized
            // renderings of "Yes"/"On" so a non-English VM is not misdetected.
            if (Regex.IsMatch(line, "\\b(yes|on|true|1|si|s\\u00ec|attivo|attivato)\\b",
                              RegexOptions.IgnoreCase)) return true;
        }
        return false;
    }

    public static bool EnableTestSigning(SetupLog log)
    {
        var result = Sys.Run("bcdedit.exe", "/set testsigning on", 60000);
        log.Command(result);
        if (!result.Started || result.ExitCode != 0)
            throw new SetupException(12,
                "Test signing could not be enabled (bcdedit exit " + result.ExitCode + "). " +
                "Secure Boot or a Windows policy may block it. " + NlText.Clip(result.Output, 1000));
        log.Warn("Test signing enabled; a reboot is required before the test-signed driver can load.");
        return true;
    }

    public static string BoundDriverVersion(SetupLog log, List<DriverPackageInfo> packages, string hwid)
    {
        foreach (var instance in RootInstanceIds(hwid))
        {
            try
            {
                using (var baseKey = NlReg.OpenBase())
                using (var key = baseKey.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\" + instance))
                {
                    if (key == null) continue;
                    var driverValue = key.GetValue("Driver") as string;
                    if (string.IsNullOrWhiteSpace(driverValue)) continue;
                    string infName = System.IO.Path.GetFileName(driverValue.Replace('/', '\\'));
                    if (string.IsNullOrEmpty(infName)) continue;
                    foreach (var package in packages)
                        if (string.Equals(package.PublishedName, infName, StringComparison.OrdinalIgnoreCase))
                            return package.DriverVer;
                }
            }
            catch (Exception ex)
            {
                log.Warn("Could not read the bound driver for " + instance + ": " + ex.Message);
            }
        }
        return "";
    }
}

internal sealed class EndpointReport
{
    public int TotalCount;
    public int ActiveCount;
    public int ActiveCapture;
    public int ActiveRender;
    public readonly List<string> Names = new List<string>();
    public readonly List<string> MissingExpected = new List<string>();

    public string Summary
    {
        get
        {
            return "endpoints=" + TotalCount + " active=" + ActiveCount +
                   " captureActive=" + ActiveCapture + " renderActive=" + ActiveRender +
                   (MissingExpected.Count > 0 ? " missing=" + string.Join(",", MissingExpected.ToArray()) : " missing=none");
        }
    }
}

internal static class NlEndpoints
{
    private static string ReadFriendlyName(RegistryKey endpointKey)
    {
        try
        {
            using (var properties = endpointKey.OpenSubKey("Properties"))
            {
                if (properties == null) return "";
                object value = properties.GetValue(NlConst.FriendlyNameValueName);
                if (value is string) return ((string)value).Trim();
                if (value is byte[])
                {
                    var bytes = (byte[])value;
                    return Encoding.Unicode.GetString(bytes).TrimEnd('\0', ' ').Trim();
                }
            }
        }
        catch { }
        return "";
    }

    private static int ReadDeviceState(RegistryKey endpointKey)
    {
        try
        {
            object value = endpointKey.GetValue("DeviceState");
            if (value is int) return (int)value;
            if (value != null) return Convert.ToInt32(value, CultureInfo.InvariantCulture);
        }
        catch { }
        return 0;
    }

    public static EndpointReport Collect(SetupLog log)
    {
        var report = new EndpointReport();
        foreach (var direction in new[] { "Render", "Capture" })
        {
            try
            {
                using (var baseKey = NlReg.OpenBase())
                using (var root = baseKey.OpenSubKey(
                           @"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\" + direction))
                {
                    if (root == null) continue;
                    foreach (var name in root.GetSubKeyNames())
                    {
                        using (var endpoint = root.OpenSubKey(name))
                        {
                            if (endpoint == null) continue;
                            string friendlyName = ReadFriendlyName(endpoint);
                            if (string.IsNullOrEmpty(friendlyName)) continue;
                            bool nlMatrix = friendlyName.StartsWith("NL Mic", StringComparison.OrdinalIgnoreCase)
                                         || friendlyName.StartsWith("NL Return", StringComparison.OrdinalIgnoreCase);
                            if (!nlMatrix) continue;

                            int state = ReadDeviceState(endpoint);
                            bool active = (state & 1) == 1;
                            report.TotalCount++;
                            if (active)
                            {
                                report.ActiveCount++;
                                if (friendlyName.StartsWith("NL Mic", StringComparison.OrdinalIgnoreCase)) report.ActiveCapture++;
                                else report.ActiveRender++;
                            }
                            report.Names.Add(friendlyName + (active ? " (active)" : " (inactive)"));
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                log.Warn("Could not enumerate " + direction + " audio endpoints: " + ex.Message);
            }
        }

        foreach (var expected in NlConst.ExpectedEndpointNames)
        {
            bool found = false;
            foreach (var name in report.Names)
                if (name.StartsWith(expected, StringComparison.OrdinalIgnoreCase)) { found = true; break; }
            if (!found) report.MissingExpected.Add(expected);
        }

        log.Info("Audio endpoint check: " + report.Summary);
        foreach (var name in report.Names) log.Info("  endpoint: " + name);
        return report;
    }

    public static EndpointReport WaitFor(SetupLog log, int timeoutMs)
    {
        var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        EndpointReport report = Collect(log);
        while (DateTime.UtcNow < deadline && (report.ActiveCapture == 0 || report.ActiveRender == 0))
        {
            Thread.Sleep(1000);
            report = Collect(log);
        }
        return report;
    }
}

internal sealed class HashEntry
{
    public string Hash = "";
    public string RelativePath = "";
}

internal static class NlManifest
{
    public static List<HashEntry> Parse(string manifestPath)
    {
        var entries = new List<HashEntry>();
        if (!File.Exists(manifestPath))
            throw new SetupException(4, "SHA256SUMS.txt is missing from the payload: " + manifestPath);
        foreach (var raw in File.ReadAllLines(manifestPath))
        {
            string line = raw.Trim();
            if (line.Length == 0 || line.StartsWith("#", StringComparison.Ordinal)) continue;
            int split = line.IndexOf("  ", StringComparison.Ordinal);
            if (split <= 0) split = line.IndexOf(' ');
            if (split <= 0) continue;
            string hash = line.Substring(0, split).Trim().ToLowerInvariant();
            string relative = line.Substring(split).TrimStart(' ', '\t', '*').Trim();
            if (relative.Length == 0 || hash.Length < 32) continue;
            relative = relative.Replace('/', '\\');
            if (System.IO.Path.IsPathRooted(relative) || relative.Contains(".."))
                throw new SetupException(4, "SHA256SUMS.txt contains an unsafe path: " + relative);
            entries.Add(new HashEntry { Hash = hash, RelativePath = relative });
        }
        return entries;
    }

    public static string Sha256(string path)
    {
        using (var sha = SHA256.Create())
        using (var stream = File.OpenRead(path))
            return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
    }

    public static List<string> VerifyPayload(string payloadRoot, List<HashEntry> entries, SetupLog log)
    {
        var problems = new List<string>();
        string root = NlPaths.Normalize(payloadRoot);
        foreach (var entry in entries)
        {
            string fullPath;
            try { fullPath = System.IO.Path.GetFullPath(System.IO.Path.Combine(root, entry.RelativePath)); }
            catch { problems.Add("invalid path: " + entry.RelativePath); continue; }
            if (!fullPath.StartsWith(root + "\\", StringComparison.OrdinalIgnoreCase))
            {
                problems.Add("path escapes the payload: " + entry.RelativePath);
                continue;
            }
            if (!File.Exists(fullPath)) { problems.Add("missing: " + entry.RelativePath); continue; }
            string actual;
            try { actual = Sha256(fullPath); }
            catch (Exception ex) { problems.Add("cannot hash " + entry.RelativePath + ": " + ex.Message); continue; }
            if (!string.Equals(actual, entry.Hash, StringComparison.OrdinalIgnoreCase))
                problems.Add("SHA256 mismatch: " + entry.RelativePath);
        }
        if (problems.Count > 0) log.Error("Payload integrity problems: " + problems.Count);
        return problems;
    }

    public static string MapInstallPath(string appDir, string relativePath)
    {
        string first = relativePath.Split('\\')[0];
        if (string.Equals(first, "app", StringComparison.OrdinalIgnoreCase))
            return System.IO.Path.Combine(appDir, relativePath.Substring(4));
        if (string.Equals(first, "driver", StringComparison.OrdinalIgnoreCase))
            return System.IO.Path.Combine(appDir, "driver", relativePath.Substring(7));
        if (string.Equals(first, "licenses", StringComparison.OrdinalIgnoreCase))
            return System.IO.Path.Combine(appDir, "LICENSES", relativePath.Substring(9));
        if (relativePath.IndexOf('\\') < 0)
            return System.IO.Path.Combine(appDir, relativePath);
        return null;
    }

    public static List<string> VerifyInstall(string appDir, List<HashEntry> entries, SetupLog log)
    {
        var problems = new List<string>();
        if (!Directory.Exists(appDir))
        {
            problems.Add("installation folder is missing");
            return problems;
        }
        foreach (var entry in entries)
        {
            string destination = MapInstallPath(appDir, entry.RelativePath);
            if (destination == null) continue;
            if (!File.Exists(destination)) { problems.Add("missing: " + entry.RelativePath); continue; }
            string actual;
            try { actual = Sha256(destination); }
            catch (Exception ex) { problems.Add("cannot hash " + entry.RelativePath + ": " + ex.Message); continue; }
            if (!string.Equals(actual, entry.Hash, StringComparison.OrdinalIgnoreCase))
                problems.Add("modified: " + entry.RelativePath);
        }
        return problems;
    }
}

internal static class NlAppFiles
{
    private static readonly string[] KnownProcesses =
    {
        NlConst.UiExeName, NlConst.RouterExeName, "NLMatrixLive.exe",
        "NLMatrixBridge.exe", "NLMatrixProbe.exe"
    };

    public static void StopKnownProcesses(SetupLog log, string appDir)
    {
        string prefix = "";
        try { if (!string.IsNullOrEmpty(appDir)) prefix = NlPaths.Normalize(appDir) + "\\"; }
        catch { }
        int current = Process.GetCurrentProcess().Id;
        foreach (var process in Process.GetProcesses())
        {
            try
            {
                if (process.Id == current) continue;
                string name = process.ProcessName + ".exe";
                bool known = false;
                foreach (var item in KnownProcesses)
                    if (string.Equals(item, name, StringComparison.OrdinalIgnoreCase)) { known = true; break; }
                if (!known) continue;

                string path = "";
                try { path = process.MainModule != null ? process.MainModule.FileName : ""; } catch { }
                if (!string.IsNullOrEmpty(path) && prefix.Length > 0 &&
                    !path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                {
                    log.Info("Leaving " + name + " (pid " + process.Id + ") running: " + path);
                    continue;
                }
                log.Info("Stopping " + name + " (pid " + process.Id + ").");
                try { process.Kill(); process.WaitForExit(3000); } catch { }
            }
            catch { }
        }
    }

    public static void Deploy(string payloadRoot, string appDir, SetupLog log)
    {
        string reason;
        if (!NlPaths.IsSafeDirectory(appDir, out reason))
            throw new SetupException(5, "Refusing to use installation folder '" + appDir + "': " + reason);

        StopKnownProcesses(log, appDir);
        CleanDirectory(appDir, log);

        CopyDirectory(System.IO.Path.Combine(payloadRoot, "app"), appDir, false, log);
        CopyDirectory(System.IO.Path.Combine(payloadRoot, "driver"), System.IO.Path.Combine(appDir, "driver"), false, log);
        CopyDirectory(System.IO.Path.Combine(payloadRoot, "licenses"), System.IO.Path.Combine(appDir, "LICENSES"), false, log);
        foreach (var name in new[] { "BUILD_INFO.json", "VERSION.txt", "SHA256SUMS.txt" })
        {
            string source = System.IO.Path.Combine(payloadRoot, name);
            if (File.Exists(source))
            {
                File.Copy(source, System.IO.Path.Combine(appDir, name), true);
                log.Info("Copied " + name + ".");
            }
        }
        CopySelf(log, appDir);
        log.Info("Application files deployed to " + appDir + ".");
    }

    public static void CleanDirectory(string appDir, SetupLog log)
    {
        string reason;
        if (!NlPaths.IsSafeDirectory(appDir, out reason))
            throw new SetupException(5, "Refusing to clean installation folder '" + appDir + "': " + reason);

        if (!Directory.Exists(appDir))
        {
            Directory.CreateDirectory(appDir);
            return;
        }
        if (!NlPaths.LooksLikeOurDirectory(appDir))
            throw new SetupException(5,
                "Installation folder '" + appDir + "' contains unrelated files and is not an NL Audio Matrix folder. " +
                "Choose another folder with /dir=<path> or remove it manually.");

        try
        {
            Directory.Delete(appDir, true);
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException || ex is IOException)
        {
            // A mapped runtime image can keep the folder locked even after taskkill.
            string old = appDir + ".old-" + Guid.NewGuid().ToString("N");
            log.Warn("Could not delete the previous installation folder (" + ex.Message + "); moving it aside.");
            try
            {
                Directory.Move(appDir, old);
            }
            catch (Exception moveError)
            {
                throw new SetupException(5,
                    "The previous installation folder '" + appDir + "' is locked and could not be replaced: " +
                    moveError.Message + ". Close NL Audio Matrix and retry.");
            }
            NlPaths.ScheduleDelete(old, log);
        }
        Directory.CreateDirectory(appDir);
        NlPaths.CleanupStaleSiblings(appDir, log);
    }

    private static void CopyDirectory(string source, string destination, bool overwrite, SetupLog log)
    {
        if (!Directory.Exists(source))
            throw new SetupException(5, "Payload folder is missing: " + source);
        Directory.CreateDirectory(destination);
        foreach (var file in Directory.GetFiles(source, "*", SearchOption.AllDirectories))
        {
            string relative = file.Substring(source.Length).TrimStart('\\');
            string target = System.IO.Path.Combine(destination, relative);
            string directory = System.IO.Path.GetDirectoryName(target);
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
            File.Copy(file, target, overwrite);
        }
        log.Info("Copied " + source + " -> " + destination);
    }

    public static void CopySelf(SetupLog log, string appDir)
    {
        try
        {
            string source = System.Reflection.Assembly.GetExecutingAssembly().Location;
            if (string.IsNullOrEmpty(source) || !File.Exists(source)) return;
            string target = System.IO.Path.Combine(appDir, NlConst.SetupExeName);
            if (string.Equals(NlPaths.Normalize(source), NlPaths.Normalize(target), StringComparison.OrdinalIgnoreCase))
                return;
            if (File.Exists(target) && string.Equals(NlMatrixHash(target), NlMatrixHash(source), StringComparison.OrdinalIgnoreCase))
                return;
            File.Copy(source, target, true);
            log.Info("Copied setup executable to " + target + " (for Add/Remove Programs repair).");
        }
        catch (Exception ex)
        {
            log.Warn("Could not keep a setup copy for repair: " + ex.Message);
        }
    }

    private static string NlMatrixHash(string path)
    {
        try { return NlManifest.Sha256(path); } catch { return ""; }
    }

    public static long DirectorySizeKb(string path)
    {
        long bytes = 0;
        try
        {
            foreach (var file in Directory.GetFiles(path, "*", SearchOption.AllDirectories))
            {
                try { bytes += new FileInfo(file).Length; } catch { }
            }
        }
        catch { }
        return bytes / 1024;
    }
}

internal static class NlShell
{
    public static string StartMenuFolder()
    {
        return System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms), NlConst.AppName);
    }

    public static bool WriteStartMenu(SetupLog log, string appDir)
    {
        bool ok = true;
        try
        {
            string folder = StartMenuFolder();
            Directory.CreateDirectory(folder);
            ok &= AddShortcut(System.IO.Path.Combine(folder, NlConst.AppName + ".lnk"),
                System.IO.Path.Combine(appDir, NlConst.UiExeName), "", "NL Audio Matrix patchbay",
                System.IO.Path.Combine(appDir, NlConst.UiExeName), log);
            ok &= AddShortcut(System.IO.Path.Combine(folder, "Uninstall NL Audio Matrix.lnk"),
                System.IO.Path.Combine(appDir, NlConst.UninstallerExeName), "", "Uninstall NL Audio Matrix",
                System.IO.Path.Combine(appDir, NlConst.UninstallerExeName), log);

            // Remove a legacy shortcut that older builds placed directly in Programs.
            string legacy = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms),
                                                   NlConst.AppName + ".lnk");
            if (File.Exists(legacy))
            {
                try { File.Delete(legacy); log.Info("Removed legacy Start Menu shortcut " + legacy); }
                catch { }
            }
        }
        catch (Exception ex)
        {
            log.Warn("Could not create the Start Menu entries: " + ex.Message);
            ok = false;
        }
        return ok;
    }

    public static bool AddShortcut(string linkPath, string targetPath, string arguments, string description,
                                   string iconPath, SetupLog log)
    {
        try
        {
            var shellType = Type.GetTypeFromProgID("WScript.Shell");
            if (shellType == null)
            {
                log.Warn("WScript.Shell is not available; cannot create " + linkPath);
                return false;
            }
            dynamic shell = Activator.CreateInstance(shellType);
            dynamic shortcut = shell.CreateShortcut(linkPath);
            shortcut.TargetPath = targetPath;
            shortcut.Arguments = arguments ?? "";
            shortcut.WorkingDirectory = System.IO.Path.GetDirectoryName(targetPath);
            shortcut.Description = description ?? "";
            if (!string.IsNullOrEmpty(iconPath)) shortcut.IconLocation = iconPath + ",0";
            shortcut.Save();
            log.Info("Start Menu shortcut written: " + linkPath);
            return true;
        }
        catch (Exception ex)
        {
            log.Warn("Could not write the shortcut " + linkPath + ": " + ex.Message);
            return false;
        }
    }

    public static void RemoveStartMenu(SetupLog log)
    {
        try
        {
            string folder = StartMenuFolder();
            if (Directory.Exists(folder))
            {
                Directory.Delete(folder, true);
                log.Info("Removed Start Menu folder " + folder);
            }
            string legacy = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms),
                                                   NlConst.AppName + ".lnk");
            if (File.Exists(legacy))
            {
                File.Delete(legacy);
                log.Info("Removed Start Menu shortcut " + legacy);
            }
        }
        catch (Exception ex)
        {
            log.Warn("Could not remove the Start Menu entries: " + ex.Message);
        }
    }
}

internal sealed class UninstallResult
{
    public bool NothingInstalled;
    public int RemovedDevices;
    public int RemovedPackages;
    public int RemovedCertificates;
    public bool AppFilesRemoved;
    public bool AppFilesScheduled;
    public bool RebootRecommended;
    public readonly List<string> Warnings = new List<string>();
    public string AppDir = "";
    public string CertFile = "";
}

internal static class NlUninstall
{
    public static UninstallResult Run(SetupLog log, string appDirOverride, out int exitCode)
    {
        var result = new UninstallResult();
        exitCode = 0;

        string appDir = ResolveAppDir(log, appDirOverride);
        result.AppDir = appDir;
        log.Info("Resolved installation folder: " + appDir);

        string devcon = NlDevices.FindDevcon(log, appDir, null);
        var packages = NlDrivers.FindInstalled(log);
        bool rootDevice = NlDevices.Exists(log, devcon, NlConst.RootHwid);
        bool legacyDevice = NlDevices.Exists(log, devcon, NlConst.LegacyHwid);
        bool arpEntry = NlReg.HasUninstallEntry();
        bool startMenu = Directory.Exists(NlShell.StartMenuFolder());
        bool appDirExists = Directory.Exists(appDir);
        bool ourAppDir = appDirExists && !NlPaths.IsEmptyDirectory(appDir) && NlPaths.LooksLikeOurDirectory(appDir);
        bool certificate = NlCert.IsAnyTrusted(log);
        bool service = NlDevices.ServiceExists(log);

        log.Info("Current state: arp=" + arpEntry + " startMenu=" + startMenu + " appDir=" + appDirExists +
                 " devices(root=" + rootDevice + ", legacy=" + legacyDevice + ")" +
                 " driverPackages=" + packages.Count + " certificates=" + certificate + " service=" + service);

        if (!arpEntry && !startMenu && !ourAppDir && !rootDevice && !legacyDevice &&
            packages.Count == 0 && !certificate && !service)
        {
            result.NothingInstalled = true;
            result.AppFilesRemoved = true;
            log.Info("Nothing is installed; no removal was necessary.");
            exitCode = 0;
            return result;
        }

        log.Step("Stopping NL Audio Matrix processes");
        NlAppFiles.StopKnownProcesses(log, appDir);

        // Keep a copy of the read-only verifier outside the application folder:
        // after a reboot-required removal it confirms that nothing is left.
        string uninstallVersion = NlDiagnostics.ReadVersionValue(appDir, "version");
        string verifySource = System.IO.Path.Combine(appDir, "NLMatrixVerify.exe");
        string verifyCopy = System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            NlConst.AppName, "NLMatrixVerify.exe");
        try
        {
            if (File.Exists(verifySource))
            {
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(verifyCopy));
                File.Copy(verifySource, verifyCopy, true);
                log.Info("Kept the verification helper for the post-reboot check: " + verifyCopy);
            }
        }
        catch (Exception ex) { log.Warn("Could not keep the verification helper: " + ex.Message); }

        bool reboot = false;

        log.Step("Removing NL Matrix audio devices (only the NLMatrix hardware IDs)");
        if (rootDevice || NlDevices.Exists(log, devcon, NlConst.RootHwid))
        {
            bool deviceReboot;
            bool removed = NlDevices.RemoveDevice(log, devcon, NlConst.RootHwid, out deviceReboot);
            reboot |= deviceReboot;
            if (removed) result.RemovedDevices++;
            else { result.Warnings.Add("The NLMatrix device (Root\\NLMatrixVirtualAudio) could not be removed; a reboot is required."); reboot = true; }
        }
        else log.Info("No " + NlConst.RootHwid + " device is present.");

        if (legacyDevice || NlDevices.Exists(log, devcon, NlConst.LegacyHwid))
        {
            bool deviceReboot;
            bool removed = NlDevices.RemoveDevice(log, devcon, NlConst.LegacyHwid, out deviceReboot);
            reboot |= deviceReboot;
            if (removed) result.RemovedDevices++;
            else { result.Warnings.Add("The legacy device (Root\\sysvad_ComponentizedAudioSample) could not be removed; a reboot is required."); reboot = true; }
        }
        else log.Info("No legacy " + NlConst.LegacyHwid + " device is present.");

        log.Step("Removing the NLMatrix driver service");
        NlDevices.DeleteService(log);

        log.Step("Removing NLMatrix driver packages from the DriverStore");
        int removedPackages;
        bool packageReboot;
        NlDrivers.DeleteInstalled(log, out removedPackages, out packageReboot);
        result.RemovedPackages = removedPackages;
        reboot |= packageReboot;

        log.Step("Removing the NLMatrix test certificate");
        result.CertFile = System.IO.Path.Combine(appDir, "driver", NlConst.CerName);
        result.RemovedCertificates = NlCert.RemoveTrusted(result.CertFile, log);

        log.Step("Removing Add/Remove Programs and Start Menu entries");
        NlReg.DeleteUninstallEntries(log);
        NlShell.RemoveStartMenu(log);

        log.Step("Cleaning temporary setup files");
        CleanupTemp(log);

        log.Step("Removing application files");
        DeleteAppDirectory(log, appDir, result);

        // Final state check.
        bool deviceStill = NlDevices.Exists(log, "", NlConst.RootHwid) || NlDevices.Exists(log, "", NlConst.LegacyHwid);
        if (deviceStill)
        {
            result.Warnings.Add("At least one NLMatrix device instance is still present; a reboot is required to finish removal.");
            reboot = true;
        }
        result.RebootRecommended = reboot;
        exitCode = reboot ? 20 : 0;

        // After a reboot-required removal, verify the removal automatically at
        // the next logon.  The verifier runs without administrator rights, so
        // Windows shows no UAC prompt for this check.
        if (reboot && File.Exists(verifyCopy))
        {
            NlPendingReboot.Write(log, "uninstall", appDir, uninstallVersion);
            NlPendingReboot.RegisterRunOnce(log, verifyCopy, "--post-reboot");
        }

        log.Info("Uninstall summary: devices=" + result.RemovedDevices + " driverPackages=" + result.RemovedPackages +
                 " certificates=" + result.RemovedCertificates + " appDirRemoved=" + result.AppFilesRemoved +
                 " appDirScheduled=" + result.AppFilesScheduled + " reboot=" + result.RebootRecommended);
        log.Info("Windows audio defaults and other vendors' audio devices were not modified.");
        log.Info("User presets/config in %APPDATA%\\" + NlConst.AppName + " were kept.");
        return result;
    }

    private static string ResolveAppDir(SetupLog log, string overridePath)
    {
        if (!string.IsNullOrWhiteSpace(overridePath))
        {
            log.Info("Using installation folder from the command line: " + overridePath);
            return overridePath;
        }
        string fromRegistry = NlReg.ReadInstallLocation();
        if (!string.IsNullOrWhiteSpace(fromRegistry))
        {
            log.Info("Using installation folder from Add/Remove Programs: " + fromRegistry);
            return fromRegistry;
        }
        if (Directory.Exists(NlConst.DefaultAppDir) && NlPaths.LooksLikeOurDirectory(NlConst.DefaultAppDir))
        {
            log.Info("No installation folder registered; using the default folder: " + NlConst.DefaultAppDir);
            return NlConst.DefaultAppDir;
        }
        string besideExe = System.IO.Path.GetDirectoryName(System.Reflection.Assembly.GetExecutingAssembly().Location);
        if (!string.IsNullOrWhiteSpace(besideExe))
        {
            log.Info("No installation folder registered; using the uninstaller folder: " + besideExe);
            return besideExe;
        }
        return NlConst.DefaultAppDir;
    }

    private static void CleanupTemp(SetupLog log)
    {
        try
        {
            string temp = System.IO.Path.GetTempPath();
            foreach (var directory in Directory.GetDirectories(temp, "NLMatrixSetup-*"))
            {
                string name = System.IO.Path.GetFileName(directory);
                if (string.IsNullOrEmpty(name) ||
                    !name.StartsWith("NLMatrixSetup-", StringComparison.OrdinalIgnoreCase)) continue;
                try
                {
                    Directory.Delete(directory, true);
                    log.Info("Removed temporary payload " + directory);
                }
                catch
                {
                    NlPaths.ScheduleDelete(directory, log);
                }
            }
            foreach (var pattern in new[] { "nlmatrix-*.cmd", "nlmatrix-*.log" })
            {
                foreach (var file in Directory.GetFiles(temp, pattern))
                {
                    try { File.Delete(file); log.Info("Removed temporary file " + file); }
                    catch { }
                }
            }

            // Setup copies left by the deployment scripts (scoped by our exact name).
            string windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
            foreach (var file in new[]
                     {
                         System.IO.Path.Combine(temp, "NLMatrixSetup.exe"),
                         System.IO.Path.Combine(temp, "NLMatrixSetup-error.log"),
                         System.IO.Path.Combine(windows, "Temp", "NLMatrixSetup.exe")
                     })
            {
                try
                {
                    if (File.Exists(file))
                    {
                        File.Delete(file);
                        log.Info("Removed temporary setup file " + file);
                    }
                }
                catch (Exception ex)
                {
                    log.Info("Could not remove temporary setup file " + file + ": " + ex.Message);
                }
            }
        }
        catch (Exception ex)
        {
            log.Warn("Could not clean temporary setup files: " + ex.Message);
        }
    }

    private static void DeleteAppDirectory(SetupLog log, string appDir, UninstallResult result)
    {
        if (string.IsNullOrWhiteSpace(appDir))
        {
            result.AppFilesRemoved = true;
            return;
        }
        string reason;
        if (!NlPaths.IsSafeDirectory(appDir, out reason))
        {
            log.Warn("Not deleting installation folder '" + appDir + "': " + reason);
            result.Warnings.Add("Installation folder was not deleted (" + reason + ").");
            result.AppFilesRemoved = true;
            return;
        }
        if (Directory.Exists(appDir) && !NlPaths.LooksLikeOurDirectory(appDir))
        {
            log.Warn("Not deleting '" + appDir + "': it does not look like an NL Audio Matrix folder.");
            result.Warnings.Add("Installation folder was not deleted because it contains unrelated files.");
            result.AppFilesRemoved = true;
            return;
        }

        if (Directory.Exists(appDir))
        {
            try
            {
                Directory.Delete(appDir, true);
                result.AppFilesRemoved = true;
                log.Info("Removed installation folder " + appDir);
            }
            catch (Exception ex)
            {
                log.Warn("Could not delete all application files immediately (" + ex.Message +
                         "); scheduling deletion after this process exits.");
                NlAppFiles.StopKnownProcesses(log, appDir);
                NlPaths.ScheduleDelete(appDir, log);
                result.AppFilesScheduled = true;
            }
        }
        else
        {
            result.AppFilesRemoved = true;
        }
        NlPaths.CleanupStaleSiblings(appDir, log);
    }
}
