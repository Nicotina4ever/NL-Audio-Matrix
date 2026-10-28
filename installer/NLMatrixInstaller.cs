using System.Reflection;
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("NL Audio Matrix Setup")]
[assembly: AssemblyDescription("NL Audio Matrix installer")]
[assembly: AssemblyCompany("Nicola Lanci")]
[assembly: AssemblyProduct("NL Audio Matrix")]
[assembly: AssemblyCopyright("Copyright (c) Nicola Lanci")]
[assembly: AssemblyVersion("0.5.0.0")]
[assembly: AssemblyFileVersion("0.5.0.0")]
[assembly: AssemblyInformationalVersion("0.5.0-alpha")]

internal static class Installer
{
    private const string AppName = "NL Audio Matrix";
    private const string RootHwid = "Root\\NLMatrixVirtualAudio";
    private const string LegacyHwid = "Root\\sysvad_ComponentizedAudioSample";
    private static bool silent;
    private static bool launch;
    private static string appDir = @"C:\NLMatrix";

    private static bool IsAdmin()
    {
        using (var identity = WindowsIdentity.GetCurrent())
            return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    private static void Info(string text)
    {
        if (!silent) MessageBox.Show(text, AppName);
    }

    private static void Error(string text)
    {
        if (!silent)
            MessageBox.Show(text, AppName, MessageBoxButtons.OK, MessageBoxIcon.Error);
    }

    private static int Run(string exe, string arguments)
    {
        try
        {
            var psi = new ProcessStartInfo(exe, arguments)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            using (var p = Process.Start(psi))
            {
                p.StandardOutput.ReadToEnd();
                p.StandardError.ReadToEnd();
                p.WaitForExit();
                return p.ExitCode;
            }
        }
        catch { return -1; }
    }

    private static string Hash(string path)
    {
        if (!File.Exists(path)) return "";
        using (var sha = SHA256.Create())
        using (var stream = File.OpenRead(path))
            return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "");
    }

    private static string ReadText(string path)
    {
        return File.Exists(path) ? File.ReadAllText(path) : "";
    }

    private static void CleanDirectory(string path)
    {
        var full = Path.GetFullPath(path).TrimEnd('\\');
        if (full.Length < 4 || Path.GetPathRoot(full).TrimEnd('\\').Equals(full, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Refusing to clean an unsafe installation path: " + full);
        if (Directory.Exists(full)) Directory.Delete(full, true);
        Directory.CreateDirectory(full);
    }

    private static void CopyTree(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var file in Directory.GetFiles(from, "*", SearchOption.AllDirectories))
        {
            var rel = file.Substring(from.Length).TrimStart('\\');
            var dst = Path.Combine(to, rel);
            Directory.CreateDirectory(Path.GetDirectoryName(dst));
            File.Copy(file, dst, true);
        }
    }

    private static void AddShortcut(string target, string link)
    {
        try
        {
            var shellType = Type.GetTypeFromProgID("WScript.Shell");
            dynamic shell = Activator.CreateInstance(shellType);
            dynamic shortcut = shell.CreateShortcut(link);
            shortcut.TargetPath = target;
            shortcut.WorkingDirectory = Path.GetDirectoryName(target);
            shortcut.Description = AppName;
            shortcut.Save();
        }
        catch { }
    }

    private static string ReadSetting(string path, string key)
    {
        foreach (var line in ReadText(path).Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries))
        {
            var idx = line.IndexOf('=');
            if (idx > 0 && line.Substring(0, idx).Trim().Equals(key, StringComparison.OrdinalIgnoreCase))
                return line.Substring(idx + 1).Trim();
        }
        return "";
    }

    private static bool TestSigningEnabled()
    {
        // Avoid enabling test signing when it is already active.  The installer
        // runs only on the disposable test VM.
        var psi = new ProcessStartInfo("bcdedit.exe", "/enum {current}")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        using (var p = Process.Start(psi))
        {
            var text = (p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd()).ToLowerInvariant();
            p.WaitForExit();
            return text.Contains("testsigning") && text.Contains("yes");
        }
    }

    private static bool InstallDriver(string driverDir)
    {
        var devcon = Path.Combine(driverDir, "devcon.exe");
        var inf = Path.Combine(driverDir, "ComponentizedAudioSample.inf");
        var sys = Path.Combine(driverDir, "NLMatrixAudio.sys");
        var cer = Path.Combine(driverDir, "NLMatrixTest.cer");
        if (!File.Exists(devcon) || !File.Exists(inf) || !File.Exists(sys) || !File.Exists(cer))
            throw new IOException("Driver payload is incomplete.");

        var installedSys = Path.Combine(Environment.SystemDirectory, "drivers", "NLMatrixAudio.sys");
        var sameDriver = File.Exists(installedSys)
                         && Hash(installedSys).Equals(Hash(sys), StringComparison.OrdinalIgnoreCase);
        var devicePresent = Run(devcon, "status " + RootHwid) == 0;
        if (sameDriver && devicePresent)
        {
            Console.WriteLine("NLMatrix driver already current; DriverStore untouched.");
            return false;
        }

        var reboot = false;
        if (!TestSigningEnabled())
        {
            Run("bcdedit.exe", "/set testsigning on");
            reboot = true;
        }
        Run("certutil.exe", "-addstore -f Root \"" + cer + "\"");
        Run("certutil.exe", "-addstore -f TrustedPublisher \"" + cer + "\"");

        if (Run("pnputil.exe", "/add-driver \"" + inf + "\" /install") != 0)
            throw new IOException("pnputil failed to stage the NLMatrix driver.");

        if (Run(devcon, "status " + LegacyHwid) == 0)
            Run(devcon, "remove " + LegacyHwid);

        if (Run(devcon, "status " + RootHwid) == 0)
        {
            if (Run(devcon, "update \"" + inf + "\" " + RootHwid) != 0)
                throw new IOException("devcon update failed.");
        }
        else if (Run(devcon, "install \"" + inf + "\" " + RootHwid) != 0)
            throw new IOException("devcon install failed.");
        return reboot;
    }

