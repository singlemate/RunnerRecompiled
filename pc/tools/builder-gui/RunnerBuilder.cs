// The Windows window of RunnerBuilder: asks for the game folder, then runs pc/tools/builder.py.
// C# 5, so that the csc.exe that comes with Windows compiles it (make_exe.py). No build logic here.
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("RunnerBuilder")]
[assembly: AssemblyVersion("0.3.1.0")]

/* Interface language: the Russian source text is the key into the embedded tables (i18n_<lang>). */
static class Lang {
    public static readonly string Code = Detect();
    static System.Collections.Generic.Dictionary<string, string> map;

    static string Detect() {
        string l = Environment.GetEnvironmentVariable("RUNNER_LANG");  // for tests
        if (string.IsNullOrEmpty(l)) l = System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName.ToLowerInvariant();
        if (l == "ru" || l == "uk" || l == "be") return "ru";
        return Assembly.GetExecutingAssembly().GetManifestResourceInfo("i18n_" + l) != null ? l : "en";
    }

    static string Unescape(string s) { return s.Replace("\\\\", "\u0001").Replace("\\n", "\n").Replace("\u0001", "\\"); }

    public static string T(string source) {
        if (Code == "ru") return source;
        if (map == null) {
            var m = new System.Collections.Generic.Dictionary<string, string>();
            using (var res = Assembly.GetExecutingAssembly().GetManifestResourceStream("i18n_" + Code))
            using (var r = new StreamReader(res, Encoding.UTF8)) {
                for (string line; (line = r.ReadLine()) != null; ) {
                    int tab = line.IndexOf('\t');
                    if (tab > 0) m[Unescape(line.Substring(0, tab))] = Unescape(line.Substring(tab + 1));
                }
            }
            map = m;
        }
        string v;
        return map.TryGetValue(source, out v) ? v : source;
    }
}

static class Program {
    [STAThread]
    static void Main(string[] args) {
        ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new BuilderForm(args));
    }
}

/* Thin progress line in the style of the app: a share when known, a moving block when not. */
sealed class Meter : Control {
    int percent = -1, phase;
    public bool Running;
    public int Percent { get { return percent; } set { percent = value; Invalidate(); } }

    public Meter() { DoubleBuffered = true; SetStyle(ControlStyles.ResizeRedraw, true); }

    public void Advance() {
        if (!Running || percent >= 0) return;
        phase = (phase + 6) % (Width + 160);
        Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e) {
        using (var track = new SolidBrush(Color.FromArgb(0xE8, 0xEC, 0xEF))) e.Graphics.FillRectangle(track, ClientRectangle);
        using (var red = new SolidBrush(Color.FromArgb(0xE2, 0x4B, 0x4A))) {
            if (percent >= 0) e.Graphics.FillRectangle(red, 0, 0, Math.Min(percent, 100) * Width / 100, Height);
            else if (Running) e.Graphics.FillRectangle(red, phase - 160, 0, 160, Height);
        }
    }
}

sealed class BuilderForm : Form {
    const int VersionCode = 6;
    const string VersionName = "0.3.1";
    const string UpdateUrl = "https://raw.githubusercontent.com/singlemate/RunnerRecompiled/main/version-builder.json";
    const string ProjectUrl = "https://github.com/singlemate/RunnerRecompiled";
    const string TermsUrl = "https://developer.android.com/studio/terms";
    const string PythonUrl = "https://www.python.org/ftp/python/3.13.1/python-3.13.1-embed-amd64.zip";
    const string PythonSha256 = "7b7923ff0183a8b8fca90f6047184b419b108cb437f75fc1c002f9d2f8bcec16";

    readonly TextBox game = new TextBox(), output = new TextBox(), log = new TextBox();
    readonly CheckBox terms = new CheckBox();
    readonly Button start = new Button(), openOut = new Button(), install = new Button();
    readonly Label status = new Label();
    readonly Label elapsed = new Label();
    readonly Meter meter = new Meter();
    readonly System.Windows.Forms.Timer tick = new System.Windows.Forms.Timer();
    readonly Stopwatch clock = new Stopwatch();
    Process running;
    string doneFolder;
    // The current run: a build (builder.py) or an install on the tablet (device.py).
    bool installing, installed;
    // For tests: RunnerBuilder.exe --auto <game> <out> <log> presses Build and leaves when it ends.
    string autoLog;

