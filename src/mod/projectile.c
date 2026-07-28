/* Projectiles — things enemies throw.
 *
 * These are spawned during play, so unlike a map's enemies they have no identity both machines
 * can derive independently: spawn order diverges the moment two players do different things
 * (docs/symbols.md §3). The owner names each one and everybody else adopts it.
 *
 * Without that, every peer produces its own from its own copy of the enemy, aimed at whoever is
 * nearest *there* — so the same fight looks different on each screen and a projectile that hits
 * you may never have existed for the other player.
 *
 * Identity, position and lifetime all travel in the object frame rather than in a separate spawn
 * message: an object carrying an actor id is one the receiver must spawn, and an object the frame
 * stops mentioning is one that has hit something or expired. One mechanism, nothing to keep in
 * step with anything else.
 */

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "prop.h"
#include "actor.h"
#include "bkrecomp_api.h"

#include "banjocoop/protocol.h"
#include "enemy.h"
#include "projectile.h"

extern ActorArray *suBaddieActorArray;

/* Things enemies throw.
 *
 * These are spawned during play, so they have no shared identity the way a map's enemies do — the
 * owner names each one and everybody else adopts it. Without that, every peer produces its own
 * from its own copy of the enemy, aimed at whoever is nearest *there*, and the same fight looks
 * different on each screen: a projectile that hits you may never have existed for the other
 * player. */
static const u16 k_projectile_markers[] = {
    MARKER_C_ORANGE_PROJECTILE, /* Chimpy's oranges */
    MARKER_B2_SNOWBALL,         /* Sir Slush */
    MARKER_298_MUMMUM_BALL,
    /* Gruntilda's spells. Hers matter more than most: a fireball that exists on one screen and
     * not another is the difference between a fair fight and an unwinnable one. */
    MARKER_25C_GRUNTY_SPELL_FIREBALL,
    MARKER_25D_GREEN_BLAST,
    MARKER_280_GRUNTY_SPELL_GREEN_ATTACK,
};

#define PROJECTILE_MARKER_COUNT (sizeof(k_projectile_markers) / sizeof(k_projectile_markers[0]))

/* Projectiles we have spawned because the owner told us to, so they can be positioned each frame
 * and removed when the owner stops mentioning them. Markers, never Actor pointers — the actor
 * array reallocates (docs §13). */
#define PROJECTILE_MAX 16u

typedef struct {
    u32 net_id;
    ActorMarker *marker;
} RemoteProjectile;

static RemoteProjectile s_remote_projectiles[PROJECTILE_MAX];
static u32 s_remote_projectile_count = 0;

/* Set while we spawn the owner's projectile below.
 *
 * The spawn patch suppresses projectiles a non-owner creates for itself — and the actor we adopt
 * is created by exactly the call it is watching. Without this guard a non-owner would suppress the
 * one projectile it is supposed to keep, and no projectile would ever appear on a client. Same
 * shape, and the same reason, as s_replaying_hit in enemy.c. */
static u32 s_adopting = 0;

u32 projectile_is_adopting(void) {
    return s_adopting;
}

u32 marker_is_projectile(ActorMarker *marker) {
    if (marker == NULL) {
        return 0;
    }
    for (u32 i = 0; i < PROJECTILE_MARKER_COUNT; i++) {
        if ((u32)marker->id == (u32)k_projectile_markers[i]) {
            return 1;
        }
    }
    return 0;
}

static RemoteProjectile *find_remote_projectile(ActorMarker *marker) {
    for (u32 i = 0; i < s_remote_projectile_count; i++) {
        if (s_remote_projectiles[i].marker == marker) {
            return &s_remote_projectiles[i];
        }
    }
    return NULL;
}

static u32 remote_projectile_exists(u32 net_id) {
    for (u32 i = 0; i < s_remote_projectile_count; i++) {
        if (s_remote_projectiles[i].net_id == net_id) {
            return 1;
        }
    }
    return 0;
}

static void forget_remote_projectiles(void) {
    s_remote_projectile_count = 0;
}

