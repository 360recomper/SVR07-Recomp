using System.Runtime.Intrinsics.X86;

namespace SVR07Launcher;

/// <summary>
/// Picks the game exe for this CPU (user decision 2026-11-12, memory svr07-avx2-release). A release ships two builds of
/// the same game: svr07.exe = the plain x86-64 build (build-native.bat hooks) and svr07_v3.exe = the x86-64-v3 build
/// (build-native-v3.bat: AVX2, BMI1/2, FMA, LZCNT, MOVBE, F16C; ~5% less CPU per frame). The v3 exe crashes at once on
/// CPUs without those instructions (Intel before Haswell, many Pentium/Celeron, AMD before Excavator) or on an OS that
/// does not save AVX state, so it is started only when all of them are there. svr07.exe is always the fallback, and a
/// folder without svr07_v3.exe simply runs svr07.exe. Never ship svr07_v3.exe alone. Template: UFC launcher Cpu.cs
/// (there the names are the other way round).
/// </summary>
public static class Cpu
{
    /// <summary>All x86-64-v3 features. Avx.IsSupported / Avx2.IsSupported are false when the OS has not enabled the
    /// AVX register state (OSXSAVE + XCR0), so "OS supports it" is covered too.</summary>
    public static bool SupportsX64V3
    {
        get
        {
            try
            {
                // Test override: behave like a CPU without x86-64-v3 (checks the svr07.exe path on any PC).
                if (Environment.GetEnvironmentVariable("SVR07_FORCE_COMPAT") == "1") return false;
                if (!(Avx.IsSupported && Avx2.IsSupported && Bmi1.IsSupported && Bmi2.IsSupported && Fma.IsSupported &&
                      Lzcnt.IsSupported && X86Base.IsSupported))
                    return false;
                var (_, _, ecx, _) = X86Base.CpuId(1, 0);
                const int Movbe = 1 << 22, F16c = 1 << 29;
                return (ecx & Movbe) != 0 && (ecx & F16c) != 0;
            }
            catch { return false; }
        }
    }

    /// <summary>The exe to start: svr07_v3.exe when it exists and this CPU has x86-64-v3, else svr07.exe.</summary>
    public static string GameExeForThisCpu() =>
        File.Exists(Paths.GameExeV3) && SupportsX64V3 ? Paths.GameExeV3 : Paths.GameExe;

    /// <summary>One line for the launcher log.</summary>
    public static string Describe() =>
        $"cpu x86-64-v3 {(SupportsX64V3 ? "yes" : "no")}, v3 exe {(File.Exists(Paths.GameExeV3) ? "present" : "absent")} -> {Path.GetFileName(GameExeForThisCpu())}";
}
