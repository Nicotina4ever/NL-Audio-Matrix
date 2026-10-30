// NLSetupUI.cs — the NL Audio Matrix setup wizard.
//
// Opened when NLMatrixSetup.exe starts without an explicit --mode.  It presents
// the current installation state, lets the user choose install / upgrade / repair
// / uninstall, shows the Windows security requirements for the test-signed NL
// driver (and explains them for OEM or self-built PCs with Secure Boot), runs the
// operation with live progress, then reports the result and handles the reboot.
//
// The wizard never changes Secure Boot, HVCI or bcdedit on its own: it tells the
// user what to change, and only enables test signing when the user ticks the
// explicit option (/accept-testsigning equivalent).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows.Forms;

internal static class NlSetupUi
{
    public static int Run(SetupOptions initial, string[] args)
    {
        try
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
        }
        catch { }
        using (var wizard = new SetupWizard(initial, args))
        {
            Application.Run(wizard);
            return wizard.ExitCode;
        }
    }
}

internal sealed class SetupWizard : Form
{
    private static readonly Color SidebarColor = Color.FromArgb(31, 36, 48);
    private static readonly Color AccentColor = Color.FromArgb(47, 111, 237);
    private static readonly Color MutedColor = Color.FromArgb(96, 104, 120);
    private static readonly Color CardColor = Color.FromArgb(247, 249, 252);
    private static readonly Color OkColor = Color.FromArgb(22, 130, 93);
    private static readonly Color WarnColor = Color.FromArgb(176, 106, 12);
    private static readonly Color BadColor = Color.FromArgb(178, 45, 45);

    private readonly SetupOptions options;
    private readonly string[] rawArgs;
    private readonly List<Label> stepLabels = new List<Label>();

    private Panel content;
    private Panel pageWelcome;
    private Panel pageOptions;
    private Panel pageProgress;
    private Panel pageFinish;

    private Button btnBack;
    private Button btnNext;
    private Button btnCloseWithoutReboot;
    private Button btnOpenLog;
    private Button btnLaunchApp;

    private Label lblState;
    private Label lblScanStatus;
    private Label packageInfoLabel;
    private RadioButton rbInstall;
    private RadioButton rbUpgrade;
    private RadioButton rbRepair;
    private RadioButton rbUninstall;
    private RadioButton[] actionRadios;

    private TextBox txtDir;
    private CheckBox chkLaunch;
    private CheckBox chkAcceptTestSigning;
    private Label lblSecurityHeadline;
    private TextBox txtSecurity;

    private Label lblProgressTitle;
    private ProgressBar progressBar;
    private TextBox txtProgressLog;

    private Label lblFinishTitle;
    private TextBox txtFinishSummary;
    private CheckBox chkRegisterVerify;
    private Label lblReboot;

    private SetupDiagnosticsReport scan;
    private bool scanRunning;
    private string scanError = "";
    private string logPath = "";
    private Thread worker;
    private int resultCode = -1;
    private string resultSummary = "";
    private bool resultReboot;
    private int pageIndex;
    private bool closed = false;

    public int ExitCode { get; private set; }

    public SetupWizard(SetupOptions initial, string[] args)
    {
        options = initial;
        rawArgs = args ?? new string[0];
        ExitCode = 0;
        BuildLayout();
        Load += OnLoaded;
        FormClosing += OnFormClosing;
    }

