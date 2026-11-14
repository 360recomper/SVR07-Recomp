// Native renderer switch (RR port of UFC-native src/native/ufc_native.h).
//
// svr07_renderer (emulated | native) is read once in Svr07App::LatchRenderer
// (src/svr07_app.h in the game tree) and latched in g_svr07_native. Every native hook
// starts with `if (!g_ufc3_native) ...` (the UFC name, kept so the ported sources read like UFC's),
// so with the default (emulated) the game runs exactly as the v1.5c exe.

#pragma once

#ifndef SVR07_NATIVE_LATCH_DEFINED
#define SVR07_NATIVE_LATCH_DEFINED
inline bool g_svr07_native = false;
#endif
#define g_ufc3_native g_svr07_native
