// SvR07's XDK D3D layout as offsets (lane L3, 2026-11-10): SvR07's counterpart of UFC-native's
// src/native/ufc_d3d.h and RR's rr_d3d.h. The same layout as structs is reblue/gpu/d3d.h's
// SVR07_NATIVE_RENDERER branch; its static_asserts compare every struct offset with these constants.
//
// SvR07 links XDK D3D9 2.0.3215.1 (base default.xex, NOT the title update: TU1 moves every address).
// Device = 0x5000 bytes (Direct3D_CreateDevice sub_821DECF0, li r3,20480). D3D library
// 0x821D2A60-0x821EB230. Each offset names the function whose instruction proves it, read from
// SVR07-Recomp-native/generated/default (reports/L3-d3d-verification.md has the table). Offline
// evidence only: the census (src/dev/svr07_d3d_census.cpp, Stop 1) checks them at run time.
// Every number not proven by an instruction carries "SVR07-UNVERIFIED" (grep lists them).
//
// SvR07 against Rumble Roses (the tree this was copied from): 24-byte resource header with the
// refcount at +4 (RR: low byte of Common), VB/IB 32 bytes (RR 20), texture 52 (RR 40), surface 48
// (RR 64), stream table = 4-byte VB pointers with the offset folded into the vertex fetch constant
// (RR: 8-byte {offset, vb}), register shadows = UFC/SvR-2010 table - 0x80 (RR: + 0x440), dirty masks
// at +0x00 and the ring at +0x28 (RR: ring +0x00, masks +0x10). Same as RR: 26 bound-texture slots,
// integer viewport (no SetViewportF, no flags field), no BeginIndexedVertices / DrawVerticesUP.

#pragma once

#include <cstdint>