    static string Home {
        get {
            // aapt2 cannot open a path with non-ASCII letters, and user names often have them.
            string usual = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RunnerBuilder");
            if (Regex.IsMatch(usual, "^[\\x20-\\x7E]+$")) return usual;
            return Path.Combine(Path.GetPathRoot(Environment.SystemDirectory), "RunnerBuilder");
        }
    }

    static readonly Color Paper = Color.FromArgb(0xF7, 0xF8, 0xF9), Ink = Color.FromArgb(0x14, 0x14, 0x14),
        Muted = Color.FromArgb(0x6A, 0x76, 0x80), Rule = Color.FromArgb(0xE8, 0xEC, 0xEF), Red = Color.FromArgb(0xE2, 0x4B, 0x4A);

    static Label Kicker(string text) {
        return new Label { Text = text, AutoSize = true, ForeColor = Red, Font = new Font("Segoe UI", 8.5f, FontStyle.Bold), Margin = new Padding(0, 14, 0, 6) };
    }

    static void Flat(Button b, bool primary) {
        b.FlatStyle = FlatStyle.Flat;
        b.AutoSize = true;
        b.Cursor = Cursors.Hand;
        b.UseVisualStyleBackColor = false;
        b.BackColor = primary ? Ink : Color.White;
        b.ForeColor = primary ? Color.White : Ink;
        b.FlatAppearance.BorderColor = Ink;
        b.FlatAppearance.BorderSize = 1;
        b.FlatAppearance.MouseOverBackColor = primary ? Red : Rule;
        b.FlatAppearance.MouseDownBackColor = primary ? Red : Rule;
    }

    public BuilderForm(string[] args) {
        Text = "RunnerBuilder " + VersionName;
        ClientSize = new Size(760, 640);
        MinimumSize = new Size(660, 560);
        Font = new Font("Segoe UI", 10f);
        BackColor = Paper;
        ForeColor = Ink;
        AutoScaleMode = AutoScaleMode.Dpi;
        StartPosition = FormStartPosition.CenterScreen;
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { }

        var table = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(26, 6, 26, 20), ColumnCount = 3, RowCount = 11 };
        table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        for (int i = 0; i < 11; i++) table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        table.RowStyles[9] = new RowStyle(SizeType.Percent, 100);

        var intro = new Label {
            AutoSize = true, Margin = new Padding(0, 0, 0, 4), MaximumSize = new Size(700, 0), ForeColor = Muted,
            Text = Lang.T("Программа соберёт приложение для Android из вашей лицензионной копии Mirror's Edge из GOG ") +
                   Lang.T("и подготовит папку, которую останется скопировать на планшет.")
        };
        table.Controls.Add(intro, 0, 0); table.SetColumnSpan(intro, 3);

