// svr07 - game-code hooks.
//
// Each hook replaces a weak recompiled function by name; the original stays
// callable as __imp__<name>.

#include "generated/default/svr07_init.h"

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(entrance_60fps, true, "Game",
                    "Keep entrances (and other scenes the game drops to 30 fps) at 60 fps");

// The game's frame-rate mode lives in a global struct at 0x8295D1A4: +8 is the
// target fps (60/50, or 30/25), +28/+48/+52 the matching timesteps. Logic and
// animation advance by those timesteps, and the D3D presentation interval is
// derived from +8 (30/25 -> wait 2 vblanks per frame).
//
// sub_82429A30 switches 60 -> 30 (50 -> 25). The entrance code calls it on the
// 360 to fit its frame budget; sub_82429AA8 switches back afterwards. Skipping
// the switch keeps the 60 fps mode, so entrances render at 60 fps at the right
// speed.
REX_HOOK_RAW(sub_82429A30) {
  if (REXCVAR_GET(entrance_60fps)) {
    REXKRNL_INFO("entrance_60fps: kept 60 fps mode (skipped switch to 30)");
    return;
  }
  __imp__sub_82429A30(ctx, base);
}
