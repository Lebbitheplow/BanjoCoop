/* How often a replicated object is worth publishing, given how far it is from anyone who could
 * see it.
 *
 * Separated from enemy.c for one reason: it is the only part of the send-rate decision that can be
 * checked without a running game. Everything around it needs the actor array, the marker table and
 * a live session; this needs three numbers. It compiles for MIPS in the mod and for the host in
 * tests/modlogic — which is what `bc_u32`/`bc_f32` from protocol.h are for.
 *
 * Deliberately free of game headers. Anything added here that needs one belongs in enemy.c.
 */

#ifndef BANJOCOOP_TIER_H
#define BANJOCOOP_TIER_H

#include "banjocoop/protocol.h"

/* Distance bands, as squared distances so nothing has to take a square root per object per frame.
 *
 * The only anchor the codebase offers is ENEMY_SNAP_DISTANCE (220.0f) — the distance at which two
 * peers' simulations are judged to have genuinely diverged rather than merely drifted. These are
 * several multiples of that, on the reasoning that an object worth correcting at all is worth
 * publishing often, and an object further away than a player can meaningfully see is not.
 *
 * THEY ARE FIRST GUESSES. Nothing establishes BK's world scale, and the objtier log line exists
 * to replace them with measured ones. See docs/symbols.md §35.
 */
#define BC_TIER_NEAR 1000.0f
#define BC_TIER_MID 2500.0f
#define BC_TIER_FAR 6000.0f

#define BC_TIER_NEAR_D2 (BC_TIER_NEAR * BC_TIER_NEAR)
#define BC_TIER_MID_D2 (BC_TIER_MID * BC_TIER_MID)
#define BC_TIER_FAR_D2 (BC_TIER_FAR * BC_TIER_FAR)

/* Which band a squared distance falls in. Ordered so a larger number is further away, and so the
 * diagnostic can index an array with it. */
#define BC_TIER_INDEX_NEAR 0u
#define BC_TIER_INDEX_MID 1u
#define BC_TIER_INDEX_FAR 2u
#define BC_TIER_INDEX_CULLED 3u
#define BC_TIER_COUNT 4u

bc_u32 bc_tier_of(bc_f32 dist2);

/* Frames between publishes for a band. 0 means never — the object is culled.
 *
 * Not a smooth falloff: a receiver only ever corrects an object past ENEMY_SNAP_DISTANCE, so what
 * matters is whether a correction arrives within the time it takes to drift that far, and three
 * bands express that as well as a curve would while staying legible in a log line. */
bc_u32 bc_tier_period(bc_u32 tier);

/* Whether this object publishes on this tick.
 *
 * Phased by net_id rather than gated on the tick alone. Without it every object in a band shares a
 * frame, so a map's whole mid-band lands together and the object frame alternates between empty
 * and over budget — the traffic is the same but the peaks are far worse, and the peaks are what
 * overflow BCNET_MAX_OBJECTS.
 */
bc_u32 bc_tier_publishes(bc_u32 net_id, bc_f32 dist2, bc_u32 tick);

#endif
