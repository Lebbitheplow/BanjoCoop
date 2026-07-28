#ifndef BANJOCOOP_API_INTERNAL_H
#define BANJOCOOP_API_INTERNAL_H

#include "banjocoop/protocol.h"

#define BANJOCOOP_SCOPE_ALL 0u
#define BANJOCOOP_SCOPE_MAP 1u
#define BANJOCOOP_SCOPE_LEVEL 2u

/* Refresh what the exports report. Called once per frame before anything can ask. */
void api_begin_frame(bc_incoming *inc);

/* Hand a custom message to whichever mods subscribed. */
void api_deliver(const bc_event *ev);

#endif
