#pragma once

#include "platform/PlatformConfig.h"
#include "platform/PlatformTuning.h"

#if PLATFORM_PC_LEGACY && PLATFORM_FAST_BLOCK_COLLISIONS

#include <vector>

class AxisAlignedBB;
class World;

void pcLegacyCollectBlockCollisions(World *world, AxisAlignedBB *mask, std::vector<AxisAlignedBB *> &collisions);
bool pcLegacyHasBlockCollision(World *world, AxisAlignedBB *mask, std::vector<AxisAlignedBB *> &scratch);

#endif