    // --------------------------------------------------------------- layout
    private void BuildLayout()
    {
        Text = "NL Audio Matrix Setup";
        Font = new Font("Segoe UI", 9.5f);
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(920, 620);
        MinimumSize = new Size(920, 620);
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.White;

        // Explicit layout: docking a Fill panel together with the sidebar and the
        // footer made the WinForms z-order decide the geometry (the content ran
        // under the sidebar).  Fixed bounds keep the three regions exact at every
        // DPI because the form itself is scaled as a whole.
        var sidebar = new Panel
        {
            Location = new Point(0, 0),
            Size = new Size(236, 620),
            BackColor = SidebarColor
        };

        var product = new Label
        {
            Text = "NL Audio Matrix",
            ForeColor = Color.White,
            Font = new Font("Segoe UI Semibold", 15f),
            AutoSize = false,
            Location = new Point(22, 26),
            Size = new Size(196, 30)
        };
        sidebar.Controls.Add(product);

        var tagline = new Label
        {
            Text = "Setup",
            ForeColor = Color.FromArgb(150, 160, 180),
            Font = new Font("Segoe UI", 9f),
            Location = new Point(24, 58),
            Size = new Size(196, 18)
        };
        sidebar.Controls.Add(tagline);

        string[] steps = { "1.  Benvenuto", "2.  Opzioni", "3.  Installazione", "4.  Completato" };
        for (int i = 0; i < steps.Length; i++)
        {
            var label = new Label
            {
                Text = steps[i],
                ForeColor = Color.FromArgb(170, 178, 194),
                Font = new Font("Segoe UI", 9.5f),
                Location = new Point(24, 120 + i * 30),
                Size = new Size(200, 22)
            };
            sidebar.Controls.Add(label);
            stepLabels.Add(label);
        }

        packageInfoLabel = new Label
        {
            Text = "Pacchetto: rilevamento…\r\nWindows 10/11 x64",
            ForeColor = Color.FromArgb(130, 140, 158),
            Font = new Font("Segoe UI", 8.5f),
            Location = new Point(24, 500),
            Size = new Size(200, 60)
        };
        sidebar.Controls.Add(packageInfoLabel);

        var footer = new Panel
        {
            Location = new Point(0, 558),
            Size = new Size(920, 62),
            BackColor = Color.FromArgb(244, 246, 249)
        };
        content = new Panel
        {
            Location = new Point(236, 0),
            Size = new Size(684, 558),
            BackColor = Color.White
        };
        // The sidebar is added last so it is drawn above the content area.
        Controls.Add(content);
        Controls.Add(footer);
        Controls.Add(sidebar);

        btnBack = new Button
        {
            Text = "Indietro",
            Size = new Size(100, 30),
            Location = new Point(600, 16),
            FlatStyle = FlatStyle.System
        };
        btnBack.Click += (s, e) => ShowPage(pageIndex - 1);
        footer.Controls.Add(btnBack);

        btnNext = new Button
        {
            Text = "Avanti",
            Size = new Size(120, 30),
            Location = new Point(706, 16),
            FlatStyle = FlatStyle.System
        };
        btnNext.Click += (s, e) => OnNext();
        footer.Controls.Add(btnNext);

        btnCloseWithoutReboot = new Button
        {
            Text = "Chiudi senza riavviare",
            Size = new Size(160, 30),
            Location = new Point(536, 16),
            FlatStyle = FlatStyle.System,
            Visible = false
        };
        btnCloseWithoutReboot.Click += (s, e) => Close();
        footer.Controls.Add(btnCloseWithoutReboot);

        var helpLink = new LinkLabel
        {
            Text = "Guida e requisiti",
            Location = new Point(22, 22),
            Size = new Size(140, 20),
            LinkColor = AccentColor
        };
        helpLink.LinkClicked += (s, e) => ShowGuide();
        footer.Controls.Add(helpLink);

        BuildWelcomePage();
        BuildOptionsPage();
        BuildProgressPage();
        BuildFinishPage();
        content.Controls.Add(pageWelcome);
        content.Controls.Add(pageOptions);
        content.Controls.Add(pageProgress);
        content.Controls.Add(pageFinish);
        ShowPage(0);
    }

    private static string ReadPackageVersionText()
    {
        try
        {
            var assembly = typeof(NlSetupUi).Assembly;
            var version = assembly.GetName().Version;
            if (version != null) return version.Major + "." + version.Minor + "." + version.Build;
        }
        catch { }
        return "unknown";
    }

    // ------------------------------------------------------------- welcome
    private void BuildWelcomePage()
    {
        pageWelcome = NewPage();
        var title = NewTitle("Installa NL Audio Matrix");
        title.Top = 34;
        pageWelcome.Controls.Add(title);
        var subtitle = new Label
        {
            Text = "App, motore audio e driver virtuali NL Mic 1-4 / NL Return 1-4.",
            ForeColor = MutedColor,
            Location = new Point(40, 88),
            Size = new Size(620, 22)
        };
        pageWelcome.Controls.Add(subtitle);

        var card = new Panel
        {
            Location = new Point(40, 122),
            Size = new Size(604, 144),
            BackColor = CardColor,
            BorderStyle = BorderStyle.FixedSingle
        };
        pageWelcome.Controls.Add(card);
        var cardTitle = new Label
        {
            Text = "Stato attuale del computer",
            Font = new Font("Segoe UI Semibold", 9.5f),
            Location = new Point(14, 10),
            Size = new Size(560, 20)
        };
        card.Controls.Add(cardTitle);
        lblScanStatus = new Label
        {
            Text = "Analisi dello stato in corso…",
            Location = new Point(14, 34),
            Size = new Size(560, 18),
            ForeColor = MutedColor
        };
        card.Controls.Add(lblScanStatus);
        lblState = new Label
        {
            Text = "",
            Location = new Point(14, 56),
            Size = new Size(576, 80)
        };
        card.Controls.Add(lblState);

        var actionTitle = new Label
        {
            Text = "Cosa vuoi fare?",
            Font = new Font("Segoe UI Semibold", 10f),
            Location = new Point(40, 268),
            Size = new Size(300, 22)
        };
        pageWelcome.Controls.Add(actionTitle);

        rbInstall = MakeActionRadio("Installa", "Prima installazione dei componenti NL su questo computer.", 40, 296);
        rbUpgrade = MakeActionRadio("Aggiorna", "Porta un'installazione esistente a questa versione, conservando profili e preset.", 40, 336);
        rbRepair = MakeActionRadio("Ripara", "Ripristina file, driver, certificato, servizio ed endpoint mancanti o danneggiati.", 40, 376);
        rbUninstall = MakeActionRadio("Disinstalla", "Rimuove app, driver e certificato. I profili utente restano.", 40, 416);
        actionRadios = new[] { rbInstall, rbUpgrade, rbRepair, rbUninstall };
        rbInstall.Checked = true;

        var diag = new Button
        {
            Text = "Diagnostica (sola lettura)",
            Location = new Point(460, 470),
            Size = new Size(184, 30),
            FlatStyle = FlatStyle.System
        };
        diag.Click += (s, e) => ShowDiagnostics();
        pageWelcome.Controls.Add(diag);
    }

