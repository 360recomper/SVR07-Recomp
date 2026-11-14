// svr07 - L21 frame-tied stepper finder (reports/L21-fps-stepper-finder.md). Test only, everything off by default.
//
// Port of UFC 3's NR-F1 tools (UFC-fps srcnim_probe.h and anim_step_trace.h, same author/project
// family; FINDINGS-fps-unlock.md 2n-2q) to the SvR07 native tree:
//
//  [probe]     memory probe. At a trigger (seconds since the first main-loop frame, a main-loop frame number, or a
//              scene the game settled in: menu / entrance / load / prematch / match / pause / postmatch):
//                1. snapshot every committed, readable guest range below 0xC0000000 (0xC0000000+ alias physical);
//                2. next frame: keep 4-byte aligned floats that moved by one of the fixed steps (+-1/60, 1/30, ...),
//                   or (mode any) by any constant amount, or (mode ratio) by a constant factor x *= k;
//                3. confirm_frames more frames: keep only those that moved the same way every frame;
//                4. log the survivors, then x64 hardware write watchpoints (DR0-DR3) on up to 4 of them for
//                   watch_frames frames: the writing host code -> containing guest function sub_XXXXXXXX (+ the
//                   guest PC from the generated source when the PDB and generated/ are there), writes and frames hit.
//              A system stepped by the game's step (block +0x30) moves by ticks/60 per frame (1/240 at 240 Hz in the
//              live match), so it never matches 1/60: what matches is frame-tied.
//  [steptrace] up to 4 guest functions (svr07_steptrace_funcs, addresses) get hardware execute breakpoints: per
//              function the (caller lr, step argument) pairs and counts, FIXED when the argument is one of the
//              fixed per-frame constants while the frame's ticks != 1, and the guest + host call chains the first
//              times a fixed value arrives. Execute breakpoints see only real calls: a caller the compiler inlined
//              the function into is not seen (use a REX_HOOK_RAW for that function then).
//
// Inert when off: svr07_probe_frame() parses the cvars once, and with no trigger and no function list it returns
// on one bool test; no handler, no debug registers, no memory reads. svr07_probe_jitter() returns 1.0.
//
// Included once, at the end of svr07_fps.cpp (uses its scene state: g_match_layout, g_entrance_open, flow/motion
// frames, the event player pointer, the frame block).

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <vector>

#include <rex/ppc/context.h>

#pragma comment(lib, "dbghelp.lib")

REXCVAR_DEFINE_STRING(svr07_anim_probe_at, "", "SvR",
                      "Test (L21): comma list of seconds since the first frame; at each, find guest floats stepped by "
                      "a fixed amount every frame and log the code that writes them ([probe])")
    .debug_only();
REXCVAR_DEFINE_STRING(svr07_anim_probe_frames, "", "SvR",
                      "Test (L21): comma list of main-loop frame numbers to probe at (as svr07_anim_probe_at)")
    .debug_only();
REXCVAR_DEFINE_STRING(svr07_anim_probe_scenes, "", "SvR",
                      "Test (L21): probe when the game has stayed svr07_anim_probe_scene_delay_s in one of these "
                      "scenes: comma list of menu,entrance,load,prematch,match,pause,postmatch or all")
    .debug_only();
REXCVAR_DEFINE_DOUBLE(svr07_anim_probe_scene_delay_s, 4.0, "SvR",
                      "Test (L21): seconds a scene must last before a scene trigger fires (probe and step trace)")
    .debug_only();
REXCVAR_DEFINE_INT32(svr07_anim_probe_scene_max, 2, "SvR",
                     "Test (L21): scene-triggered probes per scene kind per run (e.g. 2 = both entrances)")
    .debug_only();
REXCVAR_DEFINE_STRING(svr07_anim_probe_mode, "fixed", "SvR",
                      "Test (L21): fixed = floats stepped +-one of svr07_anim_probe_steps per frame; any = any "
                      "constant change per frame (use with svr07_anim_probe_jitter); ratio = x *= k per frame "
                      "(decays / smoothers that ignore the step)")
    .debug_only();
REXCVAR_DEFINE_STRING(svr07_anim_probe_steps, "1/60,1/30,0.5,2,1", "SvR",
                      "Test (L21): fixed per-frame steps to look for (fractions allowed; both signs are matched). "
                      "The order is the watch priority")
    .debug_only();
REXCVAR_DEFINE_INT32(svr07_anim_probe_watches, 4, "SvR",
                     "Test (L21): write watchpoints per probe (1..4; fewer while the step trace holds debug registers)")
    .debug_only();
REXCVAR_DEFINE_INT32(svr07_anim_probe_watch_frames, 120, "SvR", "Test (L21): frames the write watchpoints stay on")
    .debug_only();
REXCVAR_DEFINE_INT32(svr07_anim_probe_confirm_frames, 5, "SvR",
                     "Test (L21): frames a candidate must keep the same step after the first diff")
    .debug_only();
REXCVAR_DEFINE_INT32(svr07_anim_probe_count_every, 30, "SvR",
                     "Test (L21): during the watch, log the writes per watched address every N frames (0 = off)")
    .debug_only();
REXCVAR_DEFINE_BOOL(svr07_anim_probe_log, true, "SvR",
                    "Test (L21): list the surviving candidates (address=value) per step; false = counts and writers "
                    "only")
    .debug_only();
REXCVAR_DEFINE_INT32(svr07_anim_probe_list, 40, "SvR", "Test (L21): candidates listed per step (any/ratio: x3)")
    .debug_only();
REXCVAR_DEFINE_DOUBLE(svr07_anim_probe_jitter, 0.0, "SvR",
                      "Test (L21): while a probe compares frames in the live high-fps match, the written step "
                      "alternates x(1-j) / x(1+j) (same average): step-scaled values change unevenly and drop out, "
                      "frame-tied ones stay even. 0 = off; 0.2 is a good value")
    .debug_only();
REXCVAR_DEFINE_STRING(svr07_steptrace_funcs, "", "SvR",
                      "Test (L21): comma list of up to 4 guest function addresses (hex, e.g. 8225AB08); logs their "
                      "callers and step arguments ([steptrace]) via hardware execute breakpoints")
    .debug_only();
REXCVAR_DEFINE_STRING(svr07_steptrace_arg, "f1", "SvR",
                      "Test (L21): the argument register logged per call: f1..f8 (float) or r3..r10 (int)")
    .debug_only();
REXCVAR_DEFINE_STRING(svr07_steptrace_fixed, "1,2,0.5,1/60,1/30", "SvR",
                      "Test (L21): argument values counted as FIXED per-frame constants (when the frame's ticks != 1)")
    .debug_only();
