// svr07 - high-frame-rate stage 1: the frame-step writer (reports/L6-fps-plan.md steps 1-2,
// reports/L7-fps-stage1.md).
//
// SvR07 keeps its frame rate in a block at 0x8295D1A4: +0x08 rate int (60/50/30/25), +0x0C lock,
// +0x1C fps float, +0x30 step float (1.0 at 60, 2.0 at 30), +0x34 second step (= +0x30 at 60/30).
// Only the game's three setters write it (sub_824297D8 init, sub_82429A30 60->30, sub_82429AA8 30->60);
// 138 loads of +0x30 in 95 functions read it at use time. The main loop sub_82445240 runs one
// logic step per presented frame with that fixed step, so a frame longer than 16.7 ms is slow motion
// and a faster guest vblank alone runs the whole game fast.
//
// This file writes fps / step / step2 once per main-loop iteration, from the top of the loop
// (sub_824243F0 = the first call of each iteration, its only caller; it signals the worker slots, so
// the write lands before any of this frame's logic runs, and the previous frame's workers were
// already waited for by sub_82424398). It writes only while the rate field says 60 and leaves the
// rate field alone, so code that branches on the rate keeps its 60 path and the present interval
// (sub_82429F90: rate 30/25 -> 2) does not change. Same method as UFC 3 (UFC-fps
// src/adaptive_step.h, MIT, adapted from UFC3MONS app/common/ufc_adaptive_rate.cpp:
// Copyright (c) 2026 monscriser, UFC3MONS contributors; permission notice as in that file).
//
// Everything is off by default: with every svr07_fps_* / svr07_adaptive_step / svr07_high_fps_hz cvar
// at its default the hook only calls the game's function, and the carry hooks leave f0 as loaded.
//
// Tick carry: 11 functions turn the step into a whole number (fctiwz/fctidz) and add it to a counter
// (L6 4d); at a step under 1.0 that is 0 and the counter stops. config/fps_midasm.toml puts a mid-asm
// hook right after each one's single load of +0x30; it swaps the loaded value for this frame's carried
// whole step (0,1,0,1 at 0.5). Unlike UFC's swap of the block word around the call, nothing else
// (callees, other threads) sees a changed block. L10: the carry also runs above 1.0 while our writer owns the
// block (1,2,1,2 at 1.5), and below 1.0 the entrance/camera event clock uses it too (end of this file).

#include "svr07_fps.h"

#include "generated/default/svr07_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

// L21 frame-tied stepper finder (src/svr07_probe.h, included at the end of this file; test only, off by default).
namespace svr07_probe {
void Frame(uint8_t* base);
double Jitter();  // 1.0 unless a probe with svr07_anim_probe_jitter is comparing frames
}  // namespace svr07_probe

REXCVAR_DEFINE_BOOL(svr07_fps_log, false, "SvR",
                    "Log presents/s, rate, step, game s per wall s, frame time range and the int(step) "
                    "carry sites every svr07_fps_log_s ([fpslog]); also entrance start/end times")
    .debug_only();
REXCVAR_DEFINE_DOUBLE(svr07_fps_log_s, 2.0, "SvR", "Seconds between [fpslog] lines (0.5..60)").debug_only();
REXCVAR_DEFINE_DOUBLE(svr07_fps_test_ticks, 0.0, "SvR",
                      "Test only (half-step probe): write this step (60 Hz ticks per frame, e.g. 0.5) whenever "
                      "the game runs at rate 60, whatever the real frame time. Logic that follows the step "
                      "then runs at that fraction of normal speed; 0 = off")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_adaptive_step, false, "SvR",
                    "Step follows the real frame time (smoothed): between 60 and svr07_adaptive_step_min_fps "
                    "it keeps game speed when frames run long (slow-motion fix); with svr07_high_fps_hz it "
                    "also absorbs dips below that rate")
    .debug_only();
REXCVAR_DEFINE_DOUBLE(svr07_adaptive_step_min_fps, 30.0, "SvR",
                      "Lowest frame rate the adaptive step follows, 30..60; below it the game slows down")
    .debug_only();
REXCVAR_DEFINE_DOUBLE(svr07_high_fps_hz, 0.0, "SvR",
                      "Above-60 mode: the guest vblank rate this run uses (set video_mode_refresh_rate to the "
                      "same value, e.g. 120 or 240). The step becomes 60/hz (0.5 at 120, 0.25 at 240); "
                      "0 or <= 60 = off")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_fps_tick_carry, true, "SvR",
                    "With a step under 1.0, the 11 functions that use int(step) get a whole step that carries "
                    "the remainder (0,1,0,1 at 0.5) instead of 0. Does nothing at step 1.0 (the default)")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_fps_tick_carry_high, true, "SvR",
                    "Also carry above step 1.0 while our writer sets the step (slow-motion fix): int(1.5) = 1 "
                    "every frame becomes 1,2,1,2 (reports/L10-fps-entrance-end.md). Needs svr07_fps_tick_carry")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_fps_event_carry, false, "SvR",
                    "Experiment (OFF: made entrance #1 hang in svr07-fps-6a, reports/L10): below step 1.0 the "
                    "entrance/camera event clock advances by the carried whole step (0/1) and the part trigger "
                    "windows use it. Does nothing at step >= 1.0")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_fps_cut_lookahead, true, "SvR",
                    "Entrance cut end: sub_824A9390 starts the next camera cut when clock + step >= cut end, one "
                    "frame before the cut's script runs out. With a step that grows between two frames the script "
                    "can run out first and the entrance stops for good (reports/L10). While our writer sets the "
                    "step this test looks ahead max(step, 1.0) (= the 60 fps lookahead) below 1.0 and step + 0.5 "
                    "above it. Does nothing at step 1.0")
    .debug_only();
REXCVAR_DEFINE_DOUBLE(svr07_fps_test_cap, 0.0, "SvR",
                      "Test only: cap the main loop to this many frames per second to imitate a slow PC "
                      "(works with the step off too); 0 = no cap")
    .debug_only();
// L17 (reports/L17-fps-scene-gating.md): only the live match runs above 60.
REXCVAR_DEFINE_BOOL(svr07_hfps_scene_gate, true, "SvR",
                    "High-fps mode (svr07_high_fps_hz > 60): only the live match runs at the high rate. Menus, "
                    "entrances, the pre-match scene, loads and camera scripts run exactly as stock (the game's own "
                    "frame block, no carry) and the guest is paced at the game's own rate (60, or 30 when the game "
                    "asks for it) by waiting hz/rate guest vblanks per frame. false = high rate everywhere (fps15)")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_entrance_console, true, "SvR",
                    "High-fps mode: entrances run at the console's 30 fps (the game's own 60 -> 30 switch goes "
                    "through even with entrance_60fps). false = entrance_60fps decides, as at 60 Hz")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_match_cams_stock, true, "SvR",
                    "High-fps mode: while the camera/event player runs a script inside the match (pre-match intro, "
                    "cinematic cameras) the stock rate is used too. false = only loads/entrances/menus are stock")
    .debug_only();
REXCVAR_DEFINE_DOUBLE(svr07_hfps_live_delay_s, 0.25, "SvR",
                      "High-fps mode: the live-match condition must hold this long before the high rate starts "
                      "(leaving is immediate). 0..5")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_mash_fix, true, "SvR",
                    "High-fps live match: the pad 'changed this frame' test (sub_8210F630, the button-mash input of "
                    "get-up / hold / lock-up struggles) answers per 60 Hz tick: a tick with a change counts on every "
                    "frame of the next tick, so frame-scaled meters (x 60/fps) and the 8-frame mash rings get the "
                    "same total per press as at 60 (reports/L17)")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_script_fix, true, "SvR",
                    "Below step 1.0 while our writer sets the step: the two 30 Hz motion/event players that run a "
                    "script index while int(clock) is even (sub_8225C7C8 motion + its event track, sub_82254570 "
                    "parts motion) step the odd half by the step, not 1.0 (else 1.6x fast at 240), and fire each "
                    "index's events once, not on every frame the index lasts (4x at 240: repeated sounds/impacts)")
    .debug_only();
// L18 (reports/L18-fps-frame-tied-scan.md): per-frame steppers in the live match. All act only while the high rate
// is on (live match) and our writer sets a step under 1.0; integer counters then move by the whole 60 Hz ticks of
// the frame (0 or 1 at 240), float increments by x step. One switch per group (config/fps_midasm.toml L18 block).
REXCVAR_DEFINE_BOOL(svr07_hfps_obj_player_fix, true, "SvR",
                    "High-fps live match: the third 30 Hz motion player sub_822677E0 (props / objects / crowd-type "
                    "objects): odd tick steps by the step (else 1.6x fast) and each script index fires its events "
                    "(sounds, impacts) once, not 4x")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_reversal, true, "SvR",
                    "High-fps live match: reversal press timer (wrestler +608: reversal window, 1 s lockout, LT+RT "
                    "window) and the counter-move window (+920) count 60 Hz ticks, not frames")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_clock, true, "SvR",
                    "High-fps live match: the match clock / time limit counter (0x829489C8) counts ticks")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_refcount, true, "SvR",
                    "High-fps live match: the referee pin count (sub_821A3180 +92) counts ticks")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_ai, true, "SvR",
                    "High-fps live match: CPU AI timers (action +52/+56 counters incl. the CPU reversal cooldown, "
                    "decision / rescan / tactic countdowns) count ticks")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_struggle, true, "SvR",
                    "High-fps live match: hold length (300), submission phase windows (90/390), tug meter, "
                    "mistimed-press lockout and the once-a-second submission pulse count ticks")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_states, true, "SvR",
                    "High-fps live match: wrestler state timers (special window 600, input buffer 15, lockouts, "
                    "small task timers 30/60/120, 10-frame hold, body-part damage regen) count ticks")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_physics, true, "SvR",
                    "High-fps live match: the fall arc (sub_820EBFD8) and dive arc (sub_820FE1F8) integrate gravity "
                    "and velocity x step")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_sound_timers, true, "SvR",
                    "High-fps live match: delayed reaction sounds (SFXE +1568/+1580, 30 frames), crowd cheer "
                    "crossfade (360 frames), stream fades and the announcer duck fade run per tick / x step")
    .debug_only();
// L19 (reports/L19-fps-frame-tied-2.md): the rest of L18's list. Same gating as L18 (live match, our step under 1.0).
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_cues, true, "SvR",
                    "High-fps live match: wrestler 'recently X' cue timers (+900 / +946 / +952 / +953) count ticks "
                    "(the first step stays immediate so '== 1' readers fire once)")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_carry, true, "SvR",
                    "High-fps live match: stick-driven carry / drive movement (sub_820EE588): speed / stick update "
                    "once per tick, position add x step, task counters by ticks")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_turn, true, "SvR",
                    "High-fps live match: turn to face (sub_820F41B0): turn per frame x step, frame counter by ticks")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_spin, true, "SvR",
                    "High-fps live match: spin state 250 (sub_820FD408): rotation per frame x step")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_cam_ramps, true, "SvR",
                    "High-fps live match: match camera mode ramps (sub_8214E2B8) step x step")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_follow, true, "SvR",
                    "High-fps live match: follow point smoothing (sub_82194548): k' = 1-(1-k)^step")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_ik, true, "SvR",
                    "High-fps live match: hand / foot IK offset fade (sub_8225AB08) counts ticks")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_keyframes, true, "SvR",
                    "High-fps live match: keyframed transform blends (sub_822635B8) advance by the step")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_anim_item, true, "SvR",
                    "High-fps live match: animated item clock (sub_822E0858) advances rate x step")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_ladder, true, "SvR",
                    "High-fps live match: hanging ladder sway (sub_82199CF0) phase counts ticks, drawn with the tick "
                    "fraction")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_scene_fades, true, "SvR",
                    "High-fps live match: scene-task fades / delays (sub_8218E020 / 8218EC58 / 8218E6A0) count ticks; "
                    "their one-shot steps fire once")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_bone, true, "SvR",
                    "High-fps live match: move 15121 bone rotate steps (sub_82192EE8) count ticks")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_effect, true, "SvR",
                    "High-fps live match: short effect (sub_82125D78) lifetime by ticks, motion integrated exactly "
                    "over the step")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_shake, true, "SvR",
                    "High-fps live match: shake / bounce container (sub_820E0540) timer, damping and offset run once "
                    "per tick")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_pad_history, true, "SvR",
                    "High-fps live match: pad press history (sub_8210FD30 +36 / +38 = pressed 1 / 2 frames ago, used "
                    "by sub_8210FB08 modes 1 / 4) holds presses 1 / 2 ticks ago instead")
    .debug_only();
// L20 (reports/L20-fps-move-cameras.md): camera scripts in the live match, pause / post-match gating.
REXCVAR_DEFINE_BOOL(svr07_hfps_cam_player_fix, true, "SvR",
                    "High-fps live match: the camera/event player (sub_8225E490) applies a script index and runs its "
                    "commands once per index, not on every frame of the even tick (4x at 240: restarted blends, "
                    "repeated commands); its two frame-counted blends (sub_82251FC8 at +212 / +236) count ticks "
                    "(smoothed with the tick fraction) and the FOV smoothing (sub_8225D9C8) uses k' = 1-(1-k)^step")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_move_cams_high, true, "SvR",
                    "High-fps mode: move-camera scripts in the fight (started by the move-camera code sub_82107A08 / "
                    "82107BD8 / 82107DC8 / 82109768 / 820EBB08) keep the high rate instead of dropping to stock. Needs "
                    "svr07_hfps_cam_player_fix. Other scripts (pre-match, intro, AI, casket) follow "
                    "svr07_hfps_match_cams_stock")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_casket_stock, true, "SvR",
                    "High-fps mode: the casket-match struggle (sub_821A7390 states 1-8: frame-counted tug meter, "
                    "mash per changed frame) runs at the stock rate")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_cam_flash, true, "SvR",
                    "High-fps live match: camera-flash emitter (sub_82264E28, started by camera commands) runs per "
                    "tick (duration, spawn rolls), the flash light (+10884) and each flash's life (sub_822641E8) count "
                    "ticks")
    .debug_only();
// L22 (reports/L22-fps-anim-clocks.md): model material shader clocks.
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_anim_clocks, true, "SvR",
                    "High-fps live match: the model material time clocks (sub_8242E958, the shader constant g_fTime "
                    "of every material entry, += +216 (1.0) per pose update, wrap at 100000) advance by the step")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_flow_gate, true, "SvR",
                    "High-fps mode: the high rate needs the match-flow state machine (sub_8219D408) in state 18 (the "
                    "fight) and running in the last 3 frames: pre-match states, post-match states and a stopped "
                    "match scene (pause menu) run at stock")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_frozen_gate, true, "SvR",
                    "High-fps mode: the high rate needs the motion players (sub_8225C7C8) to have run in the last 3 "
                    "frames; a frozen match (pause menu) runs at stock")
    .debug_only();
