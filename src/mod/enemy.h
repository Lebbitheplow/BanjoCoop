#ifndef BANJOCOOP_ENEMY_H
#define BANJOCOOP_ENEMY_H

#include "banjocoop/protocol.h"

/* Enemy replication (Phase 5).
 *
 * Exactly one peer simulates a given enemy and publishes its state; everyone else stops running
 * its AI and applies what arrives. That is the ownership model the plan settled on in place of
 * deterministic lockstep, which is not achievable over recompiled code we do not control.
 *
 * For now the owner is always the host. Per-object ownership with proximity assignment and
 * transfer is the next step, and nothing here assumes the owner will stay the host.
 */

/* Called once per frame from the frame hook, after the world layer has run.
 *   - on the owner: fills `out` with the state of every enemy it is simulating
 *   - on everyone else: applies `in` and keeps local AI suppressed
 */
void enemy_sync(bc_incoming *inc, bc_outgoing *out, u32 map_id);

/* Forget everything learned about the current map's enemies. Net ids are per-map, so a map change
 * or a disconnect invalidates all of it. */
void enemy_reset(void);

/* Whether this peer owned the objects in its map as of the last enemy_sync.
 *
 * Ownership is recomputed every frame from state every peer already has, but callers outside the
 * frame hook cannot recompute it — the spawn patch runs from inside game code, wherever an enemy
 * decided to throw something. This hands them the frame's answer. 0 until the first sync, so
 * nothing is ever suppressed on the strength of a guess. */
u32 enemy_owns_objects_here(void);

/* Record that an enemy is dead, so a local copy still standing is removed on sight. Called both
 * when we detect a local death and when another player reports one. */
void enemy_mark_dead(u32 net_id);

/* Implemented by world.c: queue a reliable BC_EV_ENEMY_DEAD for the current map. Lives there so
 * enemy.c does not need its own copy of the event queue and its guards. */
void bc_enemy_report_death(u32 net_id);

/* Implemented by world.c: report that we landed a hit on an enemy, so every other peer can apply
 * the same one to their copy. */
void bc_enemy_report_hit(u32 net_id, u32 collision_type);

/* Replay a hit somebody else landed. Finds the enemy by net id and runs the game's own damage
 * path on it. */
void enemy_apply_hit(u32 net_id, u32 collision_type);

/* Implemented by world_apply.c: queue a marker to be despawned once we are outside whatever game
 * function asked for it. */
void bc_defer_despawn(ActorMarker *marker);

#endif
