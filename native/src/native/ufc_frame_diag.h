// Frame hitch diagnostics (svr_frame_diag.cpp): time spent per frame on the work that can stall
// a frame, logged for frames slower than svr_frame_diag_ms.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace bd::gpu {

enum class SvrDiag : int {
  kPipelineCompile,  // pipelines compiled on the render thread
  kTextureMirror,    // host textures built from the game's textures
  kTextureCheck,     // per-frame change checks of bound textures (includes kTextureMirror)
  kTargetCreate,     // host render targets / surfaces created
  kGpuWait,          // waiting for the GPU to finish an earlier frame
  kAcquire,          // waiting for the swap chain image
  kDraw,             // whole draw calls on the render thread (includes kTextureCheck/kPipelineCompile)
  kHashBytes,        // bytes hashed by the texture checks (the "us" field carries bytes)
  kGuestGpuWait,     // the game's D3D waiting for the emulated GPU's fences (svr_gpu_wait_diag)
  kTargetFill,       // resolve targets filled from guest memory when first sampled (NR-12a)
  kTextureHash,      // NR-14d: full hashes inside kTextureCheck
  kMirrorCreate,     // NR-14d: inside kTextureMirror, host texture + view + descriptor
  kMirrorUpload,     // NR-14d: inside kTextureMirror, bytes copied to the upload ring
  kUploadChunk,      // NR-14d: upload ring chunks made on the frame thread (no spare ready)
  kMirrorUntile,     // NR-14d: inside kTextureMirror, tiled/linear guest bytes to host rows
  kCount
};

void SvrDiagAdd(SvrDiag what, uint64_t microseconds);
void SvrDiagEndFrame();  // once per presented frame (SvrOnFrame)

// The timers run around every draw and texture check, so they read the CPU's timestamp counter
// (a few ns) rather than the OS clock; SvrDiagEndFrame converts the ticks to time.
inline uint64_t SvrDiagTicks() {
#if defined(_M_X64) || defined(__x86_64__)
  return __rdtsc();
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
#endif
}
void SvrDiagAddTicks(SvrDiag what, uint64_t ticks);

class SvrDiagTimer {
 public:
  explicit SvrDiagTimer(SvrDiag what) : what_(what), start_(SvrDiagTicks()) {}
  ~SvrDiagTimer() { SvrDiagAddTicks(what_, SvrDiagTicks() - start_); }
  SvrDiagTimer(const SvrDiagTimer&) = delete;
  SvrDiagTimer& operator=(const SvrDiagTimer&) = delete;

 private:
  SvrDiag what_;
  uint64_t start_;
};

// NR-P1, svr07_native_perf: one [nperf] line a second saying where the frame thread's time went.
// Zones are exclusive (an inner zone stops the outer one's clock), so the numbers add up to the
// frame time by construction; kGame is whatever no zone claims. Off, a zone is one relaxed load.
enum class SvrZoneId : int {
  kGame,             // recompiled game code: simulation, scene walk, or a guest spin
  kGuestD3D,         // the game's own D3D function inside a hook (ring writes for the null GPU)
  kGuestWait,        // the game's D3D blocked on a fence (sub_8236F328)
  kGuestSleep,       // NR-P6d: the Swap throttle's host sleep (idle, like kPaceFrame)
  kHookState,        // host side of a hook with no zone of its own (shaders, targets, scissor)
  kSetTexture,       // SetTexture hook, mirror checks included
  kBindGeometry,     // SvrBindDrawGeometry: mirror lookups, samples, host setters
  kGeomHash,         // inside it: full hashes, ring copies, new host buffers
  kUnlock,           // Lock/Unlock invalidation: the g_mirrors range walk
  kUpVerts,          // BeginVertices/EndVertices data swapped and uploaded
  kDraw,             // DispatchDraw, less the zones below
  kRenderState,      // FlushRenderState: device registers read, scissor set
  kPipeline,         // pipeline hash, lookup, bind
  kShaderConstants,  // VS and PS blocks: compare, copy, swap, root descriptor
  kSharedConstants,  // the shared block: 32 slots rebuilt, compared, uploaded
  kVertexBind,       // computed-fetch streams, vertex and index buffer binds
  kDrawCall,         // the driver's draw call
  kResolve,          // UfcResolveObserved
  kClear,            // Video::Clear
  kPresentRecord,    // Video::Present, less the zones below
  kSubmit,           // ExecuteCommandLists
  kPresentCall,      // swap chain present
  kFenceWait,        // waiting for the frame slot's GPU fence
  kDrainSlot,        // deferred frees
  kPaceFrame,        // the host's 60 fps sleep
  kOnFrame,          // SvrOnFrame: counters, log lines
  // L15 (reports/L15-fps-spike-game-side.md): guest waits that used to count as `game`.
  kGuestJoin,        // main loop: worker join sub_82424398 + its CS enter sub_82429C90
  kGuestRing,        // D3D CP polls outside BlockOnFence: sub_821DC760 (ring space), sub_821DC6C8, sub_821EA078
  kGuestKWait,       // guest kernel waits: WaitForSingleObjectEx / WaitForMultipleObjectsEx / SleepEx wrappers
  kUpBatch,          // L16: UP quad batching, the merge test and the appended vertices
  kCount
};

