using System.Diagnostics;
using System.Text;

namespace SVR07Launcher;

/// <summary>
/// The native D3D12 renderer (SVR07-Recomp-native). Native-only for players (FINDINGS-native-only.md, as UFC): there is
/// no Emulated choice and no silent fallback. When the shader cache can't be built or the game's native device can't
/// start, <see cref="RendererError"/> says what failed and what to do. The launcher still passes --svr07_renderer=native
/// (the game also defaults to it). Ported from the Rumble Roses / UFC launchers' NativeRenderer.cs.
/// </summary>
public static class NativeRenderer
{
    /// <summary>svr07.exe exits with this when the native device can't be created (src/svr07_renderer_fail.h).</summary>
    public const int DeviceFailedExitCode = 3;

    public const string NativeArg = "--svr07_renderer=native";

    // Logged only by builds with the native hooks (src/svr07_app.h LatchRenderer: "svr07 native: renderer on (plume
    // D3D12)"). Scaffolding/census builds say "not implemented"; a nonative build has no svr07_renderer cvar at all,
    // so the launcher must not pass the flag to it.
    static readonly byte[] Marker = Encoding.ASCII.GetBytes("native: renderer on (");

    static readonly Dictionary<string, (string Key, bool Has)> scanned = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>True when this exe has the native renderer compiled in. Scans each exe once per size + date.</summary>
    public static bool AvailableIn(string exe)
    {
        try
        {
            var fi = new FileInfo(exe);
            if (!fi.Exists) return false;
            var key = $"{fi.Length}:{fi.LastWriteTimeUtc.Ticks}";
            lock (scanned)
            {
                if (scanned.TryGetValue(fi.FullName, out var s) && s.Key == key) return s.Has;
                var has = ExeHasMarker(fi.FullName);
                scanned[fi.FullName] = (key, has);
                return has;
            }
        }
        catch { return false; }
    }

    /// <summary>The exe this CPU starts (Cpu.GameExeForThisCpu) has the native renderer.</summary>
    public static bool Available => AvailableIn(Cpu.GameExeForThisCpu());

    public static bool ExeHasMarker(string exe)
    {
        using var f = new FileStream(exe, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 1 << 16);
        var buf = new byte[4 << 20];
        int keep = 0, n;
        while ((n = f.Read(buf, keep, buf.Length - keep)) > 0)
        {
            int len = keep + n;
            if (buf.AsSpan(0, len).IndexOf(Marker) >= 0) return true;
            keep = Math.Min(Marker.Length - 1, len);
            Array.Copy(buf, len - keep, buf, 0, keep);
        }
        return false;
    }

    /// <summary>The game's exit codes for "the renderer can't start" (src/svr07_renderer_fail.h): 3 = native device
    /// could not be created, 4 = no shader cache. Only meaningful together with a fresh native-error.txt (a plain
    /// crash can also exit with 3).</summary>
    public const int CacheMissingExitCode = 4;

    /// <summary>native-error.txt beside svr07.exe: what failed, the GPU and its feature level, written by the game.</summary>
    public static string ErrorFile => Path.Combine(Paths.Root, "native-error.txt");

    /// <summary>Removed before every start so a leftover from an earlier failure is never read as this run's.</summary>
    public static void ClearError()
    {
        try { File.Delete(ErrorFile); } catch { }
    }

    /// <summary>The "key: value" lines of native-error.txt (empty when the file is missing).</summary>
    public static Dictionary<string, string> ReadError()
    {
        var d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            foreach (var line in File.ReadAllLines(ErrorFile))
            {
                int i = line.IndexOf(':');
                if (i > 0) d[line[..i].Trim()] = line[(i + 1)..].Trim();
            }
        }
        catch { }
        return d;
    }

    /// <summary>Makes sure the shader cache is there and current (builds it if not). Null = ready, else the reason it
    /// could not be built (the caller shows <see cref="RendererError"/>). Blocks: call off the UI thread.</summary>
    public static ShaderCache.Result? PrepareCache(IProgress<string>? progress = null, CancellationToken ct = default)
    {
        var r = ShaderCache.EnsurePresent(Paths.Assets, progress, ct);
        return r.Ok ? null : r;
    }

    /// <summary>svr07.toml from an older launcher may hold svr07_renderer = "emulated" (the old Settings choice). The game
    /// ignores it now, but the line is put back to "native" so nobody reads it as a setting. Only that line changes.</summary>
    public static void FixOldRendererSetting()
    {
        try
        {
            if (!File.Exists(Paths.Toml)) return;
            var lines = File.ReadAllLines(Paths.Toml);
            bool changed = false;
            for (int i = 0; i < lines.Length; i++)
            {
                var t = lines[i].TrimStart();
                if (t.StartsWith('#') || !t.StartsWith("svr07_renderer", StringComparison.Ordinal)) continue;
                var rest = t["svr07_renderer".Length..].TrimStart();
                if (!rest.StartsWith('=')) continue;
                if (!rest[1..].Contains("emulated", StringComparison.OrdinalIgnoreCase)) continue;
                lines[i] = "svr07_renderer = \"native\"";
                changed = true;
            }
            if (!changed) return;
            File.WriteAllLines(Paths.Toml, lines);
            Log("svr07.toml: old svr07_renderer = \"emulated\" set back to \"native\"");
        }
        catch (Exception ex) { Log("could not fix svr07_renderer in svr07.toml: " + ex.Message); }
    }

    public static string LogFile => Path.Combine(Paths.Logs, "launcher-native.log");

    internal static void Log(string line)
    {
        try
        {
            Directory.CreateDirectory(Paths.Logs);
            File.AppendAllText(LogFile, $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {line}{Environment.NewLine}");
        }
        catch { }
    }
}