namespace svr07::d3d {

constexpr uint32_t kDeviceSize = 0x5000;  // Direct3D_CreateDevice sub_821DECF0: li r3,20480

// Dirty masks (be_u64 x5) and the ring.
constexpr uint32_t kDirtyMasks = 0x000;     // m_Mask[5], +0x00..+0x27:
constexpr uint32_t kVsConstDirty = 0x000;   //   [0] DrawIndexedVertices sub_821D9D78: ld +0 -> upload +0x700 as reg 0x4000; SetVS body ORs into it
constexpr uint32_t kPsConstDirty = 0x008;   //   [1] same draw: ld +8 -> upload +0x1700 as reg 0x4400; SetPS sub_821DAAA8 ORs into it
constexpr uint32_t kStateDirty = 0x010;     //   [2] SetVertexDeclaration sub_821DAE80 ORs 1<<51; SetTexture / SetStreamSource OR their mask arg
constexpr uint32_t kDirtyMaskCount = 5;     //   [3] +0x18 (SetViewport ORs 4..128), [4] +0x20 (draw: ld +32)
constexpr uint32_t kRing = 0x028;           // ring write pointer (stwu pre-increment): commit sub_821E2760 lwz/stw +40
constexpr uint32_t kRingGuarantee = 0x030;  // commit: ring > +0x30 -> KickOff/MakeSpace sub_821DD940
// SVR07-CENSUS-1 (2026-11-11, laptop, emulated, title/menus/1v1 match): ring +0x28 B21706C0 < +0x2C B22B033C, +0x2C - +0x30 = 0xA0: limit confirmed. Was: ring limit at +0x2C by Blue Dragon's layout (+0x28 ring, +0x2C limit, +0x30 guarantee); no 07 store read | census: log +0x28/+0x2C/+0x30 at the first Swap
constexpr uint32_t kRingLimit = 0x02C;

// Render-state call tables: Blue Dragon keeps them at +0x38; 07's setters (0x821D54B0-0x821D7640)
// have no direct callers. The observe-mode renderer reads register shadows, not these tables.
// SVR07-CENSUS-1 (2026-11-11, laptop, emulated, title/menus/1v1 match): 232 code pointers, first +0x38 (8241F180), last +0x3D4: confirmed. Was: dispatch table not located offline | census: count code pointers in +0x34..+0x3FC at the first draw (DumpDeviceOnce)
constexpr uint32_t kStateCallScanBegin = 0x034;
constexpr uint32_t kStateCallScanEnd = 0x400;

// Constants (_D3DConstants), Blue Dragon's place.
constexpr uint32_t kFetchConstants = 0x400;     // SetTexture sub_821D5058: addi r11,r11,1024 + 24 * sampler
constexpr uint32_t kFetchConstantSize = 24;     //   6 dwords per slot, 32 slots
constexpr uint32_t kFetchConstantCount = 32;
constexpr uint32_t kVsFloatConstants = 0x700;   // DrawIndexedVertices: addi r6,r31,1792 / li r5,16384; SetVSConstF sub_821DA738: (start + 112) * 16
constexpr uint32_t kPsFloatConstants = 0x1700;  // same draw: addi r6,r31,5888 / li r5,17408; SetPSConstF sub_821DA848: (start + 368) * 16
constexpr uint32_t kVsBoolConstants = 0x2700;   // same draw: addi r6,r31,9984 / li r5,18688 (8 dwords 0x4900..0x4907)
constexpr uint32_t kPsBoolConstants = 0x2710;   //   the second 4 of those 8 (0x4904..: PS bools by Xenos register order)

// Xenos register shadows: one dword per register, sent by the block writer sub_821E1878(dev, mask,
// first register, source) from the draws (sub_821D94A0 / 9970 / 9D78) and the state flush sub_821D8578.
// = the UFC / SvR 2010 table (0x2880..) - 0x80. Block lengths = the width of each block's mask.
constexpr uint32_t kRegs2000 = 0x2800;  // 16: li r5,8192 / addi r6,r31,10240 (mask: 16 bits)
constexpr uint32_t kRegs2080 = 0x2840;  // 3:  SetScissorRect sub_821D76A0 stores 0x2081/0x2082 at +0x2844/+0x2848
constexpr uint32_t kRegs2100 = 0x284C;  // 21: li r5,8448 / addi r6,r31,10316 (mask bits 2..22)
constexpr uint32_t kRegs2180 = 0x28A0;  // 5:  li r5,8576 / addi r6,r31,10400 (mask 31 << 44)
constexpr uint32_t kRegs2200 = 0x28B4;  // 12: li r5,8704 / addi r6,r31,10420 (mask: 12 bits)
constexpr uint32_t kRegs2280 = 0x28E4;  // 21: li r5,8832 / addi r6,r31,10468 (mask 0x1FFFFF << 39)
constexpr uint32_t kRegs2300 = 0x2938;  // 38: li r5,8960 / addi r6,r31,10552 (mask: 38 bits)
constexpr uint32_t kRegs2380 = 0x29D0;  // 8:  li r5,9088 / addi r6,r31,10704 (mask 255 << 38)
constexpr uint32_t kRegsEnd = 0x29F0;
// 0x2080 block send (found 2026-11-11): no block writer; sub_821D51D0(dev, l, t, r, b) writes its own type-0
// header `lis r10,2; ori r10,r10,8320` = 0x00022080 (3 registers from 0x2080), then 0, TL, BR built from its
// arguments. Its callers SetScissorRect / sub_821D7DC0 / sub_821D8578 pass the shadows +0x2844/+0x2848 (Resolve
// sub_821DB9C0 passes its rect first, then the shadows). The census re-checks it: "0x2080 block" lines.
constexpr uint32_t kRegs2080Count = 3;
// Registers named by the renderer (all inside the blocks above).
constexpr uint32_t kRbSurfaceInfo = 0x2800;       // 0x2000: block base (0x2001 / 0x2002 stores below)
constexpr uint32_t kRbColorInfo = 0x2804;         // 0x2001: SetRenderTarget(0): surface +0x1C -> (0 + 2561) * 4
constexpr uint32_t kRbDepthInfo = 0x2808;         // 0x2002: SetDepthStencilSurface sub_821D8290: surface +0x1C -> +10248
constexpr uint32_t kPaScScreenScissorTL = 0x2838; // 0x200E (by block position)
constexpr uint32_t kPaScWindowScissorTL = 0x2844; // 0x2081: SetScissorRect stw r11,10308(r3)
constexpr uint32_t kPaScWindowScissorBR = 0x2848; // 0x2082: SetScissorRect stw r10,10312(r3)
constexpr uint32_t kRbColorMask = 0x285C;         // 0x2104: SetRenderTarget lwz/stw +10332
constexpr uint32_t kPaClVportXScale = 0x2888;     // 0x210F..0x2114: SetViewport sub_821D7768 stfs +10376..+10396
constexpr uint32_t kRbDepthControl = 0x28B4;      // 0x2200
constexpr uint32_t kRbBlendControl0 = 0x28B8;     // 0x2201: SetRenderState_* sub_821D5550 stw r11,10424(r3)
constexpr uint32_t kRbColorControl = 0x28BC;      // 0x2202: sub_821D5510 stw r11,10428(r3)
constexpr uint32_t kRbHiZControl = 0x28C0;        // 0x2203: SetDepthStencilSurface: surface +0x20 -> +10432
constexpr uint32_t kPaClVteCntl = 0x28CC;         // 0x2206 (by block position; RR +0x2D8C, UFC/SvR +0x294C)
constexpr uint32_t kRbBlendControl1 = 0x28D8;     // 0x2209: sub_821D5550 stw r11,10456(r3)
constexpr uint32_t kRbBlendControl2 = 0x28DC;     // 0x220A: +10460
constexpr uint32_t kRbBlendControl3 = 0x28E0;     // 0x220B: +10464
constexpr uint32_t kPaSuVtxCntl = 0x2940;         // 0x2302 (by block position; RR +0x2E00, UFC/SvR +0x29C0)
constexpr uint32_t kGprAllocation = 0x2A28;       // SetShaderGPRAllocation sub_821DAF80: stw r31,10792(r30)
constexpr uint32_t kDeviceFlags = 0x2A38;         // byte flags read by EndVertices sub_821D9950 (bit 0x80: skip sub_8241F180)

// Register -> device offset for the eight blocks (0 if the register has no shadow).
constexpr uint32_t RegisterOffset(uint32_t reg) {
  struct Block {
    uint32_t first, count, offset;
  };
  constexpr Block kBlocks[] = {
      {0x2000, 16, kRegs2000}, {0x2080, kRegs2080Count, kRegs2080}, {0x2100, 21, kRegs2100},
      {0x2180, 5, kRegs2180},  {0x2200, 12, kRegs2200}, {0x2280, 21, kRegs2280},
      {0x2300, 38, kRegs2300}, {0x2380, 8, kRegs2380},
  };
  for (const Block &b : kBlocks)
    if (reg >= b.first && reg < b.first + b.count)
      return b.offset + (reg - b.first) * 4u;
  return 0;
}

constexpr uint32_t kStreamDeclStrideDiv4 = 0x2D30; // u8 x16: commit copies +0x2FF0 here (std r10,11568)
constexpr uint32_t kSamplerByteTableA = 0x2DAE;    // u8 per sampler, SetTexture lbz r9,11694(dev + s)
constexpr uint32_t kSamplerByteTableB = 0x2DC8;    // u8 per sampler, SetTexture lbz r8,11720(dev + s): 26 after A
constexpr uint32_t kVertexDeclaration = 0x2D20; // SetVertexDeclaration sub_821DAE80: stw r4,11552(r3); commit lwz r29,11552
constexpr uint32_t kIndexBuffer = 0x2F94;       // SetIndices sub_821D7B58: stw r29,12180(r31); DrawIndexedVertices lwz r22,12180
constexpr uint32_t kRenderTarget0 = 0x2F98;     // SetRenderTarget sub_821D7F60: (idx + 3046) * 4, 4 slots
constexpr uint32_t kRenderTargetCount = 4;
constexpr uint32_t kDepthStencil = 0x2FA8;      // SetDepthStencilSurface sub_821D8290: stw r30,12200(r31)
constexpr uint32_t kStreamSource = 0x2FAC;      // SetStreamSource sub_821D79A8: stwx vb,(3051 + s) * 4 (bare vb, 4 bytes)
constexpr uint32_t kStreamSourceStride = 4;     //   (RR: 8-byte {offset, vb} at +0x310C; UFC 4 bytes at +0x31A4)
constexpr uint32_t kStreamCount = 16;
// SVR07-CENSUS-1 (2026-11-11, laptop, emulated, title/menus/1v1 match): only value seen 0 (never written). Was: dword between the stream table and the strides; no D3D-library literal reads or writes it | census: log distinct values at Swap
constexpr uint32_t kStreamPad = 0x2FEC;
constexpr uint32_t kStreamStrideDiv4 = 0x2FF0;  // SetStreamSource: stb r11,12272(dev + s), stride >> 2
constexpr uint32_t kTexture = 0x3000;           // SetTexture sub_821D5058: addi r11,r4,3072; stwx r5 (no clamp on the index)
constexpr uint32_t kTextureSlots = 26;          //   26 = up to the viewport (+0x3068); 26 = the per-sampler byte tables' spacing
constexpr uint32_t kViewport = 0x3068;          // SetViewport sub_821D7768: ints X,Y,W,H +12392..+12404, floats MinZ/MaxZ +12408/+12412
constexpr uint32_t kViewportSize = 24;          //   GetViewport sub_821D7990 copies 24 bytes; no flags field
constexpr uint32_t kScissorRect = 0x3080;       // SetScissorRect sub_821D76A0: stw +12416..+12428 (LTRB ints)
constexpr uint32_t kPixelShader = 0x3090;       // SetPixelShader sub_821DAAA8: stw r4,12432(r3); commit IM_LOAD type 1 from it
constexpr uint32_t kVertexShader = 0x3094;      // SetVertexShader body sub_821DACE0: stw r31,12436(r30); commit IM_LOAD type 0 from it
// SVR07-CENSUS-1 (2026-11-11, laptop, emulated, title/menus/1v1 match): 0 at the first draw. Was: purpose of +0x30A8 (SetRenderTarget skips its compare block when non-zero; device init sub_821E4838 writes it) | census: log value at the first draw
constexpr uint32_t kRtCompareGate = 0x30A8;
constexpr uint32_t kLastSurfaces = 0x30B4;      // 5 dwords (RT0..3, DS) SetViewport / SetRenderTarget compare against +0x2F98..
// SVR07-CENSUS-1 (2026-11-11, laptop, emulated, title/menus/1v1 match): 0x0 at the first draw: not the display size (the renderer never reads it). Was: display width / height (SetViewport takes W/H from +0x3274/+0x3278 when the surfaces match the cached set) | census: log both at the first Swap
constexpr uint32_t kDisplayWidth = 0x3274;
constexpr uint32_t kDisplayHeight = 0x3278;
constexpr uint32_t kSavedRingEnd = 0x3324;      // BeginVertices sub_821D94A0 stw r11,13092; EndVertices sub_821D9950 ring = it
constexpr uint32_t kVertexShaderPublic = 0x3364;// SetVertexShader sub_821DAE30: stw r4,13156(r3), b body; GetVertexShader sub_821DAE38 reads it
constexpr uint32_t kPresentParams = 0x33E4;     // Present sub_821EB100: Swap(dev, +0x3474, addi r5,r31,13284)
constexpr uint32_t kFrontBuffer = 0x3474;       // Present: Resolve into lwz r6,13428; Swap shows it
constexpr uint32_t kBackBuffer = 0x3478;        // Present: SetRenderTarget(0, lwz r5,13432)
constexpr uint32_t kCreateFlags = 0x4FD8;       // Direct3D_CreateDevice: stw r30,20440(r31)

// Resource objects (gpu/d3d.h has them as structs). Header = Blue Dragon's / UFC's 24 bytes.
constexpr uint32_t kResourceHeaderSize = 24;    // Common, ReferenceCount, Fence, ReadFence, Identifier, BaseFlush
constexpr uint32_t kResourceRefCount = 0x04;    // AddRef sub_821D2DA0 / Release sub_821D2E18: lwarx/stwcx. on +4
constexpr uint32_t kResourceFence = 0x08;       // SetTexture / SetStreamSource / SetIndices: stw fence,8(old)
constexpr uint32_t kResourceBaseFlush = 0x14;   // every Create*: stw 0xFFFF0000,20(obj)
constexpr uint32_t kCommonTypeMask = 0xF;       // GetType sub_821D27E8: clrlwi r11,r10,28
constexpr uint32_t kCommonLevelSurface = 0x40000000u;  // surface of a texture level: parent at +0x18 (AddRef / GetType / GetDesc)
constexpr uint32_t kCommonIndex32 = 0x80000000u;       // IB Common bit 31 (DrawIndexedVertices rlwinm. 0,0,0)
// Common type values (low 4 bits): every one written by its Create*.
constexpr uint32_t kTypeVertexBuffer = 1;       // CreateVertexBuffer sub_821D2EE8: 0x00100001
constexpr uint32_t kTypeIndexBuffer = 2;        // CreateIndexBuffer sub_821D3020: 0x00100002 | format bits
constexpr uint32_t kTypeTexture = 3;            // GetType: type 3 -> dim from fetch dword5
constexpr uint32_t kTypeSurface = 4;            // GetType / AddRef: type 4 + bit 30 = level surface
constexpr uint32_t kTypeVertexDeclaration = 5;  // CreateVertexDeclaration sub_821DAEA0: 0x00100005
constexpr uint32_t kTypeVertexShader = 6;       // sub_821DAC18 (from CreateVertexShader): li r10,6 -> +0
constexpr uint32_t kTypePixelShader = 7;        // CreatePixelShader sub_821DB030: li r11,7 -> +0

constexpr uint32_t kVertexBufferSize = 32;      // CreateVertexBuffer: li r3,32
constexpr uint32_t kIndexBufferSize = 32;       // CreateIndexBuffer: li r3,32
constexpr uint32_t kBufferAddress = 0x18;       // VB: alloc | 3 (fetch dword 0); IB: alloc. Unlocks sub_821D3010 / sub_821D3130 read +24
constexpr uint32_t kBufferSize = 0x1C;          // VB: (size & 0x3FFFFFC) | 0x10000000 | 2; IB: bytes (stw r28,28)
constexpr uint32_t kTextureSize = 52;           // CreateTexture sub_821D4CF0: li r3,52
constexpr uint32_t kTextureFetch = 0x1C;        // SetTexture copies +28..+48 into the device fetch slot
constexpr uint32_t kSurfaceSize = 48;           // CreateSurface sub_821D4E10: li r3,48
constexpr uint32_t kSurfaceInfo = 0x18;         // GetDesc: lhz 24 & 3 = MultiSampleType; level surface: parent
constexpr uint32_t kSurfaceColorDepthInfo = 0x1C; // SetRenderTarget -> RB_COLOR_INFO, SetDepthStencil -> RB_DEPTH_INFO; level: top nibble = level
constexpr uint32_t kSurfaceHiControl = 0x20;    // SetDepthStencil -> RB_HIZCONTROL (+0x28C0)
constexpr uint32_t kSurfaceSizeBits = 0x24;     // GetDesc sub_821D4F38: W = (>>18 & 0x3FFF) + 1, H = (>>3 & 0x7FFF) + 1
constexpr uint32_t kSurfaceFormat = 0x28;       // GetDesc: lwz r10,40(r3) -> Desc.Format
constexpr uint32_t kVertexDeclBase = 52;        // CreateVertexDeclaration: elements copied to +52 (alloc 56 + 12n, memset 56)
constexpr uint32_t kVertexDeclCount = 0x18;     //   element count (stw r29,24)
constexpr uint32_t kVertexElementSize = 12;     //   counts 12-byte elements up to Stream 0xFF
// Shaders: the container (0x102A0E0x, microcode header) is copied behind a prefix.
constexpr uint32_t kVertexShaderPrefix = 592;   // CreateVertexShader sub_821DB120: memset 592, copy to +592
constexpr uint32_t kVertexShaderMicrocode = 0x30;   // sub_821DAC18: stw r28,48(r31) (physical alloc); IM_LOAD base
constexpr uint32_t kVertexShaderUcodeBytes = 600;   // commit sub_821E2760 / bind sub_821E2638: lwz 600 (container physicalSize)
constexpr uint32_t kPixelShaderPrefix = 56;     // CreatePixelShader sub_821DB030: memset 56, copy to +56
constexpr uint32_t kPixelShaderMicrocode = 0x18;    // CreatePixelShader: stw r30,24(r31); commit IM_LOAD base | 1
constexpr uint32_t kPixelShaderUcodeBytes = 64;     // commit: lwz r10,64(r25) (container physicalSize)

// Resolve (sub_821DB9C0): flags & 7 = render target index, 4 = depth (lwz 12200).
constexpr uint32_t kResolveTargetMask = 7;
constexpr uint32_t kResolveDepth = 4;
// Resolve's stack arguments in the CALLER's frame (game caller sub_82444708 stw r30,92(r1) / r31,100(r1);
// Present the same): ClearStencil at r1 + 0x5C, pParameters at r1 + 0x64. r10 = pClearColor (D3DVECTOR4*), f1 = ClearZ.
constexpr uint32_t kResolveStackStencil = 0x5C;
constexpr uint32_t kResolveStackParams = 0x64;

// Vertex fetch constant of stream s: 95 - s (SetStreamSource: stwx at (223 - s) * 8, size at 1788 - 8s).
constexpr uint32_t StreamFetchOffset(uint32_t stream) { return kFetchConstants + 8u * (95u - stream); }

static_assert(kVertexShader == kPixelShader + 4);
static_assert(kRenderTarget0 == kIndexBuffer + 4 && kDepthStencil == kRenderTarget0 + 4 * kRenderTargetCount);
static_assert(kStreamSource == kDepthStencil + 4);
static_assert(kStreamSource + kStreamCount * kStreamSourceStride == kStreamPad);
static_assert(kStreamPad + 4 == kStreamStrideDiv4);
static_assert(kTexture == kStreamStrideDiv4 + kStreamCount);
static_assert(kTexture + 4 * kTextureSlots == kViewport);
static_assert(kSamplerByteTableB - kSamplerByteTableA == kTextureSlots);
static_assert(kScissorRect == kViewport + kViewportSize);
static_assert(kFetchConstants + kFetchConstantSize * kFetchConstantCount == kVsFloatConstants);
static_assert(StreamFetchOffset(0) == 0x6F8 && StreamFetchOffset(15) == 0x680);
// Stream 15 = vertex fetch 80 sits in fetch slot 26: texture slots 0..25 are the ones free of streams.
static_assert((StreamFetchOffset(15) - kFetchConstants) / kFetchConstantSize == kTextureSlots);
static_assert(kVsFloatConstants + 256 * 16 == kPsFloatConstants && kPsFloatConstants + 256 * 16 == kVsBoolConstants);
static_assert(kRegs2080 == kRegs2000 + 16 * 4 && kRegs2100 == kRegs2080 + 3 * 4 && kRegs2180 == kRegs2100 + 21 * 4);
static_assert(kRegs2200 == kRegs2180 + 5 * 4 && kRegs2280 == kRegs2200 + 12 * 4 && kRegs2300 == kRegs2280 + 21 * 4);
static_assert(kRegs2380 == kRegs2300 + 38 * 4 && kRegsEnd == kRegs2380 + 8 * 4);
static_assert(RegisterOffset(0x2081) == kPaScWindowScissorTL && RegisterOffset(0x2104) == kRbColorMask);
static_assert(RegisterOffset(0x210F) == kPaClVportXScale && RegisterOffset(0x2201) == kRbBlendControl0);
static_assert(RegisterOffset(0x2202) == kRbColorControl && RegisterOffset(0x2203) == kRbHiZControl);
static_assert(RegisterOffset(0x2209) == kRbBlendControl1 && RegisterOffset(0x2206) == kPaClVteCntl);
static_assert(RegisterOffset(0x2302) == kPaSuVtxCntl && RegisterOffset(0x2002) == kRbDepthInfo);
static_assert(RegisterOffset(0x200E) == kPaScScreenScissorTL && RegisterOffset(0x2400) == 0);
static_assert(kVertexShaderUcodeBytes == kVertexShaderPrefix + 8 && kPixelShaderUcodeBytes == kPixelShaderPrefix + 8);
static_assert(kTextureFetch + 24 == kTextureSize);
static_assert(kCreateFlags < kDeviceSize && kVertexShaderPublic < kDeviceSize);

}  // namespace svr07::d3d