enum class SvrCount : int {
  kUpDraws,         // draws from BeginVertices data
  kCbBlocks,        // VS/PS uploads actually made
  kSharedUploads,   // shared blocks actually uploaded
  kPipelineBinds,
  kResolves,
  kGeomHashBytes,   // full hashes of vertex and index buffers
  kGeomRingBytes,   // dynamic buffers copied to the upload ring
  kGeomNewBuffers,  // host buffers created for changed content
  kGeomNewBytes,
  kUpBytes,
  kUnlocks,
  kUnlockWalked,    // g_mirrors nodes walked by those unlocks
  kLockMisses,      // state mutex or g_mirrors mutex found taken on the draw path
  kCbRegisters,     // float registers those uploads stored
  kSharedSlots,     // texture slots rebuilt for the shared block
  kGeomResolves,    // mirror resolves (per stream and index buffer, per draw)
  kGeomSamples,     // sample checks on the trusted path
  kGeomTrusted,     // first uses in a frame served without a full hash
  kGeomHashSmallBytes,     // part of kGeomHashBytes: buffers of at most 1 KB
  kGeomHashUnlockedBytes,  // part of kGeomHashBytes: forced by an Unlock
  kGeomTrustMisses,        // svr07_native_check_geometry: what the trusted path would have missed
  // L16 UP quad batching (svr07_native_up_batch): BeginVertices draws appended to the draw before
  // them (no host draw of their own), drawn on the fast path (constants only), and why the others
  // took the full path, by the first test that failed.
  kUpMerged,       // upm: merged into the held draw
  kUpBreakStream,  // upxq: no held draw (a command or another draw came between, or none yet)
  kUpBreakType,    // upxt: primitive type, stride, count or declaration not batchable / differs
  kUpBreakState,   // upxs: shaders, targets, textures, device registers, viewport or dirty state differ
  kUpFast,         // upf: only VS/PS float constants differ: constants bound + one draw, no full flush
  kUpBreakShared,  // upxh: shared block (fetch constants, bools, slots) differs
  kUpBreakVid,     // upxv: the vertex shader may read the vertex index (or computed fetch)
  kUpBreakRoom,    // upxr: the held draw's vertex reserve is full (or none)
  kIdxFast,        // ixf: fps13 DrawIndexedVertices fast path (constants + buffers + draw)
  kIdxFull,        // ixn: plain DrawIndexedVertices that took the full path while it was on
  kIdxNoHeld,      // ixq: no held draw (another command came between, or none)
  kIdxSig,         // ixs: shaders, declaration, targets, viewport words or register shadow differ
  kIdxPipeline,    // ixp: bound pipeline differs (shaders, topology, vertex strides)
  kIdxOther,       // ixo: dirty state, computed fetch, index range, framebuffer
  kCount
};

extern std::atomic<bool> g_svr_perf_on;

// L15: true on the thread that presents (the zones' frame thread).
bool SvrIsFrameThread();
// L15: svr07_gpu_wait_diag.cpp: the guest pacing timeline (vblanks, present decisions, throttle,
// guest waits) of the last ~45 ms, for the [npace] line after an [nspike]. "" when not recorded.
std::string SvrPaceDescribe();

int SvrZoneEnter(SvrZoneId id);
void SvrZoneLeave(int previous);
void SvrZoneSwitch(SvrZoneId id);
void SvrPerfCountAdd(SvrCount what, uint64_t n);
void SvrPerfGpuTime(uint64_t nanoseconds);
// RR-NR-10: one presented frame's time on the display (DXGI frame statistics: refreshes between it
// and the frame before, times the refresh period), and the refresh period itself.
void SvrPerfDisplayFrame(double interval_ms, double refresh_ms);  // one frame slot's GPU time, from its timestamps
void SvrPerfRenderFn(uint64_t ticks);       // one call of the game's render function
// NR-15b: the adapter the renderer draws on (PCI vendor id); the NVML columns are read only for
// an NVIDIA one, since asking NVML about a sleeping NVIDIA card stalls for ~400 ms.
void SvrPerfRenderAdapter(uint32_t vendor_id);

inline void SvrPerfCount(SvrCount what, uint64_t n) {
  if (g_svr_perf_on.load(std::memory_order_relaxed))
    SvrPerfCountAdd(what, n);
}

class SvrZone {
 public:
  explicit SvrZone(SvrZoneId id) : on_(g_svr_perf_on.load(std::memory_order_relaxed)) {
    if (on_)
      previous_ = SvrZoneEnter(id);
  }
  ~SvrZone() {
    if (on_)
      SvrZoneLeave(previous_);
  }
  SvrZone(const SvrZone&) = delete;
  SvrZone& operator=(const SvrZone&) = delete;
  // The rest of this scope belongs to another zone.
  void Switch(SvrZoneId id) {
    if (on_)
      SvrZoneSwitch(id);
  }

 private:
  bool on_;
  int previous_ = 0;
};

// frame_ring.cpp: a timestamp at the end of the slot's command list (the start is written when
// the list opens).
void SvrGpuTimeEnd(uint32_t slot);

}  // namespace bd::gpu
