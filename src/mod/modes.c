/* Race and hide-and-seek — Phase 9's "beyond co-op".
 *
 * Both are thin on purpose. Nearly everything a mode needs already exists: player positions and
 * maps arrive every frame, events are reliable and attributed, and the overlay can show a line of
 * text. A mode is therefore a rule about that data, not a new system — which is also the honest
 * test of whether the replication layer underneath is any good.
 *
 * The host owns each round. Not because clients cannot be trusted, but because somebody has to
 * decide who won, and every peer deciding independently is how you get two winners — the same
 * lesson the collectibles taught.
 */

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"
#include "recompconfig.h"

#include "banjocoop/protocol.h"
#include "modes.h"

extern void player_getPosition(f32 dst[3]);

#define MODE_OFF 0u
#define MODE_RACE 1u
#define MODE_HIDE 2u

/* How close counts as reaching the finish, or as being found. Generous — this is a party mode,
 * and arguing about whether somebody was tagged is worse than the occasional early tag. */
#define MODE_TOUCH_DISTANCE 220.0f

/* Hide-and-seek gives hiders this long before the seeker is released. */
#define MODE_HIDE_HEADSTART_FRAMES (30u * 60u)

static u32 s_mode = MODE_OFF;
static u32 s_running = 0;
static u32 s_frames = 0;
static u32 s_seeker = BCNET_MAX_PLAYERS;
static u32 s_found[BCNET_MAX_PLAYERS];

/* The race finish, set where the host stood when the round started. A fixed landmark would need
 * per-map data for every map in the game; "where I am now" needs none and lets a host pick any
 * finish they like just by standing there. */
static f32 s_finish[3];
static u32 s_finish_map = 0;

void modes_reset(void) {
    s_running = 0;
    s_frames = 0;
    s_seeker = BCNET_MAX_PLAYERS;
    for (u32 i = 0; i < BCNET_MAX_PLAYERS; i++) {
        s_found[i] = 0;
    }
}

u32 modes_active(void) {
    return s_running;
}

/* Distance to a remote player, or a huge number if they are elsewhere. */
static f32 distance_to(const bc_remote_player *r, u32 map_id, f32 self[3]) {
    if (r->state.map_id != map_id) {
        return 1.0e9f;
    }
    f32 dx = r->state.pos[0] - self[0];
    f32 dy = r->state.pos[1] - self[1];
    f32 dz = r->state.pos[2] - self[2];
    return (dx * dx) + (dy * dy) + (dz * dz);
}

void modes_apply(u32 what, u32 a, u32 b) {
    switch (what) {
        case BC_MODE_START:
            s_mode = a;
            s_seeker = b;
            s_running = 1;
            s_frames = 0;
            for (u32 i = 0; i < BCNET_MAX_PLAYERS; i++) {
                s_found[i] = 0;
            }
            recomp_printf("[banjocoop] round started: %s\n",
                          a == MODE_RACE ? "race" : "hide and seek");
            break;

        case BC_MODE_FOUND:
            if (a < BCNET_MAX_PLAYERS) {
                s_found[a] = 1;
            }
            break;

        case BC_MODE_END:
            s_running = 0;
            recomp_printf("[banjocoop] round over - player %u wins\n", a);
            break;

        default:
            break;
    }
}