// L23 (reports/L23-fps-menus-full-rate.md): menus / 2D UI at full rate (scene gate off), the Bink close race.
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_menu_anims, true, "SvR",
                    "High fps with our step under 1.0 (gate off menus, live match HUD): the 2D layout keyframe player "
                    "(sub_82422750: sprite slide / pulse / blink animations, t += speed per call) advances speed x step, "
                    "and the layout transition counter (sub_82272E70, += 60/rate per frame) counts 60 Hz ticks. Does "
                    "nothing at step 1.0 / 2.0 (stock scenes)")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_movie_sync_next, true, "SvR",
                    "Movies (any frame rate): the last-6-frames Bink NextFrame (render command 56 op 2, sub_8243F688) "
                    "runs inline on the render thread instead of on the worker queue, so a following close (op 3, "
                    "BinkClose + memset) can never overtake it (crash at 0x8270E748 reading a closed Bink handle). Op 1 "
                    "already runs NextFrame inline the same way")
    .debug_only();
// L24 (reports/L24-fps-entrances.md): entrances at the full rate.
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_parts_clock, true, "SvR",
                    "High fps with our step under 1.0: the face / finger pose blend clocks (sub_822541A0, component "
                    "vtable 0x82025D28: 30 face bones + 30 finger bones, blends started by the parts motion events "
                    "through sub_82254418) add the step instead of a fixed 1.0 per call on their rate-60 path (the "
                    "PAL / 30 fps path already adds the step). Does nothing at step 1.0 / 2.0 (stock scenes)")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_parts_clock_match, true, "SvR",
                    "svr07_hfps_fix_parts_clock also in the match scene (match heap layout, no entrance open). false = "
                    "the match keeps the fps23 behaviour (blends 4x fast at 240), entrances / menus still fixed")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_entrance_rope, true, "SvR",
                    "High fps with our step under 1.0: the entrance rope player (sub_824B7150, entrance part vtable "
                    "0x82043EE0; integer 30 Hz script clock +52, run sub_824B6CF0() = 60 Hz ticks per frame times) "
                    "advances only on frames that complete a 60 Hz tick; other frames only re-apply its current rope "
                    "pose. Does nothing at step 1.0 / 2.0 (stock scenes)")
    .debug_only();
// L25 (reports/L25-fps-ropes-fades.md): ring rope physics, full-screen colour fades, entrance blend, state waits.
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_rope_physics, true, "SvR",
                    "High fps with our step under 1.0: the ring rope physics (ROPE component slot +32 sub_821260A0 -> "
                    "sub_82149928 x3 -> RK4 substeps sub_82149298, fixed dt per call, hold timer +7248 per call) runs "
                    "only on frames that complete a 60 Hz tick (the stock fixed step, once per tick); other frames keep "
                    "the rope as it is. Does nothing at step 1.0 / 2.0 (stock scenes)")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_screen_fade, true, "SvR",
                    "High fps with our step under 1.0: the full-screen colour fade / flash (vtable 0x8202598C slot +24 "
                    "sub_822526D8 / sub_82252488) counts 60 Hz ticks (frame counter, delay, ping-pong blend "
                    "sub_82251FC8) while still drawing every frame. Does nothing at step 1.0 / 2.0 (stock scenes)")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_entrance_blend, true, "SvR",
                    "High fps with our step under 1.0: the entrance fade blend sub_824BDBC8 (class with vtables "
                    "0x82045610/18/20; sub_82251FC8 at +204, drawn by sub_824BDD70 from frac +212) counts 60 Hz ticks, "
                    "smoothed with the tick fraction. Does nothing at step 1.0 / 2.0 (stock scenes)")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_hfps_fix_state_wait, true, "SvR",
                    "High fps with our step under 1.0: the I/O state machine sub_824A0758 (vtable 0x820436C0; storage / "
                    "save states, delay +32 = 10 frames, minimum time +0x1918D4 >= 3 x rate in sub_824A0558) counts "
                    "60 Hz ticks. Does nothing at step 1.0 / 2.0 (stock scenes)")
    .debug_only();

