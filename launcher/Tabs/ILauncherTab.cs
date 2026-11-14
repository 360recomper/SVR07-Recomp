namespace SVR07Launcher.Tabs;

/// <summary>A launcher tab. To add one, implement this and add it to the list in MainForm.</summary>
public interface ILauncherTab
{
    string Title { get; }
    Control View { get; }
    /// <summary>Called each time the tab becomes visible: re-read disk state here.</summary>
    void OnShown();
}
