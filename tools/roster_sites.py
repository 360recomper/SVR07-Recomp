"""Generates the roster-expansion hooks for every place the game tests a superstar id
against 93 (the first CAW id).

  python -I tools/roster_sites.py [generated/default]

Two families of sites:

- superstar ids: each `cmpwi/cmplwi crN,rM,93` (93 = first CAW id), and each
  `cmp rM,123` (123 and up = templates, move sets, "none") in functions that handle ids
  (they also test 93 or call the id functions in ID_FUNCS); other 123 compares (often
  characters: '{' is 123) are only observed;
- model numbers ((model*10 + attire)*10 + kind; 10000 and up = CAW): each
  `addi rT,rS,-10000` followed by a sign test of rT (all 89 are followed by the
  /100 that turns a CAW model number into its slot), and each `cmp rM,10000` in the
  functions listed as `model` in the policy file.

Reads the recompiled code (the PPC asm is in its comments), finds them, works out
each address (one comment per instruction from
the function's start; checked against the code's own loc_ labels and bl return
addresses) and writes:

  config/roster_midasm.toml  [[midasm_hook]] after each compare (included by the manifest)
  src/roster_sites.inc       one wrapper per site, calling svr07::roster::OnIdCompare,
                             OnModelSub or OnModelCompare

The policy for each site comes from tools/roster_sites_policy.txt (see there);
sites not listed get the default: extra ids compare like roster ids, logged once.
"""
import glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'generated', 'default')

POLICIES = ('roster', 'observe', 'off')


