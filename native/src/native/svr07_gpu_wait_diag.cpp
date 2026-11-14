// SvR07 port of UFC's ufc_gpu_wait_diag.cpp (NR-P6d + NR-F1, UFC-fps 04f60bf / 2a63a63 / f029c66):
// times the game's own waits on the emulated GPU's fences and, on request, replaces the spin of
// Swap's frame throttle with a short host wait. Part of the GPL-3 native renderer (native/LICENSE).
//
// SvR07 map (base xex, read in generated/default, reports/L9-fps-render-speed.md section 3):
//   sub_821DD328 = BlockOnFence(dev, fence r4, wait type r5): loops while the ring's read-back
//                  pointer has not passed the fence, polling sub_821DEA60; wraps the loop in the
//                  XDK's wait-time stats (sub_821DE8B0 / sub_821DE8E0; type 3 sums into dev+20056).
//                  UFC's sub_8236F328.
//   sub_821DEA60 = the poll: 4 x 8 db16cyc, then the hang check (5000 ticks -> sub_821E4750);
//                  returns 1 = keep waiting, 0 = stop. UFC's sub_82374768. No yield: on the host
//                  this loop is a pure busy spin.
//   sub_821DD640 = the KickOff body. Swap (sub_821EAB50) calls it once at the end, return
//                  address 0x821EB0CC; it then waits with type 3 on the fence one swap back
//                  (dev+13540 / +13544 alternating by dev+20440 bit 0), calls at 0x821DD77C /
//                  0x821DD78C (return addresses 0x821DD780 / 0x821DD790). That wait is Swap's
//                  frame throttle (UFC: the call at 0x8237C360). sub_821DD640 is also reached from
//                  MakeSpace sub_821DDB40 and sub_821DD7A0 (ring full); those are left alone.
// Default (svr07_native_throttle_wait_us 0): the game's spin is kept exactly; the hooks only time
// it (zones gwait / gsleep, SvrDiag kGuestGpuWait), so a default run behaves as before.

//
// L11 (reports/L11-fps-entrance-spikes.md): late-frame flip. The guest paces itself with
// D3DDevice_SynchronizeToPresentationInterval (sub_821EA9C8, called by Present before Swap): it puts
// SCRATCH1 = 1, an INTERRUPT whose callback is sub_821EA938, and a WAIT_REG_MEM until the scratch
// writeback (+4) is 0 again into the ring. sub_821EA938 runs on the GPU command thread when the CP gets
// there: if fewer than `interval` vblanks have passed since the last flip it sets the countdown
// (dev+15156) and the vblank ISR sub_821DC940 clears the scratch on the vblank that ends it; if the
// frame is already late it may flip at once only if the scanline percent (V_COUNTER * 100 / height + 1,
// max 100) is <= the PresentImmediateThreshold in r3's low 16 bits, else it waits one more vblank. The
// SDK's V_COUNTER always reads the display height (100%), so every late frame waited a whole extra
// vblank: at 240 Hz a frame 0.1 ms late became 8.3 ms + the CP's Sleep(1) poll = the 9-11 ms [nspike]
// frames (gwait). svr07_native_late_flip (high-fps mode only) passes threshold 100: a late frame flips
// when it arrives, on-time frames still wait for their vblank. The host presents on its own (Swap hook,
// DXGI), so the guest "flip" is pacing only: nothing tears.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <windows.h>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "core/memory_helpers.h"
#include "ufc_frame_diag.h"
#include "ufc_native.h"

REXCVAR_DEFINE_BOOL(svr07_native_late_flip, true, "SvR",
                    "Native renderer (fps, only while svr07_high_fps_hz > 60): a guest frame that reaches its "
                    "present after its vblank flips at once instead of waiting one more vblank (the game's "
                    "PresentImmediateThreshold set to 100%; the emulated scanline always reads 'bottom'). "
                    "Removes the 2-vblank frames at 240 Hz (reports/L11). false = the game's own rule");
REXCVAR_DECLARE(double, svr07_high_fps_hz);
REXCVAR_DEFINE_BOOL(svr07_native_cpu_pace, true, "SvR",
                    "Native renderer (fps, only while svr07_high_fps_hz > 60): every guest present flips when the "
                    "GPU command thread reaches it, and the frame thread waits for the next guest vblank at Swap's "
                    "throttle instead, so the command thread never parks at a vblank and fences behind a present "
                    "are not held for a whole vblank (the 8-10 ms entrance spikes at 240, reports/L15). Stock 60 "
                    "unchanged. false = the game's own vblank wait in the command thread (+ svr07_native_late_flip)");
