/**
 * @file    gpu/d3d.h
 * @brief   Byte-accurate guest layouts for the guest D3D9 SDK objects the
 *          recompiled BD engine reads/writes.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Must match the guest layout byte-for-byte at the offsets the engine
 * accesses, or accessors like D3DTexture_GetLevelDesc decode garbage and
 * crash D3D12. Layouts recovered from the guest binary (D3DResource ord 933,
 * D3DTexture ord 2657, D3DSurface ord 2525, D3DVertexBuffer ord 1936,
 * D3DIndexBuffer ord 1937, D3DVertexShader ord 1641, D3DPixelShader ord 1263,
 * D3DVertexDeclaration ord 934, GPUTEXTURE_FETCH_CONSTANT ord 853). Only
 * host-accessed fields are named.
 *
 * SVR07_NATIVE_RENDERER (WWE SmackDown vs. Raw 2007, lane L3 2026-11-10): SvR07 links XDK D3D9
 * 2.0.3215.1. Its resources have Blue Dragon's / UFC's shapes: a 24-byte header with the refcount
 * at +4, 32-byte vertex / index buffers (fetch / address at +0x18), 52-byte textures (fetch constant
 * at +0x1C), 48-byte surfaces (size bits +0x24, format +0x28); only the Common type values differ
 * (VB 1 .. PS 7). Its device is 0x5000 bytes, Blue Dragon's layout + 0x10 past 0x2A00. Every offset
 * is in src/svr07_d3d.h with the instruction that proves it, and static_asserted against it below.
 * (This branch held Rumble Roses XX's numbers until 2026-11-10; RR's are in its own tree.)
 * Offsets are offline evidence until the census (Stop 1); "SVR07-UNVERIFIED" marks the guesses.
 */
#pragma once

#include <cstddef>
#include <rex/types.h>

#if defined(SVR07_NATIVE_RENDERER)
#include "svr07_d3d.h"
#endif

namespace bd::gpu {

#if defined(SVR07_NATIVE_RENDERER)
// SvR07 (XDK 2.0.3215.1) D3DResource::Common:
//   bits  0-3   resource type (below; D3DResource_GetType sub_821D27E8: clrlwi r11,r10,28)
//   bit  20     set by every Create* (oris r11,r11,16 -> 0x001000xx)
//   bits 21-22  CreateVertexBuffer / CreateIndexBuffer usage bits (0x00200000, 0x00400000)
//   bit  30     surface of a texture level: the parent texture is at +0x18 (AddRef / Release
//               sub_821D2DA0 / sub_821D2E18 forward to it, GetDesc sub_821D4F38 asks it)
//   bits 29-31  index buffer: the D3DFORMAT's low 3 bits (CreateIndexBuffer rlwinm r10,r30,29,0,2):
//               bit 31 = 32-bit indices, bits 29-30 = endian (INDEX16 = 1 -> 8in16, INDEX32 = 6 -> 8in32)
enum class D3DResourceType : u32 {
  kVertexBuffer = svr07::d3d::kTypeVertexBuffer,            // 1: CreateVertexBuffer sub_821D2EE8, 0x00100001
  kIndexBuffer = svr07::d3d::kTypeIndexBuffer,              // 2: CreateIndexBuffer sub_821D3020, 0x00100002
  kTexture = svr07::d3d::kTypeTexture,                      // 3: GetType (dim from fetch dword5)
  kSurface = svr07::d3d::kTypeSurface,                      // 4: GetType / AddRef
  kVertexDeclaration = svr07::d3d::kTypeVertexDeclaration,  // 5: CreateVertexDeclaration sub_821DAEA0
  kVertexShader = svr07::d3d::kTypeVertexShader,            // 6: CreateVertexShader -> sub_821DAC18
  kPixelShader = svr07::d3d::kTypePixelShader,              // 7: CreatePixelShader sub_821DB030
  // SVR07-CENSUS-1 (2026-11-11, laptop, emulated, title/menus/1v1 match): SetTexture objects are only Common type 3 (fetch dim 1 = 2D, 3 = cube): confirmed. Was: cube / volume textures are type 3 with a dimension in fetch dword5 (GetType returns 17/18/19/20), no own Common value seen | census: log Common & 0xF of every SetTexture argument
};
constexpr u32 kCommonTypeMask = svr07::d3d::kCommonTypeMask;
constexpr u32 kCommonCreated = 0x00100000u;
constexpr u32 kCommonLevelSurface = svr07::d3d::kCommonLevelSurface;
constexpr u32 kCommonFlagTextureBackedSurface = kCommonLevelSurface;  // UFC's name for the same bit
constexpr u32 kCommonIndex32 = svr07::d3d::kCommonIndex32;

// 24-byte header shared by every SvR07 D3D resource (Blue Dragon's / UFC's shape).
struct D3DResource {
  be_u32 Common;         // +0x00  type / flag bits (above)
  be_u32 ReferenceCount; // +0x04  AddRef / Release: lwarx/stwcx. on +4; Create* store 1
  be_u32 Fence;          // +0x08  SetTexture / SetStreamSource / SetIndices store the device fence (+0x2A1C) into the unbound one
  be_u32 ReadFence;      // +0x0C  (Blue Dragon's name; the renderer does not read it)
  be_u32 Identifier;     // +0x10  (Blue Dragon's name; the renderer does not read it)
  be_u32 BaseFlush;      // +0x14  0xFFFF0000 at create (every Create*: stw r9,20(r31))
};
static_assert(sizeof(D3DResource) == svr07::d3d::kResourceHeaderSize);
static_assert(offsetof(D3DResource, ReferenceCount) == svr07::d3d::kResourceRefCount);
static_assert(offsetof(D3DResource, Fence) == svr07::d3d::kResourceFence);
static_assert(offsetof(D3DResource, BaseFlush) == svr07::d3d::kResourceBaseFlush);

inline u32 ResourceRefCount(const D3DResource &r) { return u32(r.ReferenceCount); }
inline void SetResourceRefCount(D3DResource &r, u32 count) { r.ReferenceCount = count; }
inline u32 ResourceTypeBits(const D3DResource &r) { return u32(r.Common) & kCommonTypeMask; }
#else
// D3DRESOURCETYPE values from the guest D3D9 SDK. D3DResource::Common carries
// the type in its low 4 bits, so these double as the Common base type bits.
enum class D3DResourceType : u32 {
  kSurface = 1,
  kTexture = 3,
  kVolumeTexture = 4,
  kCubeTexture = 5,
  kVertexBuffer = 6,
  kIndexBuffer = 7,
};

// 24-byte header shared by every D3D9 resource (D3DResource ord 933).
struct D3DResource {
  be_u32 Common;         // +0x00  type/flag bits, read by D3DResource_GetType
  be_u32 ReferenceCount; // +0x04  engine increments/decrements directly
  be_u32 Fence;          // +0x08
  be_u32 ReadFence;      // +0x0C
  be_u32 Identifier;     // +0x10
  be_u32 BaseFlush;      // +0x14
};
static_assert(sizeof(D3DResource) == 24);
#endif  // SVR07_NATIVE_RENDERER

// The pViewport argument to D3DDevice_SetViewport (BD 0x82473610), and the
// layout of the device's cached viewport at D3DDevice+0x3058.
struct D3DViewport9 {
  be_u32 X;
  be_u32 Y;
  be_u32 Width;
  be_u32 Height;
  be_f32 MinZ;
  be_f32 MaxZ;
};
static_assert(sizeof(D3DViewport9) == 24);

// Guest RECT (LTRB): the pRect argument to D3DDevice_SetScissorRect
// (BD 0x82473548).
struct D3DRect {
  be_i32 left;
  be_i32 top;
  be_i32 right;
  be_i32 bottom;
};
static_assert(sizeof(D3DRect) == 16);

// GPUTEXTURE_FETCH_CONSTANT (ord 853): 6-dword Xenos texture descriptor at
// D3DTexture+0x1C. reblue-created textures leave it zero since their accessors
// are hooked. Engine-created bdAllocRenderBuffer textures carry a real one that
// native_texture_mirror decodes.
struct GpuTextureFetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(GpuTextureFetchConstant) == 24);