namespace svr07_fps {
namespace {

constexpr uint32_t kBlock = 0x8295D1A4;
constexpr uint32_t kRate = kBlock + 0x08, kFps = kBlock + 0x1C, kStep = kBlock + 0x30,
                   kStep2 = kBlock + 0x34;
constexpr double kFullMs = 1000.0 / 60.0;
constexpr double kKeepUpMs = 17.5;  // a 30-60 average under this keeps the game's exact 60 values

using Clock = std::chrono::steady_clock;

uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}
float LoadF32(uint8_t* base, uint32_t addr) {
  const uint32_t bits = Load32(base, addr);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(base + addr, &v, 4);
}
void StoreF32(uint8_t* base, uint32_t addr, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  bits = _byteswap_ulong(bits);
  std::memcpy(base + addr, &bits, 4);
}

// The block in the game's own form for a step of `step` 60 Hz ticks: fps * step = 60, step2 = step
// (as in the game's 60 and 30 cases). step 1.0 writes exactly the game's 60 values.
void WriteStep(uint8_t* base, double step) {
  if (step == 1.0) {
    StoreF32(base, kFps, 60.0f);
    StoreF32(base, kStep, 1.0f);
    StoreF32(base, kStep2, 1.0f);
    return;
  }
  StoreF32(base, kFps, float(60.0 / step));
  StoreF32(base, kStep, float(step));
  StoreF32(base, kStep2, float(step));
}

struct State {
  Clock::time_point last{};
  double smooth_ms = kFullMs;
  bool adjusted = false;  // the block holds values we wrote
  bool announced = false;
  double carry_acc = 0;
  // L17 scene gate.
  bool live = false;                // high rate on this frame
  Clock::time_point cond_since{};   // live-match condition true since (zero = false)
  double tick_acc = 0;              // 60 Hz ticks for the mash latch
  bool writer_low = false;          // L23: last frame's g_writer_low (clear the fix state when it flips)
  std::string why = "boot";
  // Log window.
  Clock::time_point log_start{};
  int frames = 0, adjusted_frames = 0;
  double step_sum = 0, min_ms = 1e9, max_ms = 0, last_step = 1.0;
  uint32_t last_rate = 0;
};
State g;  // main loop thread only (sub_824243F0 has one caller, the loop in sub_82445240)

// Game time (sum of step / 60) and wall time since the first frame, for the entrance lines.
std::atomic<double> g_game_s{0.0};
Clock::time_point g_t0{};
std::atomic<bool> g_t0_set{false};

// < 0: no carry this frame (step 1.0, or carry off): the hooks leave f0 alone.
std::atomic<double> g_whole_step{-1.0};
// >= 0: the event clock / part triggers use this whole step this frame (only below step 1.0).
std::atomic<double> g_event_step{-1.0};
// >= 0: lookahead for the cut-end test in sub_824A9390 this frame (svr07_fps_cut_lookahead).
std::atomic<double> g_cut_lookahead{-1.0};
uint8_t* g_base = nullptr;  // guest memory, for the register-only mid-asm hooks (set every frame)

// Camera / entrance event player (vtable 0x82026034, ctor sub_8225F940): one global object.
// +192 event clock (float, 60 Hz ticks; sub_8225DA68 returns it), +1060 script done, +1224 seek pending.
constexpr uint32_t kEventObjPtr = 0x82948A14;
std::atomic<uint32_t> g_part_triggers{0};

constexpr int kSites = 11;
const char* const kSiteName[kSites] = {"8222EDE0", "8222F6B8", "82261A58", "8232FF30", "824B1760", "824B2BA8",
                                       "824BBCE0", "824C09D8", "8250B360", "8250D470", "825142B8"};
std::atomic<uint32_t> g_site_calls[kSites];
std::atomic<bool> g_site_seen[kSites];

// ---- L17 scene state (reports/L17-fps-scene-gating.md) ----
// Match scene = the match heap layout 0x820070B0 is the last one carved (sub_824150D0, svr07_hooks.cpp); entrance =
// between the game's 60 -> 30 request sub_82429A30 and its 30 -> 60 switch sub_82429AA8. Live match = match scene,
// no entrance, and the camera/event player exists with no script running (+1060 done != 0). play-fps240 log: loads
// show no player, the pre-match intro is a script (done 0, ~14 s), the fight is done 1 throughout.
constexpr uint32_t kMatchHeapLayout = 0x820070B0;
std::atomic<bool> g_match_layout{false};
std::atomic<bool> g_entrance_open{false};
std::atomic<bool> g_live{false};           // the high rate is on (read by the pacer and the match fixes)
std::atomic<bool> g_writer_low{false};     // our writer owns the block with a step under 1.0 this frame
std::atomic<uint64_t> g_frame_no{0};       // main-loop frames
std::atomic<uint64_t> g_tick_no{0};        // whole 60 Hz ticks (sum of steps)
std::atomic<int> g_frame_ticks{1};         // whole 60 Hz ticks of this frame (L18 tick counters)
std::atomic<double> g_tick_frac{0.0};      // L19: fraction of a tick past the last whole tick (ladder phase)
std::atomic<double> g_tick_pos{0.0};       // L19: g_tick_no + g_tick_frac (pad press history)
std::atomic<uint32_t> g_guarded{0};        // L19 [fpslog]: one-shot equality tests cleared on 0-tick frames
std::atomic<uint32_t> g_tickfix_held{0}, g_dup_obj{0};  // [fpslog]: counter calls held on 0-tick frames, obj dups
double g_pace_acc = 0;                     // PaceVblanks: one caller thread at a time
// L20: who started the running camera script (start hooks; reset when no script runs), match-flow state, last frames
// the flow state machine / the motion players / an active casket struggle were seen (worker threads write).
enum CamKind { kCamNone = 0, kCamMove = 1, kCamOther = 2 };
std::atomic<int> g_cam_kind{kCamNone};
std::atomic<uint32_t> g_flow_state{0xFFFFFFFFu};
std::atomic<uint64_t> g_flow_frame{0}, g_motion_frame{0}, g_casket_frame{0};
std::atomic<uint32_t> g_cam_dup{0};        // [fpslog]: repeated camera script indexes skipped
// Counters for the [fpslog] line.
std::atomic<uint32_t> g_mash_held{0}, g_dup_parts{0}, g_dup_motion{0}, g_odd_fix{0};
std::atomic<uint32_t> g_parts_clk{0}, g_rope_held{0};  // L24 [fpslog]: face/finger blend adds fixed, rope passes held
// L25 [fpslog]: rope physics runs / held frames, fade counter advances / holds, entrance blend calls, state waits held
std::atomic<uint32_t> g_ropephys_run{0}, g_ropephys_held{0}, g_fade_tick{0}, g_fade_held{0}, g_ent_blend{0},
    g_state_held{0};
// Mash latch per pad component (sub_8210F630) and the repeated-index filter of the two 30 Hz players. These run
// on the game's worker threads: one mutex, a few calls per person per frame.
struct Latch {
  uint64_t tick = 0;
  bool cur = false, prev = false;
};
struct Seen {
  uint64_t val = 0, first = 0, last = 0;
};
// L19 pad press history per pad object (sub_8210FD30): new presses with their tick position.
struct PadHist {
  double last_pos = -1.0;  // tick position of the previous shift
  struct Edge {
    double pos;
    uint32_t bits;
  };
  Edge edges[16]{};
  int n = 0;
};
std::mutex g_fix_mutex;
std::unordered_map<uint32_t, Latch> g_latch;
std::unordered_map<uint64_t, Seen> g_seen;
std::unordered_map<uint32_t, PadHist> g_padhist;
// latches = false (L23): only the state of the fixes gated on g_writer_low (repeated-index filter, pad history); the
// mash latch is gated on g_live and is cleared only when g_live flips.
void ClearFixState(bool latches = true) {
  std::lock_guard lock(g_fix_mutex);
  if (latches) g_latch.clear();
  g_seen.clear();
  g_padhist.clear();
}
// True when this (key, val) was already seen on the previous frame(s) without a gap and first seen in an
// earlier frame: the same script index again. Same-frame repeats keep the first call's answer.
bool RepeatedIndex(uint64_t key, uint64_t val) {
  const uint64_t f = g_frame_no.load(std::memory_order_relaxed);
  std::lock_guard lock(g_fix_mutex);
  if (g_seen.size() > 4096) g_seen.clear();
  Seen& s = g_seen[key];
  const bool same = s.last != 0 && s.val == val && s.last + 1 >= f;
  if (!same) {
    s.val = val;
    s.first = f;
  }
  s.last = f;
  return same && s.first != f;
}

bool AnyOn() {
  return REXCVAR_GET(svr07_fps_log) || REXCVAR_GET(svr07_fps_test_ticks) > 0 || REXCVAR_GET(svr07_adaptive_step) ||
         REXCVAR_GET(svr07_high_fps_hz) > 60 || REXCVAR_GET(svr07_fps_test_cap) > 0;
}

double WallS(Clock::time_point now) {
  return g_t0_set.load() ? std::chrono::duration<double>(now - g_t0).count() : 0.0;
}

struct Entrance {
  bool open = false;
  Clock::time_point start{};
  double game_start = 0;
  int count = 0;
} g_ent;  // the setters run on the game's threads: guarded by g_ent_mutex
std::mutex g_ent_mutex;

// L17: is this frame part of the live match? Hysteresis: on after svr07_hfps_live_delay_s of the condition, off at
// once. Returns the reason for the log.
bool LiveMatch(uint8_t* base, Clock::time_point now, const char*& why) {
  if (!REXCVAR_GET(svr07_hfps_scene_gate)) {
    why = "scene gate off";
    return true;
  }
  bool cond = false;
  // L20: frames since the flow state machine / the motion players / a casket struggle last ran (Frame() has already
  // counted this frame, so 1 = ran during the previous frame).
  const uint64_t f = g_frame_no.load(std::memory_order_relaxed);
  auto stale = [f](const std::atomic<uint64_t>& seen) { return f - seen.load(std::memory_order_relaxed) > 3; };
  if (!g_match_layout.load(std::memory_order_relaxed)) {
    why = "not in the match layout (menus / entrance scene)";
  } else if (g_entrance_open.load(std::memory_order_relaxed)) {
    why = "entrance";
  } else if (const uint32_t obj = Load32(base, kEventObjPtr); obj == 0) {
    why = "match scene, no camera player (load)";
  } else if (REXCVAR_GET(svr07_hfps_flow_gate) && stale(g_flow_frame)) {
    why = "match flow not running (pause menu / scene stopped)";
  } else if (REXCVAR_GET(svr07_hfps_flow_gate) && g_flow_state.load(std::memory_order_relaxed) != 18) {
    why = g_flow_state.load(std::memory_order_relaxed) < 18 ? "match flow before the fight" : "match flow after the fight";
  } else if (REXCVAR_GET(svr07_hfps_frozen_gate) && stale(g_motion_frame)) {
    why = "match frozen, no motion (pause menu)";
  } else if (REXCVAR_GET(svr07_hfps_casket_stock) && !stale(g_casket_frame)) {
    why = "casket struggle";
  } else if (Load32(base, obj + 1060) == 0) {
    // A camera script runs. Move cameras keep the high rate (L20 camera player fixes); others stay stock.
    const bool move = g_cam_kind.load(std::memory_order_relaxed) == kCamMove &&
                      REXCVAR_GET(svr07_hfps_move_cams_high) && REXCVAR_GET(svr07_hfps_cam_player_fix);
    if (REXCVAR_GET(svr07_hfps_match_cams_stock) && !move) {
      why = "camera script running (pre-match / cinematic)";
    } else {
      why = move ? "live match (move camera)" : "live match (camera script)";
      cond = true;
    }
  } else {
    g_cam_kind.store(kCamNone, std::memory_order_relaxed);  // no script: the next start hook names the next one
    why = "live match";
    cond = true;
  }
  if (!cond) {
    g.cond_since = {};
    return false;
  }
  if (g.cond_since == Clock::time_point{}) g.cond_since = now;
  const double delay = std::clamp(REXCVAR_GET(svr07_hfps_live_delay_s), 0.0, 5.0);
  if (std::chrono::duration<double>(now - g.cond_since).count() >= delay) return true;
  why = "live match (waiting)";
  return false;
}

void Frame(uint8_t* base) {
  g_base = base;
  g_frame_no.fetch_add(1, std::memory_order_relaxed);
  if (!AnyOn() && !g.adjusted) return;  // default: nothing, not even a clock read

  auto now = Clock::now();
  const double cap = REXCVAR_GET(svr07_fps_test_cap);
  if (cap > 0 && g.last != Clock::time_point{}) {
    const auto until = g.last + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / cap));
    if (until - now > std::chrono::milliseconds(2)) std::this_thread::sleep_for(until - now - std::chrono::milliseconds(2));
    while ((now = Clock::now()) < until) std::this_thread::yield();
  }
  const double frame_ms =
      g.last == Clock::time_point{} ? kFullMs : std::chrono::duration<double, std::milli>(now - g.last).count();
  g.last = now;
  if (!g_t0_set.load()) {
    g_t0 = now;
    g_t0_set = true;
  }

  const uint32_t rate = Load32(base, kRate);
  const double test_ticks = REXCVAR_GET(svr07_fps_test_ticks);
  const double hz = REXCVAR_GET(svr07_high_fps_hz);
  const bool adaptive = REXCVAR_GET(svr07_adaptive_step);
  const double ceiling_ms = 1000.0 / std::clamp(REXCVAR_GET(svr07_adaptive_step_min_fps), 30.0, 60.0);

  // L17 scene gate: outside the live match the high-fps mode leaves the block to the game (stock), and the pacer
  // (PaceVblanks) holds the guest to the game's own rate.
  bool live = false;
  if (hz > 60) {
    const char* why = "";
    live = LiveMatch(base, now, why);
    if (live != g.live || g.why != why) {
      // Scene changes are rare (a few per match): logged without svr07_fps_log.
      REXLOG_INFO("[fpslog] scene: {} ({}), game rate {} at wall {:.2f} s", live ? "HIGH fps" : "stock rate", why,
                  rate, WallS(now));
      g.why = why;
    }
    if (live != g.live) ClearFixState();
  }
  g.live = live;
  g_live.store(live, std::memory_order_relaxed);

  if (rate == 60) {
    double step = 1.0;
    bool write = true;
    if (test_ticks > 0) {
      step = std::clamp(test_ticks, 0.1, 2.5);
      if (!g.announced) {
        g.announced = true;
        REXLOG_INFO("[fpslog] probe: writing step={:.3f} at rate 60", step);
      }
    } else if (hz > 60 && !live) {
      // L17: stock-rate scene in high-fps mode: the game's own 60 values (put back once after the match).
      write = g.adjusted;
      g.smooth_ms = kFullMs;
    } else if (hz > 60) {
      const double nominal_ms = 1000.0 / std::min(hz, 480.0);
      double ms = nominal_ms;
      if (adaptive) {
        // UFC adaptive_step.h:203-211: unclamped 0.25 average of the real frame time (clamping short
        // frames made UFC 2-6% fast), snapped to nominal within 5%, kept in [0.5x nominal, ceiling].
        if (!g.adjusted) g.smooth_ms = nominal_ms;
        g.smooth_ms += (std::min(frame_ms, ceiling_ms) - g.smooth_ms) * 0.25;
        ms = std::abs(g.smooth_ms - nominal_ms) < nominal_ms * 0.05
                 ? nominal_ms
                 : std::clamp(g.smooth_ms, nominal_ms * 0.5, ceiling_ms);
      }
      step = ms / kFullMs * svr07_probe::Jitter();  // L21 probe jitter: 1.0 unless asked for
    } else if (adaptive) {
      // UFC adaptive_step.h:228-232: 30-60 only; short average follows a drop within a few frames.
      g.smooth_ms += (std::clamp(frame_ms, kFullMs, ceiling_ms) - g.smooth_ms) * 0.25;
      const double ms = g.smooth_ms < kKeepUpMs ? kFullMs : std::min(g.smooth_ms, ceiling_ms);
      step = ms / kFullMs;
      write = step != 1.0 || g.adjusted;
    } else {
      write = g.adjusted;  // a cvar went off: put the game's 60 values back once
      g.smooth_ms = kFullMs;
    }
    if (write) {
      WriteStep(base, step);
      g.adjusted = step != 1.0;
    }
  } else {
    // 30/25 fps scenes (entrance_60fps=false) and PAL 50: the game's own setter values stay.
    g.adjusted = false;
    g.smooth_ms = kFullMs;
  }

  const double step_now = LoadF32(base, kStep);
  // L17: whole 60 Hz ticks (mash latch) and "our writer runs below step 1.0" (30 Hz player fixes).
  if (step_now > 0 && step_now < 8.0) {
    g.tick_acc += step_now;
    const double whole = std::floor(g.tick_acc + 1e-6);
    g.tick_acc = std::max(0.0, g.tick_acc - whole);
    if (whole >= 1) g_tick_no.fetch_add(uint64_t(whole), std::memory_order_relaxed);
    g_frame_ticks.store(int(whole), std::memory_order_relaxed);
  } else {
    g_frame_ticks.store(1, std::memory_order_relaxed);
  }
  g_tick_frac.store(g.tick_acc, std::memory_order_relaxed);
  g_tick_pos.store(double(g_tick_no.load(std::memory_order_relaxed)) + g.tick_acc, std::memory_order_relaxed);
  const bool writer_low = g.adjusted && step_now > 0 && step_now < 1.0;
  // L23: the tick fixes are gated on g_writer_low alone (TickFixOn): start them fresh when it flips. With the gate on
  // and a fixed step this flips on the same frame as g_live (already cleared above); with the gate off it flips at
  // rate 60 <-> 30 scene changes; with svr07_adaptive_step also when a slow frame lifts the step to >= 1.0.
  if (writer_low != g.writer_low) {
    ClearFixState(/*latches=*/false);
    g.writer_low = writer_low;
  }
  g_writer_low.store(writer_low, std::memory_order_relaxed);
  // Carry: below 1.0 always (as stage 1); above 1.0 only while our writer owns the block (the game's own
  // rate-30 step 2.0 stays untouched; int(2.0) = 2 anyway). Exactly 1.0 = stock: no carry.
  const bool low = step_now > 0 && step_now < 1.0;
  const bool high = step_now > 1.0 && g.adjusted && REXCVAR_GET(svr07_fps_tick_carry_high);
  if (REXCVAR_GET(svr07_fps_tick_carry) && (low || high)) {
    g.carry_acc += step_now;
    const double whole = std::floor(g.carry_acc + 1e-6);  // 1e-6: 3 x float(0.3333) still makes 1
    g.carry_acc = std::max(0.0, g.carry_acc - whole);
    g_whole_step.store(whole, std::memory_order_relaxed);
    g_event_step.store(low && REXCVAR_GET(svr07_fps_event_carry) ? whole : -1.0, std::memory_order_relaxed);
  } else {
    g.carry_acc = 0;
    g_whole_step.store(-1.0, std::memory_order_relaxed);
    g_event_step.store(-1.0, std::memory_order_relaxed);
  }
  double look = -1.0;
  if (REXCVAR_GET(svr07_fps_cut_lookahead) && rate == 60 && step_now > 0 && step_now != 1.0) {
    if (step_now < 1.0) look = 1.0;
    else if (g.adjusted) look = step_now + 0.5;
  }
  g_cut_lookahead.store(look, std::memory_order_relaxed);
  g_game_s.store(g_game_s.load(std::memory_order_relaxed) + step_now / 60.0, std::memory_order_relaxed);

  if (!REXCVAR_GET(svr07_fps_log)) return;
  if (g.log_start == Clock::time_point{}) g.log_start = now;
  ++g.frames;
  g.adjusted_frames += g.adjusted ? 1 : 0;
  g.step_sum += step_now;
  g.min_ms = std::min(g.min_ms, frame_ms);
  g.max_ms = std::max(g.max_ms, frame_ms);
  if (rate != g.last_rate || step_now != g.last_step) {
    if (g.last_rate != 0)
      REXLOG_INFO("[fpslog] change rate {} -> {} step {:.4f} -> {:.4f} at wall {:.2f} s", g.last_rate, rate,
                  g.last_step, step_now, WallS(now));
    g.last_rate = rate;
    g.last_step = step_now;
  }
  const double window_s = std::chrono::duration<double>(now - g.log_start).count();
  if (window_s < std::clamp(REXCVAR_GET(svr07_fps_log_s), 0.5, 60.0)) return;
  std::string sites;
  for (int i = 0; i < kSites; ++i) {
    const uint32_t n = g_site_calls[i].exchange(0, std::memory_order_relaxed);
    if (n) sites += fmt::format(" {}={}", kSiteName[i], n);
  }
  // Event clock of the camera/entrance player (L10): a clock that stops while an entrance is open = stuck script.
  std::string evt = " evt=none";
  if (const uint32_t obj = Load32(base, kEventObjPtr); obj != 0)
    evt = fmt::format(" evt_t={:.2f} evt_done={} evt_seek={} evt_carry={} part_triggers={}", LoadF32(base, obj + 192),
                      Load32(base, obj + 1060), Load32(base, obj + 1224), g_event_step.load() >= 0 ? 1 : 0,
                      g_part_triggers.exchange(0));
  // step_per_real = game seconds per wall second (1.00 = normal speed; 0.50 in the half-step probe).
  REXLOG_INFO(
      "[fpslog] fps={:.1f} rate={} step={:.4f} step_per_real={:.3f} frame_ms={:.2f}..{:.2f} adjusted={}/{} "
      "carry={}{} wall={:.1f} game={:.1f} live={} mash_held={} dup_events={}/{} odd_fix={} dup_obj={} tickfix_held={} "
      "guarded={} cam_dup={} flow={} cam={} parts_clk={} rope_held={} ropephys={}/{} fade={}/{} ent_blend={} "
      "state_held={} int_step_calls:{}",
      g.frames / window_s, rate, step_now, g.step_sum / 60.0 / window_s, g.min_ms, g.max_ms, g.adjusted_frames,
      g.frames, g_whole_step.load() >= 0 ? 1 : 0, evt, WallS(now), g_game_s.load(), g.live ? 1 : 0,
      g_mash_held.exchange(0), g_dup_motion.exchange(0), g_dup_parts.exchange(0), g_odd_fix.exchange(0),
      g_dup_obj.exchange(0), g_tickfix_held.exchange(0), g_guarded.exchange(0), g_cam_dup.exchange(0),
      int32_t(g_flow_state.load()), g_cam_kind.load(), g_parts_clk.exchange(0), g_rope_held.exchange(0),
      g_ropephys_run.exchange(0), g_ropephys_held.exchange(0), g_fade_tick.exchange(0), g_fade_held.exchange(0),
      g_ent_blend.exchange(0), g_state_held.exchange(0),
      sites.empty() ? " none" : sites);
  g.log_start = now;
  g.frames = g.adjusted_frames = 0;
  g.step_sum = 0;
  g.min_ms = 1e9;
  g.max_ms = 0;
}

void Carry(int site, PPCRegister& f0) {
  if (REXCVAR_GET(svr07_fps_log)) {
    g_site_calls[site].fetch_add(1, std::memory_order_relaxed);
    if (!g_site_seen[site].exchange(true))
      REXLOG_INFO("[fpslog] int(step) site sub_{} first call (step {:.4f}, carry {:.0f})", kSiteName[site], f0.f64,
                  g_whole_step.load());
  }
  const double whole = g_whole_step.load(std::memory_order_relaxed);
  if (whole >= 0) f0.f64 = whole;
}

// Event clock carry (L10). Seek pending (+1224, set by the script start sub_8225E1A0 for a start time > 0):
// sub_8225FA60 then calls sub_8225E490 in a loop until it returns non-zero and restores the clock, so that
// loop must keep the game's own (non-zero) step or it would never end.
bool EventSeekPending(uint32_t obj) { return g_base && obj && Load32(g_base, obj + 1224) != 0; }

}  // namespace

void OnEntranceStart() {
  g_entrance_open.store(true, std::memory_order_relaxed);  // L17 scene gate (always tracked; no cost)
  if (!REXCVAR_GET(svr07_fps_log)) return;
  std::lock_guard lock(g_ent_mutex);
  const auto now = Clock::now();
  g_ent.open = true;
  g_ent.start = now;
  g_ent.game_start = g_game_s.load();
  ++g_ent.count;
  REXLOG_INFO("[fpslog] entrance start #{} at wall {:.2f} s game {:.2f} s", g_ent.count, WallS(now),
              g_ent.game_start);
}