REXCVAR_DEFINE_STRING(svr07_steptrace_at, "", "SvR",
                      "Test (L21): comma list of seconds to start a step trace window; empty with empty "
                      "svr07_steptrace_scenes = trace from the first frame to the end")
    .debug_only();
REXCVAR_DEFINE_STRING(svr07_steptrace_scenes, "", "SvR",
                      "Test (L21): start a step trace window in these scenes (as svr07_anim_probe_scenes)")
    .debug_only();
REXCVAR_DEFINE_DOUBLE(svr07_steptrace_secs, 10.0, "SvR", "Test (L21): length of one step trace window (seconds)")
    .debug_only();
REXCVAR_DEFINE_DOUBLE(svr07_steptrace_report_s, 5.0, "SvR",
                      "Test (L21): seconds between [steptrace] count lines while a window runs")
    .debug_only();
REXCVAR_DEFINE_INT32(svr07_steptrace_stacks, 3, "SvR",
                     "Test (L21): call chains logged per function when a FIXED value arrives (0 = none)")
    .debug_only();

namespace svr07_probe {
namespace {

using PClock = std::chrono::steady_clock;

// ---------------------------------------------------------------- shared: scenes, parsing, symbols, debug regs

enum Scene { kMenu, kEntrance, kLoad, kPrematch, kMatch, kPause, kPostmatch, kScenes };
const char* const kSceneName[kScenes] = {"menu", "entrance", "load", "prematch", "match", "pause", "postmatch"};

// The scene the frame is in, from the L17/L20 scene signals (tracked by svr07_fps.cpp whatever the fps cvars).
Scene CurrentScene(uint8_t* base) {
  using namespace svr07_fps;
  const uint64_t f = g_frame_no.load(std::memory_order_relaxed);
  auto stale = [f](const std::atomic<uint64_t>& seen) { return f - seen.load(std::memory_order_relaxed) > 3; };
  if (g_entrance_open.load(std::memory_order_relaxed)) return kEntrance;
  if (!g_match_layout.load(std::memory_order_relaxed)) return kMenu;
  if (Load32(base, kEventObjPtr) == 0) return kLoad;
  if (stale(g_flow_frame)) return kPause;
  const uint32_t st = g_flow_state.load(std::memory_order_relaxed);
  if (st < 18) return kPrematch;
  if (st > 18) return kPostmatch;
  if (stale(g_motion_frame)) return kPause;
  return kMatch;
}

std::vector<std::string> SplitList(const std::string& s) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= s.size()) {
    size_t q = s.find(',', p);
    if (q == std::string::npos) q = s.size();
    std::string t = s.substr(p, q - p);
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    if (!t.empty()) out.push_back(t);
    p = q + 1;
  }
  return out;
}

// "1/60" -> 0.016667, "0.5" -> 0.5
double ParseNumber(const std::string& t) {
  const size_t slash = t.find('/');
  if (slash == std::string::npos) return std::atof(t.c_str());
  const double d = std::atof(t.substr(slash + 1).c_str());
  return d != 0 ? std::atof(t.substr(0, slash).c_str()) / d : 0.0;
}

uint32_t SceneMask(const std::string& s) {
  uint32_t m = 0;
  for (const std::string& t : SplitList(s)) {
    if (t == "all") return (1u << kScenes) - 1;
    for (int i = 0; i < kScenes; ++i)
      if (t == kSceneName[i]) m |= 1u << i;
  }
  return m;
}

// Guest function starts (PPCFuncMappings, sorted) and their host entry points, for naming.
struct FuncTable {
  std::vector<uint32_t> guest;               // sorted
  std::vector<std::pair<uint64_t, uint32_t>> host;  // host entry -> guest, sorted by host
  bool built = false;
} g_funcs;

uint64_t FollowJump(uint64_t p) {
  // incremental-link thunk (jmp rel32): the real body
  const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
  if (b[0] == 0xE9) {
    int32_t rel;
    std::memcpy(&rel, b + 1, 4);
    return p + 5 + int64_t(rel);
  }
  return p;
}

void BuildFuncs() {
  if (g_funcs.built) return;
  g_funcs.built = true;
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
    g_funcs.guest.push_back(uint32_t(m->guest));
    g_funcs.host.push_back({FollowJump(reinterpret_cast<uint64_t>(m->host)), uint32_t(m->guest)});
  }
  std::sort(g_funcs.guest.begin(), g_funcs.guest.end());
  std::sort(g_funcs.host.begin(), g_funcs.host.end());
}

// The guest function holding guest address `pc` (greatest start <= pc).
uint32_t GuestFuncOf(uint32_t pc) {
  BuildFuncs();
  auto it = std::upper_bound(g_funcs.guest.begin(), g_funcs.guest.end(), pc);
  return it == g_funcs.guest.begin() ? 0 : *(it - 1);
}

uint64_t HostEntryOf(uint32_t guest_fn) {
  BuildFuncs();
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m)
    if (uint32_t(m->guest) == guest_fn) return FollowJump(reinterpret_cast<uint64_t>(m->host));
  return 0;
}

