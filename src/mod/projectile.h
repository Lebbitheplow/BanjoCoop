#ifndef BANJOCOOP_PROJECTILE_H
#define BANJOCOOP_PROJECTILE_H

#include "banjocoop/protocol.h"

/* Slots in the object frame held back from enemies so projectiles can always be published.
 *
 * Enemies fill the frame first and used to be able to take all of it — 24 live enemies in a map
 * meant Grunty's fireballs were never sent, and a projectile that exists on one screen and not
 * another is the difference between a fair fight and an unwinnable one. Distance culling makes
 * that unlikely rather than impossible, so the reservation stays.
 *
 * Unused slots are not wasted bandwidth: the frame carries `count` objects, not a fixed 24. */
#define BC_PROJECTILE_RESERVE 8u

/* Keep the owner's projectiles, and only the owner's, on every screen. Called once per frame from
 * the enemy layer, which already knows who owns this map. */
void projectile_sync(bc_incoming *inc, bc_outgoing *out, u32 owner, bc_object_frame *frame);

/* Forget adopted projectiles — on a map change or a disconnect their ids mean nothing. */
void projectile_reset(void);

/* True for one of the marker types we treat as a projectile. Exposed for the spawn patch, which
 * has to classify an actor the instant it is created, before anything else has seen it. */
u32 marker_is_projectile(ActorMarker *marker);

/* Set while we are spawning the owner's projectile ourselves, so the spawn patch does not suppress
 * the very actor it is meant to leave alone. */
u32 projectile_is_adopting(void);

#endif
