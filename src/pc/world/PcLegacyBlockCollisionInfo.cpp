#include "pc/world/PcLegacyBlockCollisionInfo.h"

#if PLATFORM_PC_LEGACY && PLATFORM_FAST_BLOCK_COLLISIONS

const PcLegacyBlockCollisionInfo &pcLegacyGetBlockCollisionInfo(int_t blockId)
{
    return platformGetBlockCollisionInfo(blockId);
}

#endif
