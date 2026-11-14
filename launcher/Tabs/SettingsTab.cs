namespace SVR07Launcher.Tabs;

/// <summary>Writes svr07.toml. Native renderer only: no renderer, graphics backend, V-Sync or anisotropic rows (emulated-GPU settings).</summary>
public class SettingsTab : ILauncherTab
{
    readonly Panel root = new() { Dock = DockStyle.Fill, BackColor = Theme.Bg };
    readonly DarkCombo scale = Ui.Combo("1 - 720p", "2", "3 - 4K-class", "4", "5", "6 - 8K");
    readonly DarkCheck fullscreen = new DarkCheck { Text = "Fullscreen", AutoSize = true };
    readonly DarkCheck ent60 = new DarkCheck { Text = "Entrances and cutscenes at 60 fps (entrance_60fps)", AutoSize = true };
    readonly DarkCheck arenas = new DarkCheck { Text = "Show extra arenas on Arena Select (extra_arenas)", AutoSize = true };
    readonly DarkCheck roster = new DarkCheck { Text = "Load extra superstars from the mod pack (roster_extras)", AutoSize = true };
    readonly Label saved = Ui.Text("");
    bool loading;

    public string Title => "Settings";
    public Control View => root;

    public SettingsTab()
    {
        var col = Ui.Column();
        void Row(string label, Control c) { col.Controls.Add(Ui.Head(label)); col.Controls.Add(c); }
        Row("Render scale", scale);
        col.Controls.Add(Ui.Text("Higher = sharper but slower. 1 = fastest, for laptops and older GPUs."));
        col.Controls.Add(fullscreen);
        col.Controls.Add(Ui.Head("Game options (restart the game to apply)"));
        col.Controls.Add(ent60);
        col.Controls.Add(arenas);
        col.Controls.Add(roster);
        col.Controls.Add(Ui.Primary(Ui.Btn("Save settings", (_, _) => Save())));
        col.Controls.Add(saved);
        col.Controls.Add(Ui.Text("Graphics backend (Vulkan is the default and recommended), sharpening and save backup are in game too: press F4.\nSettings are stored in svr07.toml next to the launcher."));
        root.Controls.Add(col);
    }

    public void OnShown()
    {
        loading = true;
        var t = new Toml();
        scale.SelectedIndex = Math.Clamp(t.GetInt("resolution_scale", 2), 1, 6) - 1;
        fullscreen.Checked = t.GetBool("fullscreen", false);
        ent60.Checked = t.GetBool("entrance_60fps", true);
        arenas.Checked = t.GetBool("extra_arenas", true);
        roster.Checked = t.GetBool("roster_extras", true);
        saved.Text = "";
        loading = false;
    }

    void Save()
    {
        if (loading) return;
        var t = new Toml();
        t.Set("resolution_scale", scale.SelectedIndex + 1);
        t.Set("fullscreen", fullscreen.Checked);
        t.Set("entrance_60fps", ent60.Checked);
        t.Set("extra_arenas", arenas.Checked);
        t.Set("roster_extras", roster.Checked);
        t.Save();
        saved.Text = "Saved.";
    }
}
