using System.Diagnostics;

namespace SVR07Launcher;

/// <summary>The one error dialog for "the renderer can't start" (native-only, FINDINGS-native-only.md; ported from the UFC
/// launcher). It replaces the old silent switches to the emulated renderer: it says what failed (shader cache build: the
/// message and the log; game: what, GPU and Direct3D 12 feature level from native-error.txt) and what to do. "Rebuild
/// shader cache" returns <see cref="DialogResult.Retry"/>.</summary>
public static class RendererError
{
    public const string Caption = "SVR07 Launcher - the game can't start";

    public static string ReportUrl => "https://github.com/360recomper/SVR07-Recomp/issues/new";

    public static string CacheText(ShaderCache.Result r) =>
        "The shader cache could not be built, so the game was not started.\n\n" +
        "What failed: " + r.Message + "\n" +
        "Log: " + NativeRenderer.LogFile + "\n\n" +
        "What to do:\n" +
        "  1. Press Rebuild shader cache to try again.\n" +
        "  2. If it fails again: check the game files (Setup tab), that the game folder can be written to " +
        "(not inside Program Files), and that antivirus didn't remove XenosRecomp.exe or svr07_prelink.exe.\n" +
        "  3. Still failing? Report it with the log file.";

    public static string GameText(int code, IReadOnlyDictionary<string, string> e)
    {
        string what = e.GetValueOrDefault("what") ?? (code == NativeRenderer.CacheMissingExitCode
            ? "the shader cache is missing" : "the Direct3D 12 device could not be created");
        string gpu = e.GetValueOrDefault("gpu") ?? "unknown";
        string level = e.GetValueOrDefault("feature_level") ?? "unknown";
        return "The game's renderer could not start, so the game closed.\n\n" +
            "What failed: " + what + $" (exit code {code})\n" +
            "Graphics card: " + gpu + "\n" +
            "Direct3D 12 feature level: " + level + " (12_0 or newer is needed)\n" +
            "Details: " + NativeRenderer.ErrorFile + "\n\n" +
            "What to do:\n" +
            "  1. Update your graphics driver (Update GPU driver opens your card maker's page), then press Play again.\n" +
            "  2. Laptop with two graphics cards: add svr07_native_adapter = \"part of the card's name\" to svr07.toml.\n" +
            "  3. Rebuild shader cache rebuilds it and starts the game again.\n" +
            "  4. Still failing? Report it with native-error.txt and the log.";
    }

    public static DialogResult ShowCacheFailure(ShaderCache.Result r)
    {
        NativeRenderer.Log("error dialog: shader cache build failed: " + r.Message);
        return Show(CacheText(r), null, NativeRenderer.LogFile);
    }

    /// <summary>svr07.exe exited with 3 or 4 and wrote native-error.txt.</summary>
    public static DialogResult ShowGameFailure(int code)
    {
        var e = NativeRenderer.ReadError();
        return Show(GameText(code, e), e.GetValueOrDefault("gpu"),
            File.Exists(NativeRenderer.ErrorFile) ? NativeRenderer.ErrorFile : NativeRenderer.LogFile);
    }

    static DialogResult Show(string text, string? gpu, string logFile)
    {
        // Headless tests: SVR07_TEST_NO_DIALOG=1 logs the text and counts as Close.
        if (Environment.GetEnvironmentVariable("SVR07_TEST_NO_DIALOG") == "1")
        {
            NativeRenderer.Log("error dialog (test, not shown): " + text.Replace("\n", " | "));
            return DialogResult.Cancel;
        }
        return Tabs.DarkBox.Show(text, Caption, MessageBoxIcon.Error, Buttons(gpu, logFile));
    }

    public static Tabs.DarkBox.BoxButton[] Buttons(string? gpu, string logFile) => new[]
    {
        new Tabs.DarkBox.BoxButton("Rebuild shader cache", DialogResult.Retry),
        new Tabs.DarkBox.BoxButton("Update GPU driver", DialogResult.None, () => Open(DriverUrl(gpu))),
        new Tabs.DarkBox.BoxButton("Report a problem", DialogResult.None, () => Open(ReportUrl)),
        new Tabs.DarkBox.BoxButton("Show log", DialogResult.None, () => ShowFile(logFile)),
        new Tabs.DarkBox.BoxButton("Close", DialogResult.Cancel),
    };

    /// <summary>The card maker's driver page; Windows Update when the maker isn't known.</summary>
    public static string DriverUrl(string? gpu)
    {
        var g = gpu ?? "";
        if (g.Contains("NVIDIA", StringComparison.OrdinalIgnoreCase) || g.Contains("GeForce", StringComparison.OrdinalIgnoreCase))
            return "https://www.nvidia.com/en-us/drivers/";
        if (g.Contains("AMD", StringComparison.OrdinalIgnoreCase) || g.Contains("Radeon", StringComparison.OrdinalIgnoreCase))
            return "https://www.amd.com/en/support/download/drivers.html";
        if (g.Contains("Intel", StringComparison.OrdinalIgnoreCase))
            return "https://www.intel.com/content/www/us/en/support/detect.html";
        return "ms-settings:windowsupdate";
    }

    static void Open(string url) => Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });

    static void ShowFile(string path)
    {
        if (File.Exists(path)) Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{path}\"") { UseShellExecute = true });
        else Tabs.Ui.OpenFolder(Paths.Root);
    }
}
