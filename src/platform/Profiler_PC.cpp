#include "platform/Profiler.h"

#if (defined(NETCAST_PLATFORM) || defined(WEBOS_PLATFORM))
#include "platform/TvPlatform.h"
#include "platform/NetcastProfile.h"
#include <atomic>
#include <cstring>
#include <mutex>

namespace netcastprof
{
namespace
{
std::mutex g_mu;
struct Phase { const char *name; long long ns; int n; };
Phase g_phases[40];
int g_count = 0;
std::atomic<long long> g_chunkBuildNs(0), g_chunkBuilds(0), g_chunkVerts(0);
std::atomic<long long> g_genNs(0), g_popNs(0), g_loadNs(0), g_meshNs(0), g_saveNs(0), g_updNs(0);
}

void notePhase(const char *name, long long ns)
{
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < g_count; ++i)
        if (g_phases[i].name == name || std::strcmp(g_phases[i].name, name) == 0)
        {
            g_phases[i].ns += ns;
            ++g_phases[i].n;
            return;
        }
    if (g_count < 40)
        g_phases[g_count++] = { name, ns, 1 };
}

void flush(int frames)
{
    if (frames <= 0)
        return;
    char buf[900];
    int len = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (int i = 0; i < g_count; ++i)
        {
            const double ms = g_phases[i].ns / 1.0e6 / frames;
            if (ms >= 0.05 && len < 760)
                len += std::snprintf(buf + len, sizeof(buf) - len, " %s=%.2f", g_phases[i].name, ms);
            g_phases[i].ns = 0;
            g_phases[i].n = 0;
        }
    }
    tvc::log("prof phases (ms/frame):%s", len ? buf : " none");
    const double f = frames;
    tvc::log("prof workers (ms/frame): chunkBuild=%.2f (%lld builds, %lld verts) gen=%.2f populate=%.2f load=%.2f mesh=%.2f save=%.2f updates=%.2f",
                 g_chunkBuildNs.exchange(0) / 1.0e6 / f, (long long)g_chunkBuilds.exchange(0), (long long)g_chunkVerts.exchange(0),
                 g_genNs.exchange(0) / 1.0e6 / f, g_popNs.exchange(0) / 1.0e6 / f, g_loadNs.exchange(0) / 1.0e6 / f,
                 g_meshNs.exchange(0) / 1.0e6 / f, g_saveNs.exchange(0) / 1.0e6 / f, g_updNs.exchange(0) / 1.0e6 / f);
}

void addChunkBuild(long long ns, int verts) { g_chunkBuildNs += ns; ++g_chunkBuilds; g_chunkVerts += verts; }
void addGen(long long ns) { g_genNs += ns; }
void addPopulate(long long ns) { g_popNs += ns; }
void addLoad(long long ns) { g_loadNs += ns; }
void addMesh(long long ns) { g_meshNs += ns; }
void addSave(long long ns) { g_saveNs += ns; }
void addUpdates(long long ns) { g_updNs += ns; }
}
#endif

std::uint32_t platformProfileRenderPhaseBegin() { return 0; }
void platformProfileRenderPhaseEnd(std::uint32_t, PlatformRenderPhase) {}
void platformProfileTickPhase(const char* name, long long ns) {
#if (defined(NETCAST_PLATFORM) || defined(WEBOS_PLATFORM))
    netcastprof::notePhase(name, ns);
#else
    (void)name; (void)ns;
#endif
}
void platformProfileChunkBuild(long long ns, int v) {
#if (defined(NETCAST_PLATFORM) || defined(WEBOS_PLATFORM))
    netcastprof::addChunkBuild(ns, v);
#else
    (void)ns; (void)v;
#endif
}
void platformProfileChunkMeshPass(int, long long, int) {}
void platformProfileSnowColumn(bool, bool, int) {}
void platformProfilePopulatePhase(PlatformPopulatePhase, long long) {}
void platformProfileChunkLoad(long long ns) {
#if (defined(NETCAST_PLATFORM) || defined(WEBOS_PLATFORM))
    netcastprof::addLoad(ns);
#else
    (void)ns;
#endif
}
void platformProfilePopulate(long long ns) {
#if (defined(NETCAST_PLATFORM) || defined(WEBOS_PLATFORM))
    netcastprof::addPopulate(ns);
#else
    (void)ns;
#endif
}
void platformProfileGenerate(long long ns) {
#if (defined(NETCAST_PLATFORM) || defined(WEBOS_PLATFORM))
    netcastprof::addGen(ns);
#else
    (void)ns;
#endif
}
void platformProfileMesh(long long ns) {
#if (defined(NETCAST_PLATFORM) || defined(WEBOS_PLATFORM))
    netcastprof::addMesh(ns);
#else
    (void)ns;
#endif
}
void platformProfileUnloadSave(long long ns) {
#if (defined(NETCAST_PLATFORM) || defined(WEBOS_PLATFORM))
    netcastprof::addSave(ns);
#else
    (void)ns;
#endif
}
void platformProfileTickUpdates(long long ns) {
#if (defined(NETCAST_PLATFORM) || defined(WEBOS_PLATFORM))
    netcastprof::addUpdates(ns);
#else
    (void)ns;
#endif
}
void platformProfileTickQueue(long long) {}
void platformProfileMobSpawn(long long) {}
void platformProfileSaveWorldInfo(long long) {}
void platformProfileMapStorage(long long) {}
void platformProfileChunkEvict(long long) {}