    [STAThread]
    private static int Main(string[] args)
    {
        silent = Array.Exists(args, a => a.Equals("/silent", StringComparison.OrdinalIgnoreCase));
        launch = Array.Exists(args, a => a.Equals("/launch", StringComparison.OrdinalIgnoreCase));
        foreach (var arg in args)
        {
            if (arg.StartsWith("/dir=", StringComparison.OrdinalIgnoreCase))
                appDir = arg.Substring(5).Trim('"');
        }
        Application.EnableVisualStyles();
        var temp = Path.Combine(Path.GetTempPath(), "NLMatrixSetup-" + Guid.NewGuid().ToString("N"));
        try
        {
            if (!IsAdmin())
            {
                Error("This installer needs administrator rights.");
                return 1;
            }

            Directory.CreateDirectory(temp);
            var zipPath = Path.Combine(temp, "payload.zip");
            using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
            {
                if (stream == null) { Error("Embedded payload is missing."); return 2; }
                using (var file = File.Create(zipPath)) stream.CopyTo(file);
            }
            var payload = Path.Combine(temp, "payload");
            ZipFile.ExtractToDirectory(zipPath, payload);

            Run("taskkill.exe", "/f /im NLMatrixUI.exe");
            Run("taskkill.exe", "/f /im NLMatrixRouter.exe");

            // Reproducible installation folder: remove every obsolete runtime
            // artifact, then copy only the current release payload.  User
            // profiles live under %APPDATA%\NL Audio Matrix and are untouched.
            CleanDirectory(appDir);
            CopyTree(Path.Combine(payload, "app"), appDir);
            CopyTree(Path.Combine(payload, "licenses"), Path.Combine(appDir, "LICENSES"));
            CopyTree(Path.Combine(payload, "driver"), Path.Combine(appDir, "driver"));
            foreach (var name in new[] { "BUILD_INFO.json", "VERSION.txt", "SHA256SUMS.txt" })
            {
                var src = Path.Combine(payload, name);
                if (File.Exists(src)) File.Copy(src, Path.Combine(appDir, name), true);
            }

            var versionFile = Path.Combine(appDir, "VERSION.txt");
            var version = ReadSetting(versionFile, "version");
            if (version.Length == 0) version = "0.5.0-alpha";
            var buildId = ReadSetting(versionFile, "buildId");

            var driverDir = Path.Combine(appDir, "driver");
            var reboot = InstallDriver(driverDir);

            var startMenu = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms), AppName);
            Directory.CreateDirectory(startMenu);
            AddShortcut(Path.Combine(appDir, "NLMatrixUI.exe"), Path.Combine(startMenu, AppName + ".lnk"));

            using (var key = Registry.LocalMachine.CreateSubKey(
                       @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NLMatrix"))
            {
                key.SetValue("DisplayName", AppName);
                key.SetValue("DisplayVersion", version);
                key.SetValue("Publisher", "Nicola Lanci");
                key.SetValue("InstallLocation", appDir);
                key.SetValue("DisplayIcon", Path.Combine(appDir, "NLMatrixUI.exe"));
                key.SetValue("UninstallString", "\"" + Path.Combine(appDir, "NLMatrixUninstall.exe") + "\"");
                key.SetValue("NoModify", 1, RegistryValueKind.DWord);
                key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            }

            if (launch && File.Exists(Path.Combine(appDir, "NLMatrixUI.exe")))
                Process.Start(new ProcessStartInfo(Path.Combine(appDir, "NLMatrixUI.exe"))
                { WorkingDirectory = appDir, UseShellExecute = true });

            if (!silent)
            {
                var msg = "NL Audio Matrix " + version + " installed in " + appDir + ".\nBuild ID: " + buildId;
                if (reboot) msg += "\n\nA reboot is required to enable the test-signed driver.";
                MessageBox.Show(msg, AppName, MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            return reboot ? 20 : 0;
        }
        catch (Exception ex)
        {
            try { File.WriteAllText(Path.Combine(Path.GetTempPath(), "NLMatrixSetup-error.log"), ex.ToString()); } catch { }
            Error("Installation failed:\n" + ex.Message);
            return 10;
        }
        finally
        {
            try { Directory.Delete(temp, true); } catch { }
        }
    }
}