#if defined(SVR07_NATIVE_RENDERER)
// SvR07 D3DTexture / cube / volume, 52 bytes (CreateTexture sub_821D4CF0: li r3,52). SetTexture
// (sub_821D5058) copies the fetch constant from +0x1C..+0x30 into device fetch slot `sampler`
// (device + 0x400 + 24 * sampler); CreateTexture patches the base address into dword1 (+0x20) and
// the mip address into dword5 (+0x30).
struct D3DTexture {
  D3DResource resource;           // +0x00
  be_u32 MipFlush;                // +0x18  (Blue Dragon's name; not read)
  GpuTextureFetchConstant Format; // +0x1C
};
static_assert(sizeof(D3DTexture) == svr07::d3d::kTextureSize);
static_assert(offsetof(D3DTexture, Format) == svr07::d3d::kTextureFetch);
#else
// D3DTexture / D3DBaseTexture / D3DCubeTexture / D3DVolumeTexture, 52 bytes.
struct D3DTexture {
  D3DResource resource;           // +0x00
  be_u32 MipFlush;                // +0x18
  GpuTextureFetchConstant Format; // +0x1C
};
static_assert(sizeof(D3DTexture) == 52);
static_assert(offsetof(D3DTexture, Format) == 0x1C);
#endif  // SVR07_NATIVE_RENDERER

#if defined(SVR07_NATIVE_RENDERER)
// SvR07 D3DSurface, 48 bytes (CreateSurface sub_821D4E10: li r3,48; init sub_821D4840). Same fields
// as UFC's / Blue Dragon's. D3DSurface_GetDesc (sub_821D4F38), non-level path: Format = +0x28,
// Width = ((+0x24 >> 18) & 0x3FFF) + 1, Height = ((+0x24 >> 3) & 0x7FFF) + 1, MultiSampleType =
// (+0x18 >> 16) & 3. SetRenderTarget copies +0x1C into RB_COLOR_INFO, SetDepthStencilSurface +0x1C
// into RB_DEPTH_INFO and +0x20 into RB_HIZCONTROL.
// A surface of a texture level (Common & kCommonLevelSurface) reuses +0x18 as the parent texture and
// the top nibble of +0x1C as the level (GetDesc: lwz r3,24 / rlwinm r4,r11,4,28,31 -> GetLevelDesc).
struct D3DSurface {
  D3DResource resource;  // +0x00
  union {
    be_u32 SurfaceInfo;  // +0x18  GPU_SURFACEINFO (RB_SURFACE_INFO image: MSAA in bits 16-17)
    be_u32 Parent;       // +0x18  level surface: the D3DTexture it is a level of
  };
  be_u32 DepthInfo;      // +0x1C  RB_COLOR_INFO / RB_DEPTH_INFO image; level surface: level << 28
  be_u32 HiControl;      // +0x20  RB_HIZCONTROL image
  be_u32 SizeBits;       // +0x24  width-1 [31:18], height-1 [17:3]
  be_u32 Format;         // +0x28  D3DFORMAT
  // SVR07-UNVERIFIED: +0x2C named Size after Blue Dragon; GetDesc does not read it | census: none needed (renderer does not read it)
  be_u32 Size;           // +0x2C
};
static_assert(sizeof(D3DSurface) == svr07::d3d::kSurfaceSize);
static_assert(offsetof(D3DSurface, SurfaceInfo) == svr07::d3d::kSurfaceInfo);
static_assert(offsetof(D3DSurface, Parent) == svr07::d3d::kSurfaceInfo);
static_assert(offsetof(D3DSurface, DepthInfo) == svr07::d3d::kSurfaceColorDepthInfo);
static_assert(offsetof(D3DSurface, HiControl) == svr07::d3d::kSurfaceHiControl);
static_assert(offsetof(D3DSurface, SizeBits) == svr07::d3d::kSurfaceSizeBits);
static_assert(offsetof(D3DSurface, Format) == svr07::d3d::kSurfaceFormat);

