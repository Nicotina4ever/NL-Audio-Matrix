using System.Reflection;
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Principal;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("NL Audio Matrix Setup")]
[assembly: AssemblyDescription("NL Audio Matrix installer")]
[assembly: AssemblyCompany("Nicola Lanci")]
[assembly: AssemblyProduct("NL Audio Matrix")]
[assembly: AssemblyCopyright("Copyright (c) Nicola Lanci")]
[assembly: AssemblyVersion("0.4.0.0")]
[assembly: AssemblyFileVersion("0.4.0.0")]
[assembly: System.Reflection.AssemblyInformationalVersion("0.4.0-alpha")]
internal static class Installer
{
    private const string AppName = "NL Audio Matrix";
    private static string appDir;
    private static string payloadDir;
    private static bool silent;

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

    private static void CopyDirectory(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var file in Directory.GetFiles(from))
            File.Copy(file, Path.Combine(to, Path.GetFileName(file)), true);
        foreach (var dir in Directory.GetDirectories(from))
            CopyDirectory(dir, Path.Combine(to, Path.GetFileName(dir)));
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

    [STAThread]
    private static int Main(string[] args)
    {
        silent = Array.Exists(args, a => a.Equals("/silent", StringComparison.OrdinalIgnoreCase));
        Application.EnableVisualStyles();
        try
        {
            if (!IsAdmin())
            {
                Error("This development installer needs administrator rights.");
                return 1;
            }

            var temp = Path.Combine(Path.GetTempPath(), "NLMatrixSetup-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(temp);
            var zipPath = Path.Combine(temp, "payload.zip");
            using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
            {
                if (stream == null)
                {
                    Error("Embedded payload is missing.");
                    return 2;
                }
                using (var file = File.Create(zipPath))
                    stream.CopyTo(file);
            }
            ZipFile.ExtractToDirectory(zipPath, temp);
            payloadDir = Path.Combine(temp, "payload");

            appDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
                                  AppName);
            CopyDirectory(Path.Combine(payloadDir, "app"), appDir);

            var drv = Path.Combine(payloadDir, "drv");
            var cer = Path.Combine(drv, "NLMatrixTest.cer");
            var inf = Path.Combine(drv, "ComponentizedAudioSample.inf");
            var devcon = Path.Combine(drv, "devcon.exe");
            if (!File.Exists(cer) || !File.Exists(inf) || !File.Exists(devcon))
            {
                Error("Driver payload is incomplete.");
                return 3;
            }

            Run("certutil.exe", "-addstore -f Root \"" + cer + "\"");
            Run("certutil.exe", "-addstore -f TrustedPublisher \"" + cer + "\"");
            Run("bcdedit.exe", "/set testsigning on");
            Run("pnputil.exe", "/add-driver \"" + inf + "\" /install");

            // Migrate the old single-pair root device to the multi-pair identity.
            Run(devcon, "remove Root\\sysvad_ComponentizedAudioSample");
            if (Run(devcon, "status Root\\NLMatrixVirtualAudio") == 0)
                Run(devcon, "update \"" + inf + "\" Root\\NLMatrixVirtualAudio");
            else
                Run(devcon, "install \"" + inf + "\" Root\\NLMatrixVirtualAudio");

            var startMenu = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms),
                                         AppName);
            Directory.CreateDirectory(startMenu);
            AddShortcut(Path.Combine(appDir, "NLMatrixUI.exe"),
                        Path.Combine(startMenu, AppName + ".lnk"));

            using (var key = Registry.LocalMachine.CreateSubKey(
                       @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NLMatrix"))
            {
                key.SetValue("DisplayName", AppName);
                key.SetValue("DisplayVersion", "0.4.0-alpha");
                key.SetValue("Publisher", "Nicola Lanci");
                key.SetValue("InstallLocation", appDir);
                key.SetValue("DisplayIcon", Path.Combine(appDir, "NLMatrixUI.exe"));
                key.SetValue("UninstallString", "\"" + Path.Combine(appDir, "NLMatrixUninstall.exe") + "\"");
                key.SetValue("NoModify", 1, RegistryValueKind.DWord);
                key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            }

            if (!silent)
            {
                var answer = MessageBox.Show(
                    "Installation completed.\n\nWindows must restart once to enable the test-signed driver.\n\nRestart now?",
                    AppName, MessageBoxButtons.YesNo, MessageBoxIcon.Information);
                if (answer == DialogResult.Yes)
                    Run("shutdown.exe", "/r /t 5 /c \"NL Audio Matrix test driver\"");
            }

            try { Directory.Delete(temp, true); } catch { }
            return 0;
        }
        catch (Exception ex)
        {
            Error("Installation failed:\n" + ex.Message);
            return 10;
        }
    }
}