/* Only the host judges. Every peer running the same check would announce its own winner. */
static void host_judge(bc_incoming *inc, u32 map_id) {
    f32 self[3];
    player_getPosition(self);

    if (s_mode == MODE_RACE) {
        /* The host is a runner too, so check ourselves as well as everyone else. */
        if (map_id == s_finish_map) {
            f32 dx = s_finish[0] - self[0];
            f32 dy = s_finish[1] - self[1];
            f32 dz = s_finish[2] - self[2];
            if ((dx * dx) + (dy * dy) + (dz * dz) < (MODE_TOUCH_DISTANCE * MODE_TOUCH_DISTANCE)) {
                bc_mode_report(BC_MODE_END, inc->local_player_id, 0);
                modes_apply(BC_MODE_END, inc->local_player_id, 0);
                return;
            }
        }
        for (u32 i = 0; i < inc->remote_count && i < BCNET_MAX_PLAYERS; i++) {
            const bc_remote_player *r = &inc->remotes[i];
            if (r->state.map_id != s_finish_map) {
                continue;
            }
            f32 dx = s_finish[0] - r->state.pos[0];
            f32 dy = s_finish[1] - r->state.pos[1];
            f32 dz = s_finish[2] - r->state.pos[2];
            if ((dx * dx) + (dy * dy) + (dz * dz) < (MODE_TOUCH_DISTANCE * MODE_TOUCH_DISTANCE)) {
                bc_mode_report(BC_MODE_END, r->player_id, 0);
                modes_apply(BC_MODE_END, r->player_id, 0);
                return;
            }
        }
        return;
    }

    /* Hide and seek. The seeker is held still until the head start elapses. */
    if (s_frames < MODE_HIDE_HEADSTART_FRAMES) {
        return;
    }

    /* Whoever the seeker touches is found. Judged from the seeker's position, wherever they are. */
    f32 seeker_pos[3];
    u32 have_seeker = 0;
    if (s_seeker == inc->local_player_id) {
        seeker_pos[0] = self[0];
        seeker_pos[1] = self[1];
        seeker_pos[2] = self[2];
        have_seeker = 1;
    } else {
        for (u32 i = 0; i < inc->remote_count && i < BCNET_MAX_PLAYERS; i++) {
            if (inc->remotes[i].player_id == s_seeker) {
                seeker_pos[0] = inc->remotes[i].state.pos[0];
                seeker_pos[1] = inc->remotes[i].state.pos[1];
                seeker_pos[2] = inc->remotes[i].state.pos[2];
                have_seeker = 1;
                break;
            }
        }
    }
    if (!have_seeker) {
        return;
    }

    u32 remaining = 0;
    for (u32 i = 0; i < inc->remote_count && i < BCNET_MAX_PLAYERS; i++) {
        const bc_remote_player *r = &inc->remotes[i];
        if (r->player_id == s_seeker || s_found[r->player_id]) {
            continue;
        }
        remaining++;
        if (distance_to(r, map_id, seeker_pos) < (MODE_TOUCH_DISTANCE * MODE_TOUCH_DISTANCE)) {
            s_found[r->player_id] = 1;
            bc_mode_report(BC_MODE_FOUND, r->player_id, 0);
            remaining--;
            recomp_printf("[banjocoop] player %u was found\n", r->player_id);
        }
    }

    if (remaining == 0) {
        bc_mode_report(BC_MODE_END, s_seeker, 0);
        modes_apply(BC_MODE_END, s_seeker, 0);
    }
}

void modes_update(bc_incoming *inc, u32 map_id) {
    u32 want = recomp_get_config_u32("party_mode");

    if (!inc->connected) {
        modes_reset();
        return;
    }

    /* Only the host starts a round, and only when the setting changes to a real mode. */
    if (inc->is_host && want != MODE_OFF && !s_running) {
        s_mode = want;
        player_getPosition(s_finish);
        s_finish_map = map_id;
        /* The host seeks; simplest rule, and it means the person who set the mode is the one who
         * has to count. */
        u32 seeker = inc->local_player_id;
        bc_mode_report(BC_MODE_START, want, seeker);
        modes_apply(BC_MODE_START, want, seeker);
        return;
    }
    if (inc->is_host && want == MODE_OFF && s_running) {
        bc_mode_report(BC_MODE_END, BCNET_MAX_PLAYERS, 0);
        modes_apply(BC_MODE_END, BCNET_MAX_PLAYERS, 0);
        return;
    }

    if (!s_running) {
        return;
    }
    s_frames++;

    if (inc->is_host) {
        host_judge(inc, map_id);
    }
}
