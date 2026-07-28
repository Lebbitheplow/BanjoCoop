#ifndef BANJOCOOP_BOSS_H
#define BANJOCOOP_BOSS_H

#include "banjocoop/protocol.h"
#include "prop.h"

/* Boss phase transitions and the Gruntilda fight. See boss.c for why bosses need this and
 * ordinary enemies do not. */

u32 bc_marker_is_boss(ActorMarker *marker);

void boss_apply_state(u32 net_id, u32 state);
void boss_apply_fight(u32 what, u32 a, u32 b);

/* Implemented by world.c, which owns the outgoing event queue. */
void bc_boss_report_state(u32 net_id, u32 state);
void bc_boss_report_fight(u32 what, u32 a, u32 b);

#endif
