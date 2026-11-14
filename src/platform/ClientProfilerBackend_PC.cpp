#include "platform/ClientProfilerBackend.h"

#if (defined(NETCAST_PLATFORM) || defined(WEBOS_PLATFORM))
#include "platform/TvPlatform.h"
#include "platform/NetcastProfile.h"

namespace ClientProfilerBackend
{
namespace
{
long long g_ticksNs = 0, g_lightNs = 0, g_swapNs = 0, g_renderNs = 0, g_frameNs = 0;
long long g_tickCount = 0, g_chunkUpd = 0;
int g_frames = 0;
}
void frameBegin() {}
void ticks(long long ns, int n) { g_ticksNs += ns; g_tickCount += n; }
void lighting(long long ns) { g_lightNs += ns; }
void displayUpdate(long long ns) { g_swapNs += ns; }
void render(long long ns) { g_renderNs += ns; }
void frameEnd(long long frameNs, long long, long long, int, int chunkUpdates, World*, RenderGlobal*)
{
    g_frameNs += frameNs;
    g_chunkUpd += chunkUpdates;
    if (++g_frames < 90)
        return;
    const double f = g_frames;
    tvc::log("prof frame (ms): total=%.1f ticks=%.1f (%.2f/frame) lighting=%.1f swap=%.1f render=%.1f chunkUpdates=%.2f/frame",
                 g_frameNs / 1.0e6 / f, g_ticksNs / 1.0e6 / f, g_tickCount / f, g_lightNs / 1.0e6 / f,
                 g_swapNs / 1.0e6 / f, g_renderNs / 1.0e6 / f, g_chunkUpd / f);
    netcastprof::flush(g_frames);
    g_ticksNs = g_lightNs = g_swapNs = g_renderNs = g_frameNs = 0;
    g_tickCount = g_chunkUpd = 0;
    g_frames = 0;
}
}
#else
namespace ClientProfilerBackend
{
void frameBegin() {}
void ticks(long long, int) {}
void lighting(long long) {}
void displayUpdate(long long) {}
void render(long long) {}
void frameEnd(long long, long long, long long, int, int, World*, RenderGlobal*) {}
}
#endif
