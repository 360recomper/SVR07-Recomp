using System.Runtime.InteropServices;
using SVR07Launcher.Tabs;

namespace SVR07Launcher;

static class Program
{
    [DllImport("user32.dll")] static extern short GetAsyncKeyState(int vKey);

    [STAThread]
    static void Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        // --screenshot-all <dir> [100|150]: PNG of every tab and dialog at that display scale, never starts the game (UI checks).
        int si = Array.IndexOf(args, "--screenshot-all");
        int? simPct = si >= 0 && si + 2 < args.Length && int.TryParse(args[si + 2], out var sp) ? sp : null;
        Theme.Init(simPct);
        if (si >= 0 && si + 1 < args.Length) { Environment.Exit(Shots.Run(args[si + 1], simPct ?? 100)); return; }
        NativeRenderer.FixOldRendererSetting();
        // Headless (tests, test farm): --shader-cache [--force] builds the cache (exit 0 ok / 1 failed);
        // --play starts the game like the Play button with no dialogs and waits (exit = the game's exit code, 2 = not started).
        if (args.Contains("--shader-cache"))
        {
            var r = args.Contains("--force") ? ShaderCache.Build(Paths.Assets) : ShaderCache.EnsurePresent(Paths.Assets);
            Environment.Exit(r.Ok ? 0 : 1);
        }
        if (args.Contains("--play"))
        {
            if (!PlayTab.Launch(m => NativeRenderer.Log("--play: could not start: " + m), interactive: false)) Environment.Exit(2);
            Environment.Exit(WaitForNative(headless: true));
        }
        bool shift = (GetAsyncKeyState(0x10) & 0x8000) != 0;
        bool forceUi = shift || args.Contains("--settings");
        if (!forceUi && Paths.IsInstalled && Config.Load().SkipLauncher)
        {
            if (PlayTab.Launch(m => DarkBox.Show("Could not start the game: " + m, "SVR07 Launcher", MessageBoxButtons.OK, MessageBoxIcon.Error)))
            {
                WaitForNative();
                return;
            }
        }
        int ti = Array.IndexOf(args, "--tab");
        Application.Run(new MainForm(args.Contains("--settings"), ti >= 0 && ti + 1 < args.Length ? args[ti + 1] : null));
        // Play closes the window; a native start keeps this process (no window) until the game exits, so a device
        // failure (exit 0x53570003) restarts it on Emulated.
        WaitForNative();
    }

    /// <summary>Waits for the game Play started (if any). Exit code 3 / 4 with a fresh native-error.txt = the renderer
    /// could not start: <see cref="RendererError"/> says why. "Rebuild shader cache" rebuilds and starts again.
    /// headless (--play): no dialog, the code is returned. Returns the exit code of the last game process.</summary>
    static int WaitForNative(bool headless = false)
    {
        while (PlayTab.PendingNative is { } game)
        {
            int code;
            try { game.WaitForExit(); code = game.ExitCode; } catch { return 0; }
            NativeRenderer.Log($"game exited with 0x{code:X8}");
            if ((code != NativeRenderer.DeviceFailedExitCode && code != NativeRenderer.CacheMissingExitCode) || !File.Exists(NativeRenderer.ErrorFile))
                return code;
            if (headless) { NativeRenderer.Log("renderer error (see native-error.txt)"); return code; }
            if (RendererError.ShowGameFailure(code) != DialogResult.Retry) return code;
            NativeRenderer.Log("Rebuild shader cache requested from the error dialog");
            var r = ShaderCache.Build(Paths.Assets);
            if (!r.Ok && RendererError.ShowCacheFailure(r) != DialogResult.Retry) return code;
            if (!PlayTab.Launch(m => DarkBox.Show("Could not start the game: " + m, "SVR07 Launcher", MessageBoxButtons.OK, MessageBoxIcon.Error))) return code;
        }
        return 0;
    }
}

class MainForm : Form
{
    readonly List<ILauncherTab> tabs = new();
    readonly NavHost tc = new();
    readonly bool preview;

    /// <summary>--screenshot-all: never take the focus from the user.</summary>
    protected override bool ShowWithoutActivation => preview;

    internal NavHost Nav => tc;
    internal IReadOnlyList<ILauncherTab> Tabs => tabs;

    /// <param name="startTab">--tab &lt;title&gt; opens that tab (e.g. "Attire").</param>
    /// <param name="preview">--screenshot-all: nothing is shown or started.</param>
    public MainForm(bool startOnSettings, string? startTab = null, bool preview = false)
    {
        this.preview = preview;
        Text = "WWE SmackDown vs. Raw 2007 Recomp - Launcher " + AppVersion.Launcher;
        AutoScaleMode = AutoScaleMode.None;   // sizes are scaled by Theme.S
        Theme.DarkWindow(this);
        try { using var s = typeof(MainForm).Assembly.GetManifestResourceStream("Art.app.ico"); if (s != null) Icon = new Icon(s); } catch { }
        ClientSize = new Size(Theme.S(940), Theme.S(680));
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = SizeFromClientSize(new Size(Theme.S(780), Theme.S(540)));

        var play = new PlayTab();
        // Add new tabs here.
        tabs.AddRange(new ILauncherTab[] { play, new SettingsTab(), new ModsTab(), new AttireTab(), new SavesTab(), new SetupTab() });
        foreach (var t in tabs) tc.Add(t.Title, Glyph(t.Title), t.View, t is not PlayTab);
        tc.SelectedIndexChanged += (_, _) => tabs[tc.SelectedIndex].OnShown();
        Controls.Add(tc);
        Theme.DarkScroll(this);

        Shown += (_, _) =>
        {
            if (preview) return;
            if (startOnSettings) tc.SelectedIndex = 1;
            else if (tabs.FindIndex(t => t.Title.Equals(startTab, StringComparison.OrdinalIgnoreCase)) is >= 0 and var i) tc.SelectedIndex = i;
            else tc.SelectedIndex = 0;   // raises OnShown
        };
    }

    /// <summary>Sidebar icons (Segoe Fluent Icons / MDL2 Assets, both ship with Windows).</summary>
    static string Glyph(string title) => title switch
    {
        "Play" => "",
        "Settings" => "",
        "Mods" => "",
        "Attire" => "",
        "Saves" => "",
        "Setup" => "",
        _ => "",
    };

    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        if (keyData == (Keys.Control | Keys.Tab)) { tc.Next(1); return true; }
        if (keyData == (Keys.Control | Keys.Shift | Keys.Tab)) { tc.Next(-1); return true; }
        return base.ProcessCmdKey(ref msg, keyData);
    }
}
