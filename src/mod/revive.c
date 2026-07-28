/* Revive — Phase 9.
 *
 * Dying in vanilla sends you back to wherever the map started you. In co-op that is the single
 * most tedious thing that can happen: the party is split, and whoever died spends the next minute
 * walking back to where everybody else is. Two players in the same world spend more time
 * regrouping than playing.
 *
 * So when you die with a teammate in the same map, you come back next to them instead. Nothing
 * else about death changes — the animation plays, the life is spent, the health is restored — it
 * is only *where* you reappear that differs.
 *
 * Deliberately not done here: changing whether a life is spent, or reviving in place. Both alter
 * the game's difficulty rather than its logistics, and the plan puts lives firmly in the
 * per-player column.
 */

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"
#include "recompconfig.h"

#include "banjocoop/protocol.h"
#include "revive.h"

extern void player_setPosition(f32 position[3]);
extern s32 bs_getState(void);

/* BS_41_DIE. Once the player is out of this state the death sequence has finished and the game
 * has put them wherever it wanted them — which is the moment to move them. */
#define REVIVE_STATE_DIE 0x41

/* How far away a teammate can be and still be worth respawning next to. Beyond this they are
 * probably somewhere unreachable — mid-flight, inside a Mumbo hut, part-way through a
 * transition — and dropping a player there would be worse than the walk. */
#define REVIVE_MAX_DISTANCE 6000.0f

/* Placed slightly off to the side, so two players do not end up inside one another. */
#define REVIVE_OFFSET 60.0f

static u32 s_died = 0;
static u32 s_died_map = 0;

RECOMP_HOOK("bsdie_init") void banjocoop_on_death(void) {
    s_died = 1;
}

void revive_begin_frame(u32 map_id) {
    if (s_died && s_died_map == 0) {
        s_died_map = map_id;
    }
}

void revive_update(bc_incoming *inc, u32 map_id) {
    if (!s_died) {
        return;
    }
    if (recomp_get_config_u32("revive_near_teammate") == 0) {
        s_died = 0;
        s_died_map = 0;
        return;
    }

    /* Still dying — the game has not finished moving us yet. */
    if (bs_getState() == REVIVE_STATE_DIE) {
        return;
    }

    /* Died in one map and came back in another: the game sent us somewhere deliberately (a game
     * over, a level exit) and second-guessing that would be wrong. */
    if (!inc->connected || map_id != s_died_map) {
        s_died = 0;
        s_died_map = 0;
        return;
    }

    f32 self[3];
    player_getPosition(self);

    const bc_remote_player *best = NULL;
    f32 best_distance = REVIVE_MAX_DISTANCE * REVIVE_MAX_DISTANCE;
    for (u32 i = 0; i < inc->remote_count && i < BCNET_MAX_PLAYERS; i++) {
        const bc_remote_player *r = &inc->remotes[i];
        if (r->state.map_id != map_id) {
            continue;
        }
        f32 dx = r->state.pos[0] - self[0];
        f32 dy = r->state.pos[1] - self[1];
        f32 dz = r->state.pos[2] - self[2];
        f32 d2 = (dx * dx) + (dy * dy) + (dz * dz);
        if (d2 < best_distance) {
            best_distance = d2;
            best = r;
        }
    }

    s_died = 0;
    s_died_map = 0;

    if (best == NULL) {
        return; /* nobody to come back to; the game's own respawn stands */
    }

    f32 pos[3];
    pos[0] = best->state.pos[0] + REVIVE_OFFSET;
    pos[1] = best->state.pos[1];
    pos[2] = best->state.pos[2] + REVIVE_OFFSET;
    player_setPosition(pos);
    recomp_printf("[banjocoop] respawned next to player %u\n", best->player_id);
}

void revive_reset(void) {
    s_died = 0;
    s_died_map = 0;
}
