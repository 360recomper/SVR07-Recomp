namespace SVR07Launcher;

/// <summary>First-run work: the player's own SvR07 ISO -> assets\ (retail only, no title update).</summary>
public static class Setup
{
    /// <summary>repair = rewrite every file even when the size matches (fixes a modified stock file).</summary>
    public static void ImportIso(string iso, IProgress<(string, double)> progress, CancellationToken ct, bool repair = false)
    {
        using var xgd = new Xgd(iso);
        var xex = xgd.Find("default.xex") ?? throw new InvalidDataException("default.xex not found: this is not a game disc.");
        progress.Report(("Checking game version...", 0));
        if (xgd.TitleId(xex) != Xgd.TitleIdSvr07)
            throw new InvalidDataException("This is not WWE SmackDown vs. Raw 2007 (Xbox 360, title 545107E0).");
        if (xgd.Sha1(xex) != Xgd.XexSha1)
            throw new InvalidDataException(
                "Wrong game version. The recomp is built for the retail release of SmackDown vs. Raw 2007; this disc's default.xex is different.");
        Directory.CreateDirectory(Paths.Assets);
        var marker = Path.Combine(Paths.Root, "import-unfinished.txt");
        File.WriteAllText(marker, "ISO import not finished");
        xgd.ExtractAll(Paths.Assets, progress, ct, repair);
        // a half-copied install must never look finished
        if (!File.Exists(Path.Combine(Paths.Assets, "default.xex"))) throw new InvalidDataException("default.xex missing after copy.");
        File.Delete(marker);
    }
}