    private RadioButton MakeActionRadio(string text, string description, int x, int y)
    {
        var radio = new RadioButton
        {
            Text = text,
            Location = new Point(x, y),
            Size = new Size(604, 20),
            Font = new Font("Segoe UI Semibold", 9.5f)
        };
        radio.CheckedChanged += (s, e) => UpdateNextText();
        var desc = new Label
        {
            Text = description,
            Location = new Point(x + 18, y + 19),
            Size = new Size(586, 18),
            ForeColor = MutedColor,
            Font = new Font("Segoe UI", 8.75f)
        };
        pageWelcome.Controls.Add(radio);
        pageWelcome.Controls.Add(desc);
        return radio;
    }

    // ------------------------------------------------------------- options
    private void BuildOptionsPage()
    {
        pageOptions = NewPage();
        var title = NewTitle("Opzioni");
        title.Top = 34;
        pageOptions.Controls.Add(title);

        var dirLabel = new Label
        {
            Text = "Cartella di installazione",
            Location = new Point(40, 96),
            Size = new Size(300, 20)
        };
        pageOptions.Controls.Add(dirLabel);
        txtDir = new TextBox
        {
            Location = new Point(40, 118),
            Size = new Size(440, 24),
            Text = options.AppDir
        };
        pageOptions.Controls.Add(txtDir);
        var browse = new Button
        {
            Text = "Sfoglia…",
            Location = new Point(490, 117),
            Size = new Size(90, 26),
            FlatStyle = FlatStyle.System
        };
        browse.Click += OnBrowse;
        pageOptions.Controls.Add(browse);

        chkLaunch = new CheckBox
        {
            Text = "Avvia NL Audio Matrix al termine",
            Location = new Point(40, 158),
            Size = new Size(400, 22),
            Checked = true
        };
        pageOptions.Controls.Add(chkLaunch);

        var securityCard = new Panel
        {
            Location = new Point(40, 200),
            Size = new Size(604, 262),
            BackColor = CardColor,
            BorderStyle = BorderStyle.FixedSingle
        };
        pageOptions.Controls.Add(securityCard);
        lblSecurityHeadline = new Label
        {
            Text = "Requisiti del driver NL (firmato per test)",
            Font = new Font("Segoe UI Semibold", 9.5f),
            Location = new Point(14, 10),
            Size = new Size(560, 20)
        };
        securityCard.Controls.Add(lblSecurityHeadline);

        txtSecurity = new TextBox
        {
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            BorderStyle = BorderStyle.None,
            BackColor = CardColor,
            Location = new Point(14, 36),
            Size = new Size(566, 128),
            Text = "Verifica della configurazione di Windows in corso…"
        };
        securityCard.Controls.Add(txtSecurity);

        chkAcceptTestSigning = new CheckBox
        {
            Text = "Consenti a Setup di attivare la firma di test di Windows solo se necessario\r\n" +
                   "(bcdedit /set testsigning on; richiede un riavvio; riduce le protezioni di avvio)",
            Location = new Point(14, 172),
            Size = new Size(566, 44),
            Checked = false
        };
        securityCard.Controls.Add(chkAcceptTestSigning);

        var securityFooter = new Label
        {
            Text = "Setup non modifica mai Secure Boot, HVCI o driver di terzi.",
            ForeColor = MutedColor,
            Font = new Font("Segoe UI", 8.5f),
            Location = new Point(14, 222),
            Size = new Size(566, 20)
        };
        securityCard.Controls.Add(securityFooter);

        var note = new Label
        {
            Text = "Per un'installazione completa servono i driver NL. La portable (senza driver) è un pacchetto separato per Windows standard.",
            ForeColor = MutedColor,
            Font = new Font("Segoe UI", 8.5f),
            Location = new Point(40, 474),
            Size = new Size(604, 32)
        };
        pageOptions.Controls.Add(note);
    }

    private void OnBrowse(object sender, EventArgs e)
    {
        using (var dialog = new FolderBrowserDialog())
        {
            dialog.Description = "Cartella di installazione di NL Audio Matrix";
            dialog.SelectedPath = txtDir.Text;
            if (dialog.ShowDialog(this) == DialogResult.OK) txtDir.Text = dialog.SelectedPath;
        }
    }

    // ------------------------------------------------------------ progress
    private void BuildProgressPage()
    {
        pageProgress = NewPage();
        lblProgressTitle = NewTitle("Installazione in corso");
        lblProgressTitle.Top = 34;
        pageProgress.Controls.Add(lblProgressTitle);
        progressBar = new ProgressBar
        {
            Location = new Point(40, 92),
            Size = new Size(604, 16),
            Style = ProgressBarStyle.Marquee,
            MarqueeAnimationSpeed = 24
        };
        pageProgress.Controls.Add(progressBar);
        txtProgressLog = new TextBox
        {
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            Location = new Point(40, 122),
            Size = new Size(604, 400),
            BackColor = Color.FromArgb(250, 251, 253),
            Font = new Font("Consolas", 8.5f)
        };
        pageProgress.Controls.Add(txtProgressLog);
    }

