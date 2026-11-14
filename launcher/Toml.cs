namespace SVR07Launcher;

/// <summary>
/// Minimal key = value editor for svr07.toml. Only touches the keys it is asked to set and keeps every other
/// line (gpu_backend belongs to the F4 menu; the launcher never writes it).
/// </summary>
public class Toml
{
    readonly List<string> lines;
    public Toml() => lines = File.Exists(Paths.Toml) ? File.ReadAllLines(Paths.Toml).ToList() : new();

    int Find(string key) =>
        lines.FindIndex(l =>
        {
            var t = l.TrimStart();
            return !t.StartsWith('#') && t.StartsWith(key) && t[key.Length..].TrimStart().StartsWith('=');
        });

    public string? Get(string key)
    {
        int i = Find(key);
        if (i < 0) return null;
        var v = lines[i][(lines[i].IndexOf('=') + 1)..].Trim();
        int hash = v.IndexOf(" #", StringComparison.Ordinal);
        if (hash >= 0 && !v.StartsWith('"')) v = v[..hash].Trim();
        return v.Trim('"');
    }

    public int GetInt(string key, int def) => int.TryParse(Get(key), out var v) ? v : def;
    public bool GetBool(string key, bool def) => Get(key) is { } s ? s == "true" : def;

    public void Set(string key, string rawValue)
    {
        int i = Find(key);
        var line = $"{key} = {rawValue}";
        if (i >= 0) lines[i] = line; else lines.Add(line);
    }
    public void Set(string key, int v) => Set(key, v.ToString());
    public void Set(string key, bool v) => Set(key, v ? "true" : "false");
    /// <summary>Writes a string value with TOML quoting (backslashes escaped).</summary>
    public void SetString(string key, string v) => Set(key, "\"" + v.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"");
    public string? GetString(string key) => Get(key)?.Replace("\\\\", "\\");

    public void Save() => File.WriteAllLines(Paths.Toml, lines);
}