inline bool SurfaceIsLevel(const D3DSurface &s) { return (u32(s.resource.Common) & kCommonLevelSurface) != 0; }
// D3DSurface_GetDesc's non-level decode (sub_821D4F38). For a level surface the size is the
// parent's level size (D3DTexture_GetLevelDesc): these return garbage for one.
// SVR07-CENSUS-1 (2026-11-11, laptop, emulated, title/menus/1v1 match): 0 level surfaces set as RT/DS. Was: whether the game renders into level surfaces (then SizeBits / Format are not the size) | census: count SetRenderTarget / SetDepthStencilSurface arguments with Common bit 30
inline u32 SurfaceFormat(const D3DSurface &s) { return u32(s.Format); }
inline void SurfaceSize(const D3DSurface &s, u32 &width, u32 &height) {
  const u32 bits = s.SizeBits;
  width = ((bits >> 18) & 0x3FFFu) + 1u;
  height = ((bits >> 3) & 0x7FFFu) + 1u;
}
#else
// D3DSurface, 48 bytes. D3DSurface_GetDesc (0x8246D668, non-texture path)
// unpacks Width/Height from the dword at +0x24:
//   Width  = (SizeBits >> 18) + 1            (width-1 in bits [31:18])
//   Height = ((SizeBits >> 3) & 0x7FFF) + 1  (height-1 in bits [17:3])
// Bits [2:0] are pad.
struct D3DSurface {
  D3DResource resource; // +0x00
  be_u32 SurfaceInfo;   // +0x18  GPU_SURFACEINFO union word
  be_u32 DepthInfo;     // +0x1C  GPU_DEPTHINFO union word
  be_u32 HiControl;     // +0x20
  be_u32 SizeBits;      // +0x24  packed Height/Width
  be_u32 Format;        // +0x28  D3DFORMAT
  be_u32 Size;          // +0x2C
};
static_assert(sizeof(D3DSurface) == 48);
#endif  // SVR07_NATIVE_RENDERER

// Verified against the code at 0x8246D668. Written by D3DSurface_GetDesc
// and D3DTexture_GetLevelDesc.
struct D3DSurfaceDesc {
  be_u32 Format;             // +0x00  guest D3DFORMAT
  be_u32 Type;               // +0x04  D3DRESOURCETYPE
  be_u32 Usage;              // +0x08  always written 0 by reblue
  be_u32 Pool;               // +0x0C  always written 0 by reblue
  be_u32 MultiSampleType;    // +0x10  0, since reblue surfaces are COUNT_1
  be_u32 MultiSampleQuality; // +0x14  0
  be_u32 Width;              // +0x18  surface / mip-level width
  be_u32 Height;             // +0x1C  surface / mip-level height
};
static_assert(sizeof(D3DSurfaceDesc) == 32);

// Written by D3DVolumeTexture_GetLevelDesc. Adds Depth and drops the
// MultiSample fields, so NOT the D3DSurfaceDesc layout.
struct D3DVolumeDesc {
  be_u32 Format; // +0x00  guest D3DFORMAT
  be_u32 Type;   // +0x04  4 == D3DRTYPE_VOLUMETEXTURE
  be_u32 Usage;  // +0x08  0
  be_u32 Pool;   // +0x0C  0
  be_u32 Width;  // +0x10  mip-level width
  be_u32 Height; // +0x14  mip-level height
  be_u32 Depth;  // +0x18  mip-level depth
};
static_assert(sizeof(D3DVolumeDesc) == 28);

// D3DLOCKED_RECT. Points the engine at the texture's guest scratch mirror.
struct D3DLockedRect {
  be_u32 Pitch; // +0x00  row pitch in bytes
  be_u32 pBits; // +0x04  guest VA of the locked pixel scratch
};
static_assert(sizeof(D3DLockedRect) == 8);

#if defined(SVR07_NATIVE_RENDERER)
// SvR07 D3DVertexBuffer / D3DIndexBuffer, 32 bytes each (li r3,32 in both Create*).
// Vertex buffer (CreateVertexBuffer sub_821D2EE8): FetchLo (+0x18) = allocation | 3 (vertex fetch
// dword 0 with the type bits), FetchHi (+0x1C) = (Length & 0x3FFFFFC) | 0x10000000 | 2 (+ bit 27 kept
// from before) = fetch dword 1 with endian 8in32. SetStreamSource (sub_821D79A8) writes
// FetchLo + offset (as a GPU address) and FetchHi - offset into vertex fetch constant 95 - stream.
// Unlock (sub_821D3010) reads FetchLo & ~3.
// Index buffer (CreateIndexBuffer sub_821D3020): FetchLo (+0x18) = allocation (a CPU address, no tag
// bits; Unlock sub_821D3130 and DrawIndexedVertices read it raw), FetchHi (+0x1C) = Length in bytes;
// Common bits 29-31 = index size / endian (see D3DResource).
struct D3DVertexBuffer {
  D3DResource resource;
  be_u32 FetchLo; // +0x18  address | 3
  be_u32 FetchHi; // +0x1C  size | 0x10000002
};
static_assert(sizeof(D3DVertexBuffer) == svr07::d3d::kVertexBufferSize);
static_assert(offsetof(D3DVertexBuffer, FetchLo) == svr07::d3d::kBufferAddress);
static_assert(offsetof(D3DVertexBuffer, FetchHi) == svr07::d3d::kBufferSize);

struct D3DIndexBuffer {
  D3DResource resource;
  be_u32 FetchLo; // +0x18  Address: CPU address, no tag bits
  be_u32 FetchHi; // +0x1C  Size: bytes
};
static_assert(sizeof(D3DIndexBuffer) == svr07::d3d::kIndexBufferSize);
static_assert(offsetof(D3DIndexBuffer, FetchLo) == svr07::d3d::kBufferAddress);
static_assert(offsetof(D3DIndexBuffer, FetchHi) == svr07::d3d::kBufferSize);

inline u32 VertexBufferAddress(const D3DVertexBuffer &vb) { return u32(vb.FetchLo) & ~3u; }

