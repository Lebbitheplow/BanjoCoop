#ifndef BANJOCOOP_MODES_H
#define BANJOCOOP_MODES_H

#include "banjocoop/protocol.h"

/* Race and hide-and-seek. Thin by design — see modes.c. */
void modes_update(bc_incoming *inc, u32 map_id);
void modes_apply(u32 what, u32 a, u32 b);
void modes_reset(void);
u32 modes_active(void);

/* Implemented by world.c, which owns the outgoing queue. */
void bc_mode_report(u32 what, u32 a, u32 b);

#endif
