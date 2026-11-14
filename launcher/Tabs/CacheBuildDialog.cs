namespace SVR07Launcher.Tabs;

/// <summary>Modal "building the shader cache" window shown before a native start when the cache is missing or stale
/// (ShaderCache.StaleReason). Closes by itself when the cache is current; Cancel stops the tool and the game is not started.</summary>
public class CacheBuildDialog : Form
{
    public sealed record Outcome(bool Cancelled, ShaderCache.Result? Error);

    readonly Label line = new DimLabel() { AutoSize = false, Width = Theme.S(440), Height = Theme.S(36), ForeColor = Theme.Muted, Text = "Starting..." };
    readonly CancellationTokenSource cts = new();
    Outcome outcome = new(true, null);

    CacheBuildDialog()
    {
        Text = "SVR07 - building the shader cache";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = MinimizeBox = false;
        ControlBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        AutoScaleMode = AutoScaleMode.None;
        Theme.DarkWindow(this);
        ClientSize = new Size(Theme.S(480), Theme.S(190));
        var col = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, Padding = new Padding(Theme.S(18)), WrapContents = false, BackColor = Theme.Bg };
        col.Controls.Add(new DimLabel
        {
            AutoSize = true, MaximumSize = new Size(Theme.S(440), 0), ForeColor = Theme.Ink, Margin = new Padding(0, 0, 0, Theme.S(8)),
            Text = "The native renderer needs its shader cache, made once from your own game files (and again after an update or an ISO repair).",
        });
        col.Controls.Add(new AccentBar { Style = ProgressBarStyle.Marquee, Width = Theme.S(440), Height = Theme.S(14), MarqueeAnimationSpeed = 30 });
        col.Controls.Add(line);
        var cancel = new FlatButton { Text = "Cancel", Width = Theme.S(96), Height = Theme.S(34) };
        cancel.Click += (_, _) => { cancel.Enabled = false; line.Text = "Stopping..."; cts.Cancel(); };
        col.Controls.Add(cancel);
        Controls.Add(col);
        Shown += async (_, _) =>
        {
            var progress = new Progress<string>(s => line.Text = s.Length > 120 ? s[..120] : s);
            try { outcome = new(false, await Task.Run(() => NativeRenderer.PrepareCache(progress, cts.Token))); }
            catch (OperationCanceledException) { outcome = new(true, null); }
            catch (Exception ex) { outcome = new(false, new ShaderCache.Result(false, ex.Message)); }
            Close();
        };
    }

    /// <summary>Returns at once when the cache is current; else shows the window until the build ends.</summary>
    public static Outcome Run()
    {
        if (ShaderCache.Current) return new(false, null);
        using var d = new CacheBuildDialog();
        d.ShowDialog();
        return d.outcome;
    }
}