REXCVAR_DECLARE(bool, svr07_native_perf);
// L17 (src/svr07_fps.cpp, reports/L17-fps-scene-gating.md): outside the live match the high-fps mode paces the guest
// at the game's own rate: this frame waits PaceVblanks() guest vblanks (4 at 240 Hz / rate 60, 8 at rate 30).
namespace svr07_fps {
int PaceVblanks();
}

REXCVAR_DEFINE_INT32(svr07_native_throttle_wait_us, 0, "SvR",
                     "Native renderer (fps): what Swap's frame throttle (the game's fence spin, "
                     "sub_821DD328 under Swap) does after each poll that says keep waiting. 0 = the "
                     "game's own busy spin (default, as before); N > 0 = wait N us on a high-resolution "
                     "timer (UFC: 250); -1 = SwitchToThread (yield); -2 = Sleep(1) (UFC NR-P6d)");

REX_EXTERN(__imp__sub_821DD328);
REX_EXTERN(__imp__sub_821DD640);
REX_EXTERN(__imp__sub_821DEA60);
REX_EXTERN(__imp__sub_821EA938);
// L15 (measurement only, svr07_native_perf): see the block above REX_HOOK_RAW(sub_821DC940).
REX_EXTERN(__imp__sub_821DC940);
REX_EXTERN(__imp__sub_82424398);
REX_EXTERN(__imp__sub_82429C90);
REX_EXTERN(__imp__sub_821DC760);
REX_EXTERN(__imp__sub_821DC6C8);
REX_EXTERN(__imp__sub_821EA078);
REX_EXTERN(__imp__sub_821D19F8);
REX_EXTERN(__imp__sub_82571680);
REX_EXTERN(__imp__sub_821D14F8);

namespace {
constexpr uint32_t kSwapKickReturn = 0x821EB0CC;     // Swap -> sub_821DD640
constexpr uint32_t kThrottleWaitReturnA = 0x821DD780;  // sub_821DD640 -> sub_821DD328 (+13540)
constexpr uint32_t kThrottleWaitReturnB = 0x821DD790;  // sub_821DD640 -> sub_821DD328 (+13544)

void ThrottleWait(int32_t us) {
  if (us == -2) {
    Sleep(1);
    return;
  }
  if (us < 0) {
    // Give the core away but come straight back (timers can wake 1-2 ms late on a busy CPU).
    if (!SwitchToThread())
      YieldProcessor();
    return;
  }
  // One high-resolution waitable timer per thread that waits on the throttle.
  thread_local HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) {
    Sleep(1);
    return;
  }
  LARGE_INTEGER due;
  due.QuadPart = -int64_t(us) * 10;  // relative, 100 ns units
  if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
    WaitForSingleObject(timer, INFINITE);
  else
    Sleep(1);
}

// Set while Swap's own KickOff call runs, then while its throttle wait runs.
thread_local bool t_swap_kick = false;
thread_local bool t_swap_wait = false;

// Late-flip counters ([nflip] line with svr07_native_perf). sub_821EA938 runs on one thread (the GPU
// command thread), once per presented frame.
constexpr uint32_t kDevicePtrPtr = 0x82000818;  // sub_821EA938: lis r11,-32256; lwz r11,2072(r11); lwz r11,0(r11)
constexpr uint32_t kVblankCount = 15148, kLastFlip = 15152, kCountdown = 15156;
uint32_t g_flip_calls = 0, g_flip_late = 0, g_flip_forced = 0, g_flip_nodev = 0;
std::chrono::steady_clock::time_point g_flip_log{};

// L12: through bd::mem (TranslateVirtual), like the generated code's REX_LOAD_U32. The old raw
// base + addr read skipped the 0xE0000000-heap 0x1000-byte host offset, so the device pointer read
// as 0 and [nflip] logged "presents 0" in every run although the late flip acted (it does not need
// the device).
uint32_t LoadBE32(uint32_t addr) { return bd::mem::load<uint32_t>(addr); }