        var folders = Kicker(Lang.T("ПАПКИ"));
        table.Controls.Add(folders, 0, 1); table.SetColumnSpan(folders, 3);
        AddPathRow(table, 2, Lang.T("Папка игры:"), game, Lang.T("Выберите папку, куда установлена игра из GOG"));
        AddPathRow(table, 3, Lang.T("Сохранить в:"), output, Lang.T("Выберите, куда положить готовые файлы"));
        // Not Documents: it is often synced to OneDrive, and the result is about 12 GB of game files.
        output.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "RunnerRecompiled");

        terms.Text = Lang.T("Принимаю условия Android SDK");
        terms.AutoSize = true;
        terms.FlatStyle = FlatStyle.Flat;
        terms.Margin = new Padding(0, 10, 0, 0);
        table.Controls.Add(terms, 0, 4); table.SetColumnSpan(terms, 2);
        var link = new LinkLabel { Text = Lang.T("Прочитать"), AutoSize = true, Margin = new Padding(0, 12, 0, 0), LinkColor = Red, ActiveLinkColor = Ink, VisitedLinkColor = Red };
        link.LinkClicked += delegate { Process.Start(TermsUrl); };
        table.Controls.Add(link, 2, 4);

        var note = new Label {
            AutoSize = true, Margin = new Padding(0, 8, 0, 0), MaximumSize = new Size(700, 0), ForeColor = Muted, Font = new Font("Segoe UI", 9f),
            Text = Lang.T("Понадобится около 16 ГБ на диске и около 150 МБ интернета. Результат содержит данные вашей копии игры: не публикуйте и не передавайте его.")
        };
        table.Controls.Add(note, 0, 5); table.SetColumnSpan(note, 3);

        var progress = Kicker(Lang.T("СБОРКА"));
        table.Controls.Add(progress, 0, 6); table.SetColumnSpan(progress, 3);

        start.Text = Lang.T("Собрать");
        Flat(start, true);
        start.Padding = new Padding(26, 6, 26, 6);
        start.Margin = new Padding(0, 0, 16, 0);
        start.Font = new Font("Segoe UI", 10f, FontStyle.Bold);
        start.Click += delegate { StartBuild(); };
        table.Controls.Add(start, 0, 7);
        status.AutoSize = true; status.Anchor = AnchorStyles.Left; status.Text = ""; status.Font = new Font("Segoe UI", 10f, FontStyle.Bold);
        table.Controls.Add(status, 1, 7);
        elapsed.AutoSize = true; elapsed.Anchor = AnchorStyles.Right; elapsed.ForeColor = Muted; elapsed.Font = new Font("Consolas", 11f);
        table.Controls.Add(elapsed, 2, 7);

        meter.Dock = DockStyle.Fill; meter.Height = 8; meter.Margin = new Padding(0, 12, 0, 12);
        table.Controls.Add(meter, 0, 8); table.SetColumnSpan(meter, 3);

        log.Multiline = true; log.ReadOnly = true; log.ScrollBars = ScrollBars.Vertical; log.Dock = DockStyle.Fill; log.Margin = new Padding(0);
        log.BackColor = Color.White; log.ForeColor = Muted; log.BorderStyle = BorderStyle.FixedSingle; log.Font = new Font("Consolas", 9f);
        table.Controls.Add(log, 0, 9); table.SetColumnSpan(log, 3);

        openOut.Text = Lang.T("Открыть папку с результатом"); Flat(openOut, false); openOut.Visible = false;
        openOut.Padding = new Padding(10, 3, 10, 3);
        openOut.Margin = new Padding(0, 12, 12, 0);
        openOut.Click += delegate { Process.Start("explorer.exe", "\"" + doneFolder + "\""); };
        install.Text = Lang.T("Установить на планшет…"); Flat(install, true);
        install.Padding = new Padding(10, 3, 10, 3);
        install.Margin = new Padding(0, 12, 0, 0);
        install.Click += delegate { StartInstall(); };
        var after = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0), WrapContents = false };
        after.Controls.Add(install);
        after.Controls.Add(openOut);
        table.Controls.Add(after, 0, 10); table.SetColumnSpan(after, 3);
        output.TextChanged += delegate { UpdateInstall(); };
        UpdateInstall();

        var header = new Panel { Dock = DockStyle.Top, Height = 84, BackColor = Paper };
        header.Paint += PaintHeader;
        header.Resize += delegate { header.Invalidate(); };

        Controls.Add(table);
        Controls.Add(header);

        tick.Interval = 40;
        tick.Tick += delegate {
            meter.Advance();
            if (clock.IsRunning) elapsed.Text = ((int)clock.Elapsed.TotalMinutes).ToString("00") + ":" + clock.Elapsed.Seconds.ToString("00");
        };
        tick.Start();

        Shown += delegate { new Thread(CheckForUpdate) { IsBackground = true }.Start(); };
        if (args.Length == 4 && args[0] == "--auto") {
            game.Text = args[1]; output.Text = args[2]; autoLog = args[3]; terms.Checked = true;
            Shown += delegate { StartBuild(); };
        }
        FormClosing += OnClosing;
    }

    void PaintHeader(object sender, PaintEventArgs e) {
        var g = e.Graphics;
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
        int w = ((Control)sender).ClientSize.Width, h = ((Control)sender).ClientSize.Height;
        using (var red = new SolidBrush(Red))
            g.FillPolygon(red, new[] { new Point(w - 190, h), new Point(w - 150, h), new Point(w, h - 62), new Point(w, h - 40 - 62) });
        using (var big = new Font("Segoe UI", 21f, FontStyle.Bold))
        using (var small = new Font("Segoe UI", 9f))
        using (var ink = new SolidBrush(Ink)) using (var red = new SolidBrush(Red)) using (var muted = new SolidBrush(Muted)) {
            float x = 24, y = 20;
            foreach (var part in new[] { Tuple.Create("RUNNER", ink), Tuple.Create("/", red), Tuple.Create("BUILDER", ink) }) {
                g.DrawString(part.Item1, big, part.Item2, x, y, StringFormat.GenericTypographic);
                x += g.MeasureString(part.Item1, big, 1000, StringFormat.GenericTypographic).Width + 1;
            }
            g.DrawString(VersionName, small, muted, x + 10, y + 15, StringFormat.GenericTypographic);
        }
    }

    void AddPathRow(TableLayoutPanel table, int row, string caption, TextBox box, string prompt) {
        table.Controls.Add(new Label { Text = caption, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 6, 12, 0) }, 0, row);
        box.Dock = DockStyle.Fill; box.Margin = new Padding(0, 4, 8, 4); box.BorderStyle = BorderStyle.FixedSingle; box.BackColor = Color.White;
        table.Controls.Add(box, 1, row);
        var browse = new Button { Text = Lang.T("Обзор…"), Margin = new Padding(0, 2, 0, 2) };
        Flat(browse, false);
        browse.Padding = new Padding(8, 0, 8, 0);
        browse.Click += delegate {
            using (var d = new FolderBrowserDialog { Description = prompt, SelectedPath = box.Text })
                if (d.ShowDialog(this) == DialogResult.OK) box.Text = d.SelectedPath;
        };
        table.Controls.Add(browse, 2, row);
    }

    // One notice at start; any failure is silent, this is only a hint.
    void CheckForUpdate() {
        try {
            string json;
            using (var web = new WebClient()) { web.Encoding = Encoding.UTF8; json = web.DownloadString(UpdateUrl); }
            int code = int.Parse(Regex.Match(json, "\"versionCode\"\\s*:\\s*(\\d+)").Groups[1].Value);
            if (code <= VersionCode) return;
            string name = Regex.Match(json, "\"versionName\"\\s*:\\s*\"([^\"]*)\"").Groups[1].Value;
            string notes = Regex.Match(json, "\"notes\"\\s*:\\s*\"([^\"]*)\"").Groups[1].Value;
            BeginInvoke((Action)delegate {
                string text = Lang.T("Вышла версия ") + name + Lang.T(" (у вас ") + VersionName + ")." +
                              (notes.Length > 0 ? "\n\n" + notes : "") + Lang.T("\n\nОткрыть страницу проекта?");
                if (MessageBox.Show(this, text, Lang.T("Доступно обновление"), MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
                    Process.Start(ProjectUrl);
            });
        } catch (Exception) { }
    }

    void StartBuild() {
        string g = game.Text.Trim(), o = output.Text.Trim();
        if (!File.Exists(Path.Combine(g, "Binaries", "MirrorsEdge.exe"))) {
            MessageBox.Show(this, Lang.T("В этой папке нет Binaries\\MirrorsEdge.exe. Укажите папку, куда установлена игра из GOG."), Text);
            return;
        }
        if (o.Length == 0) { MessageBox.Show(this, Lang.T("Укажите, куда сохранить результат."), Text); return; }
        if (!terms.Checked) { MessageBox.Show(this, Lang.T("Для скачивания инструментов Google нужно принять условия Android SDK."), Text); return; }
        installing = false;
        start.Enabled = false; start.BackColor = Rule; start.FlatAppearance.BorderColor = Rule; install.Enabled = false;
        openOut.Visible = false; log.Clear(); doneFolder = null;
        meter.Percent = -1;
        meter.Running = true;
        clock.Restart();
        new Thread(() => Build(g, o)) { IsBackground = true }.Start();
    }

    string Apk { get { return Path.Combine(output.Text.Trim(), "RunnerRecompiled-pc-personal.apk"); } }

    // Installing needs a built APK in the output folder; it can be repeated without building again.
    void UpdateInstall() {
        bool can = running == null && start.Enabled;
        bool has = false;
        try { has = File.Exists(Apk); } catch (Exception) { }
        install.Visible = has;
        install.Enabled = can;
    }

    void StartInstall() {
        string g = game.Text.Trim();
        if (!File.Exists(Path.Combine(g, "Binaries", "MirrorsEdge.exe"))) {
            MessageBox.Show(this, Lang.T("В этой папке нет Binaries\\MirrorsEdge.exe. Укажите папку, куда установлена игра из GOG."), Text);
            return;
        }
        string extra;
        using (var d = new InstallDialog()) {
            if (d.ShowDialog(this) != DialogResult.OK) return;
            extra = d.Arguments(Quote);
        }
        installing = true; installed = false;
        start.Enabled = false; start.BackColor = Rule; start.FlatAppearance.BorderColor = Rule; install.Enabled = false;
        openOut.Visible = false; log.Clear(); doneFolder = null;
        meter.Percent = -1;
        meter.Running = true;
        clock.Restart();
        string args = " --apk " + Quote(Apk) + " --game " + Quote(g) + extra;
        new Thread(() => Run("device.py", args)) { IsBackground = true }.Start();
    }

    void Build(string g, string o) {
        Run("builder.py", " --game " + Quote(g) + " --out " + Quote(o) + " --accept-android-terms");
    }

    void Run(string script, string args) {
        try {
            string python = EnsurePython();
            SetStatus(Lang.T("Подготовка"));
            string app = UnpackPayload();
            var info = new ProcessStartInfo(python) {
                Arguments = "-I -X utf8 -u " + Quote(Path.Combine(app, "pc", "tools", script)) + args +
                            " --home " + Quote(Home) + " --lang " + Lang.Code,
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
            };
            using (var p = Process.Start(info)) {
                running = p;
                p.OutputDataReceived += (s, e) => { if (e.Data != null) Line(e.Data); };
                p.ErrorDataReceived += (s, e) => { if (e.Data != null) Line(e.Data); };
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                p.WaitForExit();
                running = null;
                Finish(p.ExitCode == 0, Lang.T("Код завершения ") + p.ExitCode);
            }
        } catch (Exception e) {
            Line(e.ToString());
            Finish(false, e.Message);
        }
    }

    string EnsurePython() {
        string dir = Path.Combine(Home, "python"), exe = Path.Combine(dir, "python.exe");
        if (File.Exists(exe)) return exe;
        SetStatus(Lang.T("Скачивание Python"));
        Directory.CreateDirectory(Home);
        string zip = Path.Combine(Home, "python.zip");
        using (var web = new WebClient()) web.DownloadFile(PythonUrl, zip);
        using (var sha = SHA256.Create()) using (var f = File.OpenRead(zip)) {
            if (BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "").ToLowerInvariant() != PythonSha256)
                throw new IOException(Lang.T("Python скачался с другой контрольной суммой."));
        }
        if (Directory.Exists(dir)) Directory.Delete(dir, true);
        ZipFile.ExtractToDirectory(zip, dir);
        File.Delete(zip);
        return exe;
    }

    // The sources are unpacked anew for every version of the program.
    static string UnpackPayload() {
        string app = Path.Combine(Home, "app-" + VersionName);
        if (Directory.Exists(app)) Directory.Delete(app, true);
        Directory.CreateDirectory(Home);
        // Sources of earlier versions are not needed any more.
        foreach (string old in Directory.GetDirectories(Home, "app-*"))
            try { Directory.Delete(old, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        string zip = Path.Combine(Home, "payload.zip");
        using (var res = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
        using (var f = File.Create(zip)) res.CopyTo(f);
        ZipFile.ExtractToDirectory(zip, app);
        File.Delete(zip);
        return app;
    }

    static string Quote(string s) {
        // Rules of CommandLineToArgvW: double the backslashes before a closing quote.
        return "\"" + Regex.Replace(s, "(\\\\*)(\"|$)", m => m.Groups[1].Value + m.Groups[1].Value + (m.Groups[2].Length > 0 ? "\\\"" : "")) + "\"";
    }

    void Line(string text) {
        if (IsDisposed) return;
        BeginInvoke((Action)delegate {
            if (text.StartsWith("DONE ")) { doneFolder = text.Substring(5); return; }
            if (text == "INSTALLED") { installed = true; return; }
            Match m;
            if (text.StartsWith("== ")) { status.Text = text.Substring(3); meter.Percent = -1; }
            else if ((m = Regex.Match(text, @"^lifted (\d+)/(\d+) (\w+)")).Success) {
                // Translating is about a third of the work and nearly all of it is the game's own module.
                int done = int.Parse(m.Groups[1].Value), total = int.Parse(m.Groups[2].Value);
                if (m.Groups[3].Value == "game") {
                    status.Text = string.Format(Lang.T("Перевод кода игры: {0}%"), done * 100 / total);
                    meter.Percent = done * 35 / total;
                }
            } else if ((m = Regex.Match(text, @"^compiled (\d+)/(\d+)")).Success) {
                int done = int.Parse(m.Groups[1].Value), total = int.Parse(m.Groups[2].Value);
                status.Text = string.Format(Lang.T("Компиляция кода: {0} из {1}"), done, total);
                meter.Percent = 35 + done * 60 / total;
            } else if ((m = Regex.Match(text, @"^  .*?(\d+)%$")).Success) {
                meter.Percent = int.Parse(m.Groups[1].Value);
            }
            log.AppendText(text + Environment.NewLine);
        });
    }

    void SetStatus(string text) { BeginInvoke((Action)delegate { status.Text = text; }); }

    void Finish(bool ok, string reason) {
        if (IsDisposed) return;
        BeginInvoke((Action)delegate {
            clock.Stop();
            meter.Running = false;
            meter.Percent = ok ? 100 : 0;
            start.Enabled = true; start.BackColor = Ink; start.FlatAppearance.BorderColor = Ink;
            if (installing) {
                status.Text = ok && installed ? Lang.T("Готово: откройте приложение на планшете")
                                              : Lang.T("Установка не удалась. Причина в журнале.");
                if (!(ok && installed)) log.AppendText(reason + Environment.NewLine);
            } else if (ok && doneFolder != null) {
                status.Text = Lang.T("Готово");
                openOut.Visible = true;
            } else {
                status.Text = Lang.T("Сборка не удалась. Причина в журнале.");
                log.AppendText(reason + Environment.NewLine);
            }
            UpdateInstall();
            if (autoLog != null) {
                File.WriteAllText(autoLog, status.Text + Environment.NewLine + log.Text, Encoding.UTF8);
                Close();
            }
        });
    }

    void OnClosing(object sender, FormClosingEventArgs e) {
        Process p = running;
        if (p == null) return;
        if (MessageBox.Show(this, installing ? Lang.T("Копирование на планшет ещё идёт. Прервать?") : Lang.T("Сборка ещё идёт. Прервать?"), Text, MessageBoxButtons.YesNo) != DialogResult.Yes) { e.Cancel = true; return; }
        try { Process.Start(new ProcessStartInfo("taskkill", "/T /F /PID " + p.Id) { CreateNoWindow = true, UseShellExecute = false }); } catch (Exception) { }
    }
}

/* How to reach the tablet: a USB cable, or Wi-Fi debugging (pairing code once, then address and port). */
sealed class InstallDialog : Form {
    readonly RadioButton usb = new RadioButton(), wifi = new RadioButton();
    readonly TextBox pair = new TextBox(), code = new TextBox(), connect = new TextBox();
    readonly Panel wifiBox = new Panel();

    static Label Note(string text) {
        return new Label { Text = text, AutoSize = true, MaximumSize = new Size(500, 0), ForeColor = Color.FromArgb(0x6A, 0x76, 0x80),
                           Margin = new Padding(22, 0, 0, 8), Font = new Font("Segoe UI", 9f) };
    }

    public InstallDialog() {
        Text = Lang.T("Установка на планшет");
        Font = new Font("Segoe UI", 10f);
        BackColor = Color.FromArgb(0xF7, 0xF8, 0xF9);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = MinimizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink;
        AutoScaleMode = AutoScaleMode.Dpi;
        Padding = new Padding(18);
        var flow = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false };
        flow.Controls.Add(new Label { AutoSize = true, MaximumSize = new Size(520, 0), Margin = new Padding(0, 0, 0, 12),
            Text = Lang.T("Программа установит приложение и скопирует на планшет файлы игры (около 12 ГБ на планшете). ") +
                   Lang.T("На планшете должен быть включён режим разработчика: Настройки → О планшете → семь раз нажать «Номер сборки».") });
        usb.Text = Lang.T("По кабелю USB"); usb.AutoSize = true; usb.Checked = true;
        flow.Controls.Add(usb);
        flow.Controls.Add(Note(Lang.T("Включите «Отладку по USB» в настройках для разработчиков, подключите кабель и разрешите отладку на экране планшета.")));
        wifi.Text = Lang.T("По Wi-Fi"); wifi.AutoSize = true;
        flow.Controls.Add(wifi);
        flow.Controls.Add(Note(Lang.T("Включите «Отладку по Wi-Fi» в настройках для разработчиков. Компьютер и планшет должны быть в одной сети.")));
        var grid = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Margin = new Padding(22, 0, 0, 0) };
        AddRow(grid, 0, Lang.T("Адрес сопряжения (IP:порт):"), pair);
        AddRow(grid, 1, Lang.T("Код сопряжения:"), code);
        AddRow(grid, 2, Lang.T("Адрес подключения (IP:порт):"), connect);
        wifiBox.AutoSize = true; wifiBox.Controls.Add(grid);
        flow.Controls.Add(wifiBox);
        flow.Controls.Add(Note(Lang.T("Адрес и код сопряжения: «Подключить устройство с помощью кода». Если этот компьютер уже сопряжён, оставьте их пустыми. ") +
                               Lang.T("Адрес подключения показан на экране «Отладка по Wi-Fi»; порт меняется при каждом включении.")));
        var buttons = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0, 10, 0, 0) };
        var ok = new Button { Text = Lang.T("Установить"), AutoSize = true, DialogResult = DialogResult.None };
        var cancel = new Button { Text = Lang.T("Отмена"), AutoSize = true, DialogResult = DialogResult.Cancel };
        ok.Click += delegate {
            if (wifi.Checked && connect.Text.Trim().Length == 0) {
                MessageBox.Show(this, Lang.T("Укажите адрес подключения с экрана «Отладка по Wi-Fi»."), Text);
                return;
            }
            if (wifi.Checked && (pair.Text.Trim().Length == 0) != (code.Text.Trim().Length == 0)) {
                MessageBox.Show(this, Lang.T("Для сопряжения нужны и адрес, и код."), Text);
                return;
            }
            DialogResult = DialogResult.OK;
        };
        buttons.Controls.Add(ok);
        buttons.Controls.Add(cancel);
        flow.Controls.Add(buttons);
        Controls.Add(flow);
        AcceptButton = ok; CancelButton = cancel;
        usb.CheckedChanged += delegate { wifiBox.Enabled = wifi.Checked; };
        wifiBox.Enabled = false;
    }

    static void AddRow(TableLayoutPanel grid, int row, string caption, TextBox box) {
        grid.Controls.Add(new Label { Text = caption, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 6, 10, 0) }, 0, row);
        box.Width = 200; box.Margin = new Padding(0, 3, 0, 3);
        grid.Controls.Add(box, 1, row);
    }

    public string Arguments(Func<string, string> quote) {
        if (!wifi.Checked) return "";
        string a = "";
        if (pair.Text.Trim().Length > 0) a += " --pair " + quote(pair.Text.Trim()) + " --code " + quote(code.Text.Trim());
        return a + " --connect " + quote(connect.Text.Trim());
    }
}
