// Saints Reborn Setup
// Builds Saints Reborn on the player's machine from their own Xbox 360 disc image.
// It installs the build tools it needs (Git, Visual Studio 2022 Build Tools with
// the C++ and Clang components, the Visual C++ runtime), downloads the source
// code from GitHub and runs scripts\setup.ps1. No game code is included.
//
// Build: installer\build.ps1 (uses the C# compiler that ships with Windows).

using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Reflection;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("Saints Reborn Setup")]
[assembly: AssemblyProduct("Saints Reborn")]
[assembly: AssemblyCompany("whompay")]
[assembly: AssemblyVersion("1.1.0.0")]
[assembly: AssemblyFileVersion("1.1.0.0")]

namespace SaintsRebornSetup
{
    static class Config
    {
        public const string Version = "1.1.0";
        public const string RepoUrl = "https://github.com/whompay/SaintsReborn.git";
        public const string RepoPage = "https://github.com/whompay/SaintsReborn";
        public const string Branch = "main";
        public const string MinGitUrl =
            "https://github.com/git-for-windows/git/releases/download/v2.47.1.windows.1/MinGit-2.47.1-64-bit.zip";
        public const string BuildToolsUrl = "https://aka.ms/vs/17/release/vs_BuildTools.exe";
        public const string VcRedistUrl = "https://aka.ms/vs/17/release/vc_redist.x64.exe";
        public static readonly string[] VsComponents = {
            "Microsoft.VisualStudio.Workload.VCTools",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "Microsoft.VisualStudio.Component.VC.CMake.Project",
            "Microsoft.VisualStudio.Component.VC.Llvm.Clang",
            "Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset",
            "Microsoft.VisualStudio.Component.Windows11SDK.22621",
        };
        public const long NeededFreeBytes = 25L * 1024 * 1024 * 1024;
    }

    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            try { ServicePointManager.SecurityProtocol = (SecurityProtocolType)3072; } catch { } // TLS 1.2
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            bool update = false;
#if UPDATER
            update = true;
#endif
            foreach (var a in args) if (a.Equals("/update", StringComparison.OrdinalIgnoreCase)) update = true;
            Application.Run(new SetupForm(update));
        }
    }

    class StepFailed : Exception { public StepFailed(string m) : base(m) { } }

    class SetupForm : Form
    {
        TextBox isoBox, dirBox, logBox;
        Button isoBrowse, dirBrowse, installBtn, cancelBtn, playBtn;
        CheckBox desktopShortcut;
        RadioButton installMode, updateMode;
        Label statusLabel, isoLabel, dirLabel;
        ProgressBar progress;
        Thread worker;
        volatile bool cancelled;
        Process current;
        StreamWriter logFile;
        readonly object logLock = new object();
        string gitDir;

        public SetupForm(bool update)
        {
            Text = "Saints Reborn Setup " + Config.Version;
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(720, 568);
            MinimumSize = new Size(620, 460);
            StartPosition = FormStartPosition.CenterScreen;
            Font = new Font("Segoe UI", 9F);
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            var title = new Label {
                Text = "Saints Reborn", Font = new Font("Segoe UI", 16F, FontStyle.Bold),
                AutoSize = true, Location = new Point(16, 12)
            };
            var intro = new Label {
                Text = "Builds the PC version of Saints Row from your own Xbox 360 disc image. " +
                       "Anything missing (Git, the Visual Studio C++ build tools, the Visual C++ runtime) " +
                       "is downloaded and installed automatically. The build takes 30-90 minutes " +
                       "and needs about 25 GB of free space (including the build tools).",
                Location = new Point(18, 48), Size = new Size(684, 48),
                Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right
            };
            installMode = new RadioButton { Text = "New install", AutoSize = true, Location = new Point(18, 100), Checked = !update };
            updateMode = new RadioButton { Text = "Update an existing install (new patches and the mod loader)", AutoSize = true, Location = new Point(130, 100), Checked = update };

            isoLabel = new Label { Text = "Saints Row disc image (.iso):", AutoSize = true, Location = new Point(18, 132) };
            isoBox = new TextBox { Location = new Point(18, 152), Width = 580, Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };
            isoBrowse = new Button { Text = "Browse...", Location = new Point(606, 150), Width = 96, Anchor = AnchorStyles.Top | AnchorStyles.Right };
            isoBrowse.Click += delegate {
                using (var d = new OpenFileDialog { Filter = "Disc image (*.iso)|*.iso|All files (*.*)|*.*", Title = "Select your Saints Row disc image" })
                    if (d.ShowDialog(this) == DialogResult.OK) isoBox.Text = d.FileName;
            };

            dirLabel = new Label { Text = "Install folder (a short path without spaces works best):", AutoSize = true, Location = new Point(18, 184) };
            dirBox = new TextBox { Location = new Point(18, 204), Width = 580, Text = DefaultInstallDir(), Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };
            dirBrowse = new Button { Text = "Browse...", Location = new Point(606, 202), Width = 96, Anchor = AnchorStyles.Top | AnchorStyles.Right };
            dirBrowse.Click += delegate {
                using (var d = new FolderBrowserDialog { Description = "Choose the Saints Reborn folder", ShowNewFolderButton = true })
                    if (d.ShowDialog(this) == DialogResult.OK) dirBox.Text = Updating || IsInstallFolder(d.SelectedPath) ? d.SelectedPath : Path.Combine(d.SelectedPath, "SaintsReborn");
            };

            desktopShortcut = new CheckBox { Text = "Create a desktop shortcut", Checked = true, AutoSize = true, Location = new Point(18, 236) };

            statusLabel = new Label { Text = "Ready.", Location = new Point(18, 266), Size = new Size(684, 20), Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };
            progress = new ProgressBar { Location = new Point(18, 288), Size = new Size(684, 18), Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };

            logBox = new TextBox {
                Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, WordWrap = false,
                Font = new Font("Consolas", 8.5F), BackColor = Color.FromArgb(24, 24, 24), ForeColor = Color.Gainsboro,
                Location = new Point(18, 314), Size = new Size(684, 206),
                Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right
            };

            installBtn = new Button { Text = "Install", Location = new Point(502, 502), Size = new Size(96, 28), Anchor = AnchorStyles.Bottom | AnchorStyles.Right };
            cancelBtn = new Button { Text = "Close", Location = new Point(606, 502), Size = new Size(96, 28), Anchor = AnchorStyles.Bottom | AnchorStyles.Right };
            playBtn = new Button { Text = "Play", Location = new Point(398, 502), Size = new Size(96, 28), Anchor = AnchorStyles.Bottom | AnchorStyles.Right, Visible = false };
            var link = new LinkLabel { Text = "Project page", AutoSize = true, Location = new Point(18, 508), Anchor = AnchorStyles.Bottom | AnchorStyles.Left };
            link.LinkClicked += delegate { OpenUrl(Config.RepoPage); };

            installBtn.Click += delegate { StartInstall(); };
            cancelBtn.Click += delegate { Close(); };
            playBtn.Click += delegate { LaunchGame(); };
            AcceptButton = installBtn;

            installMode.CheckedChanged += delegate { ApplyMode(); };
            updateMode.CheckedChanged += delegate { ApplyMode(); };
            Controls.AddRange(new Control[] { title, intro, installMode, updateMode, isoLabel, isoBox, isoBrowse, dirLabel, dirBox, dirBrowse,
                desktopShortcut, statusLabel, progress, logBox, installBtn, cancelBtn, playBtn, link });
        }

        bool Updating { get { return updateMode.Checked; } }

        void ApplyMode()
        {
            if (Updating)
            {
                Text = "Saints Reborn Updater " + Config.Version;
                isoLabel.Text = "Saints Row disc image (.iso) - only needed if the game files are missing:";
                dirLabel.Text = "Your Saints Reborn folder (the one with setup.bat and the dist folder):";
                installBtn.Text = "Update";
                string known = KnownInstallDir();
                if (known != null) dirBox.Text = known;
            }
            else
            {
                Text = "Saints Reborn Setup " + Config.Version;
                isoLabel.Text = "Saints Row disc image (.iso):";
                dirLabel.Text = "Install folder (a short path without spaces works best):";
                installBtn.Text = "Install";
            }
        }

        protected override void OnShown(EventArgs e) { base.OnShown(e); ApplyMode(); }

        // The folder of an earlier install: remembered by Setup, or the folder Setup runs from.
        static string KnownInstallDir()
        {
            try
            {
                // The old name's key is still read, for installs made before the rename.
                foreach (string key in new[] { @"Software\SaintsReborn", @"Software\SaintsRowPC" })
                    using (var k = Registry.CurrentUser.OpenSubKey(key))
                    {
                        string d = k == null ? null : k.GetValue("InstallDir") as string;
                        if (d != null && IsInstallFolder(d)) return d;
                    }
            }
            catch { }
            string here = Path.GetDirectoryName(Application.ExecutablePath);
            if (IsInstallFolder(here)) return here;
            string parent = Path.GetDirectoryName(here);
            if (parent != null && IsInstallFolder(parent)) return parent;
            return null;
        }

        static bool IsInstallFolder(string d)
        {
            try { return File.Exists(Path.Combine(d, @"scripts\setup.ps1")) || File.Exists(Path.Combine(d, "setup.bat")); }
            catch { return false; }
        }

        static void RememberInstallDir(string dir)
        {
            try { using (var k = Registry.CurrentUser.CreateSubKey(@"Software\SaintsReborn")) k.SetValue("InstallDir", dir); } catch { }
        }

        static string DefaultInstallDir()
        {
            string drive = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.Windows));
            if (string.IsNullOrEmpty(drive)) drive = @"C:\";
            return Path.Combine(drive, @"Games\SaintsReborn");
        }

        // ------------------------------------------------------------------ UI helpers

        void Ui(Action a) { if (IsDisposed) return; if (InvokeRequired) { try { BeginInvoke(a); } catch { } } else a(); }

        void Status(string text) { Log("== " + text + " =="); Ui(delegate { statusLabel.Text = text; }); }

        void Progress(int percent)
        {
            Ui(delegate {
                if (percent < 0) { progress.Style = ProgressBarStyle.Marquee; }
                else { progress.Style = ProgressBarStyle.Continuous; progress.Value = Math.Max(0, Math.Min(100, percent)); }
            });
        }

        void Log(string line)
        {
            lock (logLock) { if (logFile != null) { try { logFile.WriteLine(line); logFile.Flush(); } catch { } } }
            Ui(delegate {
                if (logBox.TextLength > 400000) logBox.Text = logBox.Text.Substring(logBox.TextLength - 200000);
                logBox.AppendText(line + Environment.NewLine);
            });
        }

        void SetRunning(bool running)
        {
            Ui(delegate {
                foreach (Control c in new Control[] { isoBox, dirBox, isoBrowse, dirBrowse, installBtn, desktopShortcut }) c.Enabled = !running;
                cancelBtn.Text = running ? "Cancel" : "Close";
            });
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (worker != null && worker.IsAlive)
            {
                if (MessageBox.Show(this, "Stop the installation? You can run Setup again later and it continues where it stopped.",
                        Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) { e.Cancel = true; return; }
                cancelled = true;
                KillCurrent();
            }
            base.OnFormClosing(e);
        }

        // ------------------------------------------------------------------ install flow

        void StartInstall()
        {
            string iso = isoBox.Text.Trim().Trim('"');
            string dir = dirBox.Text.Trim().Trim('"').TrimEnd('\\');
            if (Updating)
            {
                // Accept the dist or scripts folder too.
                string name = Path.GetFileName(dir), parent = Path.GetDirectoryName(dir);
                if (!IsInstallFolder(dir) && parent != null && IsInstallFolder(parent) &&
                    (name.Equals("dist", StringComparison.OrdinalIgnoreCase) || name.Equals("scripts", StringComparison.OrdinalIgnoreCase)))
                { dir = parent; dirBox.Text = dir; }
                if (!IsInstallFolder(dir))
                { MessageBox.Show(this, "That folder is not a Saints Reborn folder. Choose the folder that contains setup.bat and the dist folder.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
            }
            else if (IsInstallFolder(dir))
            {
                if (MessageBox.Show(this, "Saints Reborn is already in that folder. Update it instead?", Text,
                        MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
                updateMode.Checked = true;
            }
            if (Process.GetProcessesByName("saintsrow").Length > 0 || Process.GetProcessesByName("WhompaysModLoader").Length > 0)
            { MessageBox.Show(this, "Close Saints Reborn and the mod loader first.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
            bool gameExtracted = File.Exists(Path.Combine(dir, @"dist\game\default.xex"));
            if (!gameExtracted && (iso.Length == 0 || !File.Exists(iso)))
            { MessageBox.Show(this, "Select your Saints Row disc image (.iso) first.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
            if (dir.Length < 4 || !Path.IsPathRooted(dir))
            { MessageBox.Show(this, "Choose a full install folder path, for example C:\\Games\\SaintsReborn.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
            if (dir.Length > 60 &&
                MessageBox.Show(this, "The install folder path is long, which can make the build fail. Continue anyway?", Text,
                    MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
            if (!Updating && Directory.Exists(dir) && !IsInstallFolder(dir) && HasOtherFiles(dir))
            { MessageBox.Show(this, "The install folder already contains other files. Choose an empty or new folder.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }

            try
            {
                var free = new DriveInfo(Path.GetPathRoot(dir)).AvailableFreeSpace;
                if (free < Config.NeededFreeBytes && !Directory.Exists(Path.Combine(dir, "build")) &&
                    MessageBox.Show(this, string.Format("Only {0:F0} GB is free on that drive. About 25 GB is needed. Continue anyway?",
                        free / 1073741824.0), Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
            }
            catch { }

            cancelled = false;
            logBox.Clear();
            playBtn.Visible = false;
            SetRunning(true);
            bool shortcut = desktopShortcut.Checked;
            worker = new Thread(delegate() { Run(iso, dir, shortcut); }) { IsBackground = true };
            worker.SetApartmentState(ApartmentState.STA);
            worker.Start();
        }

        bool IsUpdate;

        void Run(string iso, string dir, bool shortcut)
        {
            IsUpdate = IsInstallFolder(dir);
            try
            {
                Directory.CreateDirectory(dir);
                lock (logLock) logFile = new StreamWriter(Path.Combine(dir, "setup-installer.log"), true);
                Log("Saints Reborn Setup " + Config.Version + " - " + DateTime.Now);
                Log("Install folder: " + dir);

                EnsureGit();
                string vs = EnsureBuildTools();
                Log("Visual Studio: " + vs);
                EnsureVcRuntime();
                bool changed = GetSource(dir);
                if (!changed && File.Exists(Path.Combine(dir, @"dist\saintsrow.exe")) && File.Exists(Path.Combine(dir, @"dist\WhompaysModLoader.exe")))
                {
                    DialogResult r = DialogResult.No;
                    Invoke((Action)delegate {
                        r = MessageBox.Show(this, "Saints Reborn is already up to date. Rebuild it anyway?", Text,
                            MessageBoxButtons.YesNo, MessageBoxIcon.Question);
                    });
                    if (r != DialogResult.Yes)
                    {
                        CreateShortcuts(dir, shortcut);
                        RememberInstallDir(dir);
                        Progress(100);
                        Status("Already up to date.");
                        Ui(delegate { playBtn.Visible = true; });
                        return;
                    }
                }
                BuildGame(dir, iso);
                CreateShortcuts(dir, shortcut);
                RememberInstallDir(dir);

                Progress(100);
                Status(changed && IsUpdate ? "Done! Saints Reborn is updated." : "Done! Saints Reborn is installed.");
                Log("Run " + Path.Combine(dir, @"dist\WhompaysModLoader.exe") + " to choose mods and play.");
                Ui(delegate {
                    playBtn.Visible = true;
                    MessageBox.Show(this, "Saints Reborn is ready. Press Play, or use the Saints Reborn shortcut.", Text,
                        MessageBoxButtons.OK, MessageBoxIcon.Information);
                });
            }
            catch (Exception ex)
            {
                Progress(0);
                if (cancelled) { Status("Cancelled."); }
                else
                {
                    Status("Setup failed.");
                    Log("ERROR: " + ex.Message);
                    Ui(delegate {
                        MessageBox.Show(this, ex.Message + "\n\nThe full log is in setup-installer.log in the install folder. " +
                            "Fix the problem and press Install again; finished steps are skipped.", Text,
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
                    });
                }
            }
            finally
            {
                lock (logLock) { if (logFile != null) { logFile.Dispose(); logFile = null; } }
                SetRunning(false);
            }
        }

        static bool HasOtherFiles(string dir)
        {
            foreach (var e in Directory.GetFileSystemEntries(dir))
            {
                string n = Path.GetFileName(e);
                if (!n.Equals("setup-installer.log", StringComparison.OrdinalIgnoreCase) && !n.Equals(".download", StringComparison.OrdinalIgnoreCase)) return true;
            }
            return false;
        }

        void CheckCancel() { if (cancelled) throw new StepFailed("Cancelled."); }

        // ------------------------------------------------------------------ Git

        void EnsureGit()
        {
            Status("Checking Git");
            string found = FindOnPath("git.exe");
            if (found == null)
                foreach (var root in new[] { Environment.GetEnvironmentVariable("ProgramW6432"), Environment.GetEnvironmentVariable("ProgramFiles"),
                                             Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs") })
                {
                    if (string.IsNullOrEmpty(root)) continue;
                    string p = Path.Combine(root, @"Git\cmd\git.exe");
                    if (File.Exists(p)) { found = p; break; }
                }
            string local = Path.Combine(ToolsDir(), "git");
            if (found == null && File.Exists(Path.Combine(local, @"cmd\git.exe"))) found = Path.Combine(local, @"cmd\git.exe");
            if (found == null)
            {
                Status("Downloading Git");
                string zip = Path.Combine(Path.GetTempPath(), "SaintsReborn-MinGit.zip");
                Download(Config.MinGitUrl, zip);
                if (Directory.Exists(local)) Directory.Delete(local, true);
                ZipFile.ExtractToDirectory(zip, local);
                TryDelete(zip);
                found = Path.Combine(local, @"cmd\git.exe");
                if (!File.Exists(found)) throw new StepFailed("Git could not be unpacked.");
            }
            gitDir = Path.GetDirectoryName(found);
            Log("Git: " + found);
        }

        static string ToolsDir()
        {
            string d = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"SaintsReborn\tools");
            Directory.CreateDirectory(d);
            return d;
        }

        // ------------------------------------------------------------------ Visual Studio Build Tools

        static string VsWhere()
        {
            string p = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), @"Microsoft Visual Studio\Installer\vswhere.exe");
            return File.Exists(p) ? p : null;
        }

        static string QueryVs(string products, string extraArgs)
        {
            string vswhere = VsWhere();
            if (vswhere == null) return null;
            string output = RunCapture(vswhere, "-latest -products " + products + " -version \"[17.0,18.0)\" " + extraArgs + " -property installationPath");
            string first = (output ?? "").Trim().Split('\n')[0].Trim();
            return first.Length > 0 && Directory.Exists(first) ? first : null;
        }

        static string FindCompleteVs()
        {
            return QueryVs("*", "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.Llvm.Clang");
        }

        string EnsureBuildTools()
        {
            Status("Checking the Visual Studio C++ build tools");
            string vs = FindCompleteVs();
            if (vs != null) return vs;

            string bootstrapper = Path.Combine(Path.GetTempPath(), "SaintsReborn-vs_BuildTools.exe");
            Status("Downloading the Visual Studio C++ build tools");
            Download(Config.BuildToolsUrl, bootstrapper);

            string adds = "";
            foreach (var c in Config.VsComponents) adds += " --add " + c;
            // Add the missing parts to an existing Build Tools install, otherwise install Build Tools.
            string existing = QueryVs("Microsoft.VisualStudio.Product.BuildTools", "");
            string args = existing != null
                ? "modify --installPath \"" + existing + "\"" + adds + " --includeRecommended --passive --wait --norestart"
                : adds.Trim() + " --includeRecommended --passive --wait --norestart";

            Status("Installing the Visual Studio C++ build tools (this can take 10-30 minutes)");
            Log("A Visual Studio Installer window shows the progress. Windows asks for permission first.");
            Progress(-1);
            int code = RunElevated(bootstrapper, args);
            TryDelete(bootstrapper);
            Log("Visual Studio Installer exit code: " + code);
            CheckCancel();
            if (code == 1602 || code == 1223) throw new StepFailed("The Visual Studio build tools installation was cancelled.");
            if (code == 3010) Log("Windows needs a restart to finish the build tools setup. Setup continues; restart if the build fails.");
            else if (code != 0) throw new StepFailed("Installing the Visual Studio build tools failed (exit code " + code + ").");

            vs = FindCompleteVs();
            if (vs == null)
                throw new StepFailed("The Visual Studio build tools were installed but the C++ or Clang components are missing. " +
                    "Open the Visual Studio Installer, modify Build Tools 2022 and tick 'Desktop development with C++' and " +
                    "'C++ Clang Compiler for Windows'.");
            return vs;
        }

        // ------------------------------------------------------------------ Visual C++ runtime

        void EnsureVcRuntime()
        {
            Status("Checking the Visual C++ runtime");
            if (VcRuntimeInstalled()) { Log("Visual C++ runtime: installed"); return; }
            string exe = Path.Combine(Path.GetTempPath(), "SaintsReborn-vc_redist.x64.exe");
            Status("Downloading the Visual C++ runtime");
            Download(Config.VcRedistUrl, exe);
            Status("Installing the Visual C++ runtime");
            Progress(-1);
            int code = RunElevated(exe, "/install /passive /norestart");
            TryDelete(exe);
            Log("Visual C++ runtime installer exit code: " + code);
            CheckCancel();
            if (code != 0 && code != 3010 && code != 1638 && code != 1641)
                throw new StepFailed("Installing the Visual C++ runtime failed (exit code " + code + ").");
        }

        static bool VcRuntimeInstalled()
        {
            try
            {
                using (var hk = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64))
                using (var k = hk.OpenSubKey(@"SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64"))
                {
                    if (k == null) return false;
                    object installed = k.GetValue("Installed"), major = k.GetValue("Major"), minor = k.GetValue("Minor");
                    return installed is int && (int)installed == 1 && major is int && (int)major >= 14 && minor is int && (int)minor >= 40;
                }
            }
            catch { return false; }
        }

        // ------------------------------------------------------------------ Source code

        // Returns true when the source code changed (new install or new patches).
        bool GetSource(string dir)
        {
            string git = Path.Combine(gitDir, "git.exe");
            if (IsInstallFolder(dir))
            {
                // Existing install: a git clone, or a zip download from GitHub. Bring it to the
                // latest version; the build folder and dist (game files, mod settings) are kept.
                Status("Downloading the latest Saints Reborn patches");
                Progress(-1);
                bool isRepo = Directory.Exists(Path.Combine(dir, ".git"));
                string before = isRepo ? (RunCapture(git, "-C \"" + dir + "\" rev-parse HEAD") ?? "").Trim() : "";
                if (!isRepo && RunLogged(git, "init -q \"" + dir + "\"", dir, null) != 0)
                    throw new StepFailed("Could not prepare the folder for updates (git init failed).");
                int code = RunLogged(git, "-C \"" + dir + "\" fetch --depth 1 " + Config.RepoUrl + " " + Config.Branch, dir, null);
                CheckCancel();
                if (code != 0) throw new StepFailed("Downloading the update failed (git exit code " + code + "). Check your internet connection.");
                RunLogged(git, "-C \"" + dir + "\" config core.autocrlf false", dir, null);
                string after = (RunCapture(git, "-C \"" + dir + "\" rev-parse FETCH_HEAD") ?? "").Trim();
                if (isRepo && after.Length > 0 && after == before) { Log("Source code is already the latest version (" + Short(after) + ")."); return false; }
                code = RunLogged(git, "-C \"" + dir + "\" checkout -f -B " + Config.Branch + " FETCH_HEAD", dir, null);
                if (code != 0) throw new StepFailed("Applying the update failed (git exit code " + code + ").");
                RunLogged(git, "-C \"" + dir + "\" remote remove origin", dir, null);
                RunLogged(git, "-C \"" + dir + "\" remote add origin " + Config.RepoUrl, dir, null);
                Log("Updated " + (before.Length > 0 ? Short(before) : "download") + " -> " + Short(after));
                return true;
            }
            Status("Downloading the Saints Reborn source code");
            Progress(-1);
            string tmp = Path.Combine(dir, ".download");
            if (Directory.Exists(tmp)) ForceDelete(tmp);
            int c = RunLogged(git, "clone -c core.autocrlf=false --depth 1 --branch " + Config.Branch + " --progress " + Config.RepoUrl + " \"" + tmp + "\"", dir, null);
            CheckCancel();
            if (c != 0) throw new StepFailed("Downloading the source code failed (git exit code " + c + "). Check your internet connection.");
            foreach (var entry in Directory.GetFileSystemEntries(tmp))
            {
                string target = Path.Combine(dir, Path.GetFileName(entry));
                if (Directory.Exists(entry)) Directory.Move(entry, target); else File.Move(entry, target);
            }
            ForceDelete(tmp);
            return true;
        }

        static string Short(string sha) { return sha.Length > 7 ? sha.Substring(0, 7) : sha; }

        // ------------------------------------------------------------------ Build

        static readonly Regex NinjaProgress = new Regex(@"^\[(\d+)/(\d+)\]");

        void BuildGame(string dir, string iso)
        {
            Status("Building Saints Reborn (30-90 minutes)");
            Progress(-1);
            string script = Path.Combine(dir, @"scripts\setup.ps1");
            if (!File.Exists(script)) throw new StepFailed("scripts\\setup.ps1 is missing from the source code download.");
            string ps = Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe");
            string args = "-NoProfile -ExecutionPolicy Bypass -File \"" + script + "\" -NoPause";
            if (iso.Length > 0 && File.Exists(iso)) args += " -Iso \"" + iso + "\"";

            string stage = "";
            int code = RunLogged(ps, args, dir, delegate(string line) {
                string t = line.Trim();
                if (t.StartsWith("== ") && t.EndsWith(" ==")) { stage = t.Trim('=', ' '); Ui(delegate { statusLabel.Text = stage; }); Progress(-1); }
                var m = NinjaProgress.Match(t);
                if (m.Success)
                {
                    int done = int.Parse(m.Groups[1].Value), total = int.Parse(m.Groups[2].Value);
                    if (total > 0) { Progress(done * 100 / total); Ui(delegate { statusLabel.Text = stage + string.Format(" ({0}/{1})", done, total); }); }
                }
            });
            CheckCancel();
            if (code != 0) throw new StepFailed("The build failed (exit code " + code + "). See the log above and build\\setup.log in the install folder.");
            if (!File.Exists(Path.Combine(dir, @"dist\saintsrow.exe"))) throw new StepFailed("The build finished but dist\\saintsrow.exe is missing.");
        }

        // ------------------------------------------------------------------ Shortcuts

        void CreateShortcuts(string dir, bool desktop)
        {
            Status("Creating shortcuts");
            string dist = Path.Combine(dir, "dist");
            string target = Path.Combine(dist, "WhompaysModLoader.exe");
            if (!File.Exists(target)) target = Path.Combine(dist, "saintsrow.exe");
            try
            {
                // Shortcuts from before the rename (Saints Reborn) are replaced.
                string oldMenu = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), "Saints Row PC");
                try { if (Directory.Exists(oldMenu)) Directory.Delete(oldMenu, true); } catch { }
                string oldDesktop = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Saints Row PC.lnk");
                if (File.Exists(oldDesktop)) { desktop = true; try { File.Delete(oldDesktop); } catch { } }
                try { File.Delete(Path.Combine(dir, "SaintsRowPC-Setup.exe")); } catch { }
                string menu = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), "Saints Reborn");
                Directory.CreateDirectory(menu);
                MakeShortcut(Path.Combine(menu, "Saints Reborn.lnk"), target, dist, "Play Saints Reborn");
                MakeShortcut(Path.Combine(menu, "Saints Reborn (no mod menu).lnk"), Path.Combine(dist, "saintsrow.exe"), dist, "Play Saints Reborn without the mod loader");
                string setupCopy = Path.Combine(dir, "SaintsReborn-Setup.exe");
                try { if (!string.Equals(Application.ExecutablePath, setupCopy, StringComparison.OrdinalIgnoreCase)) File.Copy(Application.ExecutablePath, setupCopy, true); } catch { }
                if (File.Exists(setupCopy)) MakeShortcut(Path.Combine(menu, "Update Saints Reborn.lnk"), setupCopy, dir, "Get the latest Saints Reborn patches and mods", "/update");
                if (desktop)
                    MakeShortcut(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Saints Reborn.lnk"), target, dist, "Play Saints Reborn");
            }
            catch (Exception ex) { Log("Could not create shortcuts: " + ex.Message); }
        }

        static void MakeShortcut(string lnk, string target, string workDir, string description, string arguments = "")
        {
            Type t = Type.GetTypeFromProgID("WScript.Shell");
            object shell = Activator.CreateInstance(t);
            object sc = t.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { lnk });
            Type st = sc.GetType();
            st.InvokeMember("TargetPath", BindingFlags.SetProperty, null, sc, new object[] { target });
            if (arguments.Length > 0) st.InvokeMember("Arguments", BindingFlags.SetProperty, null, sc, new object[] { arguments });
            st.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, sc, new object[] { workDir });
            st.InvokeMember("Description", BindingFlags.SetProperty, null, sc, new object[] { description });
            st.InvokeMember("IconLocation", BindingFlags.SetProperty, null, sc, new object[] { target + ",0" });
            st.InvokeMember("Save", BindingFlags.InvokeMethod, null, sc, null);
        }

        void LaunchGame()
        {
            string dist = Path.Combine(dirBox.Text.Trim().Trim('"'), "dist");
            string exe = Path.Combine(dist, "WhompaysModLoader.exe");
            if (!File.Exists(exe)) exe = Path.Combine(dist, "saintsrow.exe");
            try { Process.Start(new ProcessStartInfo(exe) { WorkingDirectory = dist, UseShellExecute = true }); Close(); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error); }
        }

        // ------------------------------------------------------------------ process / download helpers

        void Download(string url, string file)
        {
            Log("Downloading " + url);
            for (int attempt = 1; ; attempt++)
            {
                try
                {
                    var req = (HttpWebRequest)WebRequest.Create(url);
                    req.UserAgent = "SaintsReborn-Setup/" + Config.Version;
                    req.AllowAutoRedirect = true;
                    req.Timeout = 60000;
                    req.ReadWriteTimeout = 60000;
                    using (var resp = (HttpWebResponse)req.GetResponse())
                    using (var input = resp.GetResponseStream())
                    using (var output = File.Create(file))
                    {
                        long total = resp.ContentLength, done = 0;
                        var buf = new byte[1 << 16];
                        int n, last = -1;
                        Progress(total > 0 ? 0 : -1);
                        while ((n = input.Read(buf, 0, buf.Length)) > 0)
                        {
                            CheckCancel();
                            output.Write(buf, 0, n);
                            done += n;
                            if (total > 0) { int pct = (int)(done * 100 / total); if (pct != last) { last = pct; Progress(pct); } }
                        }
                        if (total > 0 && done != total) throw new IOException("Download was cut short.");
                    }
                    return;
                }
                catch (StepFailed) { throw; }
                catch (Exception ex)
                {
                    TryDelete(file);
                    if (attempt >= 3) throw new StepFailed("Download failed: " + url + "\n" + ex.Message);
                    Log("Download failed (" + ex.Message + "), retrying...");
                    Thread.Sleep(3000);
                }
            }
        }

        int RunElevated(string exe, string args)
        {
            Log("> " + Path.GetFileName(exe) + " " + args);
            var psi = new ProcessStartInfo(exe, args) { UseShellExecute = true, Verb = "runas" };
            try
            {
                using (var p = Process.Start(psi))
                {
                    current = p;
                    p.WaitForExit();
                    current = null;
                    return p.ExitCode;
                }
            }
            catch (System.ComponentModel.Win32Exception ex)
            {
                if (ex.NativeErrorCode == 1223) throw new StepFailed("Administrator permission is needed to install the build tools.");
                throw;
            }
        }

        int RunLogged(string exe, string args, string workDir, Action<string> onLine)
        {
            Log("> " + Path.GetFileName(exe) + " " + args);
            var psi = new ProcessStartInfo(exe, args) {
                UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = workDir,
                RedirectStandardOutput = true, RedirectStandardError = true,
            };
            if (gitDir != null) psi.EnvironmentVariables["PATH"] = gitDir + ";" + psi.EnvironmentVariables["PATH"];
            psi.EnvironmentVariables["GIT_TERMINAL_PROMPT"] = "0";
            using (var p = new Process { StartInfo = psi })
            {
                DataReceivedEventHandler h = delegate(object s, DataReceivedEventArgs e) {
                    if (e.Data == null) return;
                    Log(e.Data);
                    if (onLine != null) { try { onLine(e.Data); } catch { } }
                };
                p.OutputDataReceived += h;
                p.ErrorDataReceived += h;
                p.Start();
                current = p;
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                p.WaitForExit();
                current = null;
                return p.ExitCode;
            }
        }

        void KillCurrent()
        {
            var p = current;
            if (p == null) return;
            try { Process.Start(new ProcessStartInfo("taskkill", "/T /F /PID " + p.Id) { CreateNoWindow = true, UseShellExecute = false }).WaitForExit(10000); } catch { }
        }

        static string RunCapture(string exe, string args)
        {
            try
            {
                var psi = new ProcessStartInfo(exe, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
                using (var p = Process.Start(psi)) { string o = p.StandardOutput.ReadToEnd(); p.WaitForExit(); return o; }
            }
            catch { return null; }
        }

        static string FindOnPath(string file)
        {
            foreach (var d in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';'))
            {
                try { string p = Path.Combine(d.Trim().Trim('"'), file); if (d.Trim().Length > 0 && File.Exists(p)) return p; } catch { }
            }
            return null;
        }

        static void TryDelete(string f) { try { if (File.Exists(f)) File.Delete(f); } catch { } }

        static void ForceDelete(string dir)
        {
            foreach (var f in Directory.GetFiles(dir, "*", SearchOption.AllDirectories)) { try { File.SetAttributes(f, FileAttributes.Normal); } catch { } }
            Directory.Delete(dir, true);
        }

        static void OpenUrl(string url) { try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); } catch { } }
    }
}
