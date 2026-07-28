/* Carrying another player — Phase 9.
 *
 * The game already knows how to carry things. `BS_3A_CARRY_IDLE` / `BS_3B_CARRY_WALK` are how
 * Banjo hauls Freezeezy Peak's presents, Click Clock Wood's acorn and Blubber's gold, and the
 * mechanic takes an `ActorMarker` — it does not care what the marker is.
 *
 * A teammate is already a real actor on your screen: their puppet. So carrying a player needs no
 * new movement code, no new animations and no new physics. Hand the game the puppet's marker and
 * it does the rest, including the walk cycle and the throw.
 *
 * The only genuinely new problem is the other half. On the carrier's machine the game is moving
 * the puppet; on the carried player's own machine their real Banjo has to follow. Those two must
 * not fight:
 *
 *   - the carrier stops applying network state to that puppet, so the game owns it outright
 *   - the carried player takes their position from the carrier instead of from their own input
 *
 * Without that split it is circular — the puppet's position comes from the carried player, who is
 * being moved by the puppet.
 */

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"
#include "recompconfig.h"

#include "prop.h"
#include "banjocoop/protocol.h"
#include "carry.h"

extern void bacarry_set_marker(ActorMarker *marker);
extern ActorMarker *bacarry_get_marker(void);
extern void bs_setState(s32 state);
extern s32 bs_getState(void);
extern void player_setPosition(f32 position[3]);
extern int bakey_pressed(s32 button);

#define BS_CARRY_IDLE 0x3A
#define BS_CARRY_WALK 0x3B

/* How close a teammate must be to pick up, and how high above the carrier they ride. Matched
 * roughly to what the game uses for its own carried objects. */
#define CARRY_REACH 180.0f
#define CARRY_HEIGHT 120.0f

/* Who we are carrying, and who is carrying us. BCNET_MAX_PLAYERS means nobody. */
static u32 s_carrying = BCNET_MAX_PLAYERS;
static u32 s_carried_by = BCNET_MAX_PLAYERS;

u32 carry_is_carrying(u32 player_id) {
    return s_carrying == player_id;
}

void carry_set_carried_by(u32 carrier_id, u32 on) {
    s_carried_by = on ? carrier_id : BCNET_MAX_PLAYERS;
}

void carry_reset(void) {
    s_carrying = BCNET_MAX_PLAYERS;
    s_carried_by = BCNET_MAX_PLAYERS;
}

static u32 carry_enabled(void) {
    return recomp_get_config_u32("allow_carrying") != 0;
}

void carry_update(bc_incoming *inc, u32 map_id) {
    if (!inc->connected || !carry_enabled()) {
        if (s_carrying != BCNET_MAX_PLAYERS) {
            bc_carry_report(s_carrying, 0);
            s_carrying = BCNET_MAX_PLAYERS;
        }
        s_carried_by = BCNET_MAX_PLAYERS;
        return;
    }

    /* Being carried: our position is the carrier's business, not our input's. Overriding it every
     * frame is what makes the carry stick — anything we walk is simply undone. */
    if (s_carried_by < BCNET_MAX_PLAYERS) {
        for (u32 i = 0; i < inc->remote_count && i < BCNET_MAX_PLAYERS; i++) {
            const bc_remote_player *r = &inc->remotes[i];
            if (r->player_id != s_carried_by) {
                continue;
            }
            if (r->state.map_id != map_id) {
                /* They left the map holding us. Better to be dropped than dragged. */
                s_carried_by = BCNET_MAX_PLAYERS;
                break;
            }
            f32 pos[3];
            pos[0] = r->state.pos[0];
            pos[1] = r->state.pos[1] + CARRY_HEIGHT;
            pos[2] = r->state.pos[2];
            player_setPosition(pos);
            return;
        }
        /* They are gone entirely. */
        s_carried_by = BCNET_MAX_PLAYERS;
        return;
    }

    s32 state = bs_getState();
    u32 in_carry_state = (state == BS_CARRY_IDLE || state == BS_CARRY_WALK);

    /* Put them down: the game left the carry state, or the marker went away. */
    if (s_carrying != BCNET_MAX_PLAYERS && (!in_carry_state || bacarry_get_marker() == NULL)) {
        bc_carry_report(s_carrying, 0);
        s_carrying = BCNET_MAX_PLAYERS;
        return;
    }
    if (s_carrying != BCNET_MAX_PLAYERS) {
        return; /* already carrying somebody; the game is driving it */
    }

    /* Pick up: Z near a teammate. Only from a normal standing state, so this cannot interrupt
     * something the game is in the middle of. */
    if (in_carry_state || !bakey_pressed(BUTTON_Z)) {
        return;
    }

    f32 self[3];
    player_getPosition(self);

    for (u32 i = 0; i < inc->remote_count && i < BCNET_MAX_PLAYERS; i++) {
        const bc_remote_player *r = &inc->remotes[i];
        if (r->state.map_id != map_id) {
            continue;
        }
        f32 dx = r->state.pos[0] - self[0];
        f32 dy = r->state.pos[1] - self[1];
        f32 dz = r->state.pos[2] - self[2];
        if ((dx * dx) + (dy * dy) + (dz * dz) > (CARRY_REACH * CARRY_REACH)) {
            continue;
        }

        ActorMarker *puppet = puppet_marker_for(r->player_id);
        if (puppet == NULL) {
            continue;
        }

        /* Hand the game the puppet and let it carry, exactly as it would a present. */
        bacarry_set_marker(puppet);
        bs_setState(BS_CARRY_IDLE);
        s_carrying = r->player_id;
        bc_carry_report(r->player_id, 1);
        recomp_printf("[banjocoop] picked up player %u\n", r->player_id);
        return;
    }
}
