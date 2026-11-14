// SvR07 addresses for the XDK D3D functions that re:Blue's renderer hooks by name (lane L3,
// 2026-11-10; map = FINDINGS-native-renderer.md NR-1, each one re-read in generated/default and
// recorded in reports/L3-d3d-verification.md). Template: SvR 2008's src/native/svr_d3d_names.h and
// RR's rr_d3d_names.h.
//
// Force-included into the native renderer sources (native_renderer.cmake), so e.g.
// SVR_HOOK_AFTER(D3DDevice_Clear, ...) overrides SvR07's recompiled sub_821D93F0 and
// __imp__D3DDevice_Clear calls its original body. Base exe = default.xex WITHOUT the title update
// (TU1 moves 1,194 of 1,196 .text addresses: FINDINGS-tu1.md). XDK D3D9 2.0.3215.1, library
// 0x821D2A60-0x821EB230. A hook that names a function not listed here fails to link (__imp__) or
// stays inert (unmapped REX_HOOK name): that is on purpose.
//
// None of the game's own hooks sits in the D3D library: src/*.cpp has 52 REX_HOOK(_RAW) sites, none in
// 0x821D2A60-0x821EB230 (svr07_hooks.cpp only NAMES sub_821D3E78 / sub_821D3020 / sub_821D30D0 in
// comments; its sub_821D0558 is XMem, below the library); config/*.toml, roster_sites.inc and the
// manifest hold no address in the range, and nothing hooks sub_8241F180 (EndVertices' tail call:
// an empty `blr` with 390 callers).

#pragma once

// Device / frame
#define D3DDevice_Present                     sub_821EB100  // SetRenderTarget(0, +0x3478), Sync, Resolve(front +0x3474), viewport/scissor restore, Swap
#define D3DDevice_Swap                        sub_821EAB50  // only VdSwap caller; (dev, front +0x3474, params +0x33E4)
#define D3DDevice_SynchronizeToPresentationInterval sub_821EA9C8
#define D3DDevice_Resolve                     sub_821DB9C0  // 11-arg form: r4 flags .. r10 pClearColor, f1 Z, stencil/params at caller r1+0x5C/+0x64
#define D3DDevice_Clear                       sub_821D93F0  // (dev, Count, pRects, Flags, Color, f1 Z, r9 Stencil): loops rects into ClearF
#define D3DDevice_ClearF                      sub_821D8B38  // internal per-rect clear (dev, flags, rect, colour vec, f1 Z, stencil)
#define D3DDevice_SetRenderTarget             sub_821D7F60  // the game also calls the 1-instruction alias sub_821D8570
#define D3DDevice_SetDepthStencilSurface      sub_821D8290
#define D3DDevice_SetScissorRect              sub_821D76A0
#define D3DDevice_SetViewport                 sub_821D7768  // integer D3DVIEWPORT9; W/H clamped to the target (census + logging only)
// D3DDevice_SetViewportF: not in this XDK (integer viewport, no flags field).

// Draws. SvR07 has no BeginIndexedVertices and no DrawVerticesUP: the DRAW_INDX writers in the
// library are BeginVertices, DrawVertices, DrawIndexedVertices, ClearF, Resolve, the state flush
// sub_821D8578 (SetShaderGPRAllocation's point-list pipe drain) and sub_821E7248 (internal, calls
// SetShaderGPRAllocation). Leave those two hooks unmapped.
#define D3DDevice_BeginVertices               sub_821D94A0  // (dev, prim, count, stride); callers 82446070 (prim from record), 8244843C (prim 13 quads)
#define D3DDevice_EndVertices                 sub_821D9950  // ring = +0x3324, then tail-calls the empty sub_8241F180
#define D3DDevice_DrawVertices                sub_821D9970  // (dev, prim, start, count); 1 caller 82603E40 (prim 4, 6 vertices)
#define D3DDevice_DrawIndexedVertices         sub_821D9D78  // (dev, prim, base, start, count); 1 caller 82444B88

// Buffers: the game creates, locks and unlocks its own; the renderer only observes Unlock.
// D3DDevice_CreateVertexBuffer sub_821D2EE8 (32-byte object), D3DDevice_CreateIndexBuffer
// sub_821D3020 (32), D3DVertexBuffer_Lock sub_821D2FB0 and D3DIndexBuffer_Lock sub_821D30D0 are
// matched but deliberately NOT mapped: gpu/hooks/resource.cpp's REX_HOOKs would replace them.
#define D3DVertexBuffer_Unlock                sub_821D3010  // +0x18 & ~3, then the generic unlock sub_821D2C98
#define D3DIndexBuffer_Unlock                 sub_821D3130  // +0x18 raw, same generic unlock

// Resource creation (observed after the game's own body: SVR_HOOK_RESULT).
#define D3DDevice_CreateVertexDeclaration     sub_821DAEA0  // 56 + 12n bytes, elements at +0x34
#define D3DDevice_CreateVertexShader          sub_821DB120  // 592-byte prefix; its microcode is IM_LOADed as type 0 (vertex)
#define D3DDevice_CreatePixelShader           sub_821DB030  // 56-byte prefix; its microcode is IM_LOADed as type 1 (pixel)
// Also matched, NOT mapped (resource.cpp REX_HOOKs them in every build; mapping one replaces the
// game's function and corrupts its D3D objects): CreateTexture sub_821D4CF0 (52 bytes), CreateSurface
// sub_821D4E10 (48), Texture GetSurfaceLevel sub_821D4AA8, Texture LockRect sub_821D4B60 / Surface
// LockRect sub_821D4FD0 (body sub_821D4400), Resource Unlock sub_821D31D8 (texture) / sub_821D31F0
// (surface), AddRef sub_821D2DA0, Release sub_821D2E18, Destroy sub_821D2878, GetType sub_821D27E8,
// Direct3D_CreateDevice sub_821DECF0, SetShaderGPRAllocation sub_821DAF80, KickOff / MakeSpace
// sub_821DD940, commit sub_821E2760 (shader bind sub_821E2638), register block writer sub_821E1878.