// L17: the heap layout the game carves for the next scene (sub_824150D0 r4). The match layout 0x820070B0 is set
// after the entrances (entrance end, then the tip box load); menus / the entrance scene use others.
void OnHeapLayout(uint32_t list) {
  const bool match = list == kMatchHeapLayout;
  if (match) g_entrance_open.store(false, std::memory_order_relaxed);  // an entrance never ends after this
  if (g_match_layout.exchange(match) != match && REXCVAR_GET(svr07_high_fps_hz) > 60)
    REXLOG_INFO("[fpslog] scene: heap layout {:#x} ({})", list, match ? "match" : "not the match");
}

bool KeepEntrance60() {
  return !(REXCVAR_GET(svr07_high_fps_hz) > 60 && REXCVAR_GET(svr07_hfps_scene_gate) &&
           REXCVAR_GET(svr07_hfps_entrance_console));
}

bool HighFpsLive() { return g_live.load(std::memory_order_relaxed); }

// Guest vblanks the frame that is presenting now waits for (the native pacer, once per guest frame on one thread).
// High-fps mode outside the live match: hz / the game's rate field (240 Hz: 4 at rate 60, 8 at rate 30); a
// fraction (144 / 60 = 2.4) is carried so the average is exact. Else 1 (unchanged).
int PaceVblanks() {
  const double hz = REXCVAR_GET(svr07_high_fps_hz);
  if (hz <= 60 || g_live.load(std::memory_order_relaxed) || !g_base) {
    g_pace_acc = 0;
    return 1;
  }
  uint32_t rate = Load32(g_base, kRate);
  if (rate < 20 || rate > 60) rate = 60;
  g_pace_acc += std::min(hz, 480.0) / double(rate);
  const int n = int(std::floor(g_pace_acc + 1e-6));
  g_pace_acc = std::max(0.0, g_pace_acc - n);
  return std::max(n, 1);
}

}  // namespace svr07_fps

// Top of the main loop (sub_82445240 0x824452F8; its only call site): write the block, then signal
// the workers.
REX_HOOK_RAW(sub_824243F0) {
  svr07_fps::Frame(base);
  svr07_probe::Frame(base);  // L21 stepper finder: one bool test when off
  __imp__sub_824243F0(ctx, base);
}

// 30 -> 60 switch (only caller sub_824BD440, the entrance end). Logged; the game's body always runs.
// With entrance_60fps the rate is still 60 here and the body returns without writing.
REX_HOOK_RAW(sub_82429AA8) {
  __imp__sub_82429AA8(ctx, base);
  svr07_fps::g_entrance_open.store(false, std::memory_order_relaxed);  // L17 scene gate
  if (!REXCVAR_GET(svr07_fps_log)) return;
  using namespace svr07_fps;
  std::lock_guard lock(g_ent_mutex);
  const auto now = Clock::now();
  if (g_ent.open) {
    const double wall = std::chrono::duration<double>(now - g_ent.start).count();
    const double game = g_game_s.load() - g_ent.game_start;
    REXLOG_INFO("[fpslog] entrance end #{} entrance s={:.2f} game_s={:.2f} ratio={:.3f} at wall {:.2f} s",
                g_ent.count, wall, game, game > 0 ? wall / game : 0.0, WallS(now));
  } else {
    REXLOG_INFO("[fpslog] entrance end (no start seen) at wall {:.2f} s", WallS(now));
  }
  g_ent.open = false;
}

// Mid-asm hooks from config/fps_midasm.toml (right after each function's lfs of block +0x30).
void Svr07FpsCarry_8222EDE0(PPCRegister& f0) { svr07_fps::Carry(0, f0); }
void Svr07FpsCarry_8222F6B8(PPCRegister& f0) { svr07_fps::Carry(1, f0); }
void Svr07FpsCarry_82261A58(PPCRegister& f0) { svr07_fps::Carry(2, f0); }
void Svr07FpsCarry_8232FF30(PPCRegister& f0) { svr07_fps::Carry(3, f0); }
void Svr07FpsCarry_824B1760(PPCRegister& f0) { svr07_fps::Carry(4, f0); }
void Svr07FpsCarry_824B2BA8(PPCRegister& f0) { svr07_fps::Carry(5, f0); }
void Svr07FpsCarry_824BBCE0(PPCRegister& f0) { svr07_fps::Carry(6, f0); }
void Svr07FpsCarry_824C09D8(PPCRegister& f0) { svr07_fps::Carry(7, f0); }
void Svr07FpsCarry_8250B360(PPCRegister& f0) { svr07_fps::Carry(8, f0); }
void Svr07FpsCarry_8250D470(PPCRegister& f0) { svr07_fps::Carry(9, f0); }
void Svr07FpsCarry_825142B8(PPCRegister& f0) { svr07_fps::Carry(10, f0); }

// ---- L10: entrance / camera event clock (reports/L10-fps-entrance-end.md) ----
// The camera/entrance event player keeps its clock in 60 Hz ticks at camera object +192 and expects it on
// whole ticks: sub_8225E490 runs the script only while int(t) is even (idx = int(t)/2), the part triggers
// sub_824BFEA8 / sub_824C0028 / sub_824C0100 fire when key <= t < key + step, and sub_824BFD68 / sub_824C0368
// rewind it with t = int(max(t - step, 0)). With a step that changes from frame to frame those windows no longer
// tile (a key can fall between two frames' windows) and an entrance part is never started. Below step 1.0 the
// clock advance and the trigger windows use this frame's carried whole step instead (0 or 1), so every test sees
// the same whole-tick clock as at 60 fps.
namespace svr07_fps {
namespace {
constexpr uint32_t kLrEvtStep = 0x8225E64C;    // sub_8225E490: bl sub_82257DF8 (t += step on +192)
constexpr uint32_t kLrHoldRewind = 0x824C05B0;  // sub_824C0368: tmp = 0 + step (then t = int(t - tmp))
constexpr uint32_t kLrPartRewind = 0x824BFE04;  // sub_824BFD68: same
double EventStep() { return g_event_step.load(std::memory_order_relaxed); }
}  // namespace
}  // namespace svr07_fps

// t += step (+ the PAL rounding path). Only the three event-clock call sites above are changed.
REX_HOOK_RAW(sub_82257DF8) {
  using namespace svr07_fps;
  const double w = EventStep();
  const uint32_t lr = uint32_t(ctx.lr);
  if (w >= 0 && (lr == kLrEvtStep || lr == kLrHoldRewind || lr == kLrPartRewind)) {
    const uint32_t p = ctx.r3.u32;
    const uint32_t rate = Load32(base, kRate);
    if (rate == 60 && !(lr == kLrEvtStep && EventSeekPending(p - 192))) {
      const float t = float(LoadF32(base, p) + w);
      StoreF32(base, p, t);
      ctx.f0.f64 = w;  // what the game's body leaves in the registers (rate-60 path: bnelr)
      ctx.f13.f64 = t;
      ctx.r11.u64 = rate;
      return;
    }
  }
  __imp__sub_82257DF8(ctx, base);
}

// Part trigger (start of an entrant's entrance part; caller sub_824BFEA8). Logged only.
REX_HOOK_RAW(sub_824BFD68) {
  using namespace svr07_fps;
  if (REXCVAR_GET(svr07_fps_log)) {
    g_part_triggers.fetch_add(1, std::memory_order_relaxed);
    const uint32_t obj = Load32(base, kEventObjPtr);
    REXLOG_INFO("[fpslog] entrance part trigger part={} evt_t={:.3f} step={:.4f} evt_step={:.0f} at wall {:.2f} s",
                ctx.r4.s32, obj ? LoadF32(base, obj + 192) : -1.0f, LoadF32(base, kStep), EventStep(),
                WallS(Clock::now()));
  }
  __imp__sub_824BFD68(ctx, base);
}

// Mid-asm hooks from config/fps_midasm.toml (after the lfs of block +0x30).
// sub_8225E490 odd-tick path (0x8225E6B4): t += step without running the script.
void Svr07FpsEvtOdd(PPCRegister& f0, PPCRegister& r31) {
  using namespace svr07_fps;
  const double w = EventStep();
  if (w >= 0 && !EventSeekPending(r31.u32)) f0.f64 = w;
}
// Part trigger windows key <= t < key + step.
static void Svr07FpsEvtWindow(PPCRegister& f0) {
  const double w = svr07_fps::EventStep();
  if (w >= 0) f0.f64 = w;
}
void Svr07FpsEvtWin_824BFEA8(PPCRegister& f0) { Svr07FpsEvtWindow(f0); }
void Svr07FpsEvtWin_824C0028(PPCRegister& f0) { Svr07FpsEvtWindow(f0); }
void Svr07FpsEvtWin_824C0100(PPCRegister& f0) { Svr07FpsEvtWindow(f0); }

// L10 (corrected): cut end in sub_824A9390 (entrant vtable +4): `t = sub_8225DA68(); if (step + t >=
// float(this[36])) this[32] = 1;` and this[32] makes it call vcall +0x5C = sub_824C1180, which starts the next cut
// (sub_824BFD68(this, 255, 0)). The camera script itself stops one tick later (sub_8225FB18: +1060 = 1) and then
// the clock no longer moves, so if the latch is missed once it is missed for good. Hook right after the
// lfs f0,48(r11) at 0x824A93C8: f0 = lookahead (1.0 below step 1.0 = the 60 fps test; step + 0.5 above).
void Svr07FpsCutLookahead(PPCRegister& f0) {
  const double look = svr07_fps::g_cut_lookahead.load(std::memory_order_relaxed);
  if (look > 0 && look > f0.f64) f0.f64 = look;
}

// ---- L17: match fixes at high fps (reports/L17-fps-scene-gating.md) ----
// Mash input. sub_8210F630(pad component 'gPAD') = "the pad changed this frame" (+52 != +60 or +50 != +46). Its
// users count it per frame: sub_8210EF80 (CHP, the knock-down recovery meter at wrestler +1152) adds -60/fps per
// changed frame, sub_821047B8 / sub_821149D0 (hold and lock-up struggles) keep 8-frame rings of it and apply the
// sum x 60/fps every frame, sub_820FE1F8 ORs it into a flag. A press still makes 2 changed frames at 240, so each
// is worth a quarter (and the 8-frame ring covers 33 ms instead of 133). Fix: while the high rate is on, answer
// per 60 Hz tick - the OR of the previous tick's frames, on every frame of this tick: per press the same total as
// at 60 (4 frames x 60/240 = 1), and a held stick (a change every frame) still gives one per tick.
REX_HOOK_RAW(sub_8210F630) {
  using namespace svr07_fps;
  const uint32_t pad = ctx.r3.u32;
  __imp__sub_8210F630(ctx, base);
  if (!g_live.load(std::memory_order_relaxed) || !REXCVAR_GET(svr07_hfps_mash_fix)) return;
  const bool changed = ctx.r3.u32 != 0;
  const uint64_t t = g_tick_no.load(std::memory_order_relaxed);
  bool held;
  {
    std::lock_guard lock(g_fix_mutex);
    if (g_latch.size() > 1024) g_latch.clear();
    Latch& l = g_latch[pad];
    if (l.tick != t) {
      l.prev = t - l.tick <= 2 ? l.cur : false;
      l.cur = false;
      l.tick = t;
    }
    l.cur = l.cur || changed;
    held = l.prev;
  }
  ctx.r3.u64 = held ? 1 : 0;
  if (held) g_mash_held.fetch_add(1, std::memory_order_relaxed);
}

// 30 Hz players. Both keep a clock in 60 Hz ticks and run a script index only while int(clock) is even
// (idx = int(clock) / 2), then add the step; on odd ticks they add a whole 1.0 and do nothing else. At 60 that is
// one index per 2 frames. At step 0.25 an index lasts 4 frames (its events run 4 times: the motion player's sound /
// impact events through the 'S4AT' component vcall +20) and the odd half is one frame + 1.0, so the script runs
// 8 / 5 = 1.6x fast. Fix (our writer below step 1.0 only): odd half adds the step, events once per index.
//   sub_8225C7C8 (motion player, vtable 0x82025FD0 via sub_8225CD60): frame data sub_8224EEF0 -> r3 at 0x8225CB10;
//     event flag r10 (0x8225CB18 byte & 0x80, or 0x8225CB2C in mode 1); odd add at 0x8225CBCC.
//   sub_82254570 (parts motion, vtable 0x82025CB0, "parts motion missing" text): index fetch sub_822535D0 from
//     0x82254718 / 0x82254A58 (r8 = idx, r4 = the track cursor); odd adds at 0x82254668 / 0x8225468C / 0x82254900.
namespace svr07_fps {
namespace {
bool ScriptFixOn() {
  return g_writer_low.load(std::memory_order_relaxed) && REXCVAR_GET(svr07_hfps_script_fix);
}
void OddStep(PPCRegister& f0) {
  if (!ScriptFixOn() || !g_base) return;
  const double step = LoadF32(g_base, kStep);
  if (step <= 0 || step >= 1.0) return;
  f0.f64 = double(float(f0.f64 - 1.0 + step));  // the game's fadds with the step in place of 1.0
  g_odd_fix.fetch_add(1, std::memory_order_relaxed);
}
void MotionEvents(PPCRegister& r10, PPCRegister& r3, PPCRegister& r31) {
  if (!ScriptFixOn() || r10.u32 == 0) return;
  if (RepeatedIndex((uint64_t(0xC7C8) << 32) | r31.u32, r3.u32)) {
    r10.u64 = 0;  // same frame data as the previous frame: its events already ran
    g_dup_motion.fetch_add(1, std::memory_order_relaxed);
  }
}
}  // namespace
}  // namespace svr07_fps

void Svr07FpsOddStep_82254668(PPCRegister& f0) { svr07_fps::OddStep(f0); }
void Svr07FpsOddStep_8225468C(PPCRegister& f0) { svr07_fps::OddStep(f0); }
void Svr07FpsOddStep_82254900(PPCRegister& f0) { svr07_fps::OddStep(f0); }
void Svr07FpsOddStep_8225CBCC(PPCRegister& f0) { svr07_fps::OddStep(f0); }
void Svr07FpsMotionEvt_8225CB18(PPCRegister& r10, PPCRegister& r3, PPCRegister& r31) {
  svr07_fps::MotionEvents(r10, r3, r31);
}
void Svr07FpsMotionEvt_8225CB2C(PPCRegister& r10, PPCRegister& r3, PPCRegister& r31) {
  svr07_fps::MotionEvents(r10, r3, r31);
}

// Parts motion index fetch: a repeated index returns no data (the caller then goes to its next track).
REX_HOOK_RAW(sub_822535D0) {
  using namespace svr07_fps;
  const uint32_t lr = uint32_t(ctx.lr), cursor = ctx.r4.u32, idx = ctx.r8.u32;
  __imp__sub_822535D0(ctx, base);
  if ((lr != 0x8225471C && lr != 0x82254A5C) || ctx.r3.u32 == 0 || !ScriptFixOn()) return;
  if (RepeatedIndex((uint64_t(0x35D0) << 32) | cursor, idx)) {
    ctx.r3.u64 = 0;
    g_dup_parts.fetch_add(1, std::memory_order_relaxed);
  }
}

