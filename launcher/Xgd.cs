using System.Security.Cryptography;

namespace SVR07Launcher;

/// <summary>Xbox 360 XGD/XDVDFS reader (ported from the Rumble Roses launcher).</summary>
public class Xgd : IDisposable
{
    const int Sector = 2048;
    static readonly byte[] Magic = "MICROSOFT*XBOX*MEDIA"u8.ToArray();
    static readonly long[] Offsets = { 0x0, 0xFB20, 0x20600, 0x2080000, 0xFD90000 };

    /// <summary>SHA-1 of the retail default.xex (no title update) the recomp was built from.</summary>
    public const string XexSha1 = "5745805d807f672c7b1cb89767d7c884ecf76f53";
    /// <summary>Title id of WWE SmackDown vs. Raw 2007 (Xbox 360).</summary>
    public const uint TitleIdSvr07 = 0x545107E0;

    public record Entry(string Path, bool IsDir, long Sector, long Size);

    readonly FileStream fs;
    readonly long gameOffset;
    public List<Entry> Entries { get; } = new();

    public Xgd(string iso)
    {
        fs = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
        try
        {
            gameOffset = FindGameOffset();
            var vd = ReadAt(gameOffset + 32 * Sector, 28);
            long rootSector = BitConverter.ToUInt32(vd, 20), rootSize = BitConverter.ToUInt32(vd, 24);
            if (rootSize < 13 || rootSize > 32 << 20) throw new InvalidDataException("Suspicious root directory size");
            ReadTree(gameOffset + rootSector * Sector, rootSize, "");
        }
        catch { fs.Dispose(); throw; }
    }

    byte[] ReadAt(long off, int len)
    {
        var b = new byte[len];
        if (off < 0 || off + len > fs.Length) throw new InvalidDataException("Read outside the image");
        fs.Seek(off, SeekOrigin.Begin);
        fs.ReadExactly(b);
        return b;
    }

    long FindGameOffset()
    {
        foreach (var o in Offsets)
        {
            long m = o + 32 * Sector;
            if (m + Magic.Length > fs.Length) continue;
            if (ReadAt(m, Magic.Length).AsSpan().SequenceEqual(Magic)) return o;
        }
        throw new InvalidDataException("This is not an Xbox 360 game disc image.");
    }

    void ReadTree(long dirOff, long dirSize, string parent)
    {
        var dir = ReadAt(dirOff, (int)dirSize);
        Walk(dir, 0, parent, new HashSet<int>());
    }

    void Walk(byte[] dir, int ordinal, string parent, HashSet<int> seen)
    {
        int o = ordinal * 4;
        if (o + 14 > dir.Length) throw new InvalidDataException("Directory entry outside table");
        if (!seen.Add(ordinal)) throw new InvalidDataException("Directory tree cycle");
        int l = BitConverter.ToUInt16(dir, o), r = BitConverter.ToUInt16(dir, o + 2);
        long sector = BitConverter.ToUInt32(dir, o + 4), size = BitConverter.ToUInt32(dir, o + 8);
        bool isDir = (dir[o + 12] & 0x10) != 0;
        int nl = dir[o + 13];
        if (o + 14 + nl > dir.Length) throw new InvalidDataException("Directory name outside table");

        if (l != 0) Walk(dir, l, parent, seen);
        var name = System.Text.Encoding.UTF8.GetString(dir, o + 14, nl);
        if (name.Contains("..") || name.Contains('/') || name.Contains('\\') || name.Contains(':'))
            throw new InvalidDataException("Unsafe name in image: " + name);
        var path = parent.Length == 0 ? name : parent + "/" + name;
        Entries.Add(new Entry(path, isDir, sector, size));
        if (isDir && size != 0) ReadTree(gameOffset + sector * Sector, size, path);
        if (r != 0) Walk(dir, r, parent, seen);
    }

    public Entry? Find(string path) =>
        Entries.FirstOrDefault(e => !e.IsDir && e.Path.Equals(path, StringComparison.OrdinalIgnoreCase));

    /// <summary>Title id from the xex execution-info header (optional header key 0x00040006); 0 if absent.</summary>
    public uint TitleId(Entry e)
    {
        var h = ReadAt(gameOffset + e.Sector * Sector, (int)Math.Min(e.Size, 0x1000));
        static uint BE(byte[] b, int o) => (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]);
        if (h.Length < 0x18 || BE(h, 0) != 0x58455832) return 0;   // "XEX2"
        uint count = BE(h, 0x14);
        for (int i = 0; i < count && 0x18 + i * 8 + 8 <= h.Length; i++)
        {
            int o = 0x18 + i * 8;
            if (BE(h, o) != 0x00040006) continue;
            uint at = BE(h, o + 4);
            return at + 16 <= h.Length ? BE(h, (int)at + 12) : 0;
        }
        return 0;
    }

    public string Sha1(Entry e)
    {
        using var sha = SHA1.Create();
        var buf = new byte[1 << 20];
        fs.Seek(gameOffset + e.Sector * Sector, SeekOrigin.Begin);
        long left = e.Size;
        while (left > 0)
        {
            int n = fs.Read(buf, 0, (int)Math.Min(buf.Length, left));
            if (n <= 0) throw new EndOfStreamException("Image is truncated");
            sha.TransformBlock(buf, 0, n, null, 0);
            left -= n;
        }
        sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
        return Convert.ToHexString(sha.Hash!).ToLowerInvariant();
    }

    public void ExtractAll(string outRoot, IProgress<(string, double)> progress, CancellationToken ct, bool force = false)
    {
        var files = Entries.Where(e => !e.IsDir).ToList();
        long total = files.Sum(f => f.Size), done = 0;
        var root = Path.GetFullPath(outRoot);
        var buf = new byte[8 << 20];
        foreach (var e in files)
        {
            ct.ThrowIfCancellationRequested();
            var dest = Path.GetFullPath(Path.Combine(root, e.Path.Replace('/', '\\')));
            if (!dest.StartsWith(root + Path.DirectorySeparatorChar)) throw new InvalidDataException("Path escapes output: " + e.Path);
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            if (!force && File.Exists(dest) && new FileInfo(dest).Length == e.Size) { done += e.Size; continue; }
            var tmp = dest + ".part";
            using (var o = new FileStream(tmp, FileMode.Create, FileAccess.Write))
            {
                fs.Seek(gameOffset + e.Sector * Sector, SeekOrigin.Begin);
                long left = e.Size;
                while (left > 0)
                {
                    ct.ThrowIfCancellationRequested();
                    int n = fs.Read(buf, 0, (int)Math.Min(buf.Length, left));
                    if (n <= 0) throw new EndOfStreamException("Image is truncated");
                    o.Write(buf, 0, n);
                    left -= n; done += n;
                    progress.Report((e.Path, total == 0 ? 1 : (double)done / total));
                }
            }
            File.Move(tmp, dest, true);
        }
    }

    public void Dispose() => fs.Dispose();
}
