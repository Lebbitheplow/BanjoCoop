#ifndef BANJOCOOP_CARRY_H
#define BANJOCOOP_CARRY_H

#include "banjocoop/protocol.h"
#include "prop.h"

/* Carrying another player, using the game's own carry mechanic. See carry.c. */
void carry_update(bc_incoming *inc, u32 map_id);
void carry_set_carried_by(u32 carrier_id, u32 on);
void carry_reset(void);

/* True while we are carrying this player — the puppet layer uses it to stop applying network
 * state to a puppet the game is currently moving. */
u32 carry_is_carrying(u32 player_id);

/* Implemented by puppet.c: the marker of a remote player's puppet, or NULL. */
ActorMarker *puppet_marker_for(u32 player_id);

/* Implemented by world.c: tell everyone we picked somebody up or put them down. */
void bc_carry_report(u32 player_id, u32 on);

#endif