// DbgHelp on the exe's own PDB (svr07.pdb beside the exe, as svr_selftest_native.py copies it), private session.
HANDLE const kSymSession = reinterpret_cast<HANDLE>(uintptr_t(0x5E070021));
int g_sym_state = 0;  // 0 not tried, 1 ok, -1 failed
bool SymReady() {
  if (g_sym_state) return g_sym_state > 0;
  g_sym_state = -1;
  wchar_t exe[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
  std::wstring dir(exe);
  dir = dir.substr(0, dir.find_last_of(L"\\/"));
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
  if (!SymInitializeW(kSymSession, dir.c_str(), FALSE)) {
    REXLOG_INFO("[probe] symbols: SymInitialize failed ({}); writers logged as svr07.exe+rva", GetLastError());
    return false;
  }
  const auto* mod = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(mod + dos->e_lfanew);
  if (!SymLoadModuleExW(kSymSession, nullptr, exe, nullptr, DWORD64(mod), nt->OptionalHeader.SizeOfImage, nullptr,
                        0)) {
    REXLOG_INFO("[probe] symbols: module load failed ({})", GetLastError());
    return false;
  }
  IMAGEHLP_MODULEW64 info{};
  info.SizeOfStruct = sizeof(info);
  if (!SymGetModuleInfoW64(kSymSession, DWORD64(mod), &info) || info.SymType != SymPdb) {
    REXLOG_INFO("[probe] symbols: no matching svr07.pdb (put it beside the exe); writers logged as svr07.exe+rva and "
                "by the function table");
    return false;
  }
  g_sym_state = 1;
  return true;
}

// Generated source lines, cached per file (guest PC from a host line: count the instruction comments).
std::map<std::string, std::vector<std::string>> g_src;
const std::vector<std::string>* SourceLines(const std::string& path) {
  auto it = g_src.find(path);
  if (it != g_src.end()) return it->second.empty() ? nullptr : &it->second;
  std::vector<std::string>& v = g_src[path];
  std::ifstream in(path);
  for (std::string l; std::getline(in, l);) v.push_back(std::move(l));
  return v.empty() ? nullptr : &v;
}

// Guest PC for line `line` (1-based) of a generated svr07_recomp.N.cpp: from the nearest DEFINE_REX_FUNC(sub_X) or
// loc_X: label above it, + 4 per "\t// <instruction>" comment in between (the comment precedes its code).
uint32_t GuestPcFromSource(const std::string& path, int line) {
  const auto* v = SourceLines(path);
  if (!v || line < 1 || line > int(v->size())) return 0;
  int comments = 0;
  for (int i = line - 1; i >= 0; --i) {
    const std::string& l = (*v)[i];
    if (l.size() > 3 && l[0] == '\t' && l[1] == '/' && l[2] == '/' && l[3] == ' ') {
      ++comments;
      continue;
    }
    const char* hex = l.rfind("loc_", 0) == 0                    ? l.c_str() + 4
                      : l.rfind("DEFINE_REX_FUNC(sub_", 0) == 0 ? l.c_str() + 20
                                                                 : nullptr;
    if (hex)
      if (const uint32_t addr = uint32_t(std::strtoul(hex, nullptr, 16)); addr)
        return addr + 4 * (comments - 1 > 0 ? comments - 1 : 0);
  }
  return 0;
}

// "sub_8225AB08+0x1A4 ~pc 8225AC10 (svr07_recomp.70.cpp:1234)" / "svr07.exe+0x123456 in sub_8225AB08 (table)".
std::string NameHost(uint64_t rip, bool after_write) {
  const uint64_t exe = uint64_t(GetModuleHandleW(nullptr));
  std::string out = fmt::format("svr07.exe+0x{:X}", rip - exe);
  // The containing host function: exact guest name when its entry is a table entry.
  DWORD64 image = 0;
  if (const RUNTIME_FUNCTION* fe = RtlLookupFunctionEntry(rip, &image, nullptr)) {
    const uint64_t begin = image + fe->BeginAddress;
    BuildFuncs();
    auto it = std::lower_bound(g_funcs.host.begin(), g_funcs.host.end(), std::make_pair(begin, uint32_t(0)));
    if (it != g_funcs.host.end() && it->first == begin) out += fmt::format(" in sub_{:08X}", it->second);
  }
  if (!SymReady()) return out;
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 255;
  DWORD64 disp = 0;
  if (SymFromAddr(kSymSession, rip, &disp, sym)) out += fmt::format(" = {}+0x{:X}", sym->Name, disp);
  IMAGEHLP_LINE64 line{};
  line.SizeOfStruct = sizeof(line);
  DWORD ldisp = 0;
  // A data watchpoint reports the instruction after the store: look one byte back for its line.
  if (SymGetLineFromAddr64(kSymSession, after_write ? rip - 1 : rip, &ldisp, &line) && line.FileName) {
    std::string file = line.FileName;
    const size_t slash = file.find_last_of("\\/");
    const std::string shortname = slash == std::string::npos ? file : file.substr(slash + 1);
    if (const uint32_t pc = GuestPcFromSource(file, int(line.LineNumber)); pc)
      out += fmt::format(" ~pc {:08X}", pc);
    out += fmt::format(" ({}:{})", shortname, line.LineNumber);
  }
  return out;
}

std::string NameGuest(uint32_t pc) {
  const uint32_t fn = GuestFuncOf(pc);
  return fn ? fmt::format("{:08X} (sub_{:08X}+0x{:X})", pc, fn, pc - fn) : fmt::format("{:08X}", pc);
}

// Debug register slots, shared: the step trace takes slots from 0 up, the probe's write watches the rest.
enum SlotKind : int { kSlotOff = 0, kSlotWrite = 1, kSlotExec = 2 };
std::atomic<int> g_slot_kind[4];
uint64_t g_slot_host[4] = {};
PVOID g_veh = nullptr;

void ApplyDebugRegs() {
  uint64_t addr[4];
  int kind[4];
  for (int i = 0; i < 4; ++i) {
    addr[i] = g_slot_host[i];
    kind[i] = g_slot_kind[i].load();
  }
  std::thread([&] {
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof(te)};
    int threads = 0, on = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
      HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
      if (!t) continue;
      if (SuspendThread(t) != DWORD(-1)) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
          uint64_t dr7 = 0;
          DWORD64* drs[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
          on = 0;
          for (int i = 0; i < 4; ++i) {
            *drs[i] = kind[i] ? addr[i] : 0;
            if (kind[i] == kSlotWrite) dr7 |= (1ull << (2 * i)) | (1ull << (16 + 4 * i)) | (3ull << (18 + 4 * i));
            if (kind[i] == kSlotExec) dr7 |= 1ull << (2 * i);  // RW 00 (execute), LEN 00
            on += kind[i] ? 1 : 0;
          }
          c.Dr7 = dr7;
          c.Dr6 = 0;
          if (SetThreadContext(t, &c)) ++threads;
        }
        ResumeThread(t);
      }
      CloseHandle(t);
    }
    CloseHandle(snap);
    REXLOG_INFO("[probe] debug registers: {} slot(s) on, {} threads", on, threads);
  }).join();
}

// ---------------------------------------------------------------- probe state

struct Range {
  uint32_t va, size;
  size_t offset;
};
struct Cand {
  uint32_t va;
  uint8_t step;  // index into g_steps, bit 7 = negative; 0x7F = any / ratio
};
constexpr uint8_t kAnyStep = 0x7F;
constexpr size_t kMaxCandidates = 4u << 20;

struct Probe {
  bool on = false;
  std::vector<double> times;
  std::vector<uint64_t> frames;
  size_t next_time = 0, next_frame = 0;
  uint32_t scene_mask = 0;
  int scene_count[kScenes] = {};
  bool scene_req = false;
  int mode = 0;  // 0 fixed, 1 any, 2 ratio
  std::vector<float> steps;
  std::vector<std::string> step_names;
  int phase = 0;  // 0 idle, 1 snapshotted, 2 confirming, 3 watching
  int frames_in_phase = 0;
  int index = 0;
  std::string label;
  std::vector<Range> ranges;
  std::vector<uint8_t> snap;
  std::vector<Cand> cands;
  std::vector<float> prev, delta;
  std::vector<uint32_t> watched_objs;
  int watch_slots[4] = {};
  uint32_t watch_va[4] = {};
  int watch_count = 0;
  bool jitter_active = false;
} g_probe;

