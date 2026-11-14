"""Fetch the SVR07 native renderer's third-party sources at their pins and apply our patches (RR-NR-12).

    python -I native/tools/svr07_fetch_third_party.py [--dest DIR] [--from-dir DIR] [--check]

Port of UFC-native tools/nr_fetch_third_party.py. Clones into native/third_party/ (default; not committed,
see native/.gitignore) and applies native/patches/ in order:
  plume        zolaware/plume e0c8871: plume.patch, plume-gpu-pick.patch, plume-texture-pool.patch (UFC),
               plume-rr-pso-diag.patch (RR-NR-6 PSO failure print)
  XenosRecomp  zolaware/reblue-XenosRecomp 339af41 (+ submodules: dxc-bin, fmt, smol-v, xxHash, zstd):
               xenosrecomp.patch (SvR 2010), xenosrecomp-ufc.patch .. -ufc6.patch (UFC), xenosrecomp-0e.patch (RR)
The small vendored parts (reblue_thirdparty zstd/miniz, fidelityfx headers, licenses) are committed in
native/third_party/ as UFC commits them.

Who reads it:
  game build   cmake/native_renderer.cmake (SVR07_THIRD_PARTY, SVR07_XENOS_DIR default to native/third_party)
  shader tools native/tools/build-shader-tools.bat (XenosRecomp.exe + svr07_shader_cache.exe)

A folder that already holds the right commit is reset and re-patched. --from-dir copies plume/ and
XenosRecomp/ from a local folder (e.g. UFC-native/third_party) instead of cloning (submodules come along).
--check only reports whether each folder is at its pin with exactly our patches.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

NATIVE = Path(__file__).resolve().parent.parent
REPOS = [
    # (folder, url, commit, patches in order)
    ("plume", "https://github.com/zolaware/plume.git", "e0c8871",
     ["plume.patch", "plume-gpu-pick.patch", "plume-texture-pool.patch", "plume-rr-pso-diag.patch"]),
    ("XenosRecomp", "https://github.com/zolaware/reblue-XenosRecomp.git", "339af41",
     ["xenosrecomp.patch", "xenosrecomp-ufc.patch", "xenosrecomp-ufc2.patch", "xenosrecomp-ufc3.patch",
      "xenosrecomp-ufc4.patch", "xenosrecomp-ufc5.patch", "xenosrecomp-ufc6.patch", "xenosrecomp-0e.patch"]),
]


def git(*args, cwd=None, capture=False):
    # autocrlf off: the patches carry the upstream line endings byte for byte.
    cmd = ["git", "-c", "core.longpaths=true", "-c", "core.autocrlf=false", *args]
    if capture:
        return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def head(path):
    r = git("-C", str(path), "rev-parse", "--short=7", "HEAD", capture=True)
    return r.stdout.strip() if r.returncode == 0 else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dest", type=Path, default=NATIVE / "third_party")
    ap.add_argument("--from-dir", type=Path, help="local folder with plume/ and XenosRecomp/")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    bad = 0
    for name, url, commit, patches in REPOS:
        dest = a.dest / name
        if a.check:
            ok = dest.exists() and head(dest) == commit
            if ok:  # pin + the last patch of the stack is in (a quick check, not a byte compare)
                r = git("-C", str(dest), "apply", "--ignore-whitespace", "--check", "-R",
                        str(NATIVE / "patches" / patches[-1]), capture=True)
                ok = r.returncode == 0
            print(f"{name}: {'OK' if ok else 'NOT at ' + commit + ' + patches'}")
            bad += not ok
            continue
        if not dest.exists():
            dest.parent.mkdir(parents=True, exist_ok=True)
            if a.from_dir:
                print(f"copying {a.from_dir / name} -> {dest}")
                shutil.copytree(a.from_dir / name, dest, symlinks=True)
            else:
                git("clone", url, str(dest))
        if head(dest) != commit:
            git("-C", str(dest), "checkout", commit)
        git("-C", str(dest), "submodule", "update", "--init", "--recursive")
        # Drop any earlier (or foreign) patch, then apply ours.
        git("-C", str(dest), "reset", "-q", "--hard", "HEAD")
        for patch in patches:
            git("-C", str(dest), "apply", "--ignore-whitespace", "--whitespace=nowarn", str(NATIVE / "patches" / patch))
        print(f"{name}: {commit} + {', '.join('patches/' + p for p in patches)}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
