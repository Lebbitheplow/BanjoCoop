#ifndef BANJOCOOP_UI_INTERNAL_H
#define BANJOCOOP_UI_INTERNAL_H

#include "banjocoop/protocol.h"

/* Shared between the overlay and the input-capturing panels. */
u32 ui_str_put(char *dst, u32 at, const char *src, u32 max);
u32 ui_str_put_u32(char *dst, u32 at, u32 value, u32 max);

/* The panels that take input: chat and the lobby. */
void ui_panels_update(bc_incoming *inc, bc_outgoing *out);

#endif