struct Hit {
  std::atomic<uint64_t> rip{0};
  std::atomic<uint32_t> n{0};
  std::atomic<uint32_t> slot_bits{0};
  std::atomic<uint64_t> last_frame{0};
  std::atomic<uint32_t> frames{0};
};
Hit g_hits[256];
std::atomic<uint32_t> g_slot_writes[4];
std::atomic<uint64_t> g_frame{0};  // probe frames (main loop), read by the handler
uint8_t* g_base = nullptr;

// ---------------------------------------------------------------- step trace state

struct TraceHit {
  std::atomic<uint64_t> key{0};  // bit 61 set | slot << 62 | (lr - 0x82000000) << 32 | arg bits
  std::atomic<uint32_t> n{0};
  std::atomic<uint32_t> fixed{0};  // calls with a FIXED value while ticks != 1
  uint32_t reported = 0, reported_fixed = 0;  // main thread
};
constexpr int kTraceSlots = 2048;
TraceHit g_trace_hits[kTraceSlots];
std::atomic<uint32_t> g_trace_overflow{0};

struct Stack {
  std::atomic<int> state{0};  // 0 free, 1 writing, 2 ready, 3 printed
  int slot = 0;
  double arg = 0;
  float ticks = 0;
  uint32_t guest[12] = {};
  uint64_t host[24] = {};
};
constexpr int kStacks = 32;
Stack g_stacks[kStacks];
std::atomic<int> g_stacks_left[4];

struct Trace {
  bool on = false;
  uint32_t fn[4] = {};
  int count = 0;
  bool arg_float = true;
  int arg_reg = 1;
  std::vector<double> fixed_vals;
  std::vector<double> times;
  size_t next_time = 0;
  uint32_t scene_mask = 0;
  int scene_count[kScenes] = {};
  bool scene_req = false;
  bool continuous = false;
  bool armed = false;
  double armed_at = 0, last_report = 0;
  std::string label;
} g_trace;
std::atomic<bool> g_trace_armed{false};

bool IsFixedArg(double a) {
  for (double v : g_trace.fixed_vals)
    if (a == double(float(v)) || a == v) return true;
  return false;
}

double ReadArg(const PPCContext* c) {
  if (g_trace.arg_float) {
    const PPCRegister* f[] = {&c->f1, &c->f2, &c->f3, &c->f4, &c->f5, &c->f6, &c->f7, &c->f8};
    return f[std::clamp(g_trace.arg_reg, 1, 8) - 1]->f64;
  }
  const PPCRegister* r[] = {&c->r3, &c->r4, &c->r5, &c->r6, &c->r7, &c->r8, &c->r9, &c->r10};
  return double(r[std::clamp(g_trace.arg_reg, 3, 10) - 3]->s32);
}

bool GuestReadable(uint32_t va) {
  if (va < 0x00010000 || va >= 0xC0000000u || !g_base) return false;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(g_base + va, &mbi, sizeof(mbi))) return false;
  return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}