// L17b: who starts camera/event scripts (all go through sub_8225F6C0 -> sub_8225E1A0). Two starters in the code:
// sub_821AD310 (return 0x821AD410: the match-flow state machine sub_8219D408 -> sub_821AD8A0 picks a random script
// id 50..53 = the pre-match intro) and sub_824A9938 (return 0x824A99D0: the entrance/cut class, vtable near
// 0x82043A24, driven by sub_824C0E20). r3 = {group, ?, id}. Logged in the high-fps mode so a run names the in-match
// scripts (fps16 desktop CPU vs CPU: a 4-5 s script every ~10 s; the user's own fight in fps15 had none in 96 s).
REX_HOOK_RAW(sub_8225F6C0) {
  using namespace svr07_fps;
  g_cam_kind.store(kCamOther, std::memory_order_relaxed);  // L20: intro / entrance cut class: never a move camera
  if (REXCVAR_GET(svr07_high_fps_hz) > 60 && g_base) {
    const uint32_t lr = uint32_t(ctx.lr), id = ctx.r3.u32;
    const char* who = lr == 0x821AD410 ? "match intro (sub_821AD310)"
                      : lr == 0x824A99D0 ? "cut class (sub_824A9938)" : "other";
    REXLOG_INFO("[fpslog] camera script start: {} lr {:#x} id {{{} {} {}}} match_layout {} entrance {} live {} at wall "
                "{:.2f} s",
                who, lr, id ? Load32(g_base, id) : 0, id ? Load32(g_base, id + 4) : 0, id ? Load32(g_base, id + 8) : 0,
                g_match_layout.load() ? 1 : 0, g_entrance_open.load() ? 1 : 0, g_live.load() ? 1 : 0,
                WallS(Clock::now()));
  }
  __imp__sub_8225F6C0(ctx, base);
}

// ---- L18: frame-tied steppers in the live match (reports/L18-fps-frame-tied-scan.md) ----
namespace svr07_fps {
enum FixGroup {
  kFixReversal, kFixClock, kFixRefCount, kFixAi, kFixStruggle, kFixStates, kFixPhysics, kFixSoundTimers,
  // L19
  kFixCues, kFixCarry, kFixTurn, kFixSpin, kFixCamRamps, kFixFollow, kFixIk, kFixKeyframes, kFixAnimItem, kFixLadder,
  kFixSceneFades, kFixBone, kFixEffect, kFixShake, kFixPadHistory,
  // L20
  kFixCamPlayer, kFixCamFlash,
  // L22
  kFixAnimClocks,
  // L23
  kFixMenuAnims,
  // L24
  kFixPartsClock, kFixEntranceRope,
  // L25
  kFixRopePhysics, kFixScreenFade, kFixEntranceBlend, kFixStateWait
};
namespace {
bool GroupOn(FixGroup g) {
  switch (g) {
    case kFixReversal: return REXCVAR_GET(svr07_hfps_fix_reversal);
    case kFixClock: return REXCVAR_GET(svr07_hfps_fix_clock);
    case kFixRefCount: return REXCVAR_GET(svr07_hfps_fix_refcount);
    case kFixAi: return REXCVAR_GET(svr07_hfps_fix_ai);
    case kFixStruggle: return REXCVAR_GET(svr07_hfps_fix_struggle);
    case kFixStates: return REXCVAR_GET(svr07_hfps_fix_states);
    case kFixPhysics: return REXCVAR_GET(svr07_hfps_fix_physics);
    case kFixSoundTimers: return REXCVAR_GET(svr07_hfps_fix_sound_timers);
    case kFixCues: return REXCVAR_GET(svr07_hfps_fix_cues);
    case kFixCarry: return REXCVAR_GET(svr07_hfps_fix_carry);
    case kFixTurn: return REXCVAR_GET(svr07_hfps_fix_turn);
    case kFixSpin: return REXCVAR_GET(svr07_hfps_fix_spin);
    case kFixCamRamps: return REXCVAR_GET(svr07_hfps_fix_cam_ramps);
    case kFixFollow: return REXCVAR_GET(svr07_hfps_fix_follow);
    case kFixIk: return REXCVAR_GET(svr07_hfps_fix_ik);
    case kFixKeyframes: return REXCVAR_GET(svr07_hfps_fix_keyframes);
    case kFixAnimItem: return REXCVAR_GET(svr07_hfps_fix_anim_item);
    case kFixLadder: return REXCVAR_GET(svr07_hfps_fix_ladder);
    case kFixSceneFades: return REXCVAR_GET(svr07_hfps_fix_scene_fades);
    case kFixBone: return REXCVAR_GET(svr07_hfps_fix_bone);
    case kFixEffect: return REXCVAR_GET(svr07_hfps_fix_effect);
    case kFixShake: return REXCVAR_GET(svr07_hfps_fix_shake);
    case kFixPadHistory: return REXCVAR_GET(svr07_hfps_fix_pad_history);
    case kFixCamPlayer: return REXCVAR_GET(svr07_hfps_cam_player_fix);
    case kFixCamFlash: return REXCVAR_GET(svr07_hfps_fix_cam_flash);
    case kFixAnimClocks: return REXCVAR_GET(svr07_hfps_fix_anim_clocks);
    case kFixMenuAnims: return REXCVAR_GET(svr07_hfps_fix_menu_anims);
    case kFixPartsClock: return REXCVAR_GET(svr07_hfps_fix_parts_clock);
    case kFixEntranceRope: return REXCVAR_GET(svr07_hfps_fix_entrance_rope);
    case kFixRopePhysics: return REXCVAR_GET(svr07_hfps_fix_rope_physics);
    case kFixScreenFade: return REXCVAR_GET(svr07_hfps_fix_screen_fade);
    case kFixEntranceBlend: return REXCVAR_GET(svr07_hfps_fix_entrance_blend);
    case kFixStateWait: return REXCVAR_GET(svr07_hfps_fix_state_wait);
  }
  return false;
}
// Our writer sets a step under 1.0 this frame (stock scenes, 60 Hz and the high-fps-off mode: never). L23: g_live was
// dropped from this test. In the high-fps mode our writer writes a step under 1.0 only while g_live is true (stock
// scenes get 1.0 and the game's rate-30 step is 2.0), so this is the same test there, and it stays right for a
// high-rate scene outside the match (gate off menus / entrance, or a future high-fps entrance with the gate on).
// Only difference: the svr07_fps_test_ticks probe (< 1.0) now also runs these fixes, as it does the 30 Hz players.
bool TickFixOn(FixGroup g) { return g_writer_low.load(std::memory_order_relaxed) && GroupOn(g); }
int FrameTicks() { return g_frame_ticks.load(std::memory_order_relaxed); }
}  // namespace
// After `addi rX,rY,1`: rX = old + ticks. 32-bit arithmetic; byte/short stores truncate as the game's own would.
void TickUp(FixGroup g, PPCRegister& r) {
  if (!TickFixOn(g)) return;
  const int t = FrameTicks();
  r.u64 = uint64_t(int64_t(int32_t(r.u32 - 1u + uint32_t(t))));
  if (t == 0) g_tickfix_held.fetch_add(1, std::memory_order_relaxed);
}
// After `addi rX,rX,-1`, or before `addic. rX,rX,-1` (then its cr0 sees the adjusted value): rX += 1 - ticks.
void TickDown(FixGroup g, PPCRegister& r) {
  if (!TickFixOn(g)) return;
  const int t = FrameTicks();
  r.u64 = uint64_t(int64_t(int32_t(r.u32 + 1u - uint32_t(t))));
  if (t == 0) g_tickfix_held.fetch_add(1, std::memory_order_relaxed);
}
// After a load of a per-frame float increment: x step.
void StepMul(FixGroup g, PPCRegister& f) {
  if (!TickFixOn(g) || !g_base) return;
  const double step = LoadF32(g_base, kStep);
  if (step > 0 && step < 1.0) f.f64 = double(float(f.f64 * step));
}
// ---- L19 helpers (reports/L19-fps-frame-tied-2.md) ----
// The step of this frame when a group's fix applies (0 < step < 1), else 0.
double FixStep(FixGroup g) {
  if (!TickFixOn(g) || !g_base) return 0.0;
  const double step = LoadF32(g_base, kStep);
  return step > 0 && step < 1.0 ? step : 0.0;
}
// TickUp whose first step from `start` is immediate (a reader tests `== start + 1` / `== start` as a one-shot, e.g.
// "just set" timers): old == start moves by 1 on any frame, later values by whole ticks. Shifts the length by < 1 tick.
void TickUpFirst(FixGroup g, PPCRegister& r, uint32_t start) {
  if (!TickFixOn(g)) return;
  if (r.u32 - 1u == start) return;  // the game's own +1
  TickUp(g, r);
}
// TickDown (after `addi -1`) whose first step from `start` is immediate (reader tests `== start`).
void TickDownFirst(FixGroup g, PPCRegister& r, uint32_t start) {
  if (!TickFixOn(g)) return;
  if (r.u32 + 1u == start) return;
  TickDown(g, r);
}
// After `addi rX,rX,+1` / a float add of 1.0: replace the 1.0 by the step (f = f - 1 + step).
void AddStep(FixGroup g, PPCRegister& f) {
  const double step = FixStep(g);
  if (step > 0) f.f64 = double(float(f.f64 - 1.0 + step));
}
// Exponential smoothing x += (t - x) * k per frame: k' = 1 - (1 - k)^step keeps the per-second response.
void ExpK(FixGroup g, PPCRegister& f) {
  const double step = FixStep(g);
  if (step > 0 && f.f64 > 0 && f.f64 < 1.0) f.f64 = double(float(1.0 - std::pow(1.0 - f.f64, step)));
}
// Guard after a compare whose result is a one-shot on a counter held for the whole tick: on 0-tick frames the
// equality is cleared, so the test passes once per value (on the tick frame where the value changed).
void EqGuard(FixGroup g, PPCCRRegister& cr) {
  if (!TickFixOn(g) || FrameTicks() != 0 || !cr.eq) return;
  cr.eq = 0;
  g_guarded.fetch_add(1, std::memory_order_relaxed);
}
// True on 0-tick frames while a group's fix applies (per-tick blocks).
bool ZeroTick(FixGroup g) { return TickFixOn(g) && FrameTicks() == 0; }
}  // namespace svr07_fps

void Svr07Tick_820FB0C4(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixReversal, r11); }
void Svr07Tick_820E9038(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixReversal, r11); }
void Svr07Tick_8219D350(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixClock, r11); }
void Svr07Tick_821A33BC(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixRefCount, r11); }
void Svr07Tick_82185DF0(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_8218806C(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_82188228(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_8218843C(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_82188644(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_82188790(PPCRegister& r9) { svr07_fps::TickUp(svr07_fps::kFixAi, r9); }
void Svr07Tick_82188C70(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_82188F10(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_821890D4(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_82189404(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_8218959C(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_821897F8(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_82189B24(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_82189D40(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_8218A5F0(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixAi, r11); }
void Svr07Tick_82136A8C(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixAi, r11); }
void Svr07Tick_8213BA50(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixAi, r11); }
void Svr07Tick_8213B1DC(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixAi, r11); }
void Svr07Tick_8213B210(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixAi, r11); }
void Svr07Tick_8213B3A0(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixAi, r11); }
void Svr07Tick_8213897C(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixAi, r11); }
void Svr07Tick_821048AC(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixStruggle, r11); }
// L19: sub_821149D0 runs a start step while +120 == 0 (0x82114BD4 / 0x82114C68): the first step is immediate.
void Svr07Tick_82114D88(PPCRegister& r11) { svr07_fps::TickUpFirst(svr07_fps::kFixStruggle, r11, 0); }
// L19: sub_82116340 tests +132 == 0 (0x82116490 / 0x82116550) as its first frame: the first step is immediate.
void Svr07Tick_821165E0(PPCRegister& r11) { svr07_fps::TickUpFirst(svr07_fps::kFixStruggle, r11, 0); }
// Before `addic. r11,r11,-1 ; bne`: the tug resolves when the result is exactly 0. L19: a meter already at 0 on a
// 0-tick frame keeps the game's -1 (stock: 0 -> -1, no second resolve) instead of staying 0 and resolving again.
void Svr07Tick_82115100(PPCRegister& r11) {
  if (r11.u32 == 0 && svr07_fps::ZeroTick(svr07_fps::kFixStruggle)) return;
  svr07_fps::TickDown(svr07_fps::kFixStruggle, r11);
}
void Svr07Tick_821151CC(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixStruggle, r11); }
void Svr07Tick_821169EC(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixStruggle, r11); }
// L19: +564 is set to 600 and sub_821084A8 tests `== 600` (one-shot): the first step is immediate.
void Svr07Tick_820FA920(PPCRegister& r10) { svr07_fps::TickDownFirst(svr07_fps::kFixStates, r10, 600); }
void Svr07Tick_820E738C(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixStates, r11); }
void Svr07Tick_820FA434(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixStates, r11); }
void Svr07Tick_82104138(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixStates, r11); }
void Svr07Tick_82105038(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixStates, r11); }
void Svr07Tick_82105874(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixStates, r11); }
void Svr07Tick_8211025C(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixStates, r11); }
void Svr07Tick_8210F148(PPCRegister& r10) { svr07_fps::TickUp(svr07_fps::kFixStates, r10); }
void Svr07Tick_8210F178(PPCRegister& r10) { svr07_fps::TickUp(svr07_fps::kFixStates, r10); }
void Svr07Tick_8210F1A8(PPCRegister& r10) { svr07_fps::TickUp(svr07_fps::kFixStates, r10); }
void Svr07Tick_8210F1D8(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixStates, r11); }
void Svr07Tick_820EC098(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixPhysics, f0); }
void Svr07Tick_820EC0A8(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixPhysics, f0); }
void Svr07Tick_820EC0B8(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixPhysics, f0); }
void Svr07Tick_820EC0C8(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixPhysics, f0); }
void Svr07Tick_82101DA0(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixPhysics, f0); }
void Svr07Tick_82101DB4(PPCRegister& f13) { svr07_fps::StepMul(svr07_fps::kFixPhysics, f13); }
void Svr07Tick_82101DB8(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixPhysics, f0); }
void Svr07Tick_823E1114(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixSoundTimers, r11); }
void Svr07Tick_823E1168(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixSoundTimers, r11); }
void Svr07Tick_823E6D00(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixSoundTimers, r11); }
void Svr07Tick_823E6D58(PPCRegister& f13) { svr07_fps::StepMul(svr07_fps::kFixSoundTimers, f13); }
void Svr07Tick_823E6710(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixSoundTimers, r11); }
void Svr07Tick_823E6778(PPCRegister& f13) { svr07_fps::StepMul(svr07_fps::kFixSoundTimers, f13); }
void Svr07Tick_823E3130(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixSoundTimers, f0); }
void Svr07Tick_823E3208(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixSoundTimers, f0); }

