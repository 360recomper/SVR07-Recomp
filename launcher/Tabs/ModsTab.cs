namespace SVR07Launcher.Tabs;

/// <summary>Picks which pack folder under mods\ is passed to the game as --mods_root (one at a time).</summary>
public class ModsTab : ILauncherTab
{
    readonly Panel root = new() { Dock = DockStyle.Fill, BackColor = Theme.Bg };
    readonly DarkList list = new() { Width = Theme.S(560), Height = Theme.S(210) };
    readonly Label info = Ui.Text("");
    List<string> packs = new();

    public string Title => "Mods";
    public Control View => root;

    public ModsTab()
    {
        var col = Ui.Column();
        col.Controls.Add(Ui.Head("Mod packs"));
        col.Controls.Add(Ui.Text("Every folder in mods\\ is a mod pack. Select one and press Use; only one pack is active at a time. " +
            "Choose \"(none)\" to play the plain retail game."));
        col.Controls.Add(list);
        var row = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, BackColor = Theme.Bg };
        row.Controls.Add(Ui.Primary(Ui.Btn("Use selected pack", (_, _) => Use(), 150)));
        row.Controls.Add(Ui.Btn("Open mods folder", (_, _) => Ui.OpenFolder(Paths.Mods)));
        col.Controls.Add(row);
        col.Controls.Add(info);
        root.Controls.Add(col);
    }

    public static List<string> Scan() =>
        Directory.Exists(Paths.Mods)
            ? Directory.GetDirectories(Paths.Mods).Select(Path.GetFileName).Where(n => n != null).Select(n => n!).OrderBy(n => n, StringComparer.OrdinalIgnoreCase).ToList()
            : new();

    /// <summary>Full path of the active pack, or null when none is selected or the folder is gone.</summary>
    public static string? ActivePackPath()
    {
        var name = Config.Load().ActivePack;
        if (name.Length == 0) return null;
        var p = Path.Combine(Paths.Mods, name);
        return Directory.Exists(p) ? p : null;
    }

    public void OnShown()
    {
        packs = Scan();
        var active = Config.Load().ActivePack;
        list.Items.Clear();
        list.Items.Add("(none) - plain retail game");
        foreach (var p in packs) list.Items.Add(p + (p == active ? "   [active]" : ""));
        list.SelectedIndex = Math.Max(0, packs.IndexOf(active) + 1);
        info.Text = packs.Count == 0 ? "No packs found in mods\\ yet." : "";
    }

    void Use()
    {
        var c = Config.Load();
        c.ActivePack = list.SelectedIndex <= 0 ? "" : packs[list.SelectedIndex - 1];
        c.Save();
        OnShown();
        info.Text = c.ActivePack.Length == 0 ? "No pack active." : $"Active pack: {c.ActivePack}.";
    }
}