void TraceCall(int slot, EXCEPTION_POINTERS* e) {
  const auto* c = reinterpret_cast<const PPCContext*>(e->ContextRecord->Rcx);
  if (!c) return;
  const uint32_t lr = uint32_t(c->lr);
  const double arg = ReadArg(c);
  const float ticks = g_base ? svr07_fps::LoadF32(g_base, svr07_fps::kStep) : 1.0f;
  const bool fixed = ticks != 1.0f && IsFixedArg(arg);
  const uint32_t argbits =
      g_trace.arg_float ? uint32_t(int32_t(std::lround(std::clamp(arg, -2e5, 2e5) * 1e4))) : uint32_t(int32_t(arg));
  const uint64_t key = (1ull << 61) | (uint64_t(slot) << 62) | (uint64_t((lr - 0x82000000u) & 0x1FFFFFFF) << 32) | argbits;
  const uint32_t h = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 53) & (kTraceSlots - 1);
  bool stored = false;
  for (uint32_t i = 0; i < 64; ++i) {
    TraceHit& s = g_trace_hits[(h + i) & (kTraceSlots - 1)];
    uint64_t cur = s.key.load(std::memory_order_relaxed);
    if (cur == 0 && s.key.compare_exchange_strong(cur, key)) cur = key;
    if (cur == key) {
      s.n.fetch_add(1, std::memory_order_relaxed);
      if (fixed) s.fixed.fetch_add(1, std::memory_order_relaxed);
      stored = true;
      break;
    }
  }
  if (!stored) g_trace_overflow.fetch_add(1, std::memory_order_relaxed);
  if (!fixed || g_stacks_left[slot].load(std::memory_order_relaxed) <= 0) return;
  if (g_stacks_left[slot].fetch_sub(1) <= 0) return;
  for (Stack& st : g_stacks) {
    int expect = 0;
    if (!st.state.compare_exchange_strong(expect, 1)) continue;
    st.slot = slot;
    st.arg = arg;
    st.ticks = ticks;
    // Guest back chain: [sp] = the caller's frame, its return address at [frame - 8] (mflr r12 / stw r12,-8(r1)).
    st.guest[0] = lr;
    uint32_t sp = c->r1.u32;
    for (int d = 1; d < 12; ++d) {
      if ((sp & 3) || !GuestReadable(sp)) break;
      const uint32_t back = svr07_fps::Load32(g_base, sp);
      if (back <= sp || back - sp > 0x100000 || !GuestReadable(back - 8)) break;
      st.guest[d] = svr07_fps::Load32(g_base, back - 8);
      sp = back;
    }
    // Host chain (generated functions = guest functions) by unwinding a copy of the faulting context.
    CONTEXT cc = *e->ContextRecord;
    for (int d = 0; d < 24; ++d) {
      st.host[d] = cc.Rip;
      DWORD64 image = 0;
      const RUNTIME_FUNCTION* fe = RtlLookupFunctionEntry(cc.Rip, &image, nullptr);
      if (!fe) {
        if (!cc.Rsp) break;
        cc.Rip = *reinterpret_cast<DWORD64*>(cc.Rsp);
        cc.Rsp += 8;
      } else {
        PVOID hd = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, cc.Rip, const_cast<RUNTIME_FUNCTION*>(fe), &cc, &hd, &frame,
                         nullptr);
      }
      if (!cc.Rip) break;
    }
    st.state.store(2);
    break;
  }
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* e) {
  if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  const uint64_t dr6 = e->ContextRecord->Dr6;
  if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
  bool any_write = false;
  for (int i = 0; i < 4; ++i) {
    if (!(dr6 & (1ull << i))) continue;
    const int kind = g_slot_kind[i].load(std::memory_order_relaxed);
    if (kind == kSlotExec) {
      TraceCall(i, e);
      e->ContextRecord->EFlags |= 0x10000;  // RF: run the instruction the execute breakpoint stopped at
    } else if (kind == kSlotWrite) {
      g_slot_writes[i].fetch_add(1, std::memory_order_relaxed);
      any_write = true;
    } else {
      e->ContextRecord->EFlags |= 0x10000;  // a slot that was just cleared (stale DR on this thread)
    }
  }
  if (any_write) {
    const uint64_t rip = uint64_t(e->ExceptionRecord->ExceptionAddress);
    const uint32_t h = uint32_t((rip * 0x9E3779B97F4A7C15ull) >> 56);
    const uint64_t f = g_frame.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < 256; ++i) {
      Hit& slot = g_hits[(h + i) & 255];
      uint64_t cur = slot.rip.load(std::memory_order_relaxed);
      if (cur == 0 && slot.rip.compare_exchange_strong(cur, rip)) cur = rip;
      if (cur == rip) {
        slot.n.fetch_add(1, std::memory_order_relaxed);
        slot.slot_bits.fetch_or(uint32_t(dr6 & 0xF), std::memory_order_relaxed);
        if (slot.last_frame.exchange(f, std::memory_order_relaxed) != f)
          slot.frames.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
  e->ContextRecord->Dr6 = 0;
  return EXCEPTION_CONTINUE_EXECUTION;
}

void EnsureHandler() {
  if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Handler);
}

// ---------------------------------------------------------------- probe

std::string StepName(uint8_t s) {
  if (s == kAnyStep) return g_probe.mode == 2 ? "ratio" : "any";
  return ((s & 0x80) ? "-" : "+") + g_probe.step_names[s & 0x7F];
}

int MatchStep(float d) {
  for (size_t i = 0; i < g_probe.steps.size(); ++i) {
    const float k = g_probe.steps[i];
    if (std::fabs(d - k) <= k * 1e-3f) return int(i);
    if (std::fabs(d + k) <= k * 1e-3f) return int(i) | 0x80;
  }
  return -1;
}

bool Excluded(uint32_t va) {
  // the frame block (written by svr07_fps.cpp's step writer)
  return va >= svr07_fps::kBlock && va < svr07_fps::kBlock + 0x40;
}

void Snapshot() {
  auto& p = g_probe;
  p.ranges.clear();
  p.snap.clear();
  size_t total = 0;
  for (uint64_t va = 0x00010000; va < 0xC0000000ull;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(g_base + va, &mbi, sizeof(mbi))) break;
    const uint64_t region_end = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - g_base) + mbi.RegionSize;
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                          (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                          PAGE_EXECUTE_READWRITE));
    if (readable) {
      const uint32_t start = uint32_t(va);
      const uint32_t end = uint32_t(std::min<uint64_t>(region_end, 0xC0000000ull));
      p.ranges.push_back({start, end - start, total});
      total += end - start;
    }
    va = std::max<uint64_t>(region_end, va + 0x1000);
  }
  p.snap.resize(total);
  for (const Range& r : p.ranges) std::memcpy(p.snap.data() + r.offset, g_base + r.va, r.size);
  REXLOG_INFO("[probe] #{} snapshot {} ranges, {} MB", p.index, p.ranges.size(), total >> 20);
}

void FirstDiff() {
  auto& p = g_probe;
  p.cands.clear();
  p.delta.clear();
  for (const Range& r : p.ranges) {
    const uint8_t* now = g_base + r.va;
    const uint8_t* was = p.snap.data() + r.offset;
    for (uint32_t o = 0; o + 4 <= r.size; o += 4) {
      if (std::memcmp(now + o, was + o, 4) == 0) continue;
      if (p.cands.size() >= kMaxCandidates) break;
      const uint32_t va = r.va + o;
      if (Excluded(va)) continue;
      uint32_t wb, nb;
      std::memcpy(&wb, was + o, 4);
      std::memcpy(&nb, now + o, 4);
      wb = _byteswap_ulong(wb);
      nb = _byteswap_ulong(nb);
      float fa, fb;
      std::memcpy(&fa, &wb, 4);
      std::memcpy(&fb, &nb, 4);
      if (!std::isfinite(fa) || !std::isfinite(fb)) continue;
      if (p.mode == 2) {
        if (!(std::fabs(fa) > 1e-4f && std::fabs(fa) < 1e6f)) continue;
        const float k = fb / fa;
        if (!(k > 0.5f && k < 2.0f && std::fabs(k - 1.0f) > 2e-5f)) continue;
        p.cands.push_back({va, kAnyStep});
        p.delta.push_back(k);
      } else if (p.mode == 1) {
        const float d = fb - fa;
        if (!(std::fabs(d) > 1e-6f && std::fabs(d) < 1e4f)) continue;
        p.cands.push_back({va, kAnyStep});
        p.delta.push_back(d);
      } else {
        const int s = MatchStep(fb - fa);
        if (s >= 0) p.cands.push_back({va, uint8_t(s)});
      }
    }
  }
  p.prev.resize(p.cands.size());
  for (size_t i = 0; i < p.cands.size(); ++i) p.prev[i] = svr07_fps::LoadF32(g_base, p.cands[i].va);
  std::vector<uint8_t>().swap(p.snap);
  REXLOG_INFO("[probe] #{} first diff: {} candidates", p.index, p.cands.size());
}

void Confirm() {
  auto& p = g_probe;
  size_t keep = 0;
  for (size_t i = 0; i < p.cands.size(); ++i) {
    const float v = svr07_fps::LoadF32(g_base, p.cands[i].va);
    bool same;
    if (p.mode == 2) {
      const float k = p.delta[i], pv = p.prev[i];
      same = std::fabs(pv) > 1e-6f && std::fabs(v / pv - k) <= std::fabs(k - 1.0f) * 2e-2f + 2e-6f;
    } else if (p.mode == 1) {
      const float d = p.delta[i];
      same = std::fabs((v - p.prev[i]) - d) <= std::fabs(d) * 1e-3f + 1e-7f;
    } else {
      same = MatchStep(v - p.prev[i]) == int(p.cands[i].step);
    }
    if (same) {
      p.cands[keep] = p.cands[i];
      p.prev[keep] = v;
      if (!p.delta.empty()) p.delta[keep] = p.delta[i];
      ++keep;
    }
  }
  p.cands.resize(keep);
  p.prev.resize(keep);
  if (!p.delta.empty()) p.delta.resize(keep);
}

