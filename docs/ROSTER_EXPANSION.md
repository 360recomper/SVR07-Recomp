# Roster expansion

Extra superstars beyond the game's 93 roster slots, as real roster members (own record, moves,
model, pictures), from `mods\roster\`. Branch `roster-expansion`.

- **Code:** `src/svr07_roster.cpp`.
- **Site hooks:** `tools/roster_sites.py` generates `config/roster_midasm.toml` and
  `src/roster_sites.inc`; policy per site in `tools/roster_sites_policy.txt`. Hand-written
  hooks: `config/roster_manual.toml`.
- **Research behind it:** `RESEARCH-roster-expansion.md` (workspace root).

## Why not just more slots

The game numbers every character in one space:

| ids | what |
|---|---|
| 0-92 | roster |
| 93-122 | the 30 CAWs (slot = id - 93) |
| 123-127 | CAW templates |
| 128-152 | CAW move sets |
| 153-166 | blank `CHAR/DAT` records |

"id >= 93 means CAW" is tested at 136 places in 88 functions. The roster's per-id tables are
fixed arrays of 93 that sit right in front of the CAWs' (records at 0x828139BC, CAWs at
0x82817A68), and the save (`CreateData.txt`) is a raw dump of that memory. Growing the roster
in place would mean moving the CAWs everywhere, save format included.

Instead, extras take **ids 167 and up** (32 for now: 167-198) and get their own storage.

## How it works

| piece | game side | what the port does |
|---|---|---|
| Record (178 bytes) | `sub_821B9A70(id, set)` | hook returns the extra's record |
| Moves profile (412 bytes per set) | `sub_821B9AF0(id, ?, set)` | hook returns the extra's profile for that set |
| GM-mode record (76 bytes) | `sub_8234BDA8(data, id)` | hook returns the extra's |
| `cmp id,93` and `cmp id,123` | 128 + 183 sites (123 only in functions that handle ids; 300 others observed) | generated hook after the compare: an extra id compares like a roster id |
| Model numbers | `(model*10 + attire)*10 + kind`; **10000 and up = a CAW** (built from edit parts): 89 `n - 10000` sign tests, 7 `cmp n,10000` in the model loaders | generated hooks: an extra's number (its id * 100 + ...) takes the below-10000 branch |
| Model entry names | `sprintf("/EMD/%04d", n)`, 4-character EPAC names | `sprintf` hook: extras' numbers get `X` + 3 base-36 digits |
| Model files | entries are found by name through `plist360.arc` | extras' entries are added to a copy of `pac\ch\ch92.pac` laid over the game's (see below) |
| Select list | `sub_822F8C08` fills 20-byte entries in a 123-entry block of its caller and passes it to the grid (`sub_82301EF0`, list at +72, count at +76) | hook builds it in a bigger block of ours (171 entries), copies the first 123 back, gives the grid ours; the five brand/mode loops get a hook at their end compare that carries them on through the extra ids |
| Select pictures | banks `MENU/SF2A/B/D`: `sub_82466220` builds a 108-entry table, `sub_82465DC8` looks one up | hook on its last step (`sub_82465CE8`) makes texture objects for the extras' PACH children (id = extra id, from `mods\portraits\<id>`) and answers lookups; silhouette when missing |
| Other select lists | `sub_82536E20` (other screens) | extras appended while there is room in the caller's 123 |

Extras are set up the first time the game asks for one: the base superstar's record, profile
(every set) and GM record are copied, then the name, ids and model numbers are set.

### Why ch92.pac

A pac of their own would need a line in `plist360.h`, and:

- UE 2.5 already lists **192 pac files, the most the game's file system is set up for**
  (`sub_821C4D18` asks `sub_8241D310` for 192 entries of 32 bytes). A 193rd crashed the boot.
- The SDK's mods overlay can't open files it *adds* (only ones it replaces): see "SDK bug".

Entries are found by name through `plist360.arc`, so any listed pac can hold them;
`FixPacIndexes` rebuilds the index for the merged file.

### The generated hooks

`tools/roster_sites.py` reads the recompiled code, finds the sites, works out their
addresses (checked against the code's own labels and `bl` return addresses; all 707 were also
checked against the instruction bytes of the game image) and writes one `[[midasm_hook]]`
per site, run right after the instruction:

```cpp
void RosterCmp_822B9840(PPCCRRegister& cr, PPCRegister& r) {
  svr07::roster::OnIdCompare(0x822B9840, svr07::roster::Site::kRoster, cr, r);
}
```

The handlers do nothing for other values, so roster ids, CAWs and loops over 0..92 are
untouched. Id-site policies: `roster` (default), `observe` (log only), `off` (not an id: text
parsers comparing a character with `]`, which is also 93).

Logs:

- `Roster: site XXXXXXXX saw extra id N` / `model site ... saw extra model N`: the first time
  an extra reaches a site. That is how unreviewed code paths show up.
- With `--roster_trace_sites=true`: the first run of every id site (any value), every model
  lookup and every failed lookup (with the guest call chain), the picture-bank steps.

Regenerate after a codegen change: `python -I tools/roster_sites.py`, then rebuild.

## Making one

`mods\roster\<folder>\`:

| file | |
|---|---|
| `manifest.txt` | `name=` (45 chars), `short=` (25), `nick=` (optional), `base=<roster id>` (stats, moves, entrance, class), `brand=0/1/2` (optional), `female=0/1` (optional) |
| `ch.pac` | a SvR07 model pac (any superstar's `pac\ch\chNN.pac`); its `EMD` entries are renumbered to the extra's id |
| `disabled` | (optional) leaves it out |

Pictures: `mods\portraits\<id>\body.png` (the existing portrait merge), id from `slots.txt`.

`mods\roster\slots.txt` keeps each folder's id (`<folder>\t<id>`), so adding or removing one
never moves another's.

Cvar `roster_extras` (default on) turns it all off.

## Status (2026-10-18)

Test: `testrun-roster`, UE 2.5 files and a copy of the save, one extra ("Edge Clone",
Edge's model and stats, id 167, a Jericho render as its picture).

| step | result |
|---|---|
| Boot | OK (control with `--roster_extras=false` also OK) |
| Select screen (exhibition, single) | **listed** with its picture, name, brand, class, overall 90; 124 entries |
| Pick him (Clean/Dirty, attire), opponent, arena | OK |
| VS screen, entrance | OK: Edge's entrance with his model and pyro |
| Match vs Triple H (5 min, COM won) | **OK**: HUD name "Clone", grapples, strikes, damage, pins |
| Results, Exit, save, back to the menus | OK, no crash |

Problems found and fixed on the way:

1. UE 2.5 is at the 192-pac limit: a new pac crashed the boot -> models go into ch92.pac.
2. The select screen's list is full with UE 2.5 + 30 CAWs (123/123) -> list moved to our block.
3. Model numbers >= 10000 are CAWs: Edge Clone was loaded as CAW slot 67 (`/ED156/...` miss,
   endless NOW LOADING) -> model-number hooks.
4. The overlay couldn't open an added file (Disc Read Error) -> ch92.pac instead.
5. The picture bank's PACH is closed before the builder returns -> pictures made in
   `sub_82465CE8`, its last step.
6. "id >= 123 = no character" checks (match setup `sub_8253A1B0`): he had no model in the
   match -> 183 more id sites (123 compares in functions that handle ids); the other 300
   123-compares are observed.

Not tested yet: tag/triple threat/rumble, Create mode on an extra, season and GM mode, more than
one extra, a model ported from another game.

## SDK bug: overlay-added files can't be opened

`VirtualFileSystem::OpenFile` (rexglue-sdk-v010 `src/filesystem/virtual_file_system.cpp`,
"If the cached entry does not exist on host anymore, invalidate it") checks
`parent host path / entry name` on the host. For a file the mods overlay *adds* that path is in
the game folder, where it doesn't exist, so the entry is deleted and the open fails
(`NtCreateFile ... 0xC000000F`, then the game's Disc Read Error). The check should use the
entry's own host path. Not fixed here (shared SDK); worked around with ch92.pac.

## Known gaps / next phases

1. **Inline per-id arrays.** Code that indexes a 93-entry table with the id without a 93 check
   (GM-mode `sub_821B8E30() + 29648 + id*76` appears inline in 0x8235x-0x8238x, unlock flags at
   0x82934520, ...) reads past the table for an extra. The site logs show where extras go; each
   such place needs its own redirect.
2. **Saves:** extras are rebuilt from their base at every start; edits (Create mode, moves) are
   not saved. Next: a sidecar file next to `CreateData.txt`.
3. **Season / GM mode / belts / records:** not reviewed. Extras are kept out of the GM draft
   pool (GM record +1 = 0).
4. **Name calls:** the announcer and commentary pick clips by id; extras have none yet.
5. Extras sort after the CAWs on the select screen (the game sorts only ids < 93).
6. Picture texture objects are not freed with their bank (40 bytes each per menu visit).
7. Ids 200-214 are picture specials; extras stop at 198 for now.
