#ifndef BANJOCOOP_PROGRESS_H
#define BANJOCOOP_PROGRESS_H

#include "banjocoop/protocol.h"

/* The progression mirror (Phase 6). The host's save is authoritative; clients mirror it. */

/* Host only: pack the four permanent stores into the outgoing mirror. */
void progress_publish(bc_outgoing *out);

/* Clients: bring local progression into line with the host's. Returns how many bits it had to
 * change, which is 0 in the steady state. */
u32 progress_apply(bc_incoming *inc);

/* Forget the logging/publish bookkeeping on disconnect, so a reconnect reports afresh. */
void progress_reset(void);

#endif