void ReportAndWatch() {
  auto& p = g_probe;
  const int confirm = std::clamp(REXCVAR_GET(svr07_anim_probe_confirm_frames), 1, 60);
  std::map<uint8_t, int> counts;
  for (const Cand& c : p.cands) ++counts[c.step];
  std::string line;
  for (auto& [s, n] : counts) line += fmt::format(" {}={}", StepName(s), n);
  REXLOG_INFO("[probe] #{} {} stepped the same every frame for {} frames: {}{}", p.index, p.label, confirm + 1,
              p.cands.size(), line);
  if (REXCVAR_GET(svr07_anim_probe_log)) {
    const int limit = std::clamp(REXCVAR_GET(svr07_anim_probe_list), 0, 2000) * (p.mode ? 3 : 1);
    for (auto& [s, n] : counts) {
      std::string list;
      int shown = 0;
      for (size_t i = 0; i < p.cands.size() && shown < limit; ++i) {
        const Cand& c = p.cands[i];
        if (c.step != s) continue;
        const float v = svr07_fps::LoadF32(g_base, c.va);
        if (p.mode == 2) list += fmt::format(" {:08X}={:.4f}/x{:.6f}", c.va, v, p.delta[i]);
        else if (p.mode == 1) list += fmt::format(" {:08X}={:.4f}/{:+.6f}", c.va, v, p.delta[i]);
        else list += fmt::format(" {:08X}={:.4f}", c.va, v);
        ++shown;
      }
      if (shown) REXLOG_INFO("[probe] #{} step {}:{}", p.index, StepName(s), list);
    }
  }
  // Pick watches: free debug register slots, one per object (va >> 8), objects not watched by an earlier probe.
  p.watch_count = 0;
  int free_slots[4], nfree = 0;
  for (int i = 0; i < 4; ++i)
    if (g_slot_kind[i].load() == kSlotOff) free_slots[nfree++] = i;
  const int want = std::min(std::clamp(REXCVAR_GET(svr07_anim_probe_watches), 0, 4), nfree);
  std::vector<size_t> order(p.cands.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  if (p.mode) {
    std::stable_sort(order.begin(), order.end(), [&p](size_t x, size_t y) {
      return p.mode == 2 ? std::fabs(p.delta[x] - 1.0f) > std::fabs(p.delta[y] - 1.0f)
                         : std::fabs(p.delta[x]) > std::fabs(p.delta[y]);
    });
  } else {
    std::stable_sort(order.begin(), order.end(),
                     [&p](size_t x, size_t y) { return (p.cands[x].step & 0x7F) < (p.cands[y].step & 0x7F); });
  }
  for (size_t i : order) {
    if (p.watch_count >= want) break;
    const uint32_t obj = p.cands[i].va >> 8;
    if (std::find(p.watched_objs.begin(), p.watched_objs.end(), obj) != p.watched_objs.end()) continue;
    p.watched_objs.push_back(obj);
    const int slot = free_slots[p.watch_count];
    p.watch_slots[p.watch_count] = slot;
    p.watch_va[p.watch_count] = p.cands[i].va;
    ++p.watch_count;
  }
  if (!p.watch_count) {
    REXLOG_INFO("[probe] #{} nothing new to watch ({} free debug registers)", p.index, nfree);
    return;
  }
  for (auto& h : g_hits) {
    h.rip = 0;
    h.n = 0;
    h.slot_bits = 0;
    h.last_frame = 0;
    h.frames = 0;
  }
  std::string w;
  for (int i = 0; i < p.watch_count; ++i) {
    const int s = p.watch_slots[i];
    g_slot_writes[s] = 0;
    g_slot_host[s] = uint64_t(g_base + p.watch_va[i]);
    g_slot_kind[s] = kSlotWrite;
    size_t ci = 0;
    while (ci < p.cands.size() && p.cands[ci].va != p.watch_va[i]) ++ci;
    w += fmt::format(" {:08X}({})", p.watch_va[i], ci < p.cands.size() ? StepName(p.cands[ci].step) : "?");
  }
  EnsureHandler();
  REXLOG_INFO("[probe] #{} watching{} for {} frames", p.index, w,
              std::clamp(REXCVAR_GET(svr07_anim_probe_watch_frames), 10, 2000));
  ApplyDebugRegs();
}

void CountLine() {
  auto& p = g_probe;
  std::string line;
  for (int i = 0; i < p.watch_count; ++i)
    line += fmt::format(" {:08X}={}", p.watch_va[i], g_slot_writes[p.watch_slots[i]].exchange(0));
  REXLOG_INFO("[probe] #{} writes per {} frames:{}", p.index, std::max(1, REXCVAR_GET(svr07_anim_probe_count_every)),
              line);
}

void ReportWriters(int frames) {
  auto& p = g_probe;
  for (int i = 0; i < p.watch_count; ++i) g_slot_kind[p.watch_slots[i]] = kSlotOff;
  ApplyDebugRegs();
  int writers = 0;
  for (auto& h : g_hits) {
    const uint64_t rip = h.rip.load();
    if (!rip) continue;
    ++writers;
    std::string watched;
    for (int i = 0; i < p.watch_count; ++i)
      if (h.slot_bits.load() & (1u << p.watch_slots[i])) watched += fmt::format(" {:08X}", p.watch_va[i]);
    const uint32_t n = h.n.load(), fr = h.frames.load();
    // A frame-tied stepper writes once (or a fixed few times) in EVERY frame; a 30 Hz grid / own-clock writer hits
    // only some frames (UFC 2p false-positive rule).
    const char* verdict = fr * 10 >= uint32_t(frames) * 9 ? "EVERY FRAME" : (fr * 10 >= uint32_t(frames) * 4 ? "most frames" : "some frames");
    REXLOG_INFO("[probe] #{} writer {} n={} frames={}/{} ({:.2f}/frame, {}) wrote{}", p.index, NameHost(rip, true), n,
                fr, frames, double(n) / std::max(1, frames), verdict, watched);
  }
  if (!writers) REXLOG_INFO("[probe] #{} no writes seen (written by another view of the page, or stopped)", p.index);
  p.watch_count = 0;
}

void StartProbe(const std::string& why, double t, uint64_t frame, Scene scene) {
  auto& p = g_probe;
  ++p.index;
  const uint32_t rate = svr07_fps::Load32(g_base, svr07_fps::kRate);
  const float step = svr07_fps::LoadF32(g_base, svr07_fps::kStep);
  p.label = fmt::format("[{} {}]", kSceneName[scene], why);
  REXLOG_INFO("[probe] #{} start ({}) scene={} at {:.1f} s frame {}: rate {} step {:.4f} live {}{}", p.index, why,
              kSceneName[scene], t, frame, rate, step, svr07_fps::g_live.load() ? 1 : 0,
              step == 1.0f || step == 2.0f ? " (stock step: per-frame = per-tick here, see L21 rules)" : "");
  Snapshot();
  p.phase = 1;
  p.frames_in_phase = 0;
}

void ProbeFrame(double t, uint64_t frame, Scene scene) {
  auto& p = g_probe;
  switch (p.phase) {
    case 0: {
      std::string why;
      if (p.next_time < p.times.size() && t >= p.times[p.next_time]) {
        why = fmt::format("at {:.0f} s", p.times[p.next_time]);
        ++p.next_time;
      } else if (p.next_frame < p.frames.size() && frame >= p.frames[p.next_frame]) {
        why = fmt::format("at frame {}", p.frames[p.next_frame]);
        ++p.next_frame;
      } else if (p.scene_req) {
        p.scene_req = false;
        ++p.scene_count[scene];
        why = fmt::format("scene {} #{}", kSceneName[scene], p.scene_count[scene]);
      }
      if (!why.empty()) StartProbe(why, t, frame, scene);
      return;
    }
    case 1:
      FirstDiff();
      p.phase = 2;
      p.frames_in_phase = 0;
      return;
    case 2:
      Confirm();
      if (++p.frames_in_phase >= std::clamp(REXCVAR_GET(svr07_anim_probe_confirm_frames), 1, 60)) {
        ReportAndWatch();
        p.phase = p.watch_count ? 3 : 0;
        p.frames_in_phase = 0;
        std::vector<Cand>().swap(p.cands);
        std::vector<float>().swap(p.prev);
        std::vector<float>().swap(p.delta);
      }
      return;
    case 3: {
      ++p.frames_in_phase;
      const int every = REXCVAR_GET(svr07_anim_probe_count_every);
      if (every > 0 && p.frames_in_phase % every == 0) CountLine();
      const int watch = std::clamp(REXCVAR_GET(svr07_anim_probe_watch_frames), 10, 2000);
      if (p.frames_in_phase >= watch) {
        ReportWriters(watch);
        p.phase = 0;
      }
      return;
    }
  }
}

// ---------------------------------------------------------------- step trace

void ArmTrace(bool on, double t, const std::string& why) {
  auto& tr = g_trace;
  for (int i = 0; i < tr.count; ++i) {
    g_slot_kind[i] = on ? kSlotExec : kSlotOff;
    g_slot_host[i] = on ? HostEntryOf(tr.fn[i]) : 0;
    if (on && !g_slot_host[i]) {
      g_slot_kind[i] = kSlotOff;
      REXLOG_INFO("[steptrace] sub_{:08X}: not in the function table, skipped", tr.fn[i]);
    }
    g_stacks_left[i] = on ? std::clamp(REXCVAR_GET(svr07_steptrace_stacks), 0, 8) : 0;
  }
  tr.armed = on;
  g_trace_armed = on;
  tr.armed_at = tr.last_report = t;
  if (on) {
    EnsureHandler();
    std::string fns;
    for (int i = 0; i < tr.count; ++i) fns += fmt::format(" sub_{:08X}", tr.fn[i]);
    REXLOG_INFO("[steptrace] window start ({}) at {:.1f} s:{} arg {}{}", why, t, fns, tr.arg_float ? "f" : "r",
                tr.arg_reg);
  }
  ApplyDebugRegs();
}

void TraceReport(double t, bool final_report) {
  auto& tr = g_trace;
  const float ticks = svr07_fps::LoadF32(g_base, svr07_fps::kStep);
  for (int slot = 0; slot < tr.count; ++slot) {
    struct Row {
      uint32_t lr;
      int32_t arg;
      uint32_t n, fixed;
    };
    std::vector<Row> rows;
    for (TraceHit& s : g_trace_hits) {
      const uint64_t key = s.key.load();
      if (!key || int(key >> 62) != slot) continue;
      const uint32_t n = s.n.load(), fx = s.fixed.load();
      if (n == s.reported) continue;
      rows.push_back({uint32_t((key >> 32) & 0x1FFFFFFF) + 0x82000000u, int32_t(uint32_t(key)), n - s.reported,
                      fx - s.reported_fixed});
      s.reported = n;
      s.reported_fixed = fx;
    }
    if (rows.empty()) continue;
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.n > b.n; });
    std::string line = fmt::format("[steptrace] sub_{:08X} ({:.1f} s, ticks {:.4f}):", tr.fn[slot], t, ticks);
    int shown = 0;
    for (const Row& r : rows) {
      if (++shown > 24) {
        line += fmt::format(" ... {} more", rows.size() - 24);
        break;
      }
      const std::string a = tr.arg_float ? fmt::format("{:.4f}", r.arg / 1e4) : fmt::format("{}", r.arg);
      line += fmt::format(" {}/{}x{}{}", NameGuest(r.lr), a, r.n, r.fixed ? fmt::format(" FIXED{}", r.fixed) : "");
    }
    REXLOG_INFO("{}", line);
  }
  if (const uint32_t o = g_trace_overflow.exchange(0)) REXLOG_INFO("[steptrace] {} calls not counted (table full)", o);
  for (Stack& st : g_stacks) {
    if (st.state.load() != 2) continue;
    std::string g;
    for (uint32_t pc : st.guest)
      if (pc) g += " " + NameGuest(pc);
    REXLOG_INFO("[steptrace] stack sub_{:08X} FIXED {:.4f} at ticks {:.4f}, guest chain:{}", tr.fn[st.slot], st.arg,
                st.ticks, g);
    std::string h;
    const uint64_t exe = uint64_t(GetModuleHandleW(nullptr));
    for (uint64_t rip : st.host) {
      if (!rip) break;
      if (rip < exe || rip >= exe + 0x20000000ull) continue;
      h += " | " + NameHost(rip, false);
    }
    REXLOG_INFO("[steptrace] stack sub_{:08X} host:{}", tr.fn[st.slot], h);
    st.state.store(3);
  }
  if (final_report) REXLOG_INFO("[steptrace] window end at {:.1f} s", t);
}

