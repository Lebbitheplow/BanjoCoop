#ifndef BANJOCOOP_PUPPET_H
#define BANJOCOOP_PUPPET_H

#include "banjocoop/protocol.h"

/* Spawn, update and despawn puppets to match the remote players in `inc`.
 * Call once per frame after bcnet_pump(). Puppets are only shown for players whose map_id
 * matches local_map_id. */
void puppet_sync(bc_incoming *inc, u32 local_map_id);

/* Despawn every puppet — used on disconnect. */
void puppet_clear_all(void);

#endif
