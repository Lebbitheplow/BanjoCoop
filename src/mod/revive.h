#ifndef BANJOCOOP_REVIVE_H
#define BANJOCOOP_REVIVE_H

#include "banjocoop/protocol.h"

/* Come back next to a teammate rather than at the map's start. See revive.c for why this is the
 * co-op pain point worth fixing and what is deliberately left alone. */
void revive_begin_frame(u32 map_id);
void revive_update(bc_incoming *inc, u32 map_id);
void revive_reset(void);

#endif
