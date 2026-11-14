// SvR07 D3D layout compile check (lane L3, 2026-11-10), like RR's src/rrxx_native.cpp: includes the
// offset header, the names header and re:Blue's struct header with SVR07_NATIVE_RENDERER and asserts
// that the three agree. Not part of any target (native/src/dev is not globbed): compile it alone,
//   clang++ -fsyntax-only -std=gnu++23 -DSVR07_NATIVE_RENDERER=1 -DSVR_NATIVE_RENDERER=1
//     -I native/src -I native/src/reblue -I native/src/native -isystem <sdk include> dev/svr07_d3d_check.cpp
// (reports/L3-d3d-verification.md has the exact line used).

#include <cstddef>
#include <cstdint>

#include "svr07_d3d.h"
#include "svr07_d3d_names.h"
#include "gpu/d3d.h"
#include "gpu/resources.h"
#include "ufc_cb_mirror.h"

namespace d = svr07::d3d;
namespace g = bd::gpu;

// Sizes.
static_assert(sizeof(g::D3DDevice) == 0x5000 && d::kDeviceSize == 0x5000, "Direct3D_CreateDevice li r3,20480");
static_assert(sizeof(g::D3DResource) == 24);
static_assert(sizeof(g::D3DVertexBuffer) == 32 && sizeof(g::D3DIndexBuffer) == 32);
static_assert(sizeof(g::D3DTexture) == 52 && sizeof(g::D3DSurface) == 48);
static_assert(sizeof(g::GuestTextureX360) == 52 && sizeof(g::GuestBufferX360) == 32);
static_assert(sizeof(g::D3DViewport9) == 24 && sizeof(g::D3DRect) == 16);
static_assert(sizeof(g::GpuTextureFetchConstant) == 24 && sizeof(g::DeviceFetchConstant) == 24);

// The fields the native path reads, at SvR07's offsets.
static_assert(offsetof(g::D3DDevice, pixelShader) == 0x3090 && offsetof(g::D3DDevice, vertexShader) == 0x3094);
static_assert(offsetof(g::D3DDevice, vertexDeclaration) == 0x2D20 && offsetof(g::D3DDevice, indexBuffer) == 0x2F94);
static_assert(offsetof(g::D3DDevice, renderTargetShadow) == 0x2F98 && offsetof(g::D3DDevice, depthStencilShadow) == 0x2FA8);
static_assert(offsetof(g::D3DDevice, streamSource) == 0x2FAC && offsetof(g::D3DDevice, streamStrideDiv4) == 0x2FF0);
static_assert(offsetof(g::D3DDevice, boundTexture) == 0x3000 && offsetof(g::D3DDevice, viewport) == 0x3068);
static_assert(offsetof(g::D3DDevice, scissorRect) == 0x3080);
static_assert(offsetof(g::D3DDevice, fetchConstants) == 0x400 && offsetof(g::D3DDevice, vsFloatConstants) == 0x700);
static_assert(offsetof(g::D3DDevice, psFloatConstants) == 0x1700 && offsetof(g::D3DDevice, vsBoolConstants) == 0x2700);
static_assert(offsetof(g::D3DDevice, rbBlendControl0) == 0x28B8 && offsetof(g::D3DDevice, rbColorControl) == 0x28BC);
static_assert(offsetof(g::D3DDevice, frontBuffer) == 0x3474 && offsetof(g::D3DDevice, backBuffer) == 0x3478);
static_assert(offsetof(g::D3DDevice, m_pRing) == 0x28 && offsetof(g::D3DDevice, vertexShaderPublic) == 0x3364);
static_assert(offsetof(g::D3DTexture, Format) == 0x1C && offsetof(g::D3DSurface, SizeBits) == 0x24);
static_assert(offsetof(g::D3DSurface, Format) == 0x28 && offsetof(g::D3DSurface, Parent) == 0x18);
static_assert(offsetof(g::D3DVertexBuffer, FetchLo) == 0x18 && offsetof(g::D3DIndexBuffer, FetchHi) == 0x1C);
static_assert(offsetof(g::D3DResource, ReferenceCount) == 4);