// sub_821147C0: r11 = 60 * (r30 / 60) before `subf. r11,r11,r30` (frame counter % 60 == 0 -> pulse sub_82114128).
// Live high fps: the result is 0 on the one frame per 60 ticks where the global tick count passes a multiple of 60.
void Svr07FpsSubPulse_82114868(PPCRegister& r11, PPCRegister& r30) {
  using namespace svr07_fps;
  if (!TickFixOn(kFixStruggle)) return;
  const bool fire = FrameTicks() > 0 && g_tick_no.load(std::memory_order_relaxed) % 60 == 0;
  r11.u64 = uint64_t(int64_t(int32_t(r30.u32 - (fire ? 0u : 1u))));
}

// Third 30 Hz player sub_822677E0 (object / prop / crowd-type motion; vtable 0x82026228). Same shape as L17's two:
// even int(t): fetch index int(t)/2, t += step, fire the frame's events through 'S4AT' vcall +20; odd int(t):
// sub_82250F70(state, int(t) + 1) jumps to the next whole tick in one frame. At step 0.25 an index lasts 4 + 1 frames
// (1.6x fast) and its events (sounds, impacts) fire 4x. Fix (our writer below step 1.0, svr07_hfps_obj_player_fix):
// the odd jump becomes t += step, and repeated frame data fires no events.
namespace svr07_fps {
namespace {
bool ObjFixOn() { return g_writer_low.load(std::memory_order_relaxed) && REXCVAR_GET(svr07_hfps_obj_player_fix); }
}  // namespace
}  // namespace svr07_fps

REX_HOOK_RAW(sub_82250F70) {
  using namespace svr07_fps;
  if (uint32_t(ctx.lr) == 0x82267A6C && ObjFixOn() && Load32(base, kRate) == 60) {
    const double step = LoadF32(base, kStep);
    if (step > 0 && step < 1.0) {
      const uint32_t p = ctx.r3.u32 + 24;
      StoreF32(base, p, float(LoadF32(base, p) + step));
      g_odd_fix.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  __imp__sub_82250F70(ctx, base);
}

void Svr07FpsObjEvt_822679D8(PPCRegister& r10, PPCRegister& r31, PPCRegister& r28) {
  using namespace svr07_fps;
  if (!ObjFixOn() || r10.u32 == 0) return;
  if (RepeatedIndex((uint64_t(0x77E0) << 32) | r28.u32, r31.u32)) {
    r10.u64 = 0;
    g_dup_obj.fetch_add(1, std::memory_order_relaxed);
  }
}

// L18: the in-match camera scripts that do not go through sub_8225F6C0 (fps17 log: "camera script running" every
// 10-17 s in CPU vs CPU with no "camera script start" line). sub_8225DA40(player, a, b, c, f1 = start) writes the
// script id {a b c} and the clock and clears +1060 directly (callers sub_82107BD8 = move cameras for moves
// 12000-12999 / 15081 / sub_8212AAA8, sub_82185238 AI, sub_82340F20); sub_8225F648 starts one through the player's
// vcall +20 (callers sub_82109768 / 82107A08 / 82107BD8 / 82107DC8 move-camera paths, 820EBB08, 8218D888, 8246B708).
// L20: the start hooks also name the script's kind for the scene gate. Move cameras = the return addresses of the
// move-camera code: sub_82107A08 (0x82107B94; also used by the casket struggle, gated separately), sub_82107BD8
// (0x82107DB0 via sub_8225F648, 0x82107D84 via sub_8225DA40), sub_82107DC8 (0x82107EEC), sub_82109768 (0x82109A94,
// 0x82109B94), sub_820EBB08 (0x820EBBB0). Everything else (pre-match 12400 from sub_8218D888 0x8218D8E4, menus
// 0x8246B7CC, AI sub_82185238 0x821852E8, sub_82340F20) is "other" and stays stock with svr07_hfps_match_cams_stock.
namespace svr07_fps {
namespace {
bool MoveCamLr(uint32_t lr) {
  switch (lr) {
    case 0x82107B94: case 0x82107DB0: case 0x82107D84: case 0x82107EEC: case 0x82109A94: case 0x82109B94:
    case 0x820EBBB0:
      return true;
    default:
      return false;
  }
}
}  // namespace
}  // namespace svr07_fps
REX_HOOK_RAW(sub_8225DA40) {
  using namespace svr07_fps;
  const uint32_t lr = uint32_t(ctx.lr);
  const bool move = MoveCamLr(lr);
  g_cam_kind.store(move ? kCamMove : kCamOther, std::memory_order_relaxed);
  if (REXCVAR_GET(svr07_high_fps_hz) > 60)
    REXLOG_INFO("[fpslog] camera script start: direct (sub_8225DA40) lr {:#x} id {{{} {} {}}} {} live {} at wall {:.2f} s",
                lr, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, move ? "move" : "other", g_live.load() ? 1 : 0,
                WallS(Clock::now()));
  __imp__sub_8225DA40(ctx, base);
}
REX_HOOK_RAW(sub_8225F648) {
  using namespace svr07_fps;
  const uint32_t lr = uint32_t(ctx.lr);
  const bool move = MoveCamLr(lr);
  g_cam_kind.store(move ? kCamMove : kCamOther, std::memory_order_relaxed);
  if (REXCVAR_GET(svr07_high_fps_hz) > 60)
    REXLOG_INFO("[fpslog] camera script start: move camera (sub_8225F648) lr {:#x} id {{{} {} {}}} {} live {} at wall "
                "{:.2f} s",
                lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, move ? "move" : "other", g_live.load() ? 1 : 0,
                WallS(Clock::now()));
  __imp__sub_8225F648(ctx, base);
}

// ---- L19: the rest of L18's frame-tied list + L18 risk guards (reports/L19-fps-frame-tied-2.md) ----
// All act only in the live match while our writer sets a step under 1.0 (TickFixOn), each group behind its cvar.
// CUE: wrestler "recently X" timers (sub_820FA3C0): 0 = off, set to 1 by an event, +1 per frame up to 6000 / 200,
// then 0. Readers test `== 1` (823E8160, 82136C38, 823E7EB8: one-shot cues), so 1 -> 2 stays immediate.
void Svr07Tick_820FA44C(PPCRegister& r11) { svr07_fps::TickUpFirst(svr07_fps::kFixCues, r11, 1); }
void Svr07Tick_820FA46C(PPCRegister& r11) { svr07_fps::TickUpFirst(svr07_fps::kFixCues, r11, 1); }
void Svr07Tick_820FA490(PPCRegister& r11) { svr07_fps::TickUpFirst(svr07_fps::kFixCues, r11, 1); }
void Svr07Tick_820FA4B4(PPCRegister& r11) { svr07_fps::TickUpFirst(svr07_fps::kFixCues, r11, 1); }

// CARRY: sub_820EE588 (stick-driven carry / drive). 820EE6D4..820EE8D0 is the per-frame stick / speed update (stick
// hold count +116, 4-entry stick history +80, smoothed turn +108, speed +64 / accel +68 ramp with clamps); it runs on
// tick frames only. The position add after it (820EE8D4: rotated (0, 0, f31) added to the carrier) runs every frame
// with f31 x step, so the distance per tick is unchanged. On 0-tick frames: not moving (the same test as 820EE848:
// +64 <= 0 and +68 == 0) -> 820EEAB4, else -> 820EE8D4.
bool Svr07Carry_820EE6D4(PPCRegister& r31) {
  using namespace svr07_fps;
  if (!ZeroTick(kFixCarry)) return false;
  return !(LoadF32(g_base, r31.u32 + 64) > 0.0f) && LoadF32(g_base, r31.u32 + 68) == 0.0f;
}
bool Svr07Carry_820EE6D8(PPCRegister& r31) {
  (void)r31;
  return svr07_fps::ZeroTick(svr07_fps::kFixCarry);
}
void Svr07Tick_820EE928(PPCRegister& f31) { svr07_fps::StepMul(svr07_fps::kFixCarry, f31); }
void Svr07Tick_820EEAC0(PPCRegister& r10) { svr07_fps::TickUp(svr07_fps::kFixCarry, r10); }
void Svr07Tick_820EEAC4(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixCarry, r11); }
// sub_820ECDB8 (called only from 820EEAD4, after the +52 increment): rule "+52 == 2*rate" runs its action each time
// it holds. +52 is held for the whole tick, so on 0-tick frames the loaded value never matches.
void Svr07Carry_820ED384(PPCRegister& r11) {
  if (svr07_fps::ZeroTick(svr07_fps::kFixCarry)) {
    r11.u64 = 0xFFFFFFFFu;
    svr07_fps::g_guarded.fetch_add(1, std::memory_order_relaxed);
  }
}

// TURN: sub_820F41B0: sub_82412BA8(angle, target, pi / +40) moves at most pi/+40 per frame (linear, snaps), +36 counts
// the frames against +40.
void Svr07Tick_820F41F0(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixTurn, r11); }
void Svr07Tick_820F4210(PPCRegister& f2) { svr07_fps::StepMul(svr07_fps::kFixTurn, f2); }
// SPIN: sub_820FD408 state 250: f29 = +-0.02 rad per frame (x2 on one path), added to both facings and used to rotate
// the partner around the spinner. A rotation by step x angle per frame is exact. (+940 already counts fps-scaled.)
void Svr07Tick_820FD720(PPCRegister& f29) { svr07_fps::StepMul(svr07_fps::kFixSpin, f29); }
// CAM: sub_8214E2B8 three linear ramps: global +-0.05 (clamped), +164 +-0.01 and +712 +-0.2 with a snap when within
// one step. Step and snap distance x step.
void Svr07Tick_8214E504(PPCRegister& f13) { svr07_fps::StepMul(svr07_fps::kFixCamRamps, f13); }
void Svr07Tick_8214E510(PPCRegister& f13) { svr07_fps::StepMul(svr07_fps::kFixCamRamps, f13); }
void Svr07Tick_8214E598(PPCRegister& f12) { svr07_fps::StepMul(svr07_fps::kFixCamRamps, f12); }
void Svr07Tick_8214E5AC(PPCRegister& f13) { svr07_fps::StepMul(svr07_fps::kFixCamRamps, f13); }
void Svr07Tick_8214E5E8(PPCRegister& f12) { svr07_fps::StepMul(svr07_fps::kFixCamRamps, f12); }
void Svr07Tick_8214E5FC(PPCRegister& f13) { svr07_fps::StepMul(svr07_fps::kFixCamRamps, f13); }
// FOLLOW: sub_82194548: +64 += (+48 - +64) * k, k = 0.2 (far) / 0.5 (near), then copied to wrestler +720.
void Svr07Tick_8219472C(PPCRegister& f0) { svr07_fps::ExpK(svr07_fps::kFixFollow, f0); }
// IK: sub_8225AB08 per limb: a new target adds an offset that fades with weight n * c (n = +4 = 9 / 3 / n, -1 per
// frame). The offset is re-added every frame (the base is re-read from the animation), so only the count changes.
void Svr07Tick_8225B318(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixIk, r11); }
// KEY: sub_822635B8: blend times +32 / +36 += 1.0 while <= +80 / +84 (blend = t / length).
void Svr07Tick_82263624(PPCRegister& f13) { svr07_fps::AddStep(svr07_fps::kFixKeyframes, f13); }
void Svr07Tick_82263868(PPCRegister& f0) { svr07_fps::AddStep(svr07_fps::kFixKeyframes, f0); }
// ITEM: sub_822E0858: time +21072 += rate +21076 per frame; loop / clamp at length +21080.
void Svr07Tick_822E0EB0(PPCRegister& f12) { svr07_fps::StepMul(svr07_fps::kFixAnimItem, f12); }

// LADDER: sub_82199CF0 +44 += 1 per frame; sub_82198F28 draws sin(+44 << 9) / cos(+44 << 8) (16-bit angles). The
// counter moves by ticks; the drawn phase adds this frame's tick fraction, so the sway stays smooth at 240.
void Svr07Tick_82199D44(PPCRegister& r10) { svr07_fps::TickUp(svr07_fps::kFixLadder, r10); }
namespace svr07_fps {
namespace {
void LadderPhase(PPCRegister& r11, double scale) {
  if (!TickFixOn(kFixLadder)) return;
  const double frac = std::clamp(g_tick_frac.load(std::memory_order_relaxed), 0.0, 0.999);
  r11.u64 = uint32_t(r11.u32 + uint32_t(frac * scale));
}
}  // namespace
}  // namespace svr07_fps
void Svr07Ladder_82198F68(PPCRegister& r11) { svr07_fps::LadderPhase(r11, 512.0); }
void Svr07Ladder_82198F8C(PPCRegister& r11) { svr07_fps::LadderPhase(r11, 256.0); }

// FADE: sub_8218E020 delay +44 then +32 += +40; sub_8218EC58 delay +56 then fade frame +48 (`== 1` sets flags once);
// sub_8218E6A0 frame +32 (alpha 128 - n; `== 128` places the person once and bumps a global table index; 256 ends).
void Svr07Tick_8218E02C(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixSceneFades, r11); }
void Svr07Tick_8218E03C(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixSceneFades, f0); }
void Svr07Tick_8218EC64(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixSceneFades, r11); }
void Svr07Tick_8218EC74(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixSceneFades, r11); }
void Svr07Guard_8218EC78(PPCCRRegister& cr6) { svr07_fps::EqGuard(svr07_fps::kFixSceneFades, cr6); }
void Svr07Tick_8218E6C4(PPCRegister& r10) { svr07_fps::TickUp(svr07_fps::kFixSceneFades, r10); }
void Svr07Guard_8218E778(PPCCRRegister& cr6) { svr07_fps::EqGuard(svr07_fps::kFixSceneFades, cr6); }
// BONE: sub_82192EE8 move 15121: angle = +52 * c, +52 = 1..5 (one per frame).
void Svr07Tick_82192F3C(PPCRegister& r10) { svr07_fps::TickUp(svr07_fps::kFixBone, r10); }

