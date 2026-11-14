import re, sys, pathlib, glob
root = pathlib.Path(sys.argv[1])
lo, hi = 0x821D2A60, 0x821EB230
# NR-1 table: 30 names (07 addresses), + Direct3D_CreateDevice and the listed extras
d3d = """821EB100 821EAB50 821EA9C8 821DB9C0 821D93F0 821D7F60 821D8570 821D8290 821D76A0 821D94A0 821D9950
821D9970 821D9D78 821D3010 821D3130 821DAEA0 821DB120 821DB030 821D5058 821DAE30 821DACE0 821DAAA8 821DAE80
821D79A8 821D7B58 821D4F38 821D3D58 821D4C30 821D3DE8 821D4C38 821D2EE8 821D3020 821D2FB0 821D30D0 821DECF0""".split()
d3d = {int(x,16) for x in d3d}
files = [p for p in list(root.glob('src/*.cpp'))+list(root.glob('src/*.h'))+list(root.glob('src/*.inc'))+list(root.glob('config/*.toml'))+[root/'svr07_manifest.toml'] if p.exists()]
tot = 0; hits = []
pat = re.compile(r'(?:sub_|0x|loc_)(82[0-9A-Fa-f]{6})')
for p in files:
    for ln, line in enumerate(p.read_text(errors='replace').splitlines(), 1):
        for m in pat.finditer(line):
            a = int(m.group(1),16); tot += 1
            if lo <= a < hi or a in d3d:
                hits.append(f"{p.name}:{ln}: {line.strip()[:160]}")
print("files", len(files), "address refs", tot, "in D3D range / D3D names:", len(hits))
for h in hits: print(h)
# hook-ish counts
hk = 0
for p in files:
    t = p.read_text(errors='replace')
    hk += len(re.findall(r'REX_HOOK', t)) + len(re.findall(r'(?im)^\s*\[\[?midasm', t)) + len(re.findall(r'midasm', t))
print("hook/midasm mentions", hk)
# Names header (L3) vs our hooked functions
names = {}
for line in (root/'native/src/svr07_d3d_names.h').read_text(errors='replace').splitlines():
    m = re.match(r'\s*#define\s+(\w+)\s+sub_(82[0-9A-Fa-f]{6})', line)
    if m: names[int(m.group(2),16)] = m.group(1)
ours = set()
for p in root.glob('src/*.cpp'):
    for m in re.finditer(r'REX_HOOK\w*\(\s*sub_(82[0-9A-Fa-f]{6})', p.read_text(errors='replace')):
        ours.add(int(m.group(1),16))
print("names header maps", len(names), "names; our REX_HOOK subs", len(ours), "; overlap:", sorted(hex(a) for a in ours & set(names)))
outside = {hex(a): n for a, n in names.items() if not (lo <= a < hi)}
print("names header entries outside the 07 D3D range:", len(outside), "(RR numbers until L3 rewrites)" if outside else "")