// Type values (Common & 0xF) as the Create* functions write them.
static_assert(uint32_t(g::D3DResourceType::kVertexBuffer) == 1 && uint32_t(g::D3DResourceType::kIndexBuffer) == 2);
static_assert(uint32_t(g::D3DResourceType::kTexture) == 3 && uint32_t(g::D3DResourceType::kSurface) == 4);
static_assert(uint32_t(g::D3DResourceType::kVertexShader) == 6 && uint32_t(g::D3DResourceType::kPixelShader) == 7);

// Tables sized from the setters.
static_assert(kUfcMirrorTextures == d::kTextureSlots, "26 bound-texture slots before the viewport");
static_assert(sizeof(g::D3DDevice::boundTexture) / sizeof(g::D3DDevice::boundTexture[0]) == 26);
static_assert(sizeof(g::D3DDevice::streamSource[0]) == 4, "SvR07 stream table holds bare vb pointers");

// Register shadows: UFC / SvR 2010 table - 0x80.
static_assert(d::RegisterOffset(0x2000) == 0x2880 - 0x80 && d::RegisterOffset(0x2080) == 0x28C0 - 0x80);
static_assert(d::RegisterOffset(0x2100) == 0x28CC - 0x80 && d::RegisterOffset(0x2180) == 0x2920 - 0x80);
static_assert(d::RegisterOffset(0x2200) == 0x2934 - 0x80 && d::RegisterOffset(0x2280) == 0x2964 - 0x80);
static_assert(d::RegisterOffset(0x2300) == 0x29B8 - 0x80 && d::RegisterOffset(0x2380) == 0x2A50 - 0x80);

// The names header maps the public SetVertexShader entry, not its body.
#define SVR07_STR2(x) #x
#define SVR07_STR(x) SVR07_STR2(x)
static_assert(__builtin_strcmp(SVR07_STR(D3DDevice_SetVertexShader), "sub_821DAE30") == 0,
              "hook the PUBLIC SetVertexShader sub_821DAE30");
static_assert(__builtin_strcmp(SVR07_STR(D3DDevice_SetPixelShader), "sub_821DAAA8") == 0);
static_assert(__builtin_strcmp(SVR07_STR(D3DDevice_CreateVertexShader), "sub_821DB120") == 0);
static_assert(__builtin_strcmp(SVR07_STR(D3DDevice_CreatePixelShader), "sub_821DB030") == 0);
// Must stay unmapped (resource.cpp REX_HOOKs would replace the game's own functions).
#define SVR07_UNMAPPED(name) static_assert(__builtin_strcmp(SVR07_STR(name), #name) == 0, #name " must stay unmapped")
SVR07_UNMAPPED(D3DDevice_CreateVertexBuffer);
SVR07_UNMAPPED(D3DDevice_CreateIndexBuffer);
SVR07_UNMAPPED(D3DVertexBuffer_Lock);
SVR07_UNMAPPED(D3DIndexBuffer_Lock);
SVR07_UNMAPPED(D3DDevice_CreateSurface);
SVR07_UNMAPPED(D3DDevice_CreateTexture);
SVR07_UNMAPPED(D3DSurface_LockRect);
SVR07_UNMAPPED(D3DLineTexture_LockRect);
SVR07_UNMAPPED(D3DResource_Unlock);
SVR07_UNMAPPED(D3DResource_Release);
SVR07_UNMAPPED(D3DResource_AddRef);
SVR07_UNMAPPED(D3DResource_Destroy);
SVR07_UNMAPPED(D3DResource_GetType);
SVR07_UNMAPPED(D3DTexture_GetSurfaceLevel);
SVR07_UNMAPPED(D3DSurface_GetDesc);
SVR07_UNMAPPED(D3DTexture_GetLevelDesc);
SVR07_UNMAPPED(D3DVolumeTexture_GetLevelDesc);
SVR07_UNMAPPED(D3DDevice_BeginIndexedVertices);
SVR07_UNMAPPED(D3DDevice_DrawVerticesUP);
SVR07_UNMAPPED(D3DDevice_SetViewportF);
SVR07_UNMAPPED(Direct3D_CreateDevice);
SVR07_UNMAPPED(D3DDevice_SetShaderGPRAllocation);