// ---- L15: guest pacing timeline (svr07_native_perf only) --------------------------------------
// The [nspike] frames at 240 are one whole vblank of gwait plus ~2 ms more `game` (reports/L15). A
// model of the guest's pacing rule (sub_821EA938 + ISR + one-back throttle) locks at 4.17 ms and
// never makes that shape, so something outside the model moves. These events say what, in time
// order: V = guest vblank ISR (VR = it released a present), P = present decision on the CP thread
// (PL = late, flipped at once; PW = waits for a vblank; the number = the game's `left`; '!' = a
// vblank happened while sub_821EA938 ran), T = Swap throttle (start, length), r/j/k = guest ring
// poll / worker join / kernel wait on the frame thread over 0.1 ms (start, length).
struct PaceEvent {
  int64_t t_ns = 0;   // steady_clock
  int32_t dur_us = 0;
  int16_t arg = 0;
  char type = 0;
  char flag = 0;
};
constexpr size_t kPaceEvents = 128;
std::mutex g_pace_mutex;
PaceEvent g_pace[kPaceEvents];
size_t g_pace_n = 0;

int64_t PaceNow() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
bool PaceOn() { return bd::gpu::g_svr_perf_on.load(std::memory_order_relaxed); }
void PaceAdd(char type, char flag, int arg, int64_t t_ns, int64_t dur_ns = 0) {
  std::lock_guard lock(g_pace_mutex);
  PaceEvent& e = g_pace[g_pace_n++ % kPaceEvents];
  e.t_ns = t_ns;
  e.dur_us = int32_t(dur_ns / 1000);
  e.arg = int16_t(arg);
  e.type = type;
  e.flag = flag;
}

// A zone on the frame thread only (other threads would add to [nperf] `other`), plus a timeline
// event when it lasted over 0.1 ms.
class FrameWait {
 public:
  FrameWait(bd::gpu::SvrZoneId id, char type)
      : on_(PaceOn() && bd::gpu::SvrIsFrameThread()), type_(type) {
    if (on_) {
      previous_ = bd::gpu::SvrZoneEnter(id);
      start_ = PaceNow();
    }
  }
  ~FrameWait() {
    if (!on_)
      return;
    bd::gpu::SvrZoneLeave(previous_);
    const int64_t dur = PaceNow() - start_;
    if (dur > 100000)
      PaceAdd(type_, 0, 0, start_, dur);
  }
  FrameWait(const FrameWait&) = delete;
  FrameWait& operator=(const FrameWait&) = delete;

 private:
  bool on_;
  char type_;
  int previous_ = 0;
  int64_t start_ = 0;
};
thread_local bool t_in_join = false;  // the join's own WaitForSingleObject stays `gjoin`

