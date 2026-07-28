#ifndef BANJOCOOP_UI_H
#define BANJOCOOP_UI_H

#include "banjocoop/protocol.h"

/* The player list overlay. Called once per frame; refreshes itself on its own cadence and does
 * nothing at all if the player has it switched off. */
void ui_update(bc_incoming *inc, bc_outgoing *out, u32 local_map, u32 frame);

/* Announce something a player did — "player2 got a Jiggy!". Called from the apply path, which is
 * the only place that knows both what happened and who caused it. */
void ui_toast_event(bc_incoming *inc, const bc_event *ev);

#endif