def load_policy():
    funcs, sites, model_funcs = {}, {}, set()
    for line in open(os.path.join(ROOT, 'tools', 'roster_sites_policy.txt'), encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        key, pol = line.split()[:2]
        if key == 'model':
            model_funcs.add(pol)
            continue
        assert pol in POLICIES, line
        (funcs if key.startswith('sub_') else sites)[key.lower() if not key.startswith('sub_') else key] = pol
    return funcs, sites, model_funcs


CMP = re.compile(r'^cmpl?wi cr(\d),r(\d+),(93|123)$')
BL = re.compile(r'^bl 0x([0-9a-f]{8})$')
ID_FUNCS = {int(x, 16) for x in '''
    821B9A70 821B9AF0 821B97C0 8234BDA8 825364D0 82536DA8 82535D40 82535D48 82537078
    82536190 82465DC8 82466830 82536210 82535D98 82535DE8 825393B0 82539660 8253A1B0'''.split()}
MSUB = re.compile(r'^addi r(\d+),r(\d+),-10000$')
MCMP = re.compile(r'^cmpl?wi cr(\d),r(\d+),10000$')
LABEL = re.compile(r'^loc_([0-9A-F]{8}):$')
LR = re.compile(r'ctx\.lr = 0x([0-9A-F]{8});')


def scan(model_funcs):
    sites, msubs, mcmps, checked = [], [], [], 0
    id_funcs = set()  # functions that test 93 or call an id function
    pending = None  # an addi rT,rS,-10000 waiting for its sign test
    for path in sorted(glob.glob(os.path.join(GEN, 'svr07_recomp.*.cpp'))):
        fn = addr = None
        last = ''
        for line in open(path, encoding='utf-8', errors='replace'):
            if line.startswith('DEFINE_REX_FUNC('):
                fn = line[16:line.index(')')]
                m = re.search(r'([0-9A-Fa-f]{8})$', fn)
                if not m:  # runtime helpers (__savegprlr_14, ...): no ids in there
                    fn = None
                    continue
                addr = int(m.group(1), 16)
                continue
            if fn is None:
                continue
            s = line.strip()
            m = LABEL.match(s)
            if m:
                at = int(m.group(1), 16)
                # Jump tables sit between blocks: the count may only jump forward,
                # and only after an unconditional branch.
                if at != addr:
                    if not (at > addr and last.split(' ')[0] in ('b', 'bctr', 'blr')):
                        raise SystemExit(f'{fn}: {s} but counted {addr:#x} after "{last}"')
                    addr = at
                checked += 1
                continue
            m = LR.search(s)
            if m and int(m.group(1), 16) != addr:
                raise SystemExit(f'{fn}: address drift at {s} (counted {addr:#x})')
            if s.startswith('// '):
                insn = s[3:].strip()
                last = insn
                m = CMP.match(insn)
                if m:
                    sites.append((addr, fn, int(m.group(1)), int(m.group(2)), insn))
                    if m.group(3) == '93':
                        id_funcs.add(fn)
                m = BL.match(insn)
                if m and int(m.group(1), 16) in ID_FUNCS:
                    id_funcs.add(fn)
                if pending:
                    p_addr, p_fn, p_reg, p_insn, left = pending
                    if re.match(rf'cmpwi cr\d,r{p_reg},0$', insn):
                        msubs.append((p_addr, p_fn, p_reg, p_insn))
                        pending = None
                    else:
                        pending = (p_addr, p_fn, p_reg, p_insn, left - 1) if left > 1 else None
                m = MSUB.match(insn)
                if m:
                    pending = (addr, fn, int(m.group(1)), insn, 4)
                m = MCMP.match(insn)
                if m and fn in model_funcs:
                    mcmps.append((addr, fn, int(m.group(1)), int(m.group(2)), insn))
                addr += 4
    # A 123 compare counts as an id site only in a function that handles ids.
    sites = [(a, f, c, r, i, i.endswith(',93') or f in id_funcs) for a, f, c, r, i in sites]
    print(f'{len(sites)} compares with 93/123, {len(msubs)} model subtractions, {len(mcmps)} model '
          f'compares; {checked} labels checked', file=sys.stderr)
    found = {f for _, f, *rest in mcmps}
    for f in sorted(model_funcs - found):
        print(f'warning: no model compare found in {f}', file=sys.stderr)
    return sites, msubs, mcmps


def main():
    funcs, by_addr, model_funcs = load_policy()
    sites, msubs, mcmps = scan(model_funcs)
    toml = ['# Generated by tools/roster_sites.py - do not edit. Policy: tools/roster_sites_policy.txt', '']
    inc = ['// Generated by tools/roster_sites.py - do not edit. Policy: tools/roster_sites_policy.txt', '']
    counts = {p: 0 for p in POLICIES}
    for addr, fn, crf, reg, insn, is_id in sites:
        pol = by_addr.get(f'{addr:08x}', funcs.get(fn, 'roster' if is_id else 'observe'))
        counts[pol] += 1
        if pol == 'off':
            continue
        name = f'RosterCmp_{addr:08X}'
        toml += ['[[midasm_hook]]', f'address = 0x{addr:08X}', f'name = "{name}"',
                 f'registers = ["cr{crf}", "r{reg}"]', 'after_instruction = true',
                 f'# {fn}: {insn} ({pol})', '']
        inc.append(f'void {name}(PPCCRRegister& cr, PPCRegister& r) '
                   f'{{ svr07::roster::OnIdCompare(0x{addr:08X}, svr07::roster::Site::k{pol.capitalize()}, cr, r); }}')
    for addr, fn, reg, insn in msubs:
        name = f'RosterModelSub_{addr:08X}'
        toml += ['[[midasm_hook]]', f'address = 0x{addr:08X}', f'name = "{name}"',
                 f'registers = ["r{reg}"]', 'after_instruction = true', f'# {fn}: {insn}', '']
        inc.append(f'void {name}(PPCRegister& r) {{ svr07::roster::OnModelSub(0x{addr:08X}, r); }}')
    for addr, fn, crf, reg, insn in mcmps:
        name = f'RosterModelCmp_{addr:08X}'
        toml += ['[[midasm_hook]]', f'address = 0x{addr:08X}', f'name = "{name}"',
                 f'registers = ["cr{crf}", "r{reg}"]', 'after_instruction = true', f'# {fn}: {insn}', '']
        inc.append(f'void {name}(PPCCRRegister& cr, PPCRegister& r) '
                   f'{{ svr07::roster::OnModelCompare(0x{addr:08X}, cr, r); }}')
    open(os.path.join(ROOT, 'config', 'roster_midasm.toml'), 'w', newline='\n').write('\n'.join(toml) + '\n')
    open(os.path.join(ROOT, 'src', 'roster_sites.inc'), 'w', newline='\n').write('\n'.join(inc) + '\n')
    print('sites:', counts, file=sys.stderr)


main()
