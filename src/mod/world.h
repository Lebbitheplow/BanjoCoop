#ifndef BANJOCOOP_WORLD_H
#define BANJOCOOP_WORLD_H

#include "banjocoop/protocol.h"

/* Shared world state (Phase 3).
 *
 * Player state is a continuous signal; world state is a sequence of discrete changes. These are
 * replicated as reliable events rather than as a per-frame snapshot, because a lost "this note is
 * gone" never gets superseded — it is a permanent divergence.
 *
 * The mod's job here is only to (a) notice a local world change and queue it, and (b) apply a
 * change the host accepted. All arbitration lives on the host, in transport.cpp.
 */

/* Cache the map/level every event queued this frame belongs to. Call before anything else in the
 * frame hook — the collection hooks fire from deep inside game code and must not have to work out
 * where they are. */
void world_begin_frame(u32 map_id, u32 level_id);

/* Apply everything the host accepted for us, then run deferred work (despawns that could not
 * safely happen inside the game function that triggered them). */
void world_apply(bc_incoming *inc);

/* The queue the collection hooks append to. Handed straight to bcnet_pump, which drains it. */
bc_outgoing *world_outgoing(void);

/* Forget all replicated world state. Called on disconnect: the registry describes one session. */
void world_reset(void);

#endif