// Shader / declaration objects: only the 24-byte header is modelled (the renderer keys them by
// address and reads the microcode the game passes to Create*). The rest, for logging and the census:
//   VS (CreateVertexShader sub_821DB120): 592-byte prefix + the container copy at +592; Common
//     0x00100006; +0x30 = physical microcode (IM_LOAD type 0 base); +600 = container physicalSize.
//   PS (CreatePixelShader sub_821DB030): 56-byte prefix + container at +56; Common 0x00100007;
//     +0x18 = physical microcode (IM_LOAD type 1 base); +64 = container physicalSize.
//   Declaration (CreateVertexDeclaration sub_821DAEA0): 56 + 12n, Common 0x00100005, count at +0x18,
//     the 12-byte elements copied to +0x34.
struct D3DVertexShader {
  D3DResource resource;
};
struct D3DPixelShader {
  D3DResource resource;
};
struct D3DVertexDeclaration {
  D3DResource resource;
};
static_assert(sizeof(D3DVertexShader) == 24);
static_assert(sizeof(D3DPixelShader) == 24);
static_assert(sizeof(D3DVertexDeclaration) == 24);
#else
// D3DVertexBuffer / D3DIndexBuffer, 32 bytes. The +0x18/+0x1C pair is the Xenos
// fetch constant, encoded two ways depending on who wrote it:
//   Pre-XGOffset (bdPhysical*Create replication, raw dwords):
//     VB writes size@FetchLo/base@FetchHi, IB writes (size|3)@FetchLo/
//     ((base & 0x3FFFFFC)|0x10000002)@FetchHi.
//   Post-XGOffsetResourceAddress (asset-loaded structs, Bootstrap path):
//     FetchLo = base_va | tag_bits, FetchHi = (size & 0x3FFFFFC) | 0x10000002.
// reblue's own mirror and bootstrap paths use the post-XGOffset encoding.
struct D3DVertexBuffer {
  D3DResource resource;
  be_u32 FetchLo; // +0x18
  be_u32 FetchHi; // +0x1C
};
static_assert(sizeof(D3DVertexBuffer) == 32);
static_assert(offsetof(D3DVertexBuffer, FetchLo) == 0x18);
static_assert(offsetof(D3DVertexBuffer, FetchHi) == 0x1C);

struct D3DIndexBuffer {
  D3DResource resource;
  be_u32 FetchLo; // +0x18  SvR (stock XDK): Address, a CPU address with no tag bits
  be_u32 FetchHi; // +0x1C  SvR (stock XDK): Size, exact bytes
};
static_assert(sizeof(D3DIndexBuffer) == 32);
static_assert(offsetof(D3DIndexBuffer, FetchLo) == 0x18);
static_assert(offsetof(D3DIndexBuffer, FetchHi) == 0x1C);

// D3DVertexShader / D3DPixelShader / D3DVertexDeclaration, 24 bytes each.
struct D3DVertexShader {
  D3DResource resource;
};
struct D3DPixelShader {
  D3DResource resource;
};
struct D3DVertexDeclaration {
  D3DResource resource;
};
static_assert(sizeof(D3DVertexShader) == 24);
static_assert(sizeof(D3DPixelShader) == 24);
static_assert(sizeof(D3DVertexDeclaration) == 24);
#endif  // SVR07_NATIVE_RENDERER

// The microcode blob CreateVertexShader/CreatePixelShader are handed, layout
// mirrors thirdparty/XenosRecomp/XenosRecomp/shader.h.
// SvR07 (like RR) hands the old 0x102A0E layout (24-byte header: flags virt phys defTab ctab shader;
// NR-3). Only flags, virtualSize, physicalSize and constantTableOffset (+0x10 in both layouts) may
// be read from an 0E container through this struct; the cache key is XXH3 of virtualSize +
// physicalSize bytes (native/patches/xenosrecomp-0e.patch). Both Create*Shader size their object
// from flags & 0x3F, virtualSize (+4) and physicalSize (+8) (sub_821DB120 / sub_821DB030).
struct ShaderContainer {
  be_u32 flags;
  be_u32 virtualSize;
  be_u32 physicalSize;
  be_u32 fieldC;
  be_u32 constantTableOffset;
  be_u32 definitionTableOffset;
  be_u32 shaderOffset;
  be_u32 field1C;
  be_u32 field20;
};
static_assert(sizeof(ShaderContainer) == 0x24);

// Direct3D_CreateDevice allocates 0x5000 via
// D3D_AllocAlignedZeroed: the 0x2A00-byte public D3DDevice plus BD-private
// slots past it.
#if defined(SVR07_NATIVE_RENDERER)
constexpr u32 kD3DDeviceAllocSize = svr07::d3d::kDeviceSize;  // Direct3D_CreateDevice sub_821DECF0, li r3,20480
#else
constexpr u32 kD3DDeviceAllocSize = 0x5000;
#endif

// dword[0] bits [1:0] of a fetch constant tag what it describes.
constexpr u32 kFetchTypeMask = 0x3;
constexpr u32 kFetchTypeTexture = 2;
constexpr u32 kFetchTypeVertex = 3;

// 24-byte Xenos GPU fetch descriptor. D3DDevice holds 32 in fetchConstants[].
struct DeviceFetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(DeviceFetchConstant) == 24);