// ---- L15 fix: pace on the frame thread, not in the CP ------------------------------------------
// svr07-desk-240h-fps10: in all 731 entrance spikes (8-12 ms, gwait > 2 ms) the Swap throttle waited
// 0 ms; the gwait was another BlockOnFence early in the frame on a fence that sat behind the previous
// present, which was parked at its vblank wait (PW) - 610 of them end exactly at that present's vblank
// release. A parked CP holds every fence behind the present for up to a whole vblank. With
// svr07_native_cpu_pace the CP never parks (every present flips when the CP reaches it) and the frame
// thread waits at Swap's throttle until a guest vblank has passed since the last frame's release: the
// same "one frame per vblank, aligned to the vblank" pacing, without blocking the fences.
bool CpuPaceOn() {
  return REXCVAR_GET(svr07_native_cpu_pace) && REXCVAR_GET(svr07_high_fps_hz) > 60.0;
}
uint32_t g_pace_release_count = 0;  // frame thread (Swap throttle)
bool g_pace_release_valid = false;
// L17b stock-rate wall-clock grid (frame thread).
std::chrono::steady_clock::time_point g_grid_next{}, g_grid_log{};
uint32_t g_grid_frames = 0, g_grid_resync = 0, g_grid_vb = 0;
void CpuPaceWait() {
  static const bool s_logged = [] {
    REXLOG_INFO("native: guest pacing on the frame thread (svr07_native_cpu_pace, high-fps mode): the GPU "
                "command thread no longer parks at the present's vblank wait");
    return true;
  }();
  (void)s_logged;
  const uint32_t dev = LoadBE32(LoadBE32(kDevicePtrPtr));
  auto* count = dev ? bd::mem::at<uint32_t>(dev + kVblankCount) : nullptr;
  if (!count)
    return;
  const auto read = [count] { return _byteswap_ulong(*reinterpret_cast<volatile uint32_t*>(count)); };
  // L17: 1 in the live match / fps15 (one guest vblank since the last release); hz / rate in stock-rate scenes.
  const uint32_t n = uint32_t(std::clamp(svr07_fps::PaceVblanks(), 1, 32));
  const double hz = REXCVAR_GET(svr07_high_fps_hz);
  using SC = std::chrono::steady_clock;
  if (n > 1) {
    // L17b: stock-rate scenes pace on a fixed wall-clock grid (n / hz per frame, the next target = the last target
    // + period), not on "n guest vblanks since the last release": fps16 entrances ran 28.8-29.3 fps (step 2.0,
    // 0.96-0.98 speed) because a late release restarted the count and the emulated vblank runs a little under hz.
    // A frame more than one period late resyncs the grid (that frame is slow, as on the console).
    const auto period = std::chrono::duration_cast<SC::duration>(std::chrono::duration<double>(n / hz));
    auto now = SC::now();
    if (g_grid_next == SC::time_point{} || now > g_grid_next + period) {
      if (g_grid_next != SC::time_point{})
        ++g_grid_resync;
      g_grid_next = now;
    }
    const auto target = g_grid_next;
    while (target - now > std::chrono::microseconds(1500)) {
      ThrottleWait(int32_t(std::chrono::duration_cast<std::chrono::microseconds>(target - now).count() - 1000));
      now = SC::now();
    }
    while (SC::now() < target)
      YieldProcessor();
    g_grid_next = target + period;
    ++g_grid_frames;
    if (g_grid_log == SC::time_point{})
      g_grid_log = target, g_grid_vb = read();
    else if (target - g_grid_log >= std::chrono::seconds(10)) {
      const double s = std::chrono::duration<double>(target - g_grid_log).count();
      REXLOG_INFO("[npace] stock-rate grid: {:.2f} fps over {:.1f} s ({} frames, {} resyncs, n {}); guest vblanks "
                  "{:.1f}/s (svr07_high_fps_hz {})",
                  g_grid_frames / s, s, g_grid_frames, g_grid_resync, n, (read() - g_grid_vb) / s, hz);
      g_grid_log = target;
      g_grid_vb = read();
      g_grid_frames = g_grid_resync = 0;
    }
  } else {
    g_grid_next = {};
    g_grid_log = {};
    g_grid_frames = g_grid_resync = 0;
    if (g_pace_release_valid && read() == g_pace_release_count) {
      // At most 2.5 vblanks, so a stopped vblank (display off, load) cannot hang the frame.
      const auto deadline = SC::now() + std::chrono::microseconds(int64_t(2.5e6 / hz));
      while (read() == g_pace_release_count && SC::now() < deadline)
        YieldProcessor();
    }
  }
  g_pace_release_count = read();
  g_pace_release_valid = true;
}
}  // namespace

namespace bd::gpu {
std::string SvrPaceDescribe() {
  PaceEvent copy[kPaceEvents];
  size_t n;
  {
    std::lock_guard lock(g_pace_mutex);
    n = std::min(g_pace_n, kPaceEvents);
    for (size_t i = 0; i < n; ++i)
      copy[i] = g_pace[(g_pace_n - n + i) % kPaceEvents];
  }
  const int64_t now = PaceNow();
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    const PaceEvent& e = copy[i];
    const double at = double(e.t_ns - now) / 1e6;
    if (at < -45.0)
      continue;
    switch (e.type) {
      case 'V':
        fmt::format_to(std::back_inserter(out), " V{}@{:.2f}", e.flag ? "R" : "", at);
        break;
      case 'P':
        fmt::format_to(std::back_inserter(out), " P{}{}{}@{:.2f}", e.flag & 1 ? "L" : "W", e.arg,
                       e.flag & 2 ? "!" : "", at);
        break;
      default:
        fmt::format_to(std::back_inserter(out), " {}@{:.2f}+{:.2f}", e.type, at, e.dur_us / 1000.0);
        break;
    }
  }
  return out;
}
}  // namespace bd::gpu

