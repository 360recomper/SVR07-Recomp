namespace SVR07Launcher.Tabs;

/// <summary>
/// Attire picker: writes the attire_picks cvar ("183:2=lesnar_hctp, 6:3=jericho68", see svr07_attires.cpp) to svr07.toml.
/// A look is a folder mods\&lt;pack&gt;\attire\&lt;look&gt;\ with ch.pac (+ front.jpg / back.jpg previews). The game copies
/// that look into the superstar's attire slot at startup.
/// </summary>
public class AttireTab : ILauncherTab
{
    readonly Panel root = new() { Dock = DockStyle.Fill, BackColor = Theme.Bg };
    readonly DarkList picks = new() { Width = Theme.S(420), Height = Theme.S(120) };
    readonly NumericUpDown id = new() { Minimum = 0, Maximum = 999, Width = Theme.S(70), BackColor = Theme.Surface, ForeColor = Theme.Ink, BorderStyle = BorderStyle.FixedSingle };
    readonly DarkCombo attire = Ui.Combo("1", "2", "3", "4");
    readonly DarkList looks = new() { Width = Theme.S(220), Height = Theme.S(190) };
    readonly PictureBox front = new() { Width = Theme.S(140), Height = Theme.S(190), SizeMode = PictureBoxSizeMode.Zoom, BorderStyle = BorderStyle.FixedSingle, BackColor = Theme.Surface, Margin = new Padding(Theme.S(10), 0, 0, 0) };
    readonly PictureBox back = new() { Width = Theme.S(140), Height = Theme.S(190), SizeMode = PictureBoxSizeMode.Zoom, BorderStyle = BorderStyle.FixedSingle, BackColor = Theme.Surface, Margin = new Padding(Theme.S(10), 0, 0, 0) };
    readonly Label info = Ui.Text("");
    readonly List<(int Id, int Attire, string Look)> list = new();
    List<string> lookNames = new();

    public string Title => "Attire";
    public Control View => root;

    public AttireTab()
    {
        var col = Ui.Column();
        col.Controls.Add(Ui.Text("Pick an alternate look for a superstar's attire slot. Superstar id (see the pack's roster list), attire 1-4, then a look from the pack's attire folder.", false));
        var add = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, Margin = new Padding(0), BackColor = Theme.Bg };
        add.Controls.Add(new DimLabel { Text = "Superstar id", AutoSize = true, Margin = new Padding(0, Theme.S(10), Theme.S(6), 0), ForeColor = Theme.Ink });
        add.Controls.Add(id);
        add.Controls.Add(new DimLabel { Text = "Attire", AutoSize = true, Margin = new Padding(Theme.S(14), Theme.S(10), Theme.S(6), 0), ForeColor = Theme.Ink });
        attire.Width = Theme.S(60);
        add.Controls.Add(attire);
        add.Controls.Add(Ui.AutoBtn("Set pick", (_, _) => SetPick()));
        col.Controls.Add(add);
        var mid = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, WrapContents = false, Margin = new Padding(0), BackColor = Theme.Bg };
        mid.Controls.AddRange(new Control[] { looks, front, back });
        col.Controls.Add(Ui.Head("Looks available"));
        col.Controls.Add(mid);
        col.Controls.Add(Ui.Head("Your picks"));
        col.Controls.Add(picks);
        var row = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, BackColor = Theme.Bg };
        row.Controls.Add(Ui.Btn("Remove selected", (_, _) => Remove(), 130));
        row.Controls.Add(Ui.Primary(Ui.Btn("Apply", (_, _) => Apply(), 90)));
        row.Controls.Add(Ui.Btn("Open attire folder", (_, _) => Ui.OpenFolder(AttireDir() ?? Path.Combine(Paths.Mods, "attire")), 130));
        col.Controls.Add(row);
        col.Controls.Add(info);
        looks.SelectedIndexChanged += (_, _) => Preview();
        root.Controls.Add(col);
    }

    static string? AttireDir() => ModsTab.ActivePackPath() is { } p ? Path.Combine(p, "attire") : null;

    public void OnShown()
    {
        lookNames = AttireDir() is { } d && Directory.Exists(d)
            ? Directory.GetDirectories(d).Where(x => File.Exists(Path.Combine(x, "ch.pac"))).Select(x => Path.GetFileName(x)!).OrderBy(n => n).ToList()
            : new();
        looks.Items.Clear();
        foreach (var n in lookNames) looks.Items.Add(n);
        if (lookNames.Count > 0) looks.SelectedIndex = 0;
        list.Clear();
        foreach (var part in (new Toml().GetString("attire_picks") ?? "").Replace(';', ',').Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var eq = part.IndexOf('=');
            var left = eq > 0 ? part[..eq].Split(':') : Array.Empty<string>();
            if (left.Length == 2 && int.TryParse(left[0], out var i) && int.TryParse(left[1], out var a)) list.Add((i, a, part[(eq + 1)..].Trim()));
        }
        Refresh();
        info.Text = ModsTab.ActivePackPath() == null ? "No mod pack active (Mods tab)." : lookNames.Count == 0 ? "This pack has no attire\\<look>\\ch.pac folders yet." : "";
    }

    void Refresh()
    {
        picks.Items.Clear();
        foreach (var p in list) picks.Items.Add($"superstar {p.Id}, attire {p.Attire}  =  {p.Look}");
    }

    void Preview()
    {
        front.Image = back.Image = null;
        if (looks.SelectedIndex < 0 || AttireDir() is not { } d) return;
        Image? Load(string f)
        {
            var p = Path.Combine(d, lookNames[looks.SelectedIndex], f);
            if (!File.Exists(p)) return null;
            try { using var fs = File.OpenRead(p); using var t = Image.FromStream(fs); return new Bitmap(t); } catch { return null; }
        }
        front.Image = Load("front.jpg");
        back.Image = Load("back.jpg");
    }

    void SetPick()
    {
        if (looks.SelectedIndex < 0) { info.Text = "Select a look first."; return; }
        int i = (int)id.Value, a = attire.SelectedIndex + 1;
        list.RemoveAll(p => p.Id == i && p.Attire == a);
        list.Add((i, a, lookNames[looks.SelectedIndex]));
        Refresh();
        info.Text = "Press Apply to save.";
    }

    void Remove()
    {
        if (picks.SelectedIndex < 0) return;
        list.RemoveAt(picks.SelectedIndex);
        Refresh();
        info.Text = "Press Apply to save.";
    }

    void Apply()
    {
        var t = new Toml();
        t.SetString("attire_picks", string.Join(", ", list.Select(p => $"{p.Id}:{p.Attire}={p.Look}")));
        t.Save();
        info.Text = $"Saved {list.Count} pick(s). Restart the game to use them.";
    }
}