void TraceFrame(double t, Scene scene) {
  auto& tr = g_trace;
  if (tr.continuous) {
    if (!tr.armed) ArmTrace(true, t, "whole run");
    if (t - tr.last_report >= std::clamp(REXCVAR_GET(svr07_steptrace_report_s), 1.0, 120.0)) {
      tr.last_report = t;
      TraceReport(t, false);
      ApplyDebugRegs();  // threads started since get the breakpoints too
    }
    return;
  }
  if (tr.armed) {
    if (t - tr.armed_at >= std::clamp(REXCVAR_GET(svr07_steptrace_secs), 1.0, 3600.0)) {
      TraceReport(t, true);
      ArmTrace(false, t, "");
    } else if (t - tr.last_report >= std::clamp(REXCVAR_GET(svr07_steptrace_report_s), 1.0, 120.0)) {
      tr.last_report = t;
      TraceReport(t, false);
    }
    return;
  }
  // The probe's write watches hold free slots; arm when they are released (trigger kept until then).
  if (g_probe.phase == 3) return;
  if (tr.next_time < tr.times.size() && t >= tr.times[tr.next_time]) {
    ArmTrace(true, t, fmt::format("at {:.0f} s, scene {}", tr.times[tr.next_time], kSceneName[scene]));
    ++tr.next_time;
  } else if (tr.scene_req) {
    tr.scene_req = false;
    ++tr.scene_count[scene];
    ArmTrace(true, t, fmt::format("scene {} #{}", kSceneName[scene], tr.scene_count[scene]));
  }
}