// Shared guest memory: the recompiled engine, reblue's hooks and the draw path
// all read and write it. Only the fields reblue reads are named. Verified
// against the recovered D3DDevice / _D3DConstants types and BD's own code. Each
// named field past the public struct cites where it came from.
#if defined(SVR07_NATIVE_RENDERER)
// SvR07 D3DDevice: 0x5000 bytes (Direct3D_CreateDevice sub_821DECF0, li r3,20480). Blue Dragon's
// layout (the #else branch below) up to the register shadows, which sit at UFC / SvR 2010's table
// - 0x80; past +0x2A00 Blue Dragon's fields + 0x10 (decl +0x2D20, RT +0x2F98, viewport +0x3068,
// shaders +0x3090). Every offset comes from src/svr07_d3d.h, which names the instruction proving it.
// Only the fields the native renderer, the draw log and the census read are named.
struct D3DDevice {
  // Dirty masks: [0] VS float constants, [1] PS float constants (the draws upload +0x700 / +0x1700
  // when set and clear them), [2] state, [3] (SetViewport ORs 0x4..0x80), [4].
  be_u64 m_Mask[5];                      // +0x000
  be_u32 m_pRing;                        // +0x028 ring write pointer (pre-increment stores)
  be_u32 m_pRingLimit;                   // +0x02C (svr07_d3d.h: unverified)
  be_u32 m_pRingGuarantee;               // +0x030 a draw kicks off when the ring passes it
  u8 pad_0034[0x400 - 0x34];             // render-state call tables somewhere here (unlocated)
  DeviceFetchConstant fetchConstants[32];  // +0x400 SetTexture: + 24 * sampler; stream s = vfetch 95 - s
  be_f32 vsFloatConstants[256][4];       // +0x700  sent as 0x4000.. (DrawIndexedVertices)
  be_f32 psFloatConstants[256][4];       // +0x1700 sent as 0x4400..
  be_u32 vsBoolConstants[4];             // +0x2700 sent with the PS ones as 8 dwords at 0x4900
  be_u32 psBoolConstants[4];             // +0x2710
  u8 pad_2720[0x2800 - 0x2720];          // int / loop constants, clip planes (not read)
  // Xenos register shadows, one dword per register (svr07_d3d.h kRegs*): sent by sub_821E1878.
  be_u32 regs2000[16];                   // +0x2800 RB_SURFACE_INFO.. (RB_COLOR_INFO +0x2804, RB_DEPTH_INFO +0x2808)
  be_u32 regs2080[3];                    // +0x2840 window offset, window scissor TL / BR (+0x2844 / +0x2848)
  be_u32 regs2100[21];                   // +0x284C (RB_COLOR_MASK +0x285C, PA_CL_VPORT +0x2888..+0x289C)
  be_u32 regs2180[5];                    // +0x28A0
  be_u32 rbDepthControl;                 // +0x28B4 0x2200 RB_DEPTHCONTROL
  be_u32 rbBlendControl0;                // +0x28B8 0x2201 RB_BLENDCONTROL0
  be_u32 rbColorControl;                 // +0x28BC 0x2202 RB_COLORCONTROL
  be_u32 regs2203[6];                    // +0x28C0 0x2203 RB_HIZCONTROL .. 0x2208 (PA_CL_VTE_CNTL 0x2206 = +0x28CC)
  be_u32 rbBlendControl1;                // +0x28D8 0x2209
  be_u32 rbBlendControl2;                // +0x28DC 0x220A
  be_u32 rbBlendControl3;                // +0x28E0 0x220B
  be_u32 regs2280[21];                   // +0x28E4
  be_u32 regs2300[38];                   // +0x2938 (PA_SU_VTX_CNTL 0x2302 = +0x2940)
  be_u32 regs2380[8];                    // +0x29D0
  u8 pad_29F0[0x2A28 - 0x29F0];
  be_u32 gprAllocation;                  // +0x2A28 SetShaderGPRAllocation's SQ_GPR_MANAGEMENT value
  u8 pad_2A2C[0x2A38 - 0x2A2C];
  u8 deviceFlags[4];                     // +0x2A38 byte flags (EndVertices tests +0x2A38 bit 0x80)
  u8 pad_2A3C[0x2D20 - 0x2A3C];
  be_u32 vertexDeclaration;              // +0x2D20 SetVertexDeclaration sub_821DAE80
  u8 pad_2D24[0x2F94 - 0x2D24];
  be_u32 indexBuffer;                    // +0x2F94 SetIndices sub_821D7B58 (pointer only)
  be_u32 renderTargetShadow[4];          // +0x2F98 SetRenderTarget sub_821D7F60 ((idx + 3046) * 4)
  be_u32 depthStencilShadow;             // +0x2FA8 SetDepthStencilSurface sub_821D8290
  be_u32 streamSource[16];               // +0x2FAC SetStreamSource sub_821D79A8: the D3DVertexBuffer per stream
  be_u32 streamPad;                      // +0x2FEC (svr07_d3d.h: unverified, census logs it)
  u8 streamStrideDiv4[16];               // +0x2FF0 stride / 4 per stream
  be_u32 boundTexture[26];               // +0x3000 SetTexture sub_821D5058 ((3072 + sampler) * 4)
  D3DViewport9 viewport;                 // +0x3068 SetViewport sub_821D7768: integer X, Y, W, H, float MinZ, MaxZ
  D3DRect scissorRect;                   // +0x3080 SetScissorRect sub_821D76A0 (no viewport flags field)
  be_u32 pixelShader;                    // +0x3090 SetPixelShader sub_821DAAA8
  be_u32 vertexShader;                   // +0x3094 SetVertexShader body sub_821DACE0
  u8 pad_3098[0x3324 - 0x3098];
  be_u32 savedRingEnd;                   // +0x3324 BeginVertices saves, EndVertices restores the ring
  u8 pad_3328[0x3364 - 0x3328];
  be_u32 vertexShaderPublic;             // +0x3364 SetVertexShader sub_821DAE30 (public entry)
  u8 pad_3368[0x33E4 - 0x3368];
  u8 presentParams[0x3474 - 0x33E4];     // +0x33E4 Swap's parameter block (size: up to the buffers)
  be_u32 frontBuffer;                    // +0x3474 Present resolves into it, Swap shows it
  be_u32 backBuffer;                     // +0x3478 Present: SetRenderTarget(0, back)
  u8 pad_347C[0x4FD8 - 0x347C];
  be_u32 createFlags;                    // +0x4FD8 Direct3D_CreateDevice
  u8 pad_4FDC[0x5000 - 0x4FDC];
};
namespace s7 = svr07::d3d;
static_assert(sizeof(D3DDevice) == s7::kDeviceSize);
static_assert(sizeof(D3DDevice) == kD3DDeviceAllocSize);
static_assert(offsetof(D3DDevice, m_Mask) == s7::kDirtyMasks);
static_assert(offsetof(D3DDevice, m_Mask) + 8 == s7::kPsConstDirty);
static_assert(offsetof(D3DDevice, m_Mask) + 16 == s7::kStateDirty);
static_assert(offsetof(D3DDevice, m_pRing) == s7::kRing);
static_assert(offsetof(D3DDevice, m_pRingLimit) == s7::kRingLimit);
static_assert(offsetof(D3DDevice, m_pRingGuarantee) == s7::kRingGuarantee);
static_assert(offsetof(D3DDevice, fetchConstants) == s7::kFetchConstants);
static_assert(offsetof(D3DDevice, vsFloatConstants) == s7::kVsFloatConstants);
static_assert(offsetof(D3DDevice, psFloatConstants) == s7::kPsFloatConstants);
static_assert(offsetof(D3DDevice, vsBoolConstants) == s7::kVsBoolConstants);
static_assert(offsetof(D3DDevice, psBoolConstants) == s7::kPsBoolConstants);
static_assert(offsetof(D3DDevice, regs2000) == s7::kRegs2000);
static_assert(offsetof(D3DDevice, regs2080) == s7::kRegs2080);
static_assert(offsetof(D3DDevice, regs2100) == s7::kRegs2100);
static_assert(offsetof(D3DDevice, regs2180) == s7::kRegs2180);
static_assert(offsetof(D3DDevice, rbDepthControl) == s7::kRbDepthControl);
static_assert(offsetof(D3DDevice, rbBlendControl0) == s7::kRbBlendControl0);
static_assert(offsetof(D3DDevice, rbColorControl) == s7::kRbColorControl);
static_assert(offsetof(D3DDevice, regs2203) == s7::kRbHiZControl);
static_assert(offsetof(D3DDevice, rbBlendControl1) == s7::kRbBlendControl1);
static_assert(offsetof(D3DDevice, rbBlendControl2) == s7::kRbBlendControl2);
static_assert(offsetof(D3DDevice, rbBlendControl3) == s7::kRbBlendControl3);
static_assert(offsetof(D3DDevice, regs2280) == s7::kRegs2280);
static_assert(offsetof(D3DDevice, regs2300) == s7::kRegs2300);
static_assert(offsetof(D3DDevice, regs2380) == s7::kRegs2380);
static_assert(offsetof(D3DDevice, gprAllocation) == s7::kGprAllocation);
static_assert(offsetof(D3DDevice, deviceFlags) == s7::kDeviceFlags);
static_assert(offsetof(D3DDevice, vertexDeclaration) == s7::kVertexDeclaration);
static_assert(offsetof(D3DDevice, indexBuffer) == s7::kIndexBuffer);
static_assert(offsetof(D3DDevice, renderTargetShadow) == s7::kRenderTarget0);
static_assert(offsetof(D3DDevice, depthStencilShadow) == s7::kDepthStencil);
static_assert(offsetof(D3DDevice, streamSource) == s7::kStreamSource);
static_assert(sizeof(D3DDevice::streamSource[0]) == s7::kStreamSourceStride);
static_assert(offsetof(D3DDevice, streamPad) == s7::kStreamPad);
static_assert(offsetof(D3DDevice, streamStrideDiv4) == s7::kStreamStrideDiv4);
static_assert(offsetof(D3DDevice, boundTexture) == s7::kTexture);
static_assert(sizeof(D3DDevice::boundTexture) / 4 == s7::kTextureSlots);
static_assert(offsetof(D3DDevice, viewport) == s7::kViewport);
static_assert(sizeof(D3DDevice::viewport) == s7::kViewportSize);
static_assert(offsetof(D3DDevice, scissorRect) == s7::kScissorRect);
static_assert(offsetof(D3DDevice, pixelShader) == s7::kPixelShader);
static_assert(offsetof(D3DDevice, vertexShader) == s7::kVertexShader);
static_assert(offsetof(D3DDevice, savedRingEnd) == s7::kSavedRingEnd);
static_assert(offsetof(D3DDevice, vertexShaderPublic) == s7::kVertexShaderPublic);
static_assert(offsetof(D3DDevice, presentParams) == s7::kPresentParams);
static_assert(offsetof(D3DDevice, frontBuffer) == s7::kFrontBuffer);
static_assert(offsetof(D3DDevice, backBuffer) == s7::kBackBuffer);
static_assert(offsetof(D3DDevice, createFlags) == s7::kCreateFlags);