    // -------------------------------------------------------------- finish
    private void BuildFinishPage()
    {
        pageFinish = NewPage();
        lblFinishTitle = NewTitle("Completato");
        lblFinishTitle.Top = 34;
        pageFinish.Controls.Add(lblFinishTitle);

        txtFinishSummary = new TextBox
        {
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            Location = new Point(40, 92),
            Size = new Size(604, 288),
            BackColor = CardColor,
            Font = new Font("Segoe UI", 9f)
        };
        pageFinish.Controls.Add(txtFinishSummary);

        lblReboot = new Label
        {
            Text = "",
            Location = new Point(40, 392),
            Size = new Size(604, 34),
            Font = new Font("Segoe UI Semibold", 9.5f),
            ForeColor = WarnColor
        };
        pageFinish.Controls.Add(lblReboot);

        chkRegisterVerify = new CheckBox
        {
            Text = "Verifica automaticamente al prossimo accesso (nessuna richiesta di amministratore)",
            Location = new Point(40, 428),
            Size = new Size(604, 22),
            Checked = true
        };
        pageFinish.Controls.Add(chkRegisterVerify);

        btnLaunchApp = new Button
        {
            Text = "Avvia NL Audio Matrix",
            Location = new Point(40, 462),
            Size = new Size(180, 30),
            FlatStyle = FlatStyle.System,
            Visible = false
        };
        btnLaunchApp.Click += OnLaunchApp;
        pageFinish.Controls.Add(btnLaunchApp);

        btnOpenLog = new Button
        {
            Text = "Apri log",
            Location = new Point(230, 462),
            Size = new Size(110, 30),
            FlatStyle = FlatStyle.System
        };
        btnOpenLog.Click += (s, e) => OpenLog();
        pageFinish.Controls.Add(btnOpenLog);
    }

    private Panel NewPage()
    {
        return new Panel { Dock = DockStyle.Fill, BackColor = Color.White, Visible = false };
    }

    private Label NewTitle(string text)
    {
        return new Label
        {
            Text = text,
            Font = new Font("Segoe UI Light", 20f),
            ForeColor = Color.FromArgb(28, 32, 40),
            Location = new Point(38, 26),
            Size = new Size(640, 42),
            AutoSize = false
        };
    }

    // ---------------------------------------------------------------- state
    private void OnLoaded(object sender, EventArgs e)
    {
        var pending = NlPendingReboot.Read();
        if (pending.Pending)
            lblScanStatus.Text = "Riavvio completato di recente: verifica dell'installazione in sospeso (fase " +
                                 (pending.Phase.Length > 0 ? pending.Phase : "sconosciuta") + ").";
        StartScan();
    }

    private void StartScan()
    {
        if (scanRunning) return;
        scanRunning = true;
        var scanLog = new SetupLog(Path.Combine(Path.GetTempPath(), "NLMatrixSetup-scan.log"));
        var thread = new Thread(() =>
        {
            SetupDiagnosticsReport report = null;
            string error = "";
            try
            {
                scanLog.Header("NL Audio Matrix Setup", "scan", rawArgs, "Read-only state scan for the wizard");
                report = Installer.ScanOnce(options, scanLog);
            }
            catch (Exception ex)
            {
                error = ex.Message;
                scanLog.Error("Scan failed: " + ex);
            }
            finally
            {
                scanLog.Result(error.Length == 0 ? 0 : 10, error);
                scanLog.Dispose();
                BeginInvoke(new Action(() =>
                {
                    scanRunning = false;
                    scan = report;
                    scanError = error;
                    ApplyScan();
                }));
            }
        });
        thread.IsBackground = true;
        thread.Start();
    }

