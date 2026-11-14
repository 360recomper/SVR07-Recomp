using System.Text.Json;

namespace SVR07Launcher;

/// <summary>launcher.json next to the exe: launcher-only state (the game's own settings live in svr07.toml).</summary>
public class Config
{
    public bool SkipLauncher { get; set; }
    /// <summary>Name of the mods\ subfolder passed as --mods_root (empty = none).</summary>
    public string ActivePack { get; set; } = "";
    static readonly JsonSerializerOptions Opts = new() { WriteIndented = true };

    public static Config Load()
    {
        try { return JsonSerializer.Deserialize<Config>(File.ReadAllText(Paths.Config)) ?? new(); }
        catch { return new(); }
    }

    public void Save() => File.WriteAllText(Paths.Config, JsonSerializer.Serialize(this, Opts));
}