// Stream N's D3DVertexBuffer. SvR07 keeps a bare pointer (UFC's shape, not RR's {offset, vb}); the
// offset lives only in vertex fetch constant 95 - N (StreamFetch in native/ufc_geometry.cpp).
inline u32 StreamVertexBuffer(const D3DDevice &d, u32 stream) { return d.streamSource[stream]; }
// Vertex fetch constant 95 - N: dword0 = GPU address of vertex 0 (offset included) | 3,
// dword1 = bytes from there to the end | endian (SetStreamSource stwx (223 - s) * 8 / stw 1788 - 8s).
inline void StreamFetchDwords(const D3DDevice &d, u32 stream, u32 &dword0, u32 &dword1) {
  const u32 constant = 95u - stream;
  const DeviceFetchConstant &slot = d.fetchConstants[constant / 3u];
  dword0 = u32(slot.dword[(constant % 3u) * 2u]);
  dword1 = u32(slot.dword[(constant % 3u) * 2u + 1u]);
}
#elif defined(SVR_NATIVE_RENDERER)
// UFC 3 D3DDevice: 0x5F80 bytes (Direct3D_CreateDevice sub_82374F80 allocates 24448). The
// constants block and register shadows match SvR 2010 (to +0x2AC0); the later fields are UFC's own
// (src/native/ufc_d3d.h names the function whose store proves each one): vertex declaration
// SvR + 0x80, the index buffer .. shaders block SvR + 0x48. Only the fields the native renderer
// reads or writes are named.
struct SvrViewport {
  be_f32 X, Y, Width, Height, MinZ, MaxZ;  // SetViewport sub_82377F58 stores floats
};
static_assert(sizeof(SvrViewport) == 24);