// FX: sub_82125D78: v (+48) += c (0.04), p (+44) += new v, lifetime +56 -= 4 (ends below 0). Lifetime: before the
// `addic. r11,r11,-4`, r11 += 4 - 4 * ticks. Motion: after `lfs f13,c` (f0 = v, f12 = v + p): f13 = c*h and
// f12 = p + h*v + c*h(h-1)/2, so the code's v + f13 and f12 + f13 give v + c*h and p + h*v + c*h(h+1)/2 (the per-tick
// sequence at every tick boundary; h = 1 gives the stock values).
void Svr07Fx_82125D8C(PPCRegister& r11) {
  using namespace svr07_fps;
  if (!TickFixOn(kFixEffect)) return;
  const int t = FrameTicks();
  r11.u64 = uint64_t(int64_t(int32_t(r11.u32 + 4u - 4u * uint32_t(t))));
  if (t == 0) g_tickfix_held.fetch_add(1, std::memory_order_relaxed);
}
void Svr07Fx_82125D90(PPCRegister& f0, PPCRegister& f12, PPCRegister& f13, PPCRegister& r3) {
  using namespace svr07_fps;
  const double h = FixStep(kFixEffect);
  if (h <= 0) return;
  const double v = f0.f64, c = f13.f64, p = LoadF32(g_base, r3.u32 + 44);
  f13.f64 = double(float(c * h));
  f12.f64 = double(float(p + h * v + c * h * (h - 1.0) * 0.5));
}

// SHAKE: sub_820E0540 (component vtable 0x82002168 slot +12): after its children's updates, timer +160 -= 1.0, vector
// +112 damped x0.5 / x0.8 in timer windows (x0.92 when idle) and += +144. That tail runs on tick frames only (exact
// at every tick; the offset moves at 60 Hz). Jump 820E05E0 -> 820E0738 (epilogue) on 0-tick frames.
bool Svr07Shake_820E05E0(PPCRegister& r31) {
  (void)r31;
  return svr07_fps::ZeroTick(svr07_fps::kFixShake);
}

// PAD: sub_8210FD30 (pad object r30 = owner + 28): each frame +38 = +36, +36 = +40, then +40 = new presses
// (+22 & ~+18). sub_8210FB08 mode 0 / 1 / 4 = pressed this frame / 1 frame ago / 2 frames ago (one-shot tests and
// "pressed now or 2 frames ago" tolerances; L18's +64 / +66 / +68 are the owner's offsets of the same fields).
// High-fps live match: +36 / +38 hold the presses made 1 / 2 ticks earlier, each on exactly one frame (the frame whose
// tick position first reaches press position + 1 / + 2). At step 1.0 this is the stock shift.
void Svr07PadHist_8210FD8C(PPCRegister& r11, PPCRegister& r10, PPCRegister& r30) {
  using namespace svr07_fps;
  if (!TickFixOn(kFixPadHistory)) return;
  constexpr double kEps = 1e-6;
  const double now = g_tick_pos.load(std::memory_order_relaxed);
  uint32_t one = 0, two = 0;
  {
    std::lock_guard lock(g_fix_mutex);
    if (g_padhist.size() > 64) g_padhist.clear();
    PadHist& h = g_padhist[r30.u32];
    const double prev = h.last_pos < 0 || h.last_pos > now ? now - 1.0 : h.last_pos;
    int keep = 0;
    for (int i = 0; i < h.n; ++i) {
      const PadHist::Edge e = h.edges[i];
      if (e.pos + 1.0 > prev + kEps && e.pos + 1.0 <= now + kEps) one |= e.bits;
      if (e.pos + 2.0 > prev + kEps && e.pos + 2.0 <= now + kEps) two |= e.bits;
      if (e.pos + 2.0 > now + kEps) h.edges[keep++] = e;  // still due for slot +38
    }
    h.n = keep;
    h.last_pos = now;
  }
  r10.u64 = one & 0xFFFF;  // -> +36
  r11.u64 = two & 0xFFFF;  // -> +38
}
void Svr07PadHist_8211001C(PPCRegister& r10, PPCRegister& r30) {
  using namespace svr07_fps;
  const uint32_t bits = r10.u32 & 0xFFFF;
  if (bits == 0 || !TickFixOn(kFixPadHistory)) return;
  const double now = g_tick_pos.load(std::memory_order_relaxed);
  std::lock_guard lock(g_fix_mutex);
  if (g_padhist.size() > 64) g_padhist.clear();
  PadHist& h = g_padhist[r30.u32];
  if (h.n == 16) {  // a new press every frame for 16 frames: drop the oldest
    for (int i = 1; i < 16; ++i) h.edges[i - 1] = h.edges[i];
    h.n = 15;
  }
  h.edges[h.n++] = {now, bits};
}

// L19 risk guards for L18's clock fix (0x829489C8 counts ticks, so a value is held for the whole tick): one-shot tests
// pass once per value. sub_820FA3C0 `clock & 63 == 0`, sub_823E4F28 `clock % (30*rate) == 0` -> sound 9302 and
// `clock % (rate*60) == 0` -> 9310 (subf. -> cr0), sub_823E5900 `clock == 180`.
void Svr07Guard_820FA80C(PPCCRRegister& cr6) { svr07_fps::EqGuard(svr07_fps::kFixClock, cr6); }
void Svr07Guard_823E51C4(PPCCRRegister& cr0) { svr07_fps::EqGuard(svr07_fps::kFixClock, cr0); }
void Svr07Guard_823E5218(PPCCRRegister& cr0) { svr07_fps::EqGuard(svr07_fps::kFixClock, cr0); }
void Svr07Guard_823E5AF0(PPCCRRegister& cr6) { svr07_fps::EqGuard(svr07_fps::kFixClock, cr6); }
// sub_82105858 (task +32 TickUp at 0x82105874): phase 1 plays sound 4455 while +32 == 1.
void Svr07Guard_82105900(PPCCRRegister& cr6) { svr07_fps::EqGuard(svr07_fps::kFixStates, cr6); }

// ---- L20: camera scripts in the live match, pause / post-match gating (reports/L20-fps-move-cameras.md) ----
// Scene signals for LiveMatch(): the match-flow state machine (match scene vtable 0x820056D8 +20; state +48, 18 = the
// fight: it runs the match clock sub_8219D318 and leaves for 19 / 20 at the end), the motion players (people animate)
// and the casket-match struggle component (vtable 0x82005C68 +12; wrestler +1040 = casket state 1..8, "CAS_IN_DEF" /
// "CAS_IN_ATK" / "CAS_CLOSE"). One relaxed store per call (the motion player: ~40 calls a frame).
REX_HOOK_RAW(sub_8219D408) {
  using namespace svr07_fps;
  g_flow_state.store(Load32(base, ctx.r3.u32 + 48), std::memory_order_relaxed);
  g_flow_frame.store(g_frame_no.load(std::memory_order_relaxed), std::memory_order_relaxed);
  __imp__sub_8219D408(ctx, base);
}
REX_HOOK_RAW(sub_8225C7C8) {
  using namespace svr07_fps;
  const uint64_t f = g_frame_no.load(std::memory_order_relaxed);
  if (g_motion_frame.load(std::memory_order_relaxed) != f) g_motion_frame.store(f, std::memory_order_relaxed);
  __imp__sub_8225C7C8(ctx, base);
}
REX_HOOK_RAW(sub_821A7390) {
  using namespace svr07_fps;
  const uint32_t who = Load32(base, ctx.r3.u32 + 28);
  if (who != 0 && Load32(base, who + 1040) - 1u < 8u)
    g_casket_frame.store(g_frame_no.load(std::memory_order_relaxed), std::memory_order_relaxed);
  __imp__sub_821A7390(ctx, base);
}

// Camera/event player sub_8225E490 (called once per frame from sub_8225FA60 with r4 = 1 while a script runs). Even
// int(t): fetch the frame data of index int(t)/2 (r30, r28 = index), t += step, run the index's commands (vcall +24 =
// sub_822601E0, 192 opcodes: jumps, seeks, targets, blends, FOV, flashes, ...) and apply its camera key (vcall +32 =
// sub_8225E898: sets the next blend over the ticks to the next key, already / step). Odd int(t): t += step only. At 60
// an index is processed once (t = 2k, then 2k+1 odd). At step 0.25 the even tick lasts 4 frames: commands 4x and the
// key re-applied 3 more times (each restart of the blend shortens the move to the next key). Fix: the jump hook right
// after t += step (0x8225E64C) sends a repeat of last frame's index (same player, same index, same frame data) to the
// function's own "processed" exit 0x8225E80C (+1172 = 0, return 1), exactly what the first frame of the index did.
// r21 = 0 (the seek loop of sub_8225FA60) forgets the record. PAL path (r27) untouched.
bool Svr07CamRepeat_8225E64C(PPCRegister& r21, PPCRegister& r27, PPCRegister& r28, PPCRegister& r30,
                             PPCRegister& r31) {
  using namespace svr07_fps;
  if (!TickFixOn(kFixCamPlayer)) return false;
  const uint64_t key = (uint64_t(0xE490) << 32) | r31.u32;
  if (r21.u32 == 0) {
    RepeatedIndex(key, ~0ull);  // seek loop: the next normal call starts fresh
    return false;
  }
  const uint32_t d = r30.u32;
  if (r27.u32 != 0 || d == 0 || d == 0xFFFFFFFFu) return false;
  if (!RepeatedIndex(key, (uint64_t(r28.u32) << 32) | d)) return false;
  g_cam_dup.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// sub_82251FC8 (blend {+0 n, +4 len, +8 frac, +12 inc}): frac = n >= len ? 1 : n < 0 ? 0 : (n+1)/(len+1), n += inc,
// once per frame. The camera player's two (+212 look-at height, +236 roll / angle; set by target commands with len =
// 2 x arg ticks) are called from sub_8225FB18 (lr 0x8225FB6C / 0x8225FE58). Fix: n counts ticks and frac =
// (n + 1 + inc*(ticks + tick_frac - 1)) / (len+1), which is linear in the global tick position (no 60 Hz stepping) and
// equals the stock value at step 1. L25: the same for the screen fade sub_82252488 (lr 0x822524B8, kFixScreenFade);
// the entrance sub_824BDBC8 tail-calls it (hooked there, below).
namespace svr07_fps {
// The tick-counted blend step (shared by the L20 camera blends and the L25 fades). Returns the number of ticks.
int TickBlend(PPCContext& ctx, uint8_t* base) {
  const uint32_t p = ctx.r3.u32;
  const int32_t n = int32_t(Load32(base, p)), len = int32_t(Load32(base, p + 4)), inc = int32_t(Load32(base, p + 12));
  const int ticks = FrameTicks();
  float f;
  if (n >= len) {
    f = 1.0f;
  } else if (n < 0) {
    f = 0.0f;
  } else {
    const double frac = std::clamp(g_tick_frac.load(std::memory_order_relaxed), 0.0, 0.999);
    f = float(std::clamp((n + 1.0 + inc * (ticks + frac - 1.0)) / (len + 1.0), 0.0, 1.0));
  }
  StoreF32(base, p + 8, f);
  Store32(base, p, uint32_t(n + inc * ticks));
  ctx.f1.f64 = f;
  ctx.f0.f64 = f;
  if (ticks == 0) g_tickfix_held.fetch_add(1, std::memory_order_relaxed);
  return ticks;
}
}  // namespace svr07_fps
REX_HOOK_RAW(sub_82251FC8) {
  using namespace svr07_fps;
  const uint32_t lr = uint32_t(ctx.lr);
  if ((lr == 0x8225FB6C || lr == 0x8225FE58) && TickFixOn(kFixCamPlayer)) {
    TickBlend(ctx, base);
    return;
  }
  if (lr == 0x822524B8 && TickFixOn(kFixScreenFade)) {
    (TickBlend(ctx, base) ? g_fade_tick : g_fade_held).fetch_add(1, std::memory_order_relaxed);
    return;
  }
  __imp__sub_82251FC8(ctx, base);
}

// sub_8225D9C8 (FOV +160 toward +164, from sub_8225FB18): with no start (+168 == 0) it smooths cur += (target - cur) * k
// per frame (k = +172, set by a command). After the fsubs at 0x8225DA2C, f0 = k: k' = 1-(1-k)^step. (The other path,
// counter +176 -= 1 with +172 per frame, is set up by sub_8225E898 as ticks / step frames: already step-aware.)
void Svr07CamFov_8225DA2C(PPCRegister& f0) { svr07_fps::ExpK(svr07_fps::kFixCamPlayer, f0); }

// Camera flashes (crowd photo flashes; camera commands 97-100 set intensity 0x82755904 and duration 0x82755908).
// sub_82264E28: while the duration > 0, -1 per frame (<= 30: fade table) and random rolls spawn flashes / a flash light
// every frame. On 0-tick frames skip it (0x82264E7C, r11 = duration > 0 -> epilogue 0x82265350): duration and rolls
// per tick. sub_82264BA0: flash light +10884 (1 = lit this frame, then a cooldown below 0; the emitter needs < -2)
// counts ticks (0x82264C04 / 0x82264D6C). Flash object update sub_822641E8: age +32 += 1, ends at life + 1 (3): ticks.
bool Svr07CamFlash_82264E7C(PPCRegister& r11) {
  return r11.s32 > 0 && svr07_fps::ZeroTick(svr07_fps::kFixCamFlash);
}
void Svr07Tick_82264C04(PPCRegister& r11) { svr07_fps::TickDown(svr07_fps::kFixCamFlash, r11); }
// Cooldown below 0: 0 -> -1 stays immediate (the restore branch runs at 0 once, as stock).
void Svr07Tick_82264D6C(PPCRegister& r11) { svr07_fps::TickDownFirst(svr07_fps::kFixCamFlash, r11, 0); }
void Svr07Tick_822641FC(PPCRegister& r11) { svr07_fps::TickUp(svr07_fps::kFixCamFlash, r11); }

// ---- L22 (reports/L22-fps-anim-clocks.md): model material time clocks ----
// sub_8242E958 = slot +84 of the model class (vtable 0x82040270, ctor sub_8242FAB0; base sub_8242C798), called at the
// end of the node pose update sub_8242CF58 (slot +44, bone matrices) once per update. It walks the material entries
// (+344, 20 bytes, count = [vcall +56 on the model data +188] +16) and does t += +216 (1.0, set by the base ctor, no
// other writer), wrapping at +16 (99999.99). t (+12) is the shader constant g_fTime that the draw slots sub_82433460 /
// sub_82433BD8 set per material (handle +584). Frame-tied: 4x fast at 240. Fix: after each of the 5 `lfs f0,216(r31)`,
// f0 x step (the 4-unrolled loop 0x8242E9D4 / EA04 / EA34 / EA64 and the tail loop 0x8242EABC).
void Svr07AnimClock_8242E9D4(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixAnimClocks, f0); }
void Svr07AnimClock_8242EA04(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixAnimClocks, f0); }
void Svr07AnimClock_8242EA34(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixAnimClocks, f0); }
void Svr07AnimClock_8242EA64(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixAnimClocks, f0); }
void Svr07AnimClock_8242EABC(PPCRegister& f0) { svr07_fps::StepMul(svr07_fps::kFixAnimClocks, f0); }