    private void ApplyScan()
    {
        if (scan == null)
        {
            lblScanStatus.Text = "Impossibile leggere lo stato: " + scanError;
            lblScanStatus.ForeColor = BadColor;
            lblState.Text = "Il pacchetto di setup non è leggibile. Chiudi e riavvia Setup.";
            return;
        }
        bool installed = scan.InstalledAppVersion.Length > 0 || scan.ArpEntry ||
                         scan.Packages.Count > 0 || (scan.Device != null && scan.Device.Present);
        bool appOlder = installed && scan.InstalledAppVersion.Length > 0 &&
                        !NlText.SameVersion(scan.InstalledAppVersion, scan.PackageVersion);
        bool healthy = scan.Healthy;

        lblScanStatus.Text = installed
            ? "NL Audio Matrix risulta installato" + (scan.InstalledAppVersion.Length > 0
                ? " (versione " + scan.InstalledAppVersion + ")" : "") +
              (healthy ? " e funzionante." : " ma non completamente funzionante.")
            : "NL Audio Matrix non risulta installato su questo computer.";
        lblScanStatus.ForeColor = installed ? (healthy ? OkColor : WarnColor) : Color.FromArgb(28, 32, 40);
        packageInfoLabel.Text = "Pacchetto: " + scan.PackageVersion + "\r\nDriver " +
                                NlText.DisplayVersion(scan.PackageDriverVersion) + "\r\nWindows 10/11 x64";

        var sb = new StringBuilder();
        sb.AppendLine("Driver caricato     : " +
                      (scan.Runtime != null && scan.Runtime.ServiceRunning
                          ? "sì (" + NlText.DisplayVersion(scan.Runtime.LoadedFileVersion) + ")"
                          : "no"));
        sb.AppendLine("Endpoint audio      : " + (scan.Endpoints != null
            ? scan.Endpoints.ActiveCount + " attivi su 8 attesi (4 NL Mic + 4 NL Return)"
            : "sconosciuti"));
        sb.AppendLine("Firma di test       : " + (scan.Security != null ? scan.Security.TestSigning : "sconosciuta") +
                      "   Secure Boot: " + (scan.Security != null ? scan.Security.SecureBoot : "sconosciuto") +
                      "   HVCI: " + (scan.Security != null ? scan.Security.Hvci : "sconosciuto"));
        sb.AppendLine("Decisione di Setup  : " + (scan.Decision.Length > 0 ? scan.Decision : "n/d"));
        lblState.Text = sb.ToString();

        rbInstall.Checked = !installed;
        rbUpgrade.Enabled = installed;
        rbRepair.Enabled = installed;
        rbUninstall.Enabled = installed;
        rbUpgrade.Text = "Aggiorna" + (appOlder ? " (versione " + scan.InstalledAppVersion + " rilevata)" : "");
        if (installed && appOlder) rbUpgrade.Checked = true;
        else if (installed) rbRepair.Checked = true;
        else rbInstall.Checked = true;

        ApplySecurityToOptions(scan.Security);
        UpdateNextText();
    }

    private void ApplySecurityToOptions(SecurityInfo security)
    {
        if (security == null)
        {
            txtSecurity.Text = "Configurazione di sicurezza non leggibile.";
            return;
        }
        var sb = new StringBuilder();
        sb.AppendLine("Secure Boot: " + security.SecureBoot + "    TESTSIGNING: " + security.TestSigning);
        sb.AppendLine("HVCI (Integrità memoria): " + security.Hvci + "    Firmware: " + security.Firmware);
        sb.AppendLine();
        if (security.Verdict == "ok")
        {
            sb.AppendLine("La configurazione attuale consente di caricare il driver firmato per test.");
            lblSecurityHeadline.ForeColor = OkColor;
        }
        else if (security.Verdict == "blocked")
        {
            sb.AppendLine("Questa configurazione di Windows blocca il driver firmato per test.");
            foreach (var line in security.Guidance) sb.AppendLine("• " + line);
            lblSecurityHeadline.ForeColor = BadColor;
        }
        else
        {
            sb.AppendLine("Stato non determinabile senza privilegi di amministratore.");
            lblSecurityHeadline.ForeColor = WarnColor;
        }
        txtSecurity.Text = sb.ToString();
    }

    // ------------------------------------------------------------- pages
    private void ShowPage(int index)
    {
        if (index < 0 || index > 3) return;
        pageIndex = index;
        pageWelcome.Visible = index == 0;
        pageOptions.Visible = index == 1;
        pageProgress.Visible = index == 2;
        pageFinish.Visible = index == 3;
        for (int i = 0; i < stepLabels.Count; i++)
        {
            stepLabels[i].ForeColor = i == index ? Color.White
                : (i < index ? Color.FromArgb(180, 220, 190) : Color.FromArgb(170, 178, 194));
            stepLabels[i].Font = new Font("Segoe UI" + (i == index ? " Semibold" : ""), 9.5f);
        }
        btnBack.Visible = index == 1;
        btnNext.Visible = index != 2;
        btnCloseWithoutReboot.Visible = false;
        UpdateNextText();
    }

    private string SelectedMode()
    {
        if (rbUninstall.Checked) return "uninstall";
        if (rbRepair.Checked) return "repair";
        if (rbUpgrade.Checked) return "upgrade";
        return "install";
    }

    private void UpdateNextText()
    {
        if (pageIndex == 0)
        {
            btnNext.Text = "Avanti";
            btnNext.Enabled = scan != null && !scanRunning;
        }
        else if (pageIndex == 1)
        {
            string mode = SelectedMode();
            btnNext.Text = mode == "uninstall" ? "Disinstalla"
                         : mode == "repair" ? "Ripara"
                         : mode == "upgrade" ? "Aggiorna" : "Installa";
            btnNext.Enabled = true;
        }
        else if (pageIndex == 3)
        {
            btnNext.Text = resultReboot ? "Riavvia ora" : "Chiudi";
            btnNext.Enabled = true;
            btnCloseWithoutReboot.Visible = resultReboot;
            btnCloseWithoutReboot.Enabled = true;
        }
        else
        {
            btnNext.Enabled = false;
        }
    }

    private void OnNext()
    {
        if (pageIndex == 0) { ShowPage(1); return; }
        if (pageIndex == 1) { StartOperation(); return; }
        if (pageIndex == 3)
        {
            if (resultReboot) RebootNow();
            Close();
        }
    }