void projectile_sync(bc_incoming *inc, bc_outgoing *out, u32 owner,
                             bc_object_frame *frame) {
    if (suBaddieActorArray == NULL) {
        return;
    }

    if (owner) {
        /* Published every frame, at every distance, deliberately — projectiles are the one thing
         * in the object frame that must NOT be throttled the way enemies are (enemy.c, tier.h).
         *
         * The asymmetry follows from what omission means. An enemy missing from a frame is simply
         * one the receiver was not told about, and it keeps running its own copy. A projectile
         * missing from a frame is one the receiver *despawns* — see the `listed` check below,
         * which is what gives a projectile a lifetime without needing a second message to end it.
         * Skipping a distant projectile to save bandwidth would delete it mid-flight on every
         * screen but the owner's.
         *
         * They are also few: PROJECTILE_MAX is 16 and the reserve is 8, against enemies that can
         * fill a map. There is little here to save. */
        for (s32 i = 0; i < suBaddieActorArray->cnt; i++) {
            Actor *actor = &suBaddieActorArray->data[i];
            if (actor->despawn_flag || !marker_is_projectile(actor->marker)) {
                continue;
            }
            if (out->objects.count >= BCNET_MAX_OBJECTS) {
                break;
            }
            bc_object_state *obj = &out->objects.objects[out->objects.count++];
            obj->net_id = bkrecomp_get_marker_spawn_index(actor->marker);
            obj->flags = BC_OBJ_ACTIVE;
            obj->actor_id = (u32)actor->modelCacheIndex;
            obj->pos[0] = actor->position[0];
            obj->pos[1] = actor->position[1];
            obj->pos[2] = actor->position[2];
            obj->yaw = actor->yaw;
            obj->anim_id = 0;
            obj->anim_frame = 0.0f;
        }
        return;
    }

    /* Not the owner. Drop anything our own enemies threw, and keep the owner's in step. */
    for (s32 i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (actor->despawn_flag || !marker_is_projectile(actor->marker)) {
            continue;
        }
        RemoteProjectile *mine = find_remote_projectile(actor->marker);
        if (mine == NULL) {
            /* Ours, not theirs. The owner's copy of this shot is the one everyone sees. */
            bc_defer_despawn(actor->marker);
            continue;
        }
        /* Still listed by the owner? */
        u32 listed = 0;
        if (frame != NULL) {
            for (u32 j = 0; j < frame->count && j < BCNET_MAX_OBJECTS; j++) {
                if (frame->objects[j].net_id == mine->net_id &&
                    frame->objects[j].actor_id != 0) {
                    actor->position[0] = frame->objects[j].pos[0];
                    actor->position[1] = frame->objects[j].pos[1];
                    actor->position[2] = frame->objects[j].pos[2];
                    actor->yaw = frame->objects[j].yaw;
                    listed = 1;
                    break;
                }
            }
        }
        if (!listed) {
            bc_defer_despawn(actor->marker);
            mine->marker = NULL;
        }
    }

    /* Compact, then spawn anything the owner has that we do not. */
    u32 kept = 0;
    for (u32 i = 0; i < s_remote_projectile_count; i++) {
        if (s_remote_projectiles[i].marker != NULL) {
            s_remote_projectiles[kept++] = s_remote_projectiles[i];
        }
    }
    s_remote_projectile_count = kept;

    if (frame == NULL) {
        return;
    }
    for (u32 j = 0; j < frame->count && j < BCNET_MAX_OBJECTS; j++) {
        bc_object_state *obj = &frame->objects[j];
        if (obj->actor_id == 0 || remote_projectile_exists(obj->net_id)) {
            continue;
        }
        if (s_remote_projectile_count >= PROJECTILE_MAX) {
            break;
        }
        f32 pos[3];
        pos[0] = obj->pos[0];
        pos[1] = obj->pos[1];
        pos[2] = obj->pos[2];
        s_adopting = 1;
        Actor *spawned = actor_spawnWithYaw_f32((s32)obj->actor_id, pos, (s32)obj->yaw);
        s_adopting = 0;
        if (spawned == NULL || spawned->marker == NULL) {
            continue;
        }
        s_remote_projectiles[s_remote_projectile_count].net_id = obj->net_id;
        s_remote_projectiles[s_remote_projectile_count].marker = spawned->marker;
        s_remote_projectile_count++;
    }
}


void projectile_reset(void) {
    forget_remote_projectiles();
}
