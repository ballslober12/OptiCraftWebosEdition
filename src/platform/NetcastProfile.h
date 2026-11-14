#pragma once
// NetCast-only timing counters (aggregated and logged by ClientProfilerBackend_PC.cpp).
namespace netcastprof
{
void notePhase(const char *name, long long ns);
void flush(int frames);
void addChunkBuild(long long ns, int verts);
void addGen(long long ns);
void addPopulate(long long ns);
void addLoad(long long ns);
void addMesh(long long ns);
void addSave(long long ns);
void addUpdates(long long ns);
}