    // --------------------------------------------------------- operation
    private void StartOperation()
    {
        string mode = SelectedMode();
        bool isUninstall = mode == "uninstall";
        if (!isUninstall)
        {
            string folder = txtDir.Text.Trim();
            string reason = "";
            if (folder.Length == 0 || !NlPaths.IsSafeDirectory(folder, out reason))
            {
                MessageBox.Show(this, "La cartella di installazione non è valida" +
                                (reason != null && reason.Length > 0 ? ": " + reason : "."),
                                "NL Audio Matrix Setup", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (scan != null && scan.Security != null && scan.Security.Verdict == "blocked")
            {
                MessageBox.Show(this,
                    "La configurazione di sicurezza di Windows blocca il driver firmato per test:\r\n\r\n" +
                    scan.Security.BlockReason + "\r\n\r\n" +
                    "Segui le istruzioni mostrate nella pagina Opzioni (o premi «Guida e requisiti») e riavvia " +
                    "Setup. Setup non modifica Secure Boot, HVCI o la configurazione di avvio.",
                    "NL Audio Matrix Setup", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (isUninstall == false && scan != null && scan.Security != null &&
                scan.Security.TestSigning == "disabled" && !chkAcceptTestSigning.Checked)
            {
                MessageBox.Show(this,
                    "La firma di test di Windows è disattivata: il driver NL non può essere caricato.\r\n\r\n" +
                    "Attiva l'opzione «Consenti a Setup di attivare la firma di test» oppure abilitala tu " +
                    "(bcdedit /set testsigning on) e riavvia.",
                    "NL Audio Matrix Setup", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (mode == "repair" || mode == "upgrade")
            {
                string answer = mode == "upgrade"
                    ? "Aggiornare NL Audio Matrix a questa versione?\r\n\r\nProfili, preset e impostazioni audio di Windows saranno conservati."
                    : "Riparare l'installazione di NL Audio Matrix?\r\n\r\nI file mancanti o danneggiati saranno ripristinati.";
                if (MessageBox.Show(this, answer, "NL Audio Matrix Setup", MessageBoxButtons.YesNo,
                                    MessageBoxIcon.Question, MessageBoxDefaultButton.Button1) != DialogResult.Yes)
                    return;
            }
        }
        else
        {
            if (MessageBox.Show(this,
                "Disinstallare NL Audio Matrix e i driver virtuali NL?\r\n\r\n" +
                "Profili e preset in %APPDATA%\\NL Audio Matrix saranno conservati. " +
                "Al termine potrebbe essere necessario un riavvio.",
                "NL Audio Matrix Setup", MessageBoxButtons.YesNo, MessageBoxIcon.Question,
                MessageBoxDefaultButton.Button2) != DialogResult.Yes)
                return;
        }

        options.Mode = mode;
        options.AppDir = txtDir.Text.Trim();
        try
        {
            options.DirSpecified = options.AppDir.Length > 0 &&
                                   !string.Equals(NlPaths.Normalize(options.AppDir),
                                                  NlPaths.Normalize(NlConst.DefaultAppDir),
                                                  StringComparison.OrdinalIgnoreCase);
        }
        catch { options.DirSpecified = options.AppDir.Length > 0; }
        options.Silent = true; // the wizard reports the result itself
        options.Launch = chkLaunch.Checked && !isUninstall;
        options.AcceptTestSigning = chkAcceptTestSigning.Checked;
        options.RegisterPostRebootVerify = !isUninstall;

        logPath = Path.Combine(Path.GetTempPath(), "NLMatrixSetup-" + mode + ".log");
        txtProgressLog.Clear();
        lblProgressTitle.Text = mode == "uninstall" ? "Disinstallazione in corso"
                              : mode == "repair" ? "Riparazione in corso"
                              : mode == "upgrade" ? "Aggiornamento in corso"
                              : "Installazione in corso";
        ShowPage(2);
        btnNext.Enabled = false;
        btnBack.Visible = false;

        SetupLog.LineWritten = OnLogLine;
        worker = new Thread(RunOperation);
        worker.IsBackground = true;
        worker.Start();
    }

    private void RunOperation()
    {
        int code = 10;
        string summary = "";
        bool reboot = false;
        var log = new SetupLog(logPath);
        try
        {
            log.Header("NL Audio Matrix Setup", options.Mode, rawArgs,
                       "Install folder: " + options.AppDir + " (from command line: " + options.DirSpecified + ")");
            Installer.ValidateEnvironment();
            code = Installer.Execute(options, log, out summary, out reboot);
        }
        catch (SetupException ex)
        {
            log.Error("Setup failed: " + ex.Message);
            summary = "Operazione non riuscita:\r\n" + ex.Message;
            code = ex.ExitCode;
            reboot = false;
        }
        catch (Exception ex)
        {
            log.Error("Unexpected error: " + ex);
            summary = "Operazione non riuscita per un errore imprevisto:\r\n" + ex.Message;
            code = 10;
            reboot = false;
        }
        finally
        {
            try { Installer.RecordPendingReboot(options, log, code, reboot); } catch { }
            log.Result(code, summary);
            Installer.CleanupPayload();
            log.Dispose();
            SetupLog.LineWritten = null;
            BeginInvoke(new Action(() => OnOperationFinished(code, summary, reboot)));
        }
    }

    private void OnLogLine(string level, string text)
    {
        if (closed) return;
        try
        {
            BeginInvoke(new Action(() =>
            {
                if (closed || txtProgressLog == null) return;
                if (txtProgressLog.Lines.Length > 900) return;
                txtProgressLog.AppendText(level.PadRight(5) + " " + text + Environment.NewLine);
            }));
        }
        catch { }
    }

    private void OnOperationFinished(int code, string summary, bool reboot)
    {
        resultCode = code;
        resultSummary = summary;
        resultReboot = reboot;

        bool success = code == 0 || code == 20;
        if (success && options.Mode == "uninstall")
        {
            lblFinishTitle.Text = "Disinstallazione completata";
            lblFinishTitle.ForeColor = OkColor;
        }
        else if (success)
        {
            lblFinishTitle.Text = reboot ? "Quasi completato: riavvia il computer" : "Installazione completata";
            lblFinishTitle.ForeColor = reboot ? WarnColor : OkColor;
        }
        else
        {
            lblFinishTitle.Text = "Operazione non completata (codice " + code + ")";
            lblFinishTitle.ForeColor = BadColor;
        }

        var sb = new StringBuilder();
        sb.AppendLine(summary);
        if (!success)
        {
            sb.AppendLine();
            sb.AppendLine("Il computer non è stato lasciato in uno stato non sicuro: driver di terzi, Secure Boot e");
            sb.AppendLine("impostazioni audio di Windows non sono stati modificati.");
            sb.AppendLine("Controlla il log per capire quale passaggio è fallito e riprova con «Ripara».");
        }
        var pending = NlPendingReboot.Read();
        if (pending.Pending)
        {
            sb.AppendLine();
            sb.AppendLine("Riavvio in sospeso: al prossimo accesso Setup completerà e verificherà l'installazione.");
        }
        sb.AppendLine();
        sb.AppendLine("Log: " + logPath);
        txtFinishSummary.Text = sb.ToString();

        lblReboot.Text = reboot
            ? "Riavvia Windows per caricare il driver firmato per test e completare l'installazione."
            : "";
        chkRegisterVerify.Visible = reboot;
        chkRegisterVerify.Checked = reboot;

        string ui = Path.Combine(options.AppDir, NlConst.UiExeName);
        btnLaunchApp.Visible = success && !reboot && options.Mode != "uninstall" && File.Exists(ui);
        btnOpenLog.Visible = File.Exists(logPath);

        ShowPage(3);
        btnBack.Visible = false;
        btnNext.Enabled = true;
        btnNext.Visible = true;
        if (reboot)
        {
            btnNext.Text = "Riavvia ora";
            btnCloseWithoutReboot.Visible = true;
            btnCloseWithoutReboot.Enabled = true;
        }
        else
        {
            btnNext.Text = "Chiudi";
            btnCloseWithoutReboot.Visible = false;
        }
    }

    private void RebootNow()
    {
        try
        {
            // RunOnce was registered by Installer.RecordPendingReboot when the
            // option was ticked; shutdown is the only system action here.
            var process = Process.Start(new ProcessStartInfo("shutdown.exe", "/r /t 20 /c \"NL Audio Matrix: riavvio per completare l'installazione\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true
            });
            if (process != null) process.WaitForExit(5000);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "Impossibile avviare il riavvio: " + ex.Message +
                                  "\r\nRiavvia Windows manualmente.", "NL Audio Matrix Setup",
                            MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private void OnLaunchApp(object sender, EventArgs e)
    {
        try
        {
            string ui = Path.Combine(options.AppDir, NlConst.UiExeName);
            if (File.Exists(ui))
                Process.Start(new ProcessStartInfo(ui) { WorkingDirectory = options.AppDir, UseShellExecute = true });
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "Impossibile avviare NL Audio Matrix: " + ex.Message,
                            "NL Audio Matrix Setup", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private void OpenLog()
    {
        try
        {
            if (File.Exists(logPath)) Process.Start("notepad.exe", Sys.Quote(logPath));
        }
        catch { }
    }

    // ---------------------------------------------------------- diagnostics
    private void ShowDiagnostics()
    {
        if (scan == null)
        {
            MessageBox.Show(this, "Diagnostica non disponibile: lo stato non è stato letto.",
                            "NL Audio Matrix Setup", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        var sb = new StringBuilder();
        scan.ToText(sb);
        ShowTextDialog("Diagnostica (sola lettura)", sb.ToString());
    }

    private void ShowGuide()
    {
        ShowTextDialog("Guida e requisiti", GuideText());
    }

    private static string GuideText()
    {
        return
            "NL AUDIO MATRIX — GUIDA RAPIDA\r\n" +
            "\r\n" +
            "Cosa installa\r\n" +
            "  • Applicazione (NLMatrixUI.exe), motore audio (NLMatrixRouter.exe) e driver virtuali\r\n" +
            "    NL Mic 1-4 e NL Return 1-4.\r\n" +
            "  • Il setup è un unico file: non serve installare a mano certificati, file SYS o INF.\r\n" +
            "  • Nessuna chiave privata di firma è inclusa nel pacchetto.\r\n" +
            "\r\n" +
            "Il driver è firmato per test\r\n" +
            "  Il driver NL non ha una firma di produzione (WHQL/EV). Windows lo carica solo con la\r\n" +
            "  «firma di test» attiva (TESTSIGNING). Setup attiva la firma di test solo se selezioni\r\n" +
            "  esplicitamente l'opzione corrispondente; non modifica mai Secure Boot né HVCI.\r\n" +
            "\r\n" +
            "PC con Secure Boot attivo (OEM/preassemblati o assemblati a mano)\r\n" +
            "  1. Disattiva Secure Boot nel firmware (BIOS/UEFI) — operazione manuale, non eseguita da Setup.\r\n" +
            "  2. Abilita la firma di test: bcdedit /set testsigning on (oppure l'opzione nel setup) e riavvia.\r\n" +
            "  3. Se anche «Integrità memoria» (HVCI) è attiva, disattivala in Sicurezza di Windows →\r\n" +
            "     Sicurezza del dispositivo → Isolamento core, poi riavvia.\r\n" +
            "  Conseguenze: con Secure Boot disattivato il PC è meno protetto all'avvio finché non\r\n" +
            "  installi un driver firmato per produzione. Nessun'altra impostazione di Windows cambia.\r\n" +
            "\r\n" +
            "Windows standard senza modalità test\r\n" +
            "  Usa il pacchetto portable: è solo user-mode, non installa driver e funziona con gli\r\n" +
            "  endpoint già presenti (per esempio la tua interfaccia ASIO e VB-CABLE).\r\n" +
            "\r\n" +
            "Cosa viene conservato\r\n" +
            "  • Profili, preset e preferenze in %APPDATA%\\NL Audio Matrix restano anche dopo update,\r\n" +
            "    riparazione e disinstallazione.\r\n" +
            "  • Driver di terzi (UAD, Focusrite, VB-CABLE) e impostazioni audio di Windows non sono toccati.\r\n" +
            "  • Gli aggiornamenti rimuovono solo i pacchetti driver NL obsoleti, mantenendo quello corrente.\r\n" +
            "\r\n" +
            "Riavvio e verifica\r\n" +
            "  Se serve un riavvio, Setup lo segnala e propone il riavvio immediato. Al successivo accesso\r\n" +
            "  una verifica automatica (senza prompt di amministratore) controlla driver, dispositivo,\r\n" +
            "  gli otto endpoint e l'avvio/arresto dell'applicazione.\r\n" +
            "\r\n" +
            "Disinstallazione\r\n" +
            "  Pannello di controllo → App → NL Audio Matrix → Disinstalla (o il Setup, voce «Disinstalla»).\r\n" +
            "  Rimuove app, driver, certificato e voci di registro; conserva i profili utente.\r\n" +
            "\r\n" +
            "Exit code (per uso da riga di comando)\r\n" +
            "  0 successo · 9 endpoint/driver non sani · 12 configurazione di sicurezza che blocca il driver\r\n" +
            "  20 riavvio necessario · altri codici: vedi il log in %TEMP%\\NLMatrixSetup-*.log\r\n";
    }

    private void ShowTextDialog(string title, string text)
    {
        using (var dialog = new Form())
        {
            dialog.Text = title;
            dialog.Font = Font;
            dialog.ClientSize = new Size(760, 540);
            dialog.StartPosition = FormStartPosition.CenterParent;
            dialog.MinimizeBox = false;
            dialog.MaximizeBox = false;
            dialog.FormBorderStyle = FormBorderStyle.FixedDialog;

            var box = new TextBox
            {
                Multiline = true,
                ReadOnly = true,
                ScrollBars = ScrollBars.Both,
                WordWrap = false,
                Location = new Point(12, 12),
                Size = new Size(736, 476),
                Text = text,
                Font = new Font("Consolas", 9f)
            };
            dialog.Controls.Add(box);

            var copy = new Button
            {
                Text = "Copia",
                Location = new Point(576, 498),
                Size = new Size(84, 30),
                FlatStyle = FlatStyle.System
            };
            copy.Click += (s, e) =>
            {
                try { Clipboard.SetText(text); } catch { }
            };
            dialog.Controls.Add(copy);

            var ok = new Button
            {
                Text = "Chiudi",
                Location = new Point(668, 498),
                Size = new Size(80, 30),
                FlatStyle = FlatStyle.System
            };
            ok.Click += (s, e) => dialog.Close();
            dialog.Controls.Add(ok);
            dialog.AcceptButton = ok;
            dialog.CancelButton = ok;
            dialog.ShowDialog(this);
        }
    }

    // ------------------------------------------------------------- closing
    private void OnFormClosing(object sender, FormClosingEventArgs e)
    {
        if (worker != null && worker.IsAlive)
        {
            MessageBox.Show(this, "Un'operazione è in corso. Attendere il completamento prima di chiudere.",
                            "NL Audio Matrix Setup", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            e.Cancel = true;
            return;
        }
        if (resultReboot && !chkRegisterVerify.Checked)
        {
            // The user does not want the automatic verification at the next logon.
            try
            {
                using (var log = new SetupLog(Path.Combine(Path.GetTempPath(), "NLMatrixSetup-runonce.log")))
                    NlPendingReboot.UnregisterRunOnce(log);
            }
            catch { }
        }
        closed = true;
        SetupLog.LineWritten = null;
        if (resultCode >= 0) ExitCode = resultCode;
    }
}
