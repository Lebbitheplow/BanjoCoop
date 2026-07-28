/* See tier.h. No game headers here on purpose — this file is compiled twice, once for MIPS in the
 * mod and once by gcc in tests/modlogic. */

#include "tier.h"

bc_u32 bc_tier_of(bc_f32 dist2) {
    if (dist2 < BC_TIER_NEAR_D2) {
        return BC_TIER_INDEX_NEAR;
    }
    if (dist2 < BC_TIER_MID_D2) {
        return BC_TIER_INDEX_MID;
    }
    if (dist2 < BC_TIER_FAR_D2) {
        return BC_TIER_INDEX_FAR;
    }
    return BC_TIER_INDEX_CULLED;
}

bc_u32 bc_tier_period(bc_u32 tier) {
    switch (tier) {
        case BC_TIER_INDEX_NEAR:
            return 1u;
        case BC_TIER_INDEX_MID:
            return 3u;
        case BC_TIER_INDEX_FAR:
            return 8u;
        default:
            return 0u;
    }
}

bc_u32 bc_tier_publishes(bc_u32 net_id, bc_f32 dist2, bc_u32 tick) {
    bc_u32 period = bc_tier_period(bc_tier_of(dist2));
    if (period == 0u) {
        return 0u;
    }
    if (period == 1u) {
        return 1u;
    }
    return ((tick + net_id) % period) == 0u ? 1u : 0u;
}
