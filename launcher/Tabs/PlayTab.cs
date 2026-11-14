using System.Diagnostics;

namespace SVR07Launcher.Tabs;

public class PlayTab : ILauncherTab
{
    readonly Panel root = new() { Dock = DockStyle.Fill, BackColor = Theme.Bg };
    readonly FlatButton play;
    readonly DarkCheck skip = new() { Text = "Skip the launcher next time (hold Shift while starting to bring it back)", AutoSize = true };
    readonly Label info = Ui.Text("");
    bool loading;

    public string Title => "Play";
    public Control View => root;

    public PlayTab()
    {
        var col = Ui.Column();
        col.Controls.Add(Ui.Text($"WWE SmackDown vs. Raw 2007 Recomp   v{AppVersion.Installed}", true));
        play = new FlatButton { Kind = BtnKind.Play, Text = "Play", Width = Theme.S(320), Height = Theme.S(82), Margin = new Padding(-Theme.S(7), Theme.S(6), 0, Theme.S(8)) };
        play.Click += (_, _) => Launch();
        col.Controls.Add(play);
        skip.CheckedChanged += (_, _) =>
        {
            if (loading) return;
            var c = Config.Load(); c.SkipLauncher = skip.Checked; c.Save();
            if (skip.Checked) WriteSettingsShortcut();
        };
        col.Controls.Add(skip);
        col.Controls.Add(info);
        root.Controls.Add(col);
        root.Controls.Add(new PlayBanner());
    }

    public void OnShown()
    {
        loading = true;
        var c = Config.Load();
        skip.Checked = c.SkipLauncher;
        loading = false;
        play.Enabled = Paths.IsInstalled;
        info.Text = Paths.IsInstalled ? PackNote() : "Game files are not installed yet: put svr07.exe next to the launcher and your extracted game files in the assets folder (Setup tab).";
    }

    static string PackNote()
    {
        var p = ModsTab.ActivePackPath();
        var pack = p == null ? "No mod pack selected (Mods tab): the game starts with the plain retail roster." : "Mod pack: " + Path.GetFileName(p);
        var renderer = !NativeRenderer.AvailableIn(Cpu.GameExeForThisCpu()) ? "Renderer: this build has no native renderer (developer build)."
            : "Renderer: Native (D3D12)." + (ShaderCache.Current ? "" : " The shader cache is built from your game files before the first start (a minute or two).");
        return pack + Environment.NewLine + renderer;
    }

    public static bool GameRunning() =>
        Process.GetProcessesByName("svr07").Length > 0 || Process.GetProcessesByName("svr07_v3").Length > 0;

    /// <summary>The run started by the last Launch: Program.Main waits for it once the window is closed and shows
    /// <see cref="RendererError"/> when it exits with 3 or 4 and native-error.txt is there.</summary>
    public static Process? PendingNative { get; private set; }

    /// <summary>A double-clickable way back in when the launcher is skipped.</summary>
    static void WriteSettingsShortcut()
    {
        try { File.WriteAllText(Path.Combine(Paths.Root, "Launcher Settings.bat"), "@echo off\r\nstart \"\" \"%~dp0SVR07 Launcher.exe\" --settings\r\n"); }
        catch { /* read-only folder: Shift still works */ }
    }

    /// <summary>The command line the game needs: data, mods and user-data roots (user_data_root must always be passed),
    /// plus --svr07_renderer=native when the exe has the native renderer (<paramref name="native"/> false = exe without it:
    /// a nonative build has no svr07_renderer cvar, so nothing is passed).</summary>
    public static List<string> GameArgs(bool native = true)
    {
        var a = new List<string> { $"--game_data_root={Paths.Assets}", $"--user_data_root={Paths.UserData}" };
        if (ModsTab.ActivePackPath() is { } m) a.Add($"--mods_root={m}");
        a.Add($"--log_file={Path.Combine(Paths.Logs, "svr07.log")}");
        if (native) a.Add(NativeRenderer.NativeArg);
        return a;
    }

    /// <summary>Starts the exe for this CPU (svr07_v3.exe only with x86-64-v3, Cpu.cs). Native renderer only.</summary>
    public static Process? StartGame()
    {
        var exe = Cpu.GameExeForThisCpu();
        Directory.CreateDirectory(Paths.UserData);
        Directory.CreateDirectory(Paths.Logs);
        NativeRenderer.FixOldRendererSetting();
        NativeRenderer.ClearError();
        bool has = NativeRenderer.AvailableIn(exe);
        var args = GameArgs(has);
        var psi = new ProcessStartInfo(exe) { WorkingDirectory = Paths.Root, UseShellExecute = false };
        foreach (var a in args) psi.ArgumentList.Add(a);
        psi.Environment["SVR07_LAUNCHER"] = "1";  // the game shows no error box of its own: RendererError does
        NativeRenderer.Log($"start {Path.GetFileName(exe)} ({Cpu.Describe()}), renderer native: " + string.Join(' ', args));
        return Process.Start(psi);
    }

    /// <param name="interactive">false = headless (--play): no dialogs, launcher.json never changed by a device failure.</param>
    public static bool Launch(Action<string>? fail = null, bool interactive = true)
    {
        if (!Paths.IsInstalled) { fail?.Invoke("Game files are not installed."); return false; }
        if (GameRunning()) { fail?.Invoke("The game is already running."); return false; }
        try
        {
            while (true)
            {
                ShaderCache.Result? bad = null;
                if (!ShaderCache.ToolsPresent)
                {
                    // Dev / test folder with a cache copied in and no tools: the game checks the cache itself (no cache = error + exit 4).
                    if (!ShaderCache.Present) bad = new ShaderCache.Result(false, "The shader cache tools (svr07_shader_cache.exe, XenosRecomp.exe, svr07_prelink.exe, dxcompiler.dll, dxil.dll) are not in the game folder.");
                    NativeRenderer.Log("shader cache tools missing; cache " + (ShaderCache.Present ? "present, used unchecked" : "missing"));
                }
                else if (interactive)
                {
                    var r = CacheBuildDialog.Run();
                    if (r.Cancelled) { fail?.Invoke("Shader cache build cancelled."); return false; }
                    bad = r.Error;
                }
                else bad = NativeRenderer.PrepareCache();
                if (bad == null) break;
                if (!interactive) { fail?.Invoke(bad.Message); return false; }
                if (RendererError.ShowCacheFailure(bad) != DialogResult.Retry) { fail?.Invoke("Shader cache build failed."); return false; }
                try { ShaderCache.Build(Paths.Assets); } catch { }  // Rebuild: force a fresh build, then loop to verify
            }
            PendingNative = StartGame();
            return true;
        }
        catch (Exception ex) { fail?.Invoke(ex.Message); return false; }
    }

    void Launch()
    {
        if (Launch(m => info.Text = "Could not start the game: " + m)) Application.Exit();
    }
}
