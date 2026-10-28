using System.Reflection;
using System;
using System.Diagnostics;
using System.IO;
using System.Security.Principal;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("NL Audio Matrix Uninstall")]
[assembly: AssemblyDescription("NL Audio Matrix uninstaller")]
[assembly: AssemblyCompany("Nicola Lanci")]
[assembly: AssemblyProduct("NL Audio Matrix")]
[assembly: AssemblyCopyright("Copyright (c) Nicola Lanci")]
[assembly: AssemblyVersion("0.5.0.0")]
[assembly: AssemblyFileVersion("0.5.0.0")]
[assembly: AssemblyInformationalVersion("0.5.0-alpha")]

internal static class Uninstaller
{
    private const string AppName = "NL Audio Matrix";
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
        if (!silent) MessageBox.Show(text, AppName, MessageBoxButtons.OK, MessageBoxIcon.Error);
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

    [STAThread]
    private static int Main(string[] args)
    {
        silent = Array.Exists(args, a => a.Equals("/silent", StringComparison.OrdinalIgnoreCase));
        Application.EnableVisualStyles();
        try
        {
            if (!IsAdmin())
            {
                Error("The uninstaller needs administrator rights.");
                return 1;
            }

            var appDir = "";
            using (var key = Registry.LocalMachine.OpenSubKey(
                       @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NLMatrix"))
                if (key != null) appDir = key.GetValue("InstallLocation") as string;
            if (string.IsNullOrWhiteSpace(appDir))
                appDir = Path.GetDirectoryName(System.Reflection.Assembly.GetExecutingAssembly().Location);

            Run("taskkill.exe", "/f /im NLMatrixUI.exe");
            Run("taskkill.exe", "/f /im NLMatrixRouter.exe");

            var devcon = Path.Combine(appDir, "driver", "devcon.exe");
            if (File.Exists(devcon))
            {
                Run(devcon, "remove Root\\NLMatrixVirtualAudio");
                Run(devcon, "remove Root\\sysvad_ComponentizedAudioSample");
            }

            // Remove only driver packages whose INF belongs to NLMatrix.  Other
            // DriverStore packages, Focusrite, VB-CABLE and all user devices are
            // deliberately left untouched.
            var windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
            foreach (var inf in Directory.GetFiles(Path.Combine(windows, "INF"), "oem*.inf"))
            {
                try
                {
                    var text = File.ReadAllText(inf);
                    if (text.IndexOf("NLMatrix Virtual Audio Device", StringComparison.OrdinalIgnoreCase) >= 0
                        || text.IndexOf("NLMatrixAudio.sys", StringComparison.OrdinalIgnoreCase) >= 0)
                        Run("pnputil.exe", "/delete-driver \"" + inf + "\" /uninstall /force");
                }
                catch { }
            }

            Run("certutil.exe", "-delstore Root \"NLMatrix Test Driver\"");
            Run("certutil.exe", "-delstore TrustedPublisher \"NLMatrix Test Driver\"");

            try
            {
                using (var key = Registry.LocalMachine.OpenSubKey(
                           @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NLMatrix", true))
                    if (key != null) key.DeleteSubKeyTree("");
            }
            catch { }

            var startMenu = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms), AppName);
            try { if (Directory.Exists(startMenu)) Directory.Delete(startMenu, true); } catch { }

            var script = Path.Combine(Path.GetTempPath(),
                                      "nlmatrix-uninstall-" + Guid.NewGuid().ToString("N") + ".cmd");
            File.WriteAllText(script,
                "@echo off\r\n" +
                "timeout /t 2 /nobreak >nul\r\n" +
                "rmdir /s /q \"" + appDir + "\"\r\n" +
                "del /q \"" + script + "\"\r\n");
            Process.Start(new ProcessStartInfo("cmd.exe", "/c \"" + script + "\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true
            });

            Info("NL Audio Matrix and its NL virtual audio driver were removed.\n\n" +
                 "A reboot is recommended if the endpoints are still visible.");
            return 0;
        }
        catch (Exception ex)
        {
            Error("Uninstallation failed:\n" + ex.Message);
            return 10;
        }
    }
}