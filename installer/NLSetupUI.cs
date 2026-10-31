// NLSetupUI.cs — the NL Audio Matrix setup wizard.
//
// Opened when NLMatrixSetup.exe starts without an explicit --mode.  It presents
// the current installation state, lets the user choose install / upgrade / repair
// / uninstall, shows the Windows security requirements for the test-signed NL
// driver (and explains them for OEM or self-built PCs with Secure Boot), runs the
// operation with live progress, then reports the result and handles the reboot.
//
// The layout uses TableLayoutPanel/FlowLayoutPanel with auto-sized rows, so text
// wraps instead of overlapping when Windows uses a larger font or display scale.
// The wizard speaks Italian and English (selector in the footer; NL_SETUP_LANG=en
// forces the second language, useful for automation).
//
// The setup is a single executable: the compressed payload (application, engine,
// driver, test certificate, licences) is embedded as payload.zip and extracted to
// %TEMP% only while Setup runs.
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
    private readonly List<Action> bindings = new List<Action>();

    private TableLayoutPanel content;
    private TableLayoutPanel pageWelcome;
    private TableLayoutPanel pageOptions;
    private TableLayoutPanel pageProgress;
    private TableLayoutPanel pageFinish;

    private Button btnBack;
    private Button btnNext;
    private Button btnCloseWithoutReboot;
    private Button btnOpenLog;
    private Button btnLaunchApp;
    private ComboBox cmbLang;

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
    private Label lblSecurityText;

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
    private string lang = "it";
    private bool applyingLanguage;

    public int ExitCode { get; private set; }

    public SetupWizard(SetupOptions initial, string[] args)
    {
        options = initial;
        rawArgs = args ?? new string[0];
        ExitCode = 0;
        lang = DetectLanguage();
        BuildLayout();
        Load += OnLoaded;
        FormClosing += OnFormClosing;
    }

    // ------------------------------------------------------------- language
    private static string DetectLanguage()
    {
        string forced = Environment.GetEnvironmentVariable("NL_SETUP_LANG");
        if (!string.IsNullOrEmpty(forced))
        {
            forced = forced.Trim().ToLowerInvariant();
            if (forced == "en" || forced == "it") return forced;
        }
        string ui = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName.ToLowerInvariant();
        return ui == "it" ? "it" : "en";
    }

    private string T(string it, string en)
    {
        return lang == "en" ? en : it;
    }

    private void Bind(Control control, string it, string en)
    {
        bindings.Add(delegate { control.Text = T(it, en); });
        control.Text = T(it, en);
    }

    private void ApplyLanguage(string newLang, bool refresh)
    {
        if (applyingLanguage) return;
        applyingLanguage = true;
        try
        {
            lang = newLang == "en" ? "en" : "it";
            foreach (Action binding in bindings)
            {
                try { binding(); } catch { }
            }
            if (cmbLang != null)
            {
                int index = lang == "en" ? 1 : 0;
                if (cmbLang.SelectedIndex != index) cmbLang.SelectedIndex = index;
            }
            if (refresh) RefreshDynamic();
        }
        finally
        {
            applyingLanguage = false;
        }
    }

    private void RefreshDynamic()
    {
        if (scan != null)
        {
            ApplyScan();
        }
        else
        {
            lblScanStatus.ForeColor = Color.FromArgb(28, 32, 40);
            if (scanRunning) lblScanStatus.Text = T("Analisi dello stato in corso…", "Scanning the current state…");
            else if (scanError.Length > 0) lblScanStatus.Text = T("Impossibile leggere lo stato: ", "Cannot read the state: ") + scanError;
            else lblScanStatus.Text = T("Analisi dello stato in corso…", "Scanning the current state…");
        }
        UpdateNextText();
        if (pageIndex == 2) lblProgressTitle.Text = ProgressTitle();
        if (pageIndex == 3) RefreshFinishText();
    }

    // --------------------------------------------------------------- layout
    private void BuildLayout()
    {
        Text = "NL Audio Matrix Setup";
        Font = new Font("Segoe UI", 9.5f);
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(960, 660);
        MinimumSize = new Size(900, 620);
        FormBorderStyle = FormBorderStyle.Sizable;
        MaximizeBox = true;
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.White;

        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 2,
            Margin = new Padding(0),
            Padding = new Padding(0),
            BackColor = Color.White
        };
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 250));
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 64));

        var sidebar = BuildSidebar();
        content = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 1,
            Margin = new Padding(0),
            Padding = new Padding(0),
            BackColor = Color.White
        };
        content.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        content.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        var footer = BuildFooter();

        root.Controls.Add(sidebar, 0, 0);
        root.Controls.Add(content, 1, 0);
        root.Controls.Add(footer, 0, 1);
        root.SetColumnSpan(footer, 2);
        Controls.Add(root);

        BuildWelcomePage();
        BuildOptionsPage();
        BuildProgressPage();
        BuildFinishPage();
        content.Controls.Add(pageWelcome, 0, 0);
        content.Controls.Add(pageOptions, 0, 0);
        content.Controls.Add(pageProgress, 0, 0);
        content.Controls.Add(pageFinish, 0, 0);
        ShowPage(0);
        ApplyLanguage(lang, false);
    }

    private TableLayoutPanel BuildSidebar()
    {
        var side = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            BackColor = SidebarColor,
            ColumnCount = 1,
            RowCount = 6,
            Padding = new Padding(22, 26, 22, 18),
            Margin = new Padding(0)
        };
        side.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        side.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        side.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        side.RowStyles.Add(new RowStyle(SizeType.Absolute, 26));
        side.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        side.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        side.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var product = new Label
        {
            AutoSize = true,
            ForeColor = Color.White,
            Font = new Font("Segoe UI Semibold", 15f),
            Margin = new Padding(0)
        };
        Bind(product, "NL Audio Matrix", "NL Audio Matrix");
        side.Controls.Add(product, 0, 0);

        var tagline = new Label
        {
            AutoSize = true,
            ForeColor = Color.FromArgb(150, 160, 180),
            Font = new Font("Segoe UI", 9f),
            Margin = new Padding(2, 2, 0, 0)
        };
        Bind(tagline, "Setup", "Setup");
        side.Controls.Add(tagline, 0, 1);

        var steps = new FlowLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            Margin = new Padding(0),
            BackColor = Color.Transparent
        };
        string[] stepsIt = { "1.  Benvenuto", "2.  Opzioni", "3.  Installazione", "4.  Completato" };
        string[] stepsEn = { "1.  Welcome", "2.  Options", "3.  Installation", "4.  Finish" };
        for (int i = 0; i < stepsIt.Length; i++)
        {
            var label = new Label
            {
                AutoSize = true,
                ForeColor = Color.FromArgb(170, 178, 194),
                Font = new Font("Segoe UI", 9.5f),
                Margin = new Padding(0, 0, 0, 12)
            };
            int index = i;
            bindings.Add(delegate { label.Text = T(stepsIt[index], stepsEn[index]); });
            label.Text = T(stepsIt[index], stepsEn[index]);
            steps.Controls.Add(label);
            stepLabels.Add(label);
        }
        side.Controls.Add(steps, 0, 3);

        packageInfoLabel = new Label
        {
            AutoSize = true,
            ForeColor = Color.FromArgb(130, 140, 158),
            Font = new Font("Segoe UI", 8.5f),
            Margin = new Padding(0),
            MaximumSize = new Size(206, 0)
        };
        Bind(packageInfoLabel, "Pacchetto: rilevamento…", "Package: detecting…");
        side.Controls.Add(packageInfoLabel, 0, 5);
        return side;
    }

    private TableLayoutPanel BuildFooter()
    {
        var footer = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            BackColor = Color.FromArgb(244, 246, 249),
            ColumnCount = 2,
            RowCount = 1,
            Margin = new Padding(0),
            Padding = new Padding(0)
        };
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        footer.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

        var left = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            Margin = new Padding(0),
            Padding = new Padding(18, 18, 0, 0),
            BackColor = Color.Transparent
        };
        var helpLink = new LinkLabel
        {
            AutoSize = true,
            LinkColor = AccentColor,
            Margin = new Padding(0, 2, 18, 0)
        };
        Bind(helpLink, "Guida e requisiti", "Guide and requirements");
        helpLink.LinkClicked += delegate { ShowGuide(); };
        left.Controls.Add(helpLink);

        var langLabel = new Label
        {
            AutoSize = true,
            ForeColor = MutedColor,
            Margin = new Padding(0, 4, 6, 0)
        };
        Bind(langLabel, "Lingua:", "Language:");
        left.Controls.Add(langLabel);

        cmbLang = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList,
            Width = 100,
            Margin = new Padding(0, 1, 0, 0)
        };
        cmbLang.Items.AddRange(new object[] { "Italiano", "English" });
        cmbLang.SelectedIndex = lang == "en" ? 1 : 0;
        cmbLang.SelectedIndexChanged += delegate
        {
            ApplyLanguage(cmbLang.SelectedIndex == 1 ? "en" : "it", true);
        };
        left.Controls.Add(cmbLang);

        var right = new FlowLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Anchor = AnchorStyles.Right,
            FlowDirection = FlowDirection.RightToLeft,
            WrapContents = false,
            Margin = new Padding(0),
            Padding = new Padding(0, 14, 18, 0),
            BackColor = Color.Transparent
        };

        btnNext = new Button
        {
            Size = new Size(130, 36),
            TextAlign = ContentAlignment.MiddleCenter,
            FlatStyle = FlatStyle.System,
            Margin = new Padding(8, 0, 0, 0)
        };
        Bind(btnNext, "Avanti", "Next");
        btnNext.Click += delegate { OnNext(); };
        right.Controls.Add(btnNext);

        btnBack = new Button
        {
            Size = new Size(110, 36),
            TextAlign = ContentAlignment.MiddleCenter,
            FlatStyle = FlatStyle.System,
            Margin = new Padding(8, 0, 0, 0),
            Visible = false
        };
        Bind(btnBack, "Indietro", "Back");
        btnBack.Click += delegate { ShowPage(pageIndex - 1); };
        right.Controls.Add(btnBack);

        btnCloseWithoutReboot = new Button
        {
            Size = new Size(240, 36),
            TextAlign = ContentAlignment.MiddleCenter,
            FlatStyle = FlatStyle.System,
            Margin = new Padding(8, 0, 0, 0),
            Visible = false
        };
        Bind(btnCloseWithoutReboot, "Chiudi senza riavviare", "Close without restarting");
        btnCloseWithoutReboot.Click += delegate { Close(); };
        right.Controls.Add(btnCloseWithoutReboot);

        footer.Controls.Add(left, 0, 0);
        footer.Controls.Add(right, 1, 0);
        return footer;
    }

    private static TableLayoutPanel NewPage()
    {
        var page = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            Visible = false,
            BackColor = Color.White,
            Padding = new Padding(36, 22, 36, 14),
            ColumnCount = 1,
            RowCount = 0,
            AutoScroll = false,
            Margin = new Padding(0)
        };
        page.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        return page;
    }

    private static void AddRow(TableLayoutPanel page, Control control, bool fill, int bottom)
    {
        int row = page.RowCount;
        page.RowCount = row + 1;
        page.RowStyles.Add(fill
            ? new RowStyle(SizeType.Percent, 100F)
            : new RowStyle(SizeType.AutoSize));
        control.Dock = fill ? DockStyle.Fill : DockStyle.Top;
        control.Margin = new Padding(0, 0, 0, bottom);
        page.Controls.Add(control, 0, row);
    }

    private static void AddRow(TableLayoutPanel page, Control control)
    {
        AddRow(page, control, false, 0);
    }

    private static TableLayoutPanel NewCard()
    {
        var card = new TableLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            BackColor = CardColor,
            ColumnCount = 1,
            RowCount = 0,
            Padding = new Padding(16, 10, 16, 6),
            Margin = new Padding(0, 2, 0, 10)
        };
        card.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        return card;
    }

    private static void AddCardRow(TableLayoutPanel card, Control control, int bottom)
    {
        int row = card.RowCount;
        card.RowCount = row + 1;
        card.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        control.Dock = DockStyle.Top;
        control.Margin = new Padding(0, 0, 0, bottom);
        card.Controls.Add(control, 0, row);
    }

    private static Label BodyLabel(string text, float size, Color color, FontStyle style)
    {
        return new Label
        {
            AutoSize = true,
            Text = text,
            Font = new Font("Segoe UI", size, style),
            ForeColor = color,
            Margin = new Padding(0, 0, 0, 8)
        };
    }

    private Label NewTitle(string it, string en)
    {
        var title = new Label
        {
            AutoSize = true,
            Font = new Font("Segoe UI Light", 20f),
            ForeColor = Color.FromArgb(28, 32, 40),
            Margin = new Padding(0, 0, 0, 8)
        };
        Bind(title, it, en);
        return title;
    }

    // ------------------------------------------------------------- welcome
    private void BuildWelcomePage()
    {
        pageWelcome = NewPage();
        AddRow(pageWelcome, NewTitle("Installa NL Audio Matrix", "Install NL Audio Matrix"));

        var subtitle = BodyLabel("App, motore audio e driver virtuali NL Mic 1-4 / NL Return 1-4.",
                                9.5f, MutedColor, FontStyle.Regular);
        Bind(subtitle, "App, motore audio e driver virtuali NL Mic 1-4 / NL Return 1-4.",
             "App, audio engine and NL Mic 1-4 / NL Return 1-4 virtual drivers.");
        AddRow(pageWelcome, subtitle);

        var card = NewCard();
        var cardTitle = new Label
        {
            AutoSize = true,
            Font = new Font("Segoe UI Semibold", 9.5f),
            Margin = new Padding(0, 0, 0, 8)
        };
        Bind(cardTitle, "Stato attuale del computer", "Current computer status");
        AddCardRow(card, cardTitle, 0);

        lblScanStatus = new Label
        {
            AutoSize = true,
            Margin = new Padding(0, 0, 0, 8)
        };
        Bind(lblScanStatus, "Analisi dello stato in corso…", "Scanning the current state…");
        AddCardRow(card, lblScanStatus, 0);

        lblState = new Label
        {
            AutoSize = true,
            Text = "",
            Margin = new Padding(0)
        };
        AddCardRow(card, lblState, 0);
        AddRow(pageWelcome, card);

        var actionTitle = new Label
        {
            AutoSize = true,
            Font = new Font("Segoe UI Semibold", 10f),
            Margin = new Padding(0, 0, 0, 6)
        };
        Bind(actionTitle, "Cosa vuoi fare?", "What do you want to do?");
        AddRow(pageWelcome, actionTitle);

        rbInstall = MakeActionRadio(pageWelcome, "Installa", "Install",
            "Prima installazione dei componenti NL su questo computer.",
            "First installation of the NL components on this computer.");
        rbUpgrade = MakeActionRadio(pageWelcome, "Aggiorna", "Upgrade",
            "Porta un'installazione esistente a questa versione, conservando profili e preset.",
            "Update an existing installation to this version, keeping profiles and presets.");
        rbRepair = MakeActionRadio(pageWelcome, "Ripara", "Repair",
            "Ripristina file, driver, certificato, servizio ed endpoint mancanti o danneggiati.",
            "Restore missing or damaged files, driver, certificate, service and endpoints.");
        rbUninstall = MakeActionRadio(pageWelcome, "Disinstalla", "Uninstall",
            "Rimuove app, driver e certificato. I profili utente restano.",
            "Removes app, driver and certificate. User profiles are kept.");
        actionRadios = new[] { rbInstall, rbUpgrade, rbRepair, rbUninstall };
        rbInstall.Checked = true;

        var diagFlow = new FlowLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            FlowDirection = FlowDirection.RightToLeft,
            WrapContents = false,
            Dock = DockStyle.Top,
            Margin = new Padding(0, 6, 0, 0)
        };
        var diag = new Button
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            MinimumSize = new Size(210, 34),
            FlatStyle = FlatStyle.System,
            Margin = new Padding(0)
        };
        Bind(diag, "Diagnostica (sola lettura)", "Diagnostics (read-only)");
        diag.Click += delegate { ShowDiagnostics(); };
        diagFlow.Controls.Add(diag);
        AddRow(pageWelcome, diagFlow);

        var singleFile = new Label
        {
            AutoSize = true,
            ForeColor = MutedColor,
            Font = new Font("Segoe UI", 8.5f),
            Margin = new Padding(0, 10, 0, 0)
        };
        Bind(singleFile,
            "Il setup è un unico file: app, motore, driver e certificato sono inclusi e vengono estratti in %TEMP% solo durante l'installazione.",
            "Setup is a single file: app, engine, driver and certificate are embedded and extracted to %TEMP% only while installing.");
        AddRow(pageWelcome, singleFile);
    }

    private RadioButton MakeActionRadio(TableLayoutPanel page, string it, string en,
                                        string descIt, string descEn)
    {
        var radio = new RadioButton
        {
            AutoSize = true,
            Font = new Font("Segoe UI Semibold", 9.5f),
            Margin = new Padding(0, 0, 0, 2)
        };
        Bind(radio, it, en);
        // Each action group lives in its own TableLayoutPanel, so WinForms does
        // not treat these radios as one group. Enforce the single selection here.
        radio.CheckedChanged += delegate
        {
            if (!radio.Checked) return;
            if (actionRadios != null)
                foreach (var other in actionRadios)
                    if (other != null && other != radio && other.Checked)
                        other.Checked = false;
            UpdateNextText();
        };

        var description = new Label
        {
            AutoSize = true,
            ForeColor = MutedColor,
            Font = new Font("Segoe UI", 8.75f),
            Margin = new Padding(20, 0, 0, 0)
        };
        Bind(description, descIt, descEn);

        var group = new TableLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 1,
            RowCount = 2,
            Dock = DockStyle.Top,
            Margin = new Padding(0, 0, 0, 10)
        };
        group.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        group.Controls.Add(radio, 0, 0);
        group.Controls.Add(description, 0, 1);
        AddRow(page, group);
        return radio;
    }

    // ------------------------------------------------------------- options
    private void BuildOptionsPage()
    {
        pageOptions = NewPage();
        AddRow(pageOptions, NewTitle("Opzioni", "Options"));

        var dirLabel = new Label
        {
            AutoSize = true,
            Margin = new Padding(0, 2, 0, 4)
        };
        Bind(dirLabel, "Cartella di installazione", "Installation folder");
        AddRow(pageOptions, dirLabel);

        var dirRow = new TableLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 2,
            RowCount = 1,
            Dock = DockStyle.Top,
            Margin = new Padding(0, 0, 0, 8)
        };
        dirRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        dirRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        txtDir = new TextBox
        {
            Dock = DockStyle.Fill,
            Text = options.AppDir,
            Margin = new Padding(0, 0, 8, 0)
        };
        var browse = new Button
        {
            Text = "…",
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            MinimumSize = new Size(96, 28),
            FlatStyle = FlatStyle.System,
            Margin = new Padding(0)
        };
        Bind(browse, "Sfoglia…", "Browse…");
        browse.Click += OnBrowse;
        dirRow.Controls.Add(txtDir, 0, 0);
        dirRow.Controls.Add(browse, 1, 0);
        AddRow(pageOptions, dirRow);

        chkLaunch = new CheckBox
        {
            AutoSize = true,
            Margin = new Padding(0, 0, 0, 8)
        };
        Bind(chkLaunch, "Avvia NL Audio Matrix al termine", "Launch NL Audio Matrix when finished");
        chkLaunch.Checked = true;
        AddRow(pageOptions, chkLaunch);

        var securityCard = NewCard();
        lblSecurityHeadline = new Label
        {
            AutoSize = true,
            Font = new Font("Segoe UI Semibold", 9.5f),
            Margin = new Padding(0, 0, 0, 8)
        };
        Bind(lblSecurityHeadline, "Requisiti del driver NL (firmato per test)",
             "NL driver requirements (test-signed)");
        AddCardRow(securityCard, lblSecurityHeadline, 0);

        lblSecurityText = new Label
        {
            AutoSize = true,
            Text = "",
            Margin = new Padding(0, 0, 0, 8)
        };
        AddCardRow(securityCard, lblSecurityText, 0);

        chkAcceptTestSigning = new CheckBox
        {
            AutoSize = true,
            Margin = new Padding(0, 0, 0, 8)
        };
        Bind(chkAcceptTestSigning,
            "Consenti a Setup di attivare la firma di test di Windows solo se necessario\r\n(bcdedit /set testsigning on; richiede un riavvio; riduce le protezioni di avvio)",
            "Allow Setup to enable Windows test signing only if needed\r\n(bcdedit /set testsigning on; needs a reboot; lowers boot protection)");
        chkAcceptTestSigning.Checked = false;
        AddCardRow(securityCard, chkAcceptTestSigning, 0);

        var securityFooter = new Label
        {
            AutoSize = true,
            ForeColor = MutedColor,
            Font = new Font("Segoe UI", 8.5f),
            Margin = new Padding(0)
        };
        Bind(securityFooter, "Setup non modifica mai Secure Boot, HVCI o driver di terzi.",
             "Setup never changes Secure Boot, HVCI or third-party drivers.");
        AddCardRow(securityCard, securityFooter, 0);
        AddRow(pageOptions, securityCard);

        var note = new Label
        {
            AutoSize = true,
            ForeColor = MutedColor,
            Font = new Font("Segoe UI", 8.5f),
            Margin = new Padding(0)
        };
        Bind(note,
            "Per un'installazione completa servono i driver NL. La portable (senza driver) è un pacchetto separato per Windows standard.",
            "A complete installation needs the NL drivers. The portable package (no driver) is separate, for standard Windows.");
        AddRow(pageOptions, note);
    }

    private void OnBrowse(object sender, EventArgs e)
    {
        using (var dialog = new FolderBrowserDialog())
        {
            dialog.Description = T("Cartella di installazione di NL Audio Matrix",
                                   "NL Audio Matrix installation folder");
            dialog.SelectedPath = txtDir.Text;
            if (dialog.ShowDialog(this) == DialogResult.OK) txtDir.Text = dialog.SelectedPath;
        }
    }

    // ------------------------------------------------------------ progress
    private void BuildProgressPage()
    {
        pageProgress = NewPage();
        lblProgressTitle = NewTitle("Installazione in corso", "Installation in progress");
        AddRow(pageProgress, lblProgressTitle);

        progressBar = new ProgressBar
        {
            Dock = DockStyle.Top,
            Height = 18,
            Style = ProgressBarStyle.Marquee,
            MarqueeAnimationSpeed = 24,
            Margin = new Padding(0, 2, 0, 12)
        };
        AddRow(pageProgress, progressBar);

        txtProgressLog = new TextBox
        {
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            Dock = DockStyle.Fill,
            BackColor = Color.FromArgb(250, 251, 253),
            Font = new Font("Consolas", 8.5f),
            Margin = new Padding(0)
        };
        AddRow(pageProgress, txtProgressLog, true, 0);
    }

    // -------------------------------------------------------------- finish
    private void BuildFinishPage()
    {
        pageFinish = NewPage();
        lblFinishTitle = NewTitle("Completato", "Finished");
        AddRow(pageFinish, lblFinishTitle);

        txtFinishSummary = new TextBox
        {
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            Dock = DockStyle.Fill,
            BackColor = CardColor,
            Font = new Font("Segoe UI", 9f),
            Margin = new Padding(0, 2, 0, 10)
        };
        AddRow(pageFinish, txtFinishSummary, true, 10);

        lblReboot = new Label
        {
            AutoSize = true,
            Font = new Font("Segoe UI Semibold", 9.5f),
            ForeColor = WarnColor,
            Margin = new Padding(0, 0, 0, 8)
        };
        AddRow(pageFinish, lblReboot);

        chkRegisterVerify = new CheckBox
        {
            AutoSize = true,
            Margin = new Padding(0, 0, 0, 10),
            Checked = true
        };
        Bind(chkRegisterVerify,
            "Verifica automaticamente al prossimo accesso (nessuna richiesta di amministratore)",
            "Verify automatically at the next sign-in (no administrator prompt)");
        AddRow(pageFinish, chkRegisterVerify);

        var buttons = new FlowLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            Dock = DockStyle.Top,
            Margin = new Padding(0)
        };
        btnLaunchApp = new Button
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            MinimumSize = new Size(180, 34),
            FlatStyle = FlatStyle.System,
            Margin = new Padding(0, 0, 8, 0),
            Visible = false
        };
        Bind(btnLaunchApp, "Avvia NL Audio Matrix", "Launch NL Audio Matrix");
        btnLaunchApp.Click += OnLaunchApp;
        buttons.Controls.Add(btnLaunchApp);

        btnOpenLog = new Button
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            MinimumSize = new Size(110, 34),
            FlatStyle = FlatStyle.System,
            Margin = new Padding(0)
        };
        Bind(btnOpenLog, "Apri log", "Open log");
        btnOpenLog.Click += delegate { OpenLog(); };
        buttons.Controls.Add(btnOpenLog);
        AddRow(pageFinish, buttons);
    }

    // ---------------------------------------------------------------- state
    private void OnLoaded(object sender, EventArgs e)
    {
        var pending = NlPendingReboot.Read();
        if (pending.Pending)
        {
            lblScanStatus.Text = T("Riavvio completato di recente: verifica dell'installazione in sospeso (fase ",
                                   "Reboot completed recently: installation check pending (phase ") +
                                  (pending.Phase.Length > 0 ? pending.Phase : T("sconosciuta", "unknown")) + ").";
        }
        StartScan();
    }

    private void StartScan()
    {
        if (scanRunning) return;
        scanRunning = true;
        var scanLog = new SetupLog(Path.Combine(Path.GetTempPath(), "NLMatrixSetup-scan.log"));
        var thread = new Thread(delegate()
        {
            SetupDiagnosticsReport report = null;
            string error = "";
            try
            {
                scanLog.Header("NL Audio Matrix Setup", "scan", rawArgs,
                               "Read-only state scan for the wizard");
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
                BeginInvoke(new Action(delegate
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
            lblScanStatus.Text = T("Impossibile leggere lo stato: ", "Cannot read the state: ") + scanError;
            lblScanStatus.ForeColor = BadColor;
            lblState.Text = T("Il pacchetto di setup non è leggibile. Chiudi e riavvia Setup.",
                              "The setup package cannot be read. Close and start Setup again.");
            return;
        }
        bool installed = scan.InstalledAppVersion.Length > 0 || scan.ArpEntry ||
                         scan.Packages.Count > 0 || (scan.Device != null && scan.Device.Present);
        bool appOlder = installed && scan.InstalledAppVersion.Length > 0 &&
                        !NlText.SameVersion(scan.InstalledAppVersion, scan.PackageVersion);
        bool healthy = scan.Healthy;

        if (installed)
        {
            lblScanStatus.Text = T("NL Audio Matrix risulta installato", "NL Audio Matrix appears installed") +
                (scan.InstalledAppVersion.Length > 0
                    ? T(" (versione ", " (version ") + scan.InstalledAppVersion + ")"
                    : "") +
                (healthy ? T(" e funzionante.", " and working.")
                         : T(" ma non completamente funzionante.", " but not fully working."));
        }
        else
        {
            lblScanStatus.Text = T("NL Audio Matrix non risulta installato su questo computer.",
                                   "NL Audio Matrix does not appear to be installed on this computer.");
        }
        lblScanStatus.ForeColor = installed ? (healthy ? OkColor : WarnColor) : Color.FromArgb(28, 32, 40);
        packageInfoLabel.Text = T("Pacchetto: ", "Package: ") + scan.PackageVersion + "\r\n" +
                                T("Driver ", "Driver ") + NlText.DisplayVersion(scan.PackageDriverVersion) + "\r\n" +
                                "Windows 10/11 x64";

        var sb = new StringBuilder();
        sb.AppendLine(T("Driver caricato     : ", "Driver loaded       : ") +
                      (scan.Runtime != null && scan.Runtime.ServiceRunning
                          ? T("sì (", "yes (") + NlText.DisplayVersion(scan.Runtime.LoadedFileVersion) + ")"
                          : T("no", "no")));
        sb.AppendLine(T("Endpoint audio      : ", "Audio endpoints     : ") + (scan.Endpoints != null
            ? scan.Endpoints.ActiveCount + T(" attivi su 8 attesi (4 NL Mic + 4 NL Return)",
                                             " active out of 8 expected (4 NL Mic + 4 NL Return)")
            : T("sconosciuti", "unknown")));
        sb.AppendLine(T("Firma di test       : ", "Test signing        : ") +
                      (scan.Security != null ? scan.Security.TestSigning : T("sconosciuta", "unknown")) +
                      "   Secure Boot: " + (scan.Security != null ? scan.Security.SecureBoot : T("sconosciuto", "unknown")) +
                      "   HVCI: " + (scan.Security != null ? scan.Security.Hvci : T("sconosciuto", "unknown")));
        sb.AppendLine(T("Decisione di Setup  : ", "Setup decision      : ") +
                      (scan.Decision.Length > 0 ? scan.Decision : T("n/d", "n/a")));
        lblState.Text = sb.ToString();

        rbInstall.Checked = !installed;
        rbUpgrade.Enabled = installed;
        rbRepair.Enabled = installed;
        rbUninstall.Enabled = installed;
        rbUpgrade.Text = T("Aggiorna", "Upgrade") + (appOlder
            ? T(" (versione ", " (version ") + scan.InstalledAppVersion + T(" rilevata)", " detected)")
            : "");
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
            lblSecurityText.Text = T("Configurazione di sicurezza non leggibile.",
                                     "Security configuration cannot be read.");
            return;
        }
        var sb = new StringBuilder();
        sb.AppendLine("Secure Boot: " + security.SecureBoot + "    TESTSIGNING: " + security.TestSigning);
        sb.AppendLine(T("HVCI (Integrità memoria): ", "HVCI (Memory integrity): ") +
                      security.Hvci + "    " + T("Firmware: ", "Firmware: ") + security.Firmware);
        sb.AppendLine();
        if (security.Verdict == "ok")
        {
            sb.AppendLine(T("La configurazione attuale consente di caricare il driver firmato per test.",
                            "The current configuration allows loading the test-signed driver."));
            lblSecurityHeadline.ForeColor = OkColor;
        }
        else if (security.Verdict == "blocked")
        {
            sb.AppendLine(T("Questa configurazione di Windows blocca il driver firmato per test.",
                            "This Windows configuration blocks the test-signed driver."));
            foreach (var line in security.Guidance) sb.AppendLine("• " + line);
            lblSecurityHeadline.ForeColor = BadColor;
        }
        else
        {
            sb.AppendLine(T("Stato non determinabile senza privilegi di amministratore.",
                            "State cannot be determined without administrator rights."));
            lblSecurityHeadline.ForeColor = WarnColor;
        }
        lblSecurityText.Text = sb.ToString();
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

    private string ProgressTitle()
    {
        switch (SelectedMode())
        {
            case "uninstall": return T("Disinstallazione in corso", "Uninstalling");
            case "repair": return T("Riparazione in corso", "Repairing");
            case "upgrade": return T("Aggiornamento in corso", "Upgrading");
            default: return T("Installazione in corso", "Installing");
        }
    }

    private void UpdateNextText()
    {
        if (btnNext == null) return;
        if (pageIndex == 0)
        {
            btnNext.Text = T("Avanti", "Next");
            btnNext.Enabled = scan != null && !scanRunning;
        }
        else if (pageIndex == 1)
        {
            string mode = SelectedMode();
            btnNext.Text = mode == "uninstall" ? T("Disinstalla", "Uninstall")
                         : mode == "repair" ? T("Ripara", "Repair")
                         : mode == "upgrade" ? T("Aggiorna", "Upgrade")
                         : T("Installa", "Install");
            btnNext.Enabled = true;
        }
        else if (pageIndex == 3)
        {
            btnNext.Text = resultReboot ? T("Riavvia ora", "Restart now") : T("Chiudi", "Close");
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
                MessageBox.Show(this, T("La cartella di installazione non è valida",
                                        "The installation folder is not valid") +
                                (reason != null && reason.Length > 0 ? ": " + reason : "."),
                                "NL Audio Matrix Setup", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (scan != null && scan.Security != null && scan.Security.Verdict == "blocked")
            {
                MessageBox.Show(this,
                    T("La configurazione di sicurezza di Windows blocca il driver firmato per test:\r\n\r\n",
                      "The Windows security configuration blocks the test-signed driver:\r\n\r\n") +
                    scan.Security.BlockReason + "\r\n\r\n" +
                    T("Segui le istruzioni mostrate nella pagina Opzioni (o premi «Guida e requisiti») e riavvia Setup. Setup non modifica Secure Boot, HVCI o la configurazione di avvio.",
                      "Follow the instructions on the Options page (or open «Guide and requirements») and start Setup again. Setup does not change Secure Boot, HVCI or the boot configuration."),
                    "NL Audio Matrix Setup", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (scan != null && scan.Security != null &&
                scan.Security.TestSigning == "reboot-required")
            {
                MessageBox.Show(this,
                    T("La firma di test di Windows è configurata, ma questo avvio non è partito con essa attiva: il driver NL non può caricare finché Windows non viene riavviato.\r\n\r\nRiavvia il PC e avvia di nuovo Setup: non occorre reinstallare nulla e nessuna impostazione viene modificata.",
                      "Windows test signing is configured, but this boot did not start with it: the NL driver cannot load until Windows restarts.\r\n\r\nRestart the PC and start Setup again: nothing has to be reinstalled and no setting is changed."),
                    "NL Audio Matrix Setup", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (scan != null && scan.Security != null &&
                scan.Security.TestSigning == "disabled" && !chkAcceptTestSigning.Checked)
            {
                MessageBox.Show(this,
                    T("La firma di test di Windows è disattivata: il driver NL non può essere caricato.\r\n\r\nAttiva l'opzione «Consenti a Setup di attivare la firma di test» oppure abilitala tu (bcdedit /set testsigning on) e riavvia.",
                      "Windows test signing is off: the NL driver cannot load.\r\n\r\nTick «Allow Setup to enable Windows test signing» or enable it yourself (bcdedit /set testsigning on) and reboot."),
                    "NL Audio Matrix Setup", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (mode == "repair" || mode == "upgrade")
            {
                string answer = mode == "upgrade"
                    ? T("Aggiornare NL Audio Matrix a questa versione?\r\n\r\nProfili, preset e impostazioni audio di Windows saranno conservati.",
                        "Upgrade NL Audio Matrix to this version?\r\n\r\nProfiles, presets and Windows audio settings will be kept.")
                    : T("Riparare l'installazione di NL Audio Matrix?\r\n\r\nI file mancanti o danneggiati saranno ripristinati.",
                        "Repair the NL Audio Matrix installation?\r\n\r\nMissing or damaged files will be restored.");
                if (MessageBox.Show(this, answer, "NL Audio Matrix Setup", MessageBoxButtons.YesNo,
                                    MessageBoxIcon.Question, MessageBoxDefaultButton.Button1) != DialogResult.Yes)
                    return;
            }
        }
        else
        {
            if (MessageBox.Show(this,
                T("Disinstallare NL Audio Matrix e i driver virtuali NL?\r\n\r\nProfili e preset in %APPDATA%\\NL Audio Matrix saranno conservati. Al termine potrebbe essere necessario un riavvio.",
                  "Uninstall NL Audio Matrix and the NL virtual drivers?\r\n\r\nProfiles and presets in %APPDATA%\\NL Audio Matrix will be kept. A reboot may be needed at the end."),
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
        lblProgressTitle.Text = ProgressTitle();
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
            summary = T("Operazione non riuscita:\r\n", "Operation failed:\r\n") + ex.Message;
            code = ex.ExitCode;
            reboot = false;
        }
        catch (Exception ex)
        {
            log.Error("Unexpected error: " + ex);
            summary = T("Operazione non riuscita per un errore imprevisto:\r\n",
                        "Operation failed with an unexpected error:\r\n") + ex.Message;
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
            BeginInvoke(new Action(delegate { OnOperationFinished(code, summary, reboot); }));
        }
    }

    private void OnLogLine(string level, string text)
    {
        if (closed) return;
        try
        {
            BeginInvoke(new Action(delegate
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
        RefreshFinishText();
        ShowPage(3);
        btnBack.Visible = false;
        btnNext.Enabled = true;
        btnNext.Visible = true;
        if (reboot)
        {
            btnNext.Text = T("Riavvia ora", "Restart now");
            btnCloseWithoutReboot.Visible = true;
            btnCloseWithoutReboot.Enabled = true;
        }
        else
        {
            btnNext.Text = T("Chiudi", "Close");
            btnCloseWithoutReboot.Visible = false;
        }
    }

    private void RefreshFinishText()
    {
        if (txtFinishSummary == null) return;
        bool success = resultCode == 0 || resultCode == 20;
        if (success && options.Mode == "uninstall")
        {
            lblFinishTitle.Text = T("Disinstallazione completata", "Uninstall completed");
            lblFinishTitle.ForeColor = OkColor;
        }
        else if (success)
        {
            lblFinishTitle.Text = resultReboot
                ? T("Quasi completato: riavvia il computer", "Almost done: restart the computer")
                : T("Installazione completata", "Installation completed");
            lblFinishTitle.ForeColor = resultReboot ? WarnColor : OkColor;
        }
        else
        {
            lblFinishTitle.Text = T("Operazione non completata (codice ", "Operation not completed (code ") +
                                  resultCode + ")";
            lblFinishTitle.ForeColor = BadColor;
        }

        var sb = new StringBuilder();
        sb.AppendLine(resultSummary);
        if (!success)
        {
            sb.AppendLine();
            sb.AppendLine(T("Il computer non è stato lasciato in uno stato non sicuro: driver di terzi, Secure Boot e",
                            "The computer was not left in an unsafe state: third-party drivers, Secure Boot and"));
            sb.AppendLine(T("impostazioni audio di Windows non sono stati modificati.",
                            "Windows audio settings were not changed."));
            sb.AppendLine(T("Controlla il log per capire quale passaggio è fallito e riprova con «Ripara».",
                            "Check the log to see which step failed and retry with «Repair»."));
        }
        var pending = NlPendingReboot.Read();
        if (pending.Pending)
        {
            sb.AppendLine();
            sb.AppendLine(T("Riavvio in sospeso: al prossimo accesso Setup completerà e verificherà l'installazione.",
                            "Reboot pending: at the next sign-in Setup will complete and verify the installation."));
        }
        sb.AppendLine();
        sb.AppendLine("Log: " + logPath);
        txtFinishSummary.Text = sb.ToString();

        lblReboot.Text = resultReboot
            ? T("Riavvia Windows per caricare il driver firmato per test e completare l'installazione.",
                "Restart Windows to load the test-signed driver and complete the installation.")
            : "";
        chkRegisterVerify.Visible = resultReboot;
        chkRegisterVerify.Checked = resultReboot;

        string ui = Path.Combine(options.AppDir, NlConst.UiExeName);
        btnLaunchApp.Visible = success && !resultReboot && options.Mode != "uninstall" && File.Exists(ui);
        btnOpenLog.Visible = logPath.Length > 0 && File.Exists(logPath);
    }

    private void RebootNow()
    {
        try
        {
            // RunOnce was registered by Installer.RecordPendingReboot when the
            // option was ticked; shutdown is the only system action here.
            string message = T("NL Audio Matrix: riavvio per completare l'installazione",
                               "NL Audio Matrix: restarting to complete the installation");
            var process = Process.Start(new ProcessStartInfo("shutdown.exe",
                "/r /t 20 /c \"" + message + "\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true
            });
            if (process != null) process.WaitForExit(5000);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this,
                T("Impossibile avviare il riavvio: ", "Cannot start the restart: ") + ex.Message +
                T("\r\nRiavvia Windows manualmente.", "\r\nRestart Windows manually."),
                "NL Audio Matrix Setup", MessageBoxButtons.OK, MessageBoxIcon.Warning);
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
            MessageBox.Show(this,
                T("Impossibile avviare NL Audio Matrix: ", "Cannot start NL Audio Matrix: ") + ex.Message,
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
            MessageBox.Show(this,
                T("Diagnostica non disponibile: lo stato non è stato letto.",
                  "Diagnostics unavailable: the state has not been read."),
                "NL Audio Matrix Setup", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        var sb = new StringBuilder();
        scan.ToText(sb);
        ShowTextDialog(T("Diagnostica (sola lettura)", "Diagnostics (read-only)"), sb.ToString());
    }

    private void ShowGuide()
    {
        ShowTextDialog(T("Guida e requisiti", "Guide and requirements"), GuideText());
    }

    private string GuideText()
    {
        var sb = new StringBuilder();
        sb.AppendLine(T("NL AUDIO MATRIX — GUIDA RAPIDA", "NL AUDIO MATRIX — QUICK GUIDE"));
        sb.AppendLine();
        sb.AppendLine(T("Cosa installa", "What Setup installs"));
        sb.AppendLine(T("  • Applicazione (NLMatrixUI.exe), motore audio (NLMatrixRouter.exe) e driver virtuali",
                        "  • Application (NLMatrixUI.exe), audio engine (NLMatrixRouter.exe) and the virtual"));
        sb.AppendLine(T("    NL Mic 1-4 e NL Return 1-4.",
                        "    drivers NL Mic 1-4 and NL Return 1-4."));
        sb.AppendLine(T("  • Il setup è un unico file: non serve installare a mano certificati, file SYS o INF.",
                        "  • Setup is one file: no manual installation of certificates, SYS or INF files."));
        sb.AppendLine(T("  • Nessuna chiave privata di firma è inclusa nel pacchetto.",
                        "  • No signing private key is included in the package."));
        sb.AppendLine();
        sb.AppendLine(T("Il driver è firmato per test", "The driver is test-signed"));
        sb.AppendLine(T("  Il driver NL non ha una firma di produzione (WHQL/EV). Windows lo carica solo con la",
                        "  The NL driver has no production signature (WHQL/EV). Windows loads it only with"));
        sb.AppendLine(T("  «firma di test» attiva (TESTSIGNING). Setup attiva la firma di test solo se selezioni",
                        "  test signing on (TESTSIGNING). Setup enables test signing only if you tick the"));
        sb.AppendLine(T("  esplicitamente l'opzione corrispondente; non modifica mai Secure Boot né HVCI.",
                        "  matching option; it never changes Secure Boot or HVCI."));
        sb.AppendLine();
        sb.AppendLine(T("PC con Secure Boot attivo (OEM/preassemblati o assemblati a mano)",
                        "PC with Secure Boot enabled (OEM/prebuilt or hand-built)"));
        sb.AppendLine(T("  1. Disattiva Secure Boot nel firmware (BIOS/UEFI) — operazione manuale, non eseguita da Setup.",
                        "  1. Disable Secure Boot in the firmware (BIOS/UEFI) — manual step, Setup does not do it."));
        sb.AppendLine(T("  2. Abilita la firma di test: bcdedit /set testsigning on (oppure l'opzione nel setup) e riavvia.",
                        "  2. Enable test signing: bcdedit /set testsigning on (or the Setup option) and reboot."));
        sb.AppendLine(T("  3. Se anche «Integrità memoria» (HVCI) è attiva, disattivala in Sicurezza di Windows →",
                        "  3. If «Memory integrity» (HVCI) is also on, turn it off in Windows Security →"));
        sb.AppendLine(T("     Sicurezza del dispositivo → Isolamento core, poi riavvia.",
                        "     Device security → Core isolation, then reboot."));
        sb.AppendLine(T("  Conseguenze: con Secure Boot disattivato il PC è meno protetto all'avvio finché non",
                        "  Consequences: with Secure Boot off the PC is less protected at boot until you"));
        sb.AppendLine(T("  installi un driver firmato per produzione. Nessun'altra impostazione di Windows cambia.",
                        "  install a production-signed driver. No other Windows setting changes."));
        sb.AppendLine();
        sb.AppendLine(T("Windows standard senza modalità test", "Standard Windows without test mode"));
        sb.AppendLine(T("  Usa il pacchetto portable: è solo user-mode, non installa driver e funziona con gli",
                        "  Use the portable package: user-mode only, it installs no driver and works with the"));
        sb.AppendLine(T("  endpoint già presenti (per esempio la tua interfaccia ASIO e VB-CABLE).",
                        "  endpoints already present (for example your ASIO interface and VB-CABLE)."));
        sb.AppendLine();
        sb.AppendLine(T("Cosa viene conservato", "What is kept"));
        sb.AppendLine(T("  • Profili, preset e preferenze in %APPDATA%\\NL Audio Matrix restano anche dopo update,",
                        "  • Profiles, presets and preferences in %APPDATA%\\NL Audio Matrix survive update,"));
        sb.AppendLine(T("    riparazione e disinstallazione.",
                        "    repair and uninstall."));
        sb.AppendLine(T("  • Driver di terzi (UAD, Focusrite, VB-CABLE) e impostazioni audio di Windows non sono toccati.",
                        "  • Third-party drivers (UAD, Focusrite, VB-CABLE) and Windows audio settings are not touched."));
        sb.AppendLine(T("  • Gli aggiornamenti rimuovono solo i pacchetti driver NL obsoleti, mantenendo quello corrente.",
                        "  • Upgrades remove only obsolete NL driver packages and keep the current one."));
        sb.AppendLine();
        sb.AppendLine(T("Riavvio e verifica", "Reboot and verification"));
        sb.AppendLine(T("  Se serve un riavvio, Setup lo segnala e propone il riavvio immediato. Al successivo accesso",
                        "  If a reboot is needed, Setup warns and offers to restart immediately. At the next sign-in"));
        sb.AppendLine(T("  una verifica automatica (senza prompt di amministratore) controlla driver, dispositivo,",
                        "  an automatic check (no administrator prompt) verifies driver, device,"));
        sb.AppendLine(T("  gli otto endpoint e l'avvio/arresto dell'applicazione.",
                        "  the eight endpoints and application start/stop."));
        sb.AppendLine();
        sb.AppendLine(T("Disinstallazione", "Uninstall"));
        sb.AppendLine(T("  Pannello di controllo → App → NL Audio Matrix → Disinstalla (o il Setup, voce «Disinstalla»).",
                        "  Control Panel → Apps → NL Audio Matrix → Uninstall (or Setup, «Uninstall»)."));
        sb.AppendLine(T("  Rimuove app, driver, certificato e voci di registro; conserva i profili utente.",
                        "  Removes app, driver, certificate and registry entries; keeps user profiles."));
        sb.AppendLine();
        sb.AppendLine(T("Exit code (per uso da riga di comando)", "Exit codes (command line)"));
        sb.AppendLine(T("  0 successo · 9 endpoint/driver non sani · 12 configurazione di sicurezza che blocca il driver",
                        "  0 success · 9 unhealthy endpoints/driver · 12 security configuration blocking the driver"));
        sb.AppendLine(T("  20 riavvio necessario · altri codici: vedi il log in %TEMP%\\NLMatrixSetup-*.log",
                        "  20 reboot required · other codes: see the log in %TEMP%\\NLMatrixSetup-*.log"));
        return sb.ToString();
    }

    private void ShowTextDialog(string title, string text)
    {
        using (var dialog = new Form())
        {
            dialog.Text = title;
            dialog.Font = Font;
            dialog.AutoScaleMode = AutoScaleMode.Dpi;
            dialog.ClientSize = new Size(820, 620);
            dialog.StartPosition = FormStartPosition.CenterParent;
            dialog.MinimizeBox = false;
            dialog.MaximizeBox = false;
            dialog.FormBorderStyle = FormBorderStyle.FixedDialog;

            var table = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                ColumnCount = 1,
                RowCount = 2,
                Padding = new Padding(12)
            };
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            table.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            table.RowStyles.Add(new RowStyle(SizeType.AutoSize));

            var box = new TextBox
            {
                Multiline = true,
                ReadOnly = true,
                ScrollBars = ScrollBars.Both,
                WordWrap = false,
                Dock = DockStyle.Fill,
                Text = text,
                Font = new Font("Consolas", 9f),
                Margin = new Padding(0, 0, 0, 10)
            };
            table.Controls.Add(box, 0, 0);

            var buttons = new FlowLayoutPanel
            {
                AutoSize = true,
                AutoSizeMode = AutoSizeMode.GrowAndShrink,
                FlowDirection = FlowDirection.RightToLeft,
                WrapContents = false,
                Dock = DockStyle.Top,
                Margin = new Padding(0)
            };
            var ok = new Button
            {
                Text = T("Chiudi", "Close"),
                AutoSize = true,
                MinimumSize = new Size(90, 30),
                FlatStyle = FlatStyle.System,
                Margin = new Padding(8, 0, 0, 0)
            };
            ok.Click += delegate { dialog.Close(); };
            buttons.Controls.Add(ok);
            var copy = new Button
            {
                Text = T("Copia", "Copy"),
                AutoSize = true,
                MinimumSize = new Size(90, 30),
                FlatStyle = FlatStyle.System,
                Margin = new Padding(0)
            };
            copy.Click += delegate
            {
                try { Clipboard.SetText(text); } catch { }
            };
            buttons.Controls.Add(copy);
            table.Controls.Add(buttons, 0, 1);
            dialog.Controls.Add(table);
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
            MessageBox.Show(this,
                T("Un'operazione è in corso. Attendere il completamento prima di chiudere.",
                  "An operation is in progress. Wait for it to finish before closing."),
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
