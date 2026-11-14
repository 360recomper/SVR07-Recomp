namespace SVR07Launcher;

/// <summary>All paths are relative to the folder holding the launcher exe (the release layout).</summary>
public static class Paths
{
    public static readonly string Root =
        Path.GetDirectoryName(Environment.ProcessPath) ?? AppContext.BaseDirectory;
    /// <summary>The plain x86-64 build: runs on every CPU (see Cpu.cs).</summary>
    public static string GameExe => Path.Combine(Root, "svr07.exe");
    /// <summary>Optional x86-64-v3 (AVX2) build of the same game; started only on CPUs that have it (Cpu.cs).</summary>
    public static string GameExeV3 => Path.Combine(Root, "svr07_v3.exe");
    /// <summary>--game_data_root: the player's own extracted game files (default.xex, pac\...).</summary>
    public static string Assets => Path.Combine(Root, "assets");
    public static string Toml => Path.Combine(Root, "svr07.toml");
    /// <summary>Each subfolder of mods\ is one pack; the chosen one is passed as --mods_root.</summary>
    public static string Mods => Path.Combine(Root, "mods");
    /// <summary>--user_data_root: saves, shader cache.</summary>
    public static string UserData => Path.Combine(Root, "userdata");
    public static string Backups => Path.Combine(Root, "save-backups");
    public static string Config => Path.Combine(Root, "launcher.json");
    public static string Logs => Path.Combine(Root, "logs");

    public static bool IsInstalled => File.Exists(GameExe) && File.Exists(Path.Combine(Assets, "default.xex"));
}