struct D3DDevice {
  be_u64 m_Mask[5];                     // +0x000
  u8 pad_0028[0x480 - 0x28];
  DeviceFetchConstant fetchConstants[32];  // +0x480  SetTexture: device + (sampler+48)*24
  be_f32 vsFloatConstants[256][4];      // +0x780  draws flush 0x4000.. from device+0x780
  be_f32 psFloatConstants[256][4];      // +0x1780 draws flush 0x4400.. from device+0x1780
  be_u32 vsBoolConstants[4];            // +0x2780 (constants block +0x80, as above)
  be_u32 psBoolConstants[4];            // +0x2790
  u8 pad_27A0[0x2938 - 0x27A0];
  be_u32 rbBlendControl0;               // +0x2938 register 0x2201 (SetRenderState_AlphaBlendEnable)
  u8 pad_293C[0x2958 - 0x293C];
  be_u32 rbBlendControl1;               // +0x2958 0x2209
  be_u32 rbBlendControl2;               // +0x295C 0x220A
  be_u32 rbBlendControl3;               // +0x2960 0x220B
  u8 pad_2964[0x2F58 - 0x2964];
  be_u32 vertexDeclaration;             // +0x2F58 SetVertexDeclaration sub_82371BB8
  u8 pad_2F5C[0x318C - 0x2F5C];
  be_u32 indexBuffer;                   // +0x318C SetIndices sub_82377900
  be_u32 renderTargetShadow[4];         // +0x3190 SetRenderTarget sub_82378088
  be_u32 depthStencilShadow;            // +0x31A0 SetDepthStencilSurface sub_82378418
  // SetStreamSource (sub_82377758): stwx vb,(3177+stream)*4 and the stride>>2 byte at
  // +0x31E8+stream. The stream's fetch constant goes to fetchConstants (vertex fetch constant 95 - stream).
  be_u32 streamSource[16];              // +0x31A4 D3DVertexBuffer per stream
  u8 pad_31E4[0x31E8 - 0x31E4];
  u8 streamStrideDiv4[16];              // +0x31E8 stride / 4 per stream
  u8 pad_31F8[0x3260 - 0x31F8];         // +0x31F8 SetTexture's texture per sampler
  SvrViewport viewport;                 // +0x3260 SetViewport
  be_u32 viewportFlags;                 // +0x3278
  D3DRect scissorRect;                  // +0x327C SetScissorRect sub_82377610
  be_u32 pixelShader;                   // +0x328C SetPixelShader sub_82371798
  be_u32 vertexShader;                  // +0x3290 SetVertexShader sub_823719A0
  u8 pad_3294[0x5F80 - 0x3294];
};
static_assert(sizeof(D3DDevice) == 0x5F80);
static_assert(offsetof(D3DDevice, fetchConstants) == 0x480);
static_assert(offsetof(D3DDevice, vsFloatConstants) == 0x780);
static_assert(offsetof(D3DDevice, psFloatConstants) == 0x1780);
static_assert(offsetof(D3DDevice, vsBoolConstants) == 0x2780);
static_assert(offsetof(D3DDevice, rbBlendControl0) == 0x2938);
static_assert(offsetof(D3DDevice, vertexDeclaration) == 0x2F58);
static_assert(offsetof(D3DDevice, indexBuffer) == 0x318C);
static_assert(offsetof(D3DDevice, renderTargetShadow) == 0x3190);
static_assert(offsetof(D3DDevice, depthStencilShadow) == 0x31A0);
static_assert(offsetof(D3DDevice, streamSource) == 0x31A4);
static_assert(offsetof(D3DDevice, streamStrideDiv4) == 0x31E8);
static_assert(offsetof(D3DDevice, viewport) == 0x3260);
static_assert(offsetof(D3DDevice, scissorRect) == 0x327C);
static_assert(offsetof(D3DDevice, pixelShader) == 0x328C);
static_assert(offsetof(D3DDevice, vertexShader) == 0x3290);
#else
struct D3DDevice {
  // m_Pending (_D3DTAGCOLLECTION): 5 dirty mask u64s. Set* setters OR into
  // m_Mask[2] (byte +0x10), SetSamplerStateInline into m_Mask[3].
  be_u64 m_Mask[5];        // +0x000
  be_u32 m_pRing;          // +0x028
  be_u32 m_pRingLimit;     // +0x02C
  be_u32 m_pRingGuarantee; // +0x030
  be_u32 m_ReferenceCount; // +0x034
  // Per-state dispatch tables, seeded by Direct3D_CreateDevice_hook /
  // CopyDispatchTable from the XEX template tables.
  be_u32 m_SetRenderStateCall[97];  // +0x038
  be_u32 m_SetSamplerStateCall[20]; // +0x1BC
  be_u32 m_GetRenderStateCall[97];  // +0x20C
  be_u32 m_GetSamplerStateCall[20]; // +0x390
  u8 pad_03E0[0x400 - 0x3E0];       // +0x3E0
  // m_Constants (_D3DConstants, +0x400..+0x27A0). The unified Xenos fetch
  // table: 32 slots shared by texture (type 2) and vertex (type 3) fetch
  // constants, flushed by CommitTextureFetchConstants 0x82486018.
  DeviceFetchConstant fetchConstants[32]; // +0x400
  be_f32 vsFloatConstants[256][4];        // +0x700
  be_f32 psFloatConstants[256][4];        // +0x1700
  // VS / PS boolean constants, 4 u32 (128 bits) each.
  // D3DDevice_SetVertexShaderConstantB writes dword (N/32)+2496 =
  // device+0x2700, and SetPixelShaderConstantB writes +2500 =
  // device+0x2710. The rest of the _D3DConstants tail is int/loop constants.
  be_u32 vsBoolConstants[4];    // +0x2700
  be_u32 psBoolConstants[4];    // +0x2710
  u8 pad_2720[0x27A0 - 0x2720]; // +0x2720  int/loop constants
  be_f32 m_ClipPlanes[6][4];    // +0x27A0
  u8 pad_2800[0x28B8 - 0x2800]; // +0x2800  GPU packets + guest tail
  // RB_BLENDCONTROL0..3 (Xenos blend ROP state), seeded to no-blend. Slot 0 is
  // non-contiguous with the 1..3 trio.
  be_u32 rbBlendControl0;       // +0x28B8
  u8 pad_28BC[0x28D8 - 0x28BC]; // +0x28BC
  be_u32 rbBlendControl1;       // +0x28D8
  be_u32 rbBlendControl2;       // +0x28DC
  be_u32 rbBlendControl3;       // +0x28E0
  u8 pad_28E4[0x2A39 - 0x28E4]; // +0x28E4
  // "Resolution applied" bit. D3DDevice_Reset/SetResolution ORs in 0x10.
  u8 resolutionApplied;         // +0x2A39
  u8 pad_2A3A[0x2D10 - 0x2A3A]; // +0x2A3A
  // D3DDevice_SetVertexDeclaration: stw r4, 0x2D10(r3).
  be_u32 vertexDeclaration;     // +0x2D10
  u8 pad_2D14[0x2D3C - 0x2D14]; // +0x2D14
  // Xenos RB_COLORCONTROL shadow. Bit 31 is the alpha blend master enable.
  // ReadDeviceRenderState reads regs[2895] (2895*4 = 0x2D3C). The regs[] index
  // == device byte offset / 4 mapping holds because regs[2606]*4 = 0x28B8, the
  // rbBlendControl0 offset asserted below.
  be_u32 rbColorControl;        // +0x2D3C
  u8 pad_2D40[0x2F88 - 0x2D40]; // +0x2D40
  // Bound RT/DS surface shadow. D3DDevice_GetRenderTarget (0x824739F0) reads
  // +0x2F88 + idx*4, and D3DDevice_GetDepthStencilSurface (0x82473A38) reads
  // +0x2F98. Hooks replaced the recompiled Set* bodies that wrote these, so the
  // hooks must maintain them or the RT stack save/restore
  // (bdRenderTargetStackPush/Pop 0x82273080/0x82273240, every Sofdec movie
  // frame) restores garbage.
  be_u32 renderTargetShadow[4]; // +0x2F88
  be_u32 depthStencilShadow;    // +0x2F98
  u8 pad_2F9C[0x3058 - 0x2F9C]; // +0x2F9C
  // D3DDevice_SetViewport stores X/Y/W/H/MinZ/MaxZ at +0x3058 and
  // the scissor rect at +0x3070.
  D3DViewport9 viewport; // +0x3058
  D3DRect scissorRect;   // +0x3070
  // BD-private slots past the public 0x2A00 struct.
  // D3DDevice_SetPixelShader: stw r29, 0x3080(r30).
  // D3DDevice_SetVertexShader: stw r29, 0x3084(r30).
  be_u32 pixelShader;           // +0x3080
  be_u32 vertexShader;          // +0x3084
  u8 pad_3088[0x3370 - 0x3088]; // +0x3088
  // D3DPRESENT_PARAMETERS copy written verbatim by D3DDevice_Reset (124 bytes).
  u8 presentParams[124];        // +0x3370
  u8 pad_33EC[0x4D38 - 0x33EC]; // +0x33EC
  // D3DDevice_SetIndices (0x8247B328): stw r30, 0x4D38(r31).
  be_u32 indexBuffer;           // +0x4D38
  u8 pad_4D3C[0x4DFC - 0x4D3C]; // +0x4D3C
  // Resolution block mirrored by D3DDevice_Reset
  // (D3D::InitializePresentationParameters): width, height, width(dup),
  // config flag.
  be_u32 resolutionWidth;       // +0x4DFC
  be_u32 resolutionHeight;      // +0x4E00
  be_u32 resolutionWidthDup;    // +0x4E04
  be_u32 resolutionConfig;      // +0x4E08
  u8 pad_4E0C[0x5000 - 0x4E0C]; // +0x4E0C
};
static_assert(sizeof(D3DDevice) == 0x5000);
static_assert(offsetof(D3DDevice, m_pRing) == 0x028);
static_assert(offsetof(D3DDevice, m_SetRenderStateCall) == 0x038);
static_assert(offsetof(D3DDevice, m_SetSamplerStateCall) == 0x1BC);
static_assert(offsetof(D3DDevice, m_GetRenderStateCall) == 0x20C);
static_assert(offsetof(D3DDevice, m_GetSamplerStateCall) == 0x390);
static_assert(offsetof(D3DDevice, fetchConstants) == 0x400);
static_assert(offsetof(D3DDevice, vsFloatConstants) == 0x700);
static_assert(offsetof(D3DDevice, psFloatConstants) == 0x1700);
static_assert(offsetof(D3DDevice, vsBoolConstants) == 0x2700);
static_assert(offsetof(D3DDevice, psBoolConstants) == 0x2710);
static_assert(offsetof(D3DDevice, m_ClipPlanes) == 0x27A0);
static_assert(offsetof(D3DDevice, vertexDeclaration) == 0x2D10);
static_assert(offsetof(D3DDevice, rbColorControl) == 0x2D3C);
static_assert(offsetof(D3DDevice, renderTargetShadow) == 0x2F88);
static_assert(offsetof(D3DDevice, depthStencilShadow) == 0x2F98);
static_assert(offsetof(D3DDevice, viewport) == 0x3058);
static_assert(offsetof(D3DDevice, scissorRect) == 0x3070);
static_assert(offsetof(D3DDevice, pixelShader) == 0x3080);
static_assert(offsetof(D3DDevice, vertexShader) == 0x3084);
static_assert(offsetof(D3DDevice, indexBuffer) == 0x4D38);
static_assert(offsetof(D3DDevice, resolutionApplied) == 0x2A39);
static_assert(offsetof(D3DDevice, presentParams) == 0x3370);
static_assert(offsetof(D3DDevice, resolutionWidth) == 0x4DFC);
static_assert(offsetof(D3DDevice, rbBlendControl0) == 0x28B8);
static_assert(offsetof(D3DDevice, rbBlendControl1) == 0x28D8);
static_assert(offsetof(D3DDevice, rbBlendControl2) == 0x28DC);
static_assert(offsetof(D3DDevice, rbBlendControl3) == 0x28E0);
#endif  // SVR07_NATIVE_RENDERER / SVR_NATIVE_RENDERER

