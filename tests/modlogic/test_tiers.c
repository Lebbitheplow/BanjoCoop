/* Does distance-based throttling send what it should, and stay quiet about the rest?
 *
 * This compiles the real src/mod/tier.c — no stubs, because it deliberately depends on nothing but
 * protocol.h. The rest of the send-rate change needs a live actor array and a running session; the
 * decision itself is three numbers in and one out, and that part can be checked here rather than
 * inferred from watching two copies of Banjo-Kazooie.
 *
 * What is NOT covered here, and has to be play-tested: whether the constants are the right size
 * for BK's world. See the objtier log line and docs/symbols.md §35.
 */

#include <stdio.h>

#include "tier.h"

static int g_failures = 0;

static void check(int cond, const char *what) {
    printf("  [%s] %s\n", cond ? " ok " : "FAIL", what);
    if (!cond) {
        g_failures++;
    }
}

/* Distances are squared everywhere in the real code, so tests take a plain distance and square it
 * here — a test that got that wrong would silently be checking a different band. */
static bc_f32 d2(bc_f32 distance) {
    return distance * distance;
}

/* How many of `ticks` consecutive frames this object publishes on. */
static int publishes_in(bc_u32 net_id, bc_f32 dist2, bc_u32 ticks) {
    int n = 0;
    for (bc_u32 t = 0; t < ticks; t++) {
        if (bc_tier_publishes(net_id, dist2, t)) {
            n++;
        }
    }
    return n;
}

int main(void) {
    printf("test: distance decides how often an object is published\n");

    /* Bands. The boundaries matter more than the middles: an off-by-one in the comparison would
     * leave the middles right and shift every band edge. */
    check(bc_tier_of(d2(0.0f)) == BC_TIER_INDEX_NEAR, "an object underfoot is near");
    check(bc_tier_of(d2(BC_TIER_NEAR - 1.0f)) == BC_TIER_INDEX_NEAR, "just inside near is near");
    check(bc_tier_of(d2(BC_TIER_NEAR + 1.0f)) == BC_TIER_INDEX_MID, "just outside near is mid");
    check(bc_tier_of(d2(BC_TIER_MID + 1.0f)) == BC_TIER_INDEX_FAR, "just outside mid is far");
    check(bc_tier_of(d2(BC_TIER_FAR + 1.0f)) == BC_TIER_INDEX_CULLED, "beyond far is culled");
    /* nearest_remote_dist2 returns 1e30 when nobody else is in the map. Publishing a map's enemies
     * to an empty map is the single largest waste this change removes, so it must cull. */
    check(bc_tier_of(1.0e30f) == BC_TIER_INDEX_CULLED, "nobody in the map culls everything");

    /* Periods. */
    check(bc_tier_period(BC_TIER_INDEX_NEAR) == 1u, "near publishes every frame");
    check(bc_tier_period(BC_TIER_INDEX_MID) == 3u, "mid publishes every third frame");
    check(bc_tier_period(BC_TIER_INDEX_FAR) == 8u, "far publishes every eighth frame");
    check(bc_tier_period(BC_TIER_INDEX_CULLED) == 0u, "culled never publishes");

    /* Rates over time. A near object must never be skipped: it is the one being fought. */
    check(publishes_in(0u, d2(10.0f), 60u) == 60, "a near object publishes on all 60 frames");
    check(publishes_in(7u, d2(10.0f), 60u) == 60, "near ignores the phase entirely");

    check(publishes_in(0u, d2(2000.0f), 60u) == 20, "a mid object publishes 20 times in 60");
    check(publishes_in(0u, d2(5000.0f), 64u) == 8, "a far object publishes 8 times in 64");
    check(publishes_in(0u, d2(9000.0f), 60u) == 0, "a culled object is never published");
    /* The phase must not change how much is sent, only when. */
    check(publishes_in(11u, d2(2000.0f), 60u) == 20, "phase does not change a mid object's rate");
    check(publishes_in(11u, d2(5000.0f), 64u) == 8, "phase does not change a far object's rate");

    /* Phasing. Without it a map's whole mid band lands on one frame and the object frame
     * alternates between empty and over budget — same total traffic, far worse peaks, and it is
     * the peaks that overflow BCNET_MAX_OBJECTS. */
    printf("test: objects in the same band are spread across frames\n");
    {
        bc_f32 mid = d2(2000.0f);
        int collisions = 0;
        for (bc_u32 t = 0; t < 3u; t++) {
            int together = bc_tier_publishes(0u, mid, t) && bc_tier_publishes(1u, mid, t);
            collisions += together;
        }
        check(collisions == 0, "two adjacent mid objects never publish on the same frame");

        /* Three objects, period three: each frame should carry exactly one of them. */
        for (bc_u32 t = 0; t < 3u; t++) {
            int n = bc_tier_publishes(0u, mid, t) + bc_tier_publishes(1u, mid, t) +
                    bc_tier_publishes(2u, mid, t);
            check(n == 1, "exactly one of three mid objects publishes each frame");
        }
    }

    if (g_failures == 0) {
        printf("\ntier tests passed\n");
        return 0;
    }
    printf("\n%d check(s) failed\n", g_failures);
    return 1;
}