// ---------------------------------------------------------------- frame

bool g_inited = false, g_any = false;
PClock::time_point g_t0;
int g_scene = -1;
double g_scene_since = 0;
bool g_scene_fired = false;

void Init() {
  g_inited = true;
  auto& p = g_probe;
  for (const std::string& s : SplitList(REXCVAR_GET(svr07_anim_probe_at))) p.times.push_back(std::atof(s.c_str()));
  for (const std::string& s : SplitList(REXCVAR_GET(svr07_anim_probe_frames)))
    p.frames.push_back(std::strtoull(s.c_str(), nullptr, 10));
  std::sort(p.times.begin(), p.times.end());
  std::sort(p.frames.begin(), p.frames.end());
  p.scene_mask = SceneMask(REXCVAR_GET(svr07_anim_probe_scenes));
  p.on = !p.times.empty() || !p.frames.empty() || p.scene_mask;
  const std::string mode = REXCVAR_GET(svr07_anim_probe_mode);
  p.mode = mode == "any" ? 1 : mode == "ratio" ? 2 : 0;
  for (const std::string& s : SplitList(REXCVAR_GET(svr07_anim_probe_steps))) {
    const double v = std::fabs(ParseNumber(s));
    if (v > 0 && p.steps.size() < 16) {
      p.steps.push_back(float(v));
      p.step_names.push_back(s);
    }
  }
  if (p.steps.empty() && p.mode == 0) p.on = false;

  auto& tr = g_trace;
  for (const std::string& s : SplitList(REXCVAR_GET(svr07_steptrace_funcs))) {
    const uint32_t a = uint32_t(std::strtoul(s.rfind("sub_", 0) == 0 ? s.c_str() + 4 : s.c_str(), nullptr, 16));
    if (a && tr.count < 4) tr.fn[tr.count++] = a;
  }
  const std::string arg = REXCVAR_GET(svr07_steptrace_arg);
  tr.arg_float = arg.empty() || arg[0] != 'r';
  tr.arg_reg = arg.size() > 1 ? std::atoi(arg.c_str() + 1) : 1;
  for (const std::string& s : SplitList(REXCVAR_GET(svr07_steptrace_fixed))) tr.fixed_vals.push_back(ParseNumber(s));
  for (const std::string& s : SplitList(REXCVAR_GET(svr07_steptrace_at))) tr.times.push_back(std::atof(s.c_str()));
  std::sort(tr.times.begin(), tr.times.end());
  tr.scene_mask = SceneMask(REXCVAR_GET(svr07_steptrace_scenes));
  tr.on = tr.count > 0;
  tr.continuous = tr.on && tr.times.empty() && !tr.scene_mask;

  g_any = p.on || tr.on;
  if (!g_any) return;
  g_t0 = PClock::now();
  BuildFuncs();
  REXLOG_INFO("[probe] L21 stepper finder on: probe {} (mode {}, {} steps, {} times, {} frames, scenes {:#x}), step "
              "trace {} ({} functions{})",
              p.on ? "on" : "off", mode, p.steps.size(), p.times.size(), p.frames.size(), p.scene_mask,
              tr.on ? "on" : "off", tr.count, tr.continuous ? ", whole run" : "");
}

}  // namespace

void Frame(uint8_t* base) {
  if (!g_inited) Init();
  if (!g_any) return;
  g_base = base;
  const uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
  const double t = std::chrono::duration<double>(PClock::now() - g_t0).count();
  const Scene scene = CurrentScene(base);
  if (scene != g_scene) {
    REXLOG_INFO("[probe] scene: {} -> {} at {:.1f} s frame {}", g_scene < 0 ? "start" : kSceneName[g_scene],
                kSceneName[scene], t, frame);
    g_scene = scene;
    g_scene_since = t;
    g_scene_fired = false;
    g_probe.scene_req = false;
    g_trace.scene_req = false;
  }
  if (!g_scene_fired && t - g_scene_since >= std::clamp(REXCVAR_GET(svr07_anim_probe_scene_delay_s), 0.0, 600.0)) {
    g_scene_fired = true;
    const int max = std::max(0, REXCVAR_GET(svr07_anim_probe_scene_max));
    if ((g_probe.scene_mask >> scene) & 1 && g_probe.scene_count[scene] < max) g_probe.scene_req = true;
    if ((g_trace.scene_mask >> scene) & 1 && g_trace.scene_count[scene] < max) g_trace.scene_req = true;
  }
  if (g_probe.on) ProbeFrame(t, frame, scene);
  if (g_trace.on) TraceFrame(t, scene);
  g_probe.jitter_active = g_probe.on && (g_probe.phase == 1 || g_probe.phase == 2);
}

double Jitter() {
  if (!g_any || !g_probe.jitter_active) return 1.0;
  const double j = std::clamp(REXCVAR_GET(svr07_anim_probe_jitter), 0.0, 0.5);
  if (j <= 0) return 1.0;
  return (g_frame.load(std::memory_order_relaxed) & 1) ? 1.0 + j : 1.0 - j;
}

}  // namespace svr07_probe