#if !defined(SVR07_NATIVE_RENDERER)
// D3DResource_GetType decodes the type from D3DResource:
//   base = Common & 0xF
//     1 -> Surface  (extended to 16 if Common & 0x40000000 + texture-backed)
//     3 -> Texture, then dim = (fetch_dword5 >> 9) & 3:
//            0 -> returns 20 (1D / line texture)
//            1 -> returns 3 if !tiled, 19 if tiled (fetch_dword1 & 0x400)
//            2 -> returns 17 (volume texture)
//            3 -> returns 18 (stacked cube)
//     6 -> VertexBuffer
//     7 -> IndexBuffer
// The Common low-4 bits select the base type. Texture dimensionality lives in
// the fetch constant. Bit 30 (0x40000000) on a surface marks it texture-backed,
// which routes D3DSurface_GetDesc through D3DTexture_GetLevelDesc.
constexpr u32 kCommonFlagTextureBackedSurface = 0x40000000u;
#endif  // !SVR07_NATIVE_RENDERER (SvR07: same bit, kCommonLevelSurface in its branch above)

constexpr u32 kClearTarget = 0x1u;
constexpr u32 kClearZBuffer = 0x10u;
constexpr u32 kClearStencil = 0x20u;
constexpr u32 kClearAll = kClearTarget | kClearZBuffer | kClearStencil;

constexpr u32 kResolveDepthStencil = 0x4u;
constexpr u32 kResolveExponentBiasShift = 26;

#if defined(SVR07_NATIVE_RENDERER)
// As SvR07's Create* functions leave a new resource: type, created bit (0x00100000), refcount 1,
// BaseFlush 0xFFFF0000 (CreateVertexBuffer / CreatePixelShader / CreateVertexDeclaration stores).
inline void InitResourceHeader(D3DResource &r, D3DResourceType type) {
  r.Common = kCommonCreated | u32(type);
  r.ReferenceCount = 1u;
  r.Fence = 0u;
  r.ReadFence = 0u;
  r.Identifier = 0u;
  r.BaseFlush = 0xFFFF0000u;
}
#else
inline void InitResourceHeader(D3DResource &r, D3DResourceType type) {
  r.Common = u32(type);
  r.ReferenceCount = 1u;
  r.Fence = 0u;
  r.ReadFence = 0u;
  r.Identifier = 0u;
  r.BaseFlush = 0u;
}
#endif  // SVR07_NATIVE_RENDERER

} // namespace bd::gpu
