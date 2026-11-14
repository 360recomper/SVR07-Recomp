# Mods

## The mods folder

Put a folder named `mods` next to `svr07.exe` (or set `mods_root` in `svr07.toml`). Any file in it
replaces the game file with the same path, and new files are added. Your game folder is never
changed, so you can switch mods off (`mods_enabled = false`) or delete the folder to go back.

```
mods\
  movies\titantron\06.bik          replaces / adds the titantron for superstar 6
  pac\menu\menuHD.pac              replaces the game's menuHD.pac
  portraits\6\body.png             select-screen pictures (below)
```

## Select-screen pictures

Make a folder named after the superstar's number (the NN in `pac\ch\chNN.pac`) under
`mods\portraits\` and put a full-body picture in it. A transparent background works best.

```
mods\portraits\6\body.png    full-body picture, any size
mods\portraits\6\face.png    optional head close-up (made from body.png if missing)
mods\portraits\6\bust.png    optional waist-up picture (made from body.png if missing)
```

`.dds` (DXT5, 256x256 face/bust, 512x512 body) is used as-is instead of a `.png`. The game builds the
pictures when it starts (a few seconds the first time, then cached in your user data folder).

## Mods installed into the game folder

Some 360 mods move entries inside the game's `.pac` files without updating `plist360.arc`, the index
the game uses to find them (the Ultimate Edition 2.5 mod does this, which crashed the game on the
first loading screen). The recomp checks the index at startup and uses a corrected copy when it
is out of date, so these mods work without any change to your files.
