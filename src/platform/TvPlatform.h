#pragma once

// Shared TV/console platform hooks.
//
// The game code names a handful of things that only a TV platform layer can
// provide: a log sink, integer settings read from a config file next to the
// executable, and the in-game on-screen keyboard. Both the NetCast target
// (src/netcast) and the webOS target (src/webos) export them under the same
// names, so the shared code includes THIS header instead of a platform one and
// talks to the `tvc` namespace alias.
//
//   NetCast build : namespace tvc = netcast;
//   webOS build   : namespace tvc = webos;
//
// Nothing is defined for the desktop / PS2 / Wii targets; the call sites are
// inside WEBOS_PLATFORM / NETCAST_PLATFORM conditionals.

#if defined(WEBOS_PLATFORM)

#include "webos/WebOSPlatform.h"
#include "webos/WebOSOsk.h"
namespace tvc = webos;

#elif defined(NETCAST_PLATFORM)

#include "netcast/NetcastPlatform.h"
#include "netcast/NetcastOsk.h"
namespace tvc = netcast;

#endif
