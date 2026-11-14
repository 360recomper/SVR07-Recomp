namespace SVR07Launcher.Tabs;

public class SetupTab : ILauncherTab
{
    readonly Panel root = new() { Dock = DockStyle.Fill, BackColor = Theme.Bg };
    readonly Label status = Ui.Text("");
    readonly ProgressBar bar = new AccentBar { Width = Theme.S(560), Height = Theme.S(14), Maximum = 1000, Margin = new Padding(0, Theme.S(8), 0, Theme.S(4)) };
    readonly Label current = Ui.Text("");
    readonly FlatButton isoBtn, cancelBtn;
    CancellationTokenSource? cts;

    public string Title => "Setup";
    public Control View => root;

    public SetupTab()
    {
        var col = Ui.Column();
        col.Controls.Add(Ui.Head("Game files"));
        col.Controls.Add(Ui.Text("Choose your own WWE SmackDown vs. Raw 2007 (Xbox 360) disc image (.iso). The launcher checks the title and version, then copies the game files into the assets folder. Only the retail disc is supported (no title update)."));
        isoBtn = Ui.Primary(Ui.Btn("Choose game ISO...", async (_, _) => await RunIso()));
        col.Controls.Add(isoBtn);
        cancelBtn = Ui.Btn("Cancel", (_, _) => cts?.Cancel(), 90);
        cancelBtn.Enabled = false;
        col.Controls.Add(bar);
        col.Controls.Add(current);
        col.Controls.Add(cancelBtn);
        col.Controls.Add(status);
        root.Controls.Add(col);
    }

    /// <summary>--screenshot-all: show the page as during an import.</summary>
    internal void PreviewBusy()
    {
        bar.Value = 420; current.Text = "pac\\wrestlers\\ch0001.pac"; isoBtn.Enabled = false; cancelBtn.Enabled = true;
    }

    public void OnShown() =>
        status.Text = Paths.IsInstalled ? "Game files: installed." : "Game files: not installed yet. Choose your ISO to begin.";

    async Task RunIso()
    {
        using var dlg = new OpenFileDialog { Title = "SmackDown vs. Raw 2007 disc image", Filter = "Disc image (*.iso)|*.iso|All files|*.*" };
        if (dlg.ShowDialog() != DialogResult.OK) return;
        var iso = dlg.FileName;
        bool repair = Paths.IsInstalled;     // re-import over a working install = repair: rewrite every file
        cts = new CancellationTokenSource();
        isoBtn.Enabled = false; cancelBtn.Enabled = true;
        var prog = new Progress<(string name, double frac)>(p =>
        {
            bar.Value = (int)Math.Clamp(p.frac * 1000, 0, 1000);
            current.Text = p.name;
        });
        try
        {
            await Task.Run(() => Setup.ImportIso(iso, prog, cts.Token, repair));
            status.Text = "Game files installed. You can press Play.";
        }
        catch (OperationCanceledException) { status.Text = "Cancelled. Run it again to continue where it stopped."; }
        catch (Exception ex) { status.Text = "Error: " + ex.Message; }
        finally
        {
            isoBtn.Enabled = true; cancelBtn.Enabled = false;
            bar.Value = 0; current.Text = "";
        }
    }
}