// ---- L23 (reports/L23-fps-menus-full-rate.md): menus / 2D UI at full rate, Bink close race ----
// 2D layout keyframe player sub_82422750 (112-byte player at +32 of the UI element class; +80 t in 60 Hz frames, +84
// speed, +88 end, +96 done): after sampling its tracks it does t += speed (0x82422DBC), loops / clamps at end, and sets
// done when t == end + speed. Hook after `lfs f12,84(r30)` (0x82422DB4): f12 = speed x step. The add, the end test and
// the done test all use this one f12 within the call, so the end logic stays exact at any step.
void Svr07MenuAnim_82422DB4(PPCRegister& f12) { svr07_fps::StepMul(svr07_fps::kFixMenuAnims, f12); }
// Layout transition counter sub_82272E70 (from sub_82273FD8): cnt (+1948 index) += 60 / rate (divwu r8 at 0x82272ED8),
// clamped to the duration (+1993 index) when it passes it, interpolated while < duration, final pose once at ==. Hook
// after the divwu: r8 = this frame's whole 60 Hz ticks (0 or 1 at 240). On a 0-tick frame cnt is unchanged and the
// interpolation / final-pose writes repeat with the same values (idempotent: they rebuild the element matrix from
// fixed sources), so the element moves at 60 Hz steps for the stock number of ticks.
void Svr07MenuTick_82272ED8(PPCRegister& r8) {
  using namespace svr07_fps;
  if (r8.u32 != 1 || !TickFixOn(kFixMenuAnims)) return;  // 1 = rate 60 (our writer only writes at rate 60)
  const int t = FrameTicks();
  r8.u64 = uint64_t(uint32_t(t));
  if (t == 0) g_tickfix_held.fetch_add(1, std::memory_order_relaxed);
}

// Bink NextFrame vs BinkClose (L23 B). Movie manager 0x82964A70 (4 slots S; movie m = [S+144], bink = [m+252]; the job
// takes arg S+176 and reads its bink from [[S+176]+252]). Render command 56 (sub_824474F8, render thread): op 1 = DoFrame + BinkNextFrame inline; op 2 (the
// last 6 frames of a movie, main thread set S+180 = 1) = DoFrame + sub_8243F688(mgr, slot), which queues the job
// sub_8243EA90(S+176) (BinkNextFrame([[S+176]+252]); [S+180] = 4) on the worker queue sub_8241DF78; op 3 = BinkClose
// (memset 0 + free) + free(m). The main thread posts op 3 without waiting for the job, so the job's NextFrame can run on
// a closed handle (crash 0x8270E748: frame table [bink+236] = 0). Fix: run the job inline, where sub_8243F688 is called
// (its only call site, 0x8244761C, lr 0x82447620): same thread, same heap context ([m+268], set by the handler) and
// same position as op 1's own inline NextFrame, so a later op 3 can never overtake it. State 4 written at once is the
// "very fast worker" case the main thread already handles (sub_8243F0C0 0x8243F188: 4 -> 0 and go on).
namespace svr07_fps {
namespace {
std::atomic<uint32_t> g_movie_sync{0};
}  // namespace
}  // namespace svr07_fps
REX_HOOK_RAW(sub_8243F688) {
  using namespace svr07_fps;
  if (REXCVAR_GET(svr07_hfps_movie_sync_next) && uint32_t(ctx.lr) == 0x82447620) {
    const uint32_t mgr = ctx.r3.u32, slot = ctx.r4.u32;
    const uint32_t s = Load32(base, mgr + (slot + 8) * 4);
    if (s == 0) return;  // the original's beqlr
    const uint32_t m = Load32(base, s + 176);
    const uint32_t bink = m ? Load32(base, m + 252) : 0;
    const uint32_t n = g_movie_sync.fetch_add(1, std::memory_order_relaxed);
    if (n < 16 || n % 64 == 0)
      REXLOG_INFO("[fpslog] movie: sync NextFrame bink={:#x} slot={} S={:#x} state={} (#{}) at wall {:.2f} s", bink, slot,
                  s, Load32(base, s + 180), n + 1, WallS(Clock::now()));
    ctx.r3.u64 = s + 176;
    sub_8243EA90(ctx, base);  // BinkNextFrame([[S+176]+252]); [S+180] = 4 (returns 0)
    ctx.r3.u64 = 1;           // what sub_8241DF78 returns (the caller ignores it)
    return;
  }
  __imp__sub_8243F688(ctx, base);
}

// ---- L24 (reports/L24-fps-entrances.md): entrances at the full rate ----
// Face / finger pose blend component (vtable 0x82025D28, ctor sub_82253E68, component id '3MFA'; bone names l_eye ..
// hana and l_yubi..): update slot +12 = sub_822541A0, called every drawn frame by the component update (the same path
// as the parts motion player sub_82254570, slot +12 of 0x82025CB0). Two arrays of 30 entries (384 bytes; +0 clock t,
// +48 length, +16 target, +112 start, -32 current): while t <= len it does t += 1.0 (rate 60, 0x8225421C / 0x82254344)
// or t += step (PAL / 30 fps, block +0x30), then current = slerp(start, target, t/len); at t > len current = target.
// Blends are started by sub_82254418 (t = 0, len = frames) from the parts motion event slot sub_82254C78, i.e. once per
// script index (L17 repeated-index filter). The clock has no events and no integer tests (only t <= len, t == len and
// the lerp), so a fractional clock is exact: the game's own PAL path (step 1.2) already does it. Fix: after the rate-60
// `fadds f0,f0,f31`: f0 = f0 - 1.0 + step (our writer below 1.0 only).
namespace svr07_fps {
namespace {
bool InMatchScene() {
  return g_match_layout.load(std::memory_order_relaxed) && !g_entrance_open.load(std::memory_order_relaxed);
}
void PartsClock(PPCRegister& f0) {
  if (!TickFixOn(kFixPartsClock) || !g_base) return;
  if (!REXCVAR_GET(svr07_hfps_fix_parts_clock_match) && InMatchScene()) return;
  const double step = LoadF32(g_base, kStep);
  if (step <= 0 || step >= 1.0) return;
  f0.f64 = double(float(f0.f64 - 1.0 + step));
  g_parts_clk.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace
}  // namespace svr07_fps
void Svr07PartsClock_8225421C(PPCRegister& f0) { svr07_fps::PartsClock(f0); }
void Svr07PartsClock_82254344(PPCRegister& f0) { svr07_fps::PartsClock(f0); }

// Entrance rope player sub_824B7150 (entrance part class, vtable slot 0x82043F04). It loops sub_824B6CF0() times per
// frame (2 at rate 30 / 25, else 1 = the 60 Hz ticks of a frame); each pass runs the 30 Hz script player sub_824B6F10
// (integer clock +52 += 1 per call, index +52/2 on even values, frame data -> pose keys +112..+120 and a lerp over +64
// passes counted in +65), the +136/+140 trigger list keyed on +52, then applies the pose to the 'ROPE' component
// (sub_82127CC0 / sub_82129C10: set rope node targets, hold timer = 60). At 240 one pass per drawn frame = 4x fast.
// Fix: jump hook before `bl sub_824B6F10` (0x824B71A4): on a frame that completes no 60 Hz tick, while the player is
// active (+128 != 0), go straight to the apply part 0x824B72D8 (re-applies the same pose: an idempotent set, as stock
// 240 also re-applied it every frame, so the rope is held exactly as before). Inactive (+128 == 0): the original runs
// (sub_824B6F10 returns 0 at once, no side effects).
bool Svr07EntRope_824B71A4(PPCRegister& r31) {
  using namespace svr07_fps;
  if (!ZeroTick(kFixEntranceRope) || !g_base || Load32(g_base, r31.u32 + 128) == 0) return false;
  g_rope_held.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// ---- L25 (reports/L25-fps-ropes-fades.md): ring rope physics, screen fades, entrance blend, state waits ----
// ROPE component (vtable 0x820035CC; slot +32 = sub_821260A0, called once per drawn frame): for its three rope sets
// (+60 / +64 / +68) it calls sub_82149928(set, n) with n = 2 at rate 30 / 25, else 1 (= 60 Hz ticks per frame). That
// marks nodes with weight +860 == 0.0 (flag +896 = 1), runs n x sub_82149298 and then resets those nodes (+860 = 1.0,
// +896 = 0). sub_82149298: hold timer +7248 -= 1, then 3 (held) or 1 RK4 passes (sub_82148E80 forces, fixed dt 1,
// 0.5 / 1/6 weights) on 96 nodes of 64 bytes. All state lives in the rope object; the draw only reads it. At 240 the
// whole thing runs once per drawn frame: ropes swing 4x fast and a hold (60) ends after 0.25 s. Fix: on a frame that
// completes no 60 Hz tick, skip the call (the stock fixed step then runs once per tick, the pose is kept as is). The
// marker pass is skipped with it, so a weight-0 node waits for the next tick frame (< 1 tick), as at stock 60.
REX_HOOK_RAW(sub_821260A0) {
  using namespace svr07_fps;
  if (ZeroTick(kFixRopePhysics)) {
    g_ropephys_held.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (TickFixOn(kFixRopePhysics)) g_ropephys_run.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_821260A0(ctx, base);
}

// Full-screen colour fade / flash (vtable 0x8202598C slot +24 = sub_822526D8, this = obj + 28). Mode 0 (+68 == 0):
// counter n (+28) vs length (+32): n < len -> colour = lerp(+20, +24, n / len), n += 1 (0x822528B8); n == len -> colour
// = +20, n += 1 (0x8225272C); n == len + 1 -> forced draw, n += 2 (0x8225274C, to len + 2 = idle); then the quad draw
// sub_82252100 (when alpha != 0 or forced). Fix: the three adds count whole 60 Hz ticks (on 0-tick frames n is held
// and the same colour is recomputed and drawn). Mode 1 (+68 != 0): sub_82252488(obj): delay +88 -= 1 per call
// (0x822526B0, ticks), else the ping-pong blend {+68 n, +72 len, +76 frac, +80 inc} via sub_82251FC8 (lr 0x822524B8,
// TickBlend above) with pass ends (inc 1 -> -1, repeat count +84, delay = +92), colour lerp by +76, then the draw.
namespace svr07_fps {
namespace {
void FadeUp(PPCRegister& r) {
  if (!TickFixOn(kFixScreenFade)) return;
  const int t = FrameTicks();
  r.u64 = uint64_t(int64_t(int32_t(r.u32 - 1u + uint32_t(t))));
  (t == 0 ? g_fade_held : g_fade_tick).fetch_add(1, std::memory_order_relaxed);
}
}  // namespace
}  // namespace svr07_fps
void Svr07Fade_8225272C(PPCRegister& r11) { svr07_fps::FadeUp(r11); }
// After `addi r11,r11,2` (len + 1 -> len + 2): r11 - 1 + ticks, so the forced-draw step lasts one tick.
void Svr07Fade_8225274C(PPCRegister& r11) { svr07_fps::FadeUp(r11); }
void Svr07Fade_822528B8(PPCRegister& r11) { svr07_fps::FadeUp(r11); }
void Svr07Fade_822526B0(PPCRegister& r11) {
  using namespace svr07_fps;
  if (!TickFixOn(kFixScreenFade)) return;
  const int t = FrameTicks();
  r11.u64 = uint64_t(int64_t(int32_t(r11.u32 + 1u - uint32_t(t))));
  (t == 0 ? g_fade_held : g_fade_tick).fetch_add(1, std::memory_order_relaxed);
}

// Entrance fade blend (class ctor sub_824BDA20; sub-objects +164 (vtable 0x82045618, list 6 via sub_824169B0) and
// +180 (0x82045610, list 23 via sub_82416B50)). sub_824BDBC8 = slot +4 at +164: `addi r3,r3,40; b sub_82251FC8` = the
// blend at obj +204 {n, len, frac +212, inc}, started by sub_824BDBD0 (fade in: n 0, inc 1, len +220) / sub_824BDD50
// (fade out: n = len = +224, inc -1); no other reader of n. The draw sub_824BDD70 (slot +4 at +180) draws while
// frac +212 > 0. Fix: the tick-counted blend (TickBlend) on the same struct.
REX_HOOK_RAW(sub_824BDBC8) {
  using namespace svr07_fps;
  if (TickFixOn(kFixEntranceBlend)) {
    ctx.r3.u64 = ctx.r3.u32 + 40;
    TickBlend(ctx, base);
    g_ent_blend.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  __imp__sub_824BDBC8(ctx, base);
}

// I/O state machine sub_824A0758 (vtable 0x820436C0; states 1-16 through a jump table; storage / save calls
// sub_82460D60 / 82460EF0 / 82461058 / 821B8xxx). Per call: time +0x1918D4 += 1 (0x824A076C); delay +32 (set to 10 by
// the states) -= 1 and return (0x824A0778); else the state. State 12 (sub_824A0558) waits for time >= 3 x rate (3 s:
// likely the save message minimum time) before state 13. Fix: both count 60 Hz ticks.
void Svr07StateWait_824A076C(PPCRegister& r9) {
  using namespace svr07_fps;
  if (!TickFixOn(kFixStateWait)) return;
  const int t = FrameTicks();
  r9.u64 = uint64_t(int64_t(int32_t(r9.u32 - 1u + uint32_t(t))));
  if (t == 0) g_state_held.fetch_add(1, std::memory_order_relaxed);
}
void Svr07StateWait_824A0778(PPCRegister& r11) {
  using namespace svr07_fps;
  if (!TickFixOn(kFixStateWait)) return;
  const int t = FrameTicks();
  r11.u64 = uint64_t(int64_t(int32_t(r11.u32 + 1u - uint32_t(t))));
  if (t == 0) g_state_held.fetch_add(1, std::memory_order_relaxed);
}

// L21 frame-tied stepper finder ([probe] / [steptrace], reports/L21-fps-stepper-finder.md). Last: it pulls in windows.h.
#include "svr07_probe.h"