// D3D swap-interval callback (see the L11 note at the top). r3 = interval << 16 | immediate threshold %.
REX_HOOK_RAW(sub_821EA938) {
  const bool force = REXCVAR_GET(svr07_native_late_flip) && REXCVAR_GET(svr07_high_fps_hz) > 60.0;
  uint32_t pace_dev = 0, pace_count = 0;
  int32_t pace_left = 0;
  if (force || REXCVAR_GET(svr07_native_perf)) {
    ++g_flip_calls;
    const uint32_t dev = LoadBE32(LoadBE32(kDevicePtrPtr));
    if (!dev) {
      ++g_flip_nodev;
    } else {
      const uint32_t interval = ctx.r3.u32 >> 16;
      // The game's own test: interval - (vblanks since the last flip) > 0 = on time (wait for the vblank).
      pace_count = LoadBE32(dev + kVblankCount);
      const int32_t left = int32_t(interval - pace_count + LoadBE32(dev + kLastFlip));
      pace_dev = dev;
      pace_left = left;
      if (left <= 0) {
        ++g_flip_late;
        if (force && (ctx.r3.u32 & 0xFFFF) < 100)
          ++g_flip_forced;
      }
    }
  }
  if (force)
    ctx.r3.u32 = (ctx.r3.u32 & 0xFFFF0000u) | 100u;  // scanline % (<= 100) is never above it: flip now
  if (!CpuPaceOn() && REXCVAR_GET(svr07_high_fps_hz) > 60.0) {
    // L17 without svr07_native_cpu_pace: the game's own vblank wait, with the interval of the game's rate at this
    // vblank rate (240 Hz: 4 for 60, 8 for 30) outside the live match. PaceVblanks() = 1 in the live match.
    const uint32_t n = uint32_t(std::clamp(svr07_fps::PaceVblanks(), 1, 32));
    if (n > 1)
      ctx.r3.u32 = (n << 16) | (ctx.r3.u32 & 0xFFFFu);
  }
  if (CpuPaceOn()) {
    // L15: never park the CP at the present's vblank wait. last_flip = count - interval makes the game's
    // own test say "late" (left = 0), and threshold 100 flips it now; the frame thread waits for the
    // vblank at Swap's throttle instead (CpuPaceWait).
    if (const uint32_t dev = LoadBE32(LoadBE32(kDevicePtrPtr)); dev) {
      const uint32_t interval = ctx.r3.u32 >> 16;
      bd::mem::store<uint32_t>(dev + kLastFlip, LoadBE32(dev + kVblankCount) - interval);
      ctx.r3.u32 = (ctx.r3.u32 & 0xFFFF0000u) | 100u;
    }
  }
  __imp__sub_821EA938(ctx, base);
  if (pace_dev && PaceOn()) {
    // L15: countdown (dev+15156) > 0 after the call = waits for a vblank; else flipped now.
    const bool waits = int32_t(LoadBE32(pace_dev + kCountdown)) > 0;
    const bool raced = LoadBE32(pace_dev + kVblankCount) != pace_count;
    PaceAdd('P', char((waits ? 0 : 1) | (raced ? 2 : 0)), pace_left, PaceNow());
  }
  if (REXCVAR_GET(svr07_native_perf)) {
    const auto now = std::chrono::steady_clock::now();
    if (g_flip_log == std::chrono::steady_clock::time_point{})
      g_flip_log = now;
    else if (now - g_flip_log >= std::chrono::seconds(5)) {
      REXLOG_INFO("[nflip] presents {} late {} flipped-at-once {} no-device {} (svr07_native_late_flip {}, hz {})",
                  g_flip_calls, g_flip_late, g_flip_forced, g_flip_nodev, force, REXCVAR_GET(svr07_high_fps_hz));
      g_flip_calls = g_flip_late = g_flip_forced = g_flip_nodev = 0;
      g_flip_log = now;
    }
  }
}

REX_HOOK_RAW(sub_821DD640) {
  const bool outer = t_swap_kick;
  t_swap_kick = static_cast<uint32_t>(ctx.lr) == kSwapKickReturn;
  __imp__sub_821DD640(ctx, base);
  t_swap_kick = outer;
}

