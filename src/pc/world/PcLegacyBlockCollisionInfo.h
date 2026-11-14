#pragma once

#include "platform/PlatformConfig.h"
#include "platform/PlatformTuning.h"

#if PLATFORM_PC_LEGACY && PLATFORM_FAST_BLOCK_COLLISIONS

#include "platform/world/PlatformBlockCollisionInfo.h"

using PcLegacyBlockCollisionInfo = PlatformBlockCollisionInfo;

const PcLegacyBlockCollisionInfo &pcLegacyGetBlockCollisionInfo(int_t blockId);

#endif