// State
#define D3DDevice_SetTexture                  sub_821D5058  // table +0x3000 (26 slots, no index clamp), fetch +0x400 + 24 * s
// VS vs PS, decided by what the GPU is told (the RR port's all-black bug came from this pair):
// the draw commit sub_821E2760 IM_LOADs the object at +0x3094 with shader type 0 (vertex) and size
// [obj+600], the object at +0x3090 with type 1 (pixel) and size [obj+64]; CreateVertexShader's
// objects (Common type 6) have the +592 prefix, CreatePixelShader's (type 7) the +56 one; the setter
// storing +0x3094 (sub_821DACE0) reads its object's data at +592 and ORs into the VS constant mask
// (+0x00, uploaded as 0x4000), the one storing +0x3090 (sub_821DAAA8) at +56 into the PS mask (+0x08,
// 0x4400). Hook the PUBLIC VS entry sub_821DAE30 (stores +0x3364, then branches to the body).
#define D3DDevice_SetVertexShader             sub_821DAE30  // public entry; body sub_821DACE0 stores +0x3094
#define D3DDevice_SetPixelShader              sub_821DAAA8  // stores +0x3090
#define D3DDevice_SetVertexDeclaration        sub_821DAE80  // stores +0x2D20
#define D3DDevice_SetStreamSource             sub_821D79A8  // (dev, stream, vb, offset, stride, mask): bare vb at +0x2FAC, offset only in the fetch
#define D3DDevice_SetIndices                  sub_821D7B58  // stores +0x2F94
// The constant setters take the dirty mask as a 5th argument (r7, u64) and have one caller each;
// most constants are written by the game straight into the device. Census only.
#define D3DDevice_SetVertexShaderConstantF    sub_821DA738  // (dev, start, data, count, mask): +0x700 + 16 * start, ORs mask into +0x00
#define D3DDevice_SetPixelShaderConstantF     sub_821DA848  // same, +0x1700, mask into +0x08

// Recorded work: none expected in SvR07 (NR-1: no command buffers, queries, tiling or ZPass found;
// Resolve has only 2 callers). UFC's hooks for them stay unmapped.

// Original bodies (__imp__ = the recompiled function, for hooks that call through).
#define __imp__D3DDevice_Present              __imp__sub_821EB100
#define __imp__D3DDevice_Swap                 __imp__sub_821EAB50
#define __imp__D3DDevice_SynchronizeToPresentationInterval __imp__sub_821EA9C8
#define __imp__D3DDevice_Resolve              __imp__sub_821DB9C0
#define __imp__D3DDevice_Clear                __imp__sub_821D93F0
#define __imp__D3DDevice_ClearF               __imp__sub_821D8B38
#define __imp__D3DDevice_SetTexture           __imp__sub_821D5058
#define __imp__D3DDevice_SetRenderTarget      __imp__sub_821D7F60
#define __imp__D3DDevice_SetDepthStencilSurface __imp__sub_821D8290
#define __imp__D3DDevice_SetScissorRect       __imp__sub_821D76A0
#define __imp__D3DDevice_SetViewport          __imp__sub_821D7768
#define __imp__D3DDevice_BeginVertices        __imp__sub_821D94A0
#define __imp__D3DDevice_EndVertices          __imp__sub_821D9950
#define __imp__D3DDevice_DrawVertices         __imp__sub_821D9970
#define __imp__D3DDevice_DrawIndexedVertices  __imp__sub_821D9D78
#define __imp__D3DDevice_CreateVertexDeclaration __imp__sub_821DAEA0
#define __imp__D3DDevice_CreateVertexShader   __imp__sub_821DB120
#define __imp__D3DDevice_CreatePixelShader    __imp__sub_821DB030
#define __imp__D3DDevice_SetVertexShader      __imp__sub_821DAE30
#define __imp__D3DDevice_SetPixelShader       __imp__sub_821DAAA8
#define __imp__D3DDevice_SetVertexDeclaration __imp__sub_821DAE80
#define __imp__D3DDevice_SetStreamSource      __imp__sub_821D79A8
#define __imp__D3DDevice_SetIndices           __imp__sub_821D7B58
#define __imp__D3DDevice_SetVertexShaderConstantF __imp__sub_821DA738
#define __imp__D3DDevice_SetPixelShaderConstantF  __imp__sub_821DA848
#define __imp__D3DVertexBuffer_Unlock         __imp__sub_821D3010
#define __imp__D3DIndexBuffer_Unlock          __imp__sub_821D3130

// Resource description getters: only the __imp__ names (resource.cpp's REX_HOOK_RAW bodies fall
// back to them; the hook names themselves stay unmapped, so the game's own getters keep running).
#define __imp__D3DSurface_GetDesc             __imp__sub_821D4F38  // level surface -> sub_821D3D58; else +0x24 / +0x28
#define __imp__D3DTexture_GetLevelDesc        __imp__sub_821D4C30  // thunk to sub_821D3D58 (medium: NR-1)
#define __imp__D3DVolumeTexture_GetLevelDesc  __imp__sub_821D4C38  // thunk to sub_821D3DE8 (medium: NR-1)