REX_HOOK_RAW(sub_821DD328) {
  bd::gpu::SvrDiagTimer wait(bd::gpu::SvrDiag::kGuestGpuWait);
  bd::gpu::SvrZone zone(bd::gpu::SvrZoneId::kGuestWait);
  const bool outer = t_swap_wait;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  t_swap_wait = t_swap_kick && (lr == kThrottleWaitReturnA || lr == kThrottleWaitReturnB);
  const bool pace = t_swap_wait && PaceOn();
  const int64_t pace_start = pace ? PaceNow() : 0;
  __imp__sub_821DD328(ctx, base);
  if (t_swap_wait && CpuPaceOn())
    CpuPaceWait();
  if (pace)
    PaceAdd('T', 0, 0, pace_start, PaceNow() - pace_start);  // L15: every Swap throttle, even 0 ms
  t_swap_wait = outer;
}

// ---- L15 measurement hooks (svr07_native_perf; nothing changes with it off) --------------------
// Guest vblank ISR / CP interrupt dispatcher (r3 = source: 0 vblank, 1 CP INTERRUPT; r4 = device).
// Both run under the SDK's global interrupt lock, so they never interleave with sub_821EA938.
REX_HOOK_RAW(sub_821DC940) {
  if (ctx.r3.u32 != 0 || !PaceOn()) {
    __imp__sub_821DC940(ctx, base);
    return;
  }
  const uint32_t dev = ctx.r4.u32;
  const int32_t before = int32_t(LoadBE32(dev + kCountdown));
  __imp__sub_821DC940(ctx, base);
  const bool released = before > 0 && int32_t(LoadBE32(dev + kCountdown)) == 0;
  PaceAdd('V', released ? 1 : 0, 0, PaceNow());
}

// Main loop worker join (sub_82445240: after the frame's logic+render, before Present) and the CS
// enter right after it. Their waits were `game`.
REX_HOOK_RAW(sub_82424398) {
  FrameWait w(bd::gpu::SvrZoneId::kGuestJoin, 'j');
  const bool outer = t_in_join;
  t_in_join = true;
  __imp__sub_82424398(ctx, base);
  t_in_join = outer;
}
REX_HOOK_RAW(sub_82429C90) {
  FrameWait w(bd::gpu::SvrZoneId::kGuestJoin, 'j');
  __imp__sub_82429C90(ctx, base);
}

// The game's D3D polls of the CP that do not go through BlockOnFence (L9 section 3).
REX_HOOK_RAW(sub_821DC760) {  // ring space: waits until the CP read pointer leaves [r4, r4+r5)
  FrameWait w(bd::gpu::SvrZoneId::kGuestRing, 'r');
  __imp__sub_821DC760(ctx, base);
}
REX_HOOK_RAW(sub_821DC6C8) {
  FrameWait w(bd::gpu::SvrZoneId::kGuestRing, 'r');
  __imp__sub_821DC6C8(ctx, base);
}
REX_HOOK_RAW(sub_821EA078) {
  FrameWait w(bd::gpu::SvrZoneId::kGuestRing, 'r');
  __imp__sub_821EA078(ctx, base);
}

// Guest kernel waits (WaitForSingleObjectEx / WaitForMultipleObjectsEx / SleepEx wrappers).
#define SVR07_KWAIT_HOOK(name)                                         \
  REX_HOOK_RAW(name) {                                                 \
    if (t_in_join) {                                                   \
      __imp__##name(ctx, base);                                        \
      return;                                                          \
    }                                                                  \
    FrameWait w(bd::gpu::SvrZoneId::kGuestKWait, 'k');                 \
    __imp__##name(ctx, base);                                          \
  }
SVR07_KWAIT_HOOK(sub_821D19F8)
SVR07_KWAIT_HOOK(sub_82571680)
SVR07_KWAIT_HOOK(sub_821D14F8)
#undef SVR07_KWAIT_HOOK

// The poll inside BlockOnFence's loop. On Swap's throttle, after a poll that says "keep waiting",
// the host thread waits as svr07_native_throttle_wait_us says. The game's own order is kept (its
// fence test after each poll, its hang check by guest ticks), so frame order and the guest's vblank
// pacing are unchanged; only the core stops burning.
REX_HOOK_RAW(sub_821DEA60) {
  __imp__sub_821DEA60(ctx, base);
  if (t_swap_wait && ctx.r3.u32 != 0) {
    const int32_t us = REXCVAR_GET(svr07_native_throttle_wait_us);
    if (us != 0) {
      bd::gpu::SvrZone sleep_zone(bd::gpu::SvrZoneId::kGuestSleep);
      ThrottleWait(us);
    }
  }
}
