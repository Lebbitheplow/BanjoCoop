/* Enemy replication — the game side of Phase 5.
 *
 * Every peer simulates enemies locally; the owner additionally publishes their position and
 * animation, and receivers snap to it only when they have drifted meaningfully apart. Deaths are
 * broadcast by whoever sees one die.
 *
 * This is not the "owner simulates, everyone else is frozen" model, and that is deliberate. An
 * enemy's damage handler lives in per-actor local state with no generic dispatch point
 * (`func_803889A0` for the grublin, a different one per enemy), while its *death sequence* lives
 * in the update function. Silencing the update function therefore leaves a client able to hit an
 * enemy but never able to kill it — precisely backwards for ganging up on one together. Letting
 * every peer run the AI keeps every player's attacks working, and correcting position only on real
 * divergence keeps them looking like the same enemy without fighting the local simulation every
 * frame.
 *
 * Death is the one thing that must not be lost, so it is a reliable event rather than a flag in
 * the position stream: a dropped "it's gone" is never superseded, it just leaves that enemy alive
 * forever on one machine.
 *
 * Identity is the map's marker spawn index, which is only stable for actors spawned from the map's
 * static spawn list. Anything spawned during play diverges between machines and is skipped rather
 * than replicated wrongly (docs/symbols.md §3).
 */

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "prop.h"
#include "actor.h"
#include "core2/anctrl.h"
#include "bkrecomp_api.h"

#include "banjocoop/protocol.h"
#include "enemy.h"
#include "projectile.h"
#include "tier.h"

/* -> src/core2/code_9E370.c. Holds every live actor; `data` is a variable-length array. */
extern ActorArray *suBaddieActorArray;

/* The one generic damage path: `marker_callCollisionFunc(this, other, type)` dispatches an
 * enemy's "ow" and "die" handlers (core2/code_A5BC0.c:1235). Every enemy's actual damage logic
 * lives in per-actor state with no common signature, so this dispatcher is the only place a hit
 * can be observed — or replayed — without knowing anything about the enemy that took it. */
/* -> src/core2/ba/marker.c. The player's own marker, which is what the engine passes as `other`
 * when the player lands a hit. Replaying with our own player's marker makes a remote hit look
 * like a local one to the enemy, which is exactly what we want. */
extern ActorMarker *playerMarker;

/* Set while replaying somebody else's hit, so our own hook does not report it straight back and
 * bounce a single hit around the session forever. */
static u32 s_replaying_hit = 0;

/* Who simulates the enemies in a map: the lowest player id present in it.
 *
 * Taken from SM64CoopDX, where the lowest global id in an area is the fallback owner. It beats
 * proximity for this job on every count that matters — every peer computes the same answer from
 * state it already has, there is no negotiation, no transfer packet, and no window where two
 * peers both think they own something. Ownership moves by itself when players come and go.
 *
 * The host owning everything, which is what this replaced, meant enemies were only corrected for
 * players in the host's map — under free-roam that is the common case, not the edge case. */
static u32 owns_enemies_here(bc_incoming *inc, u32 map_id) {
    u32 me = inc->local_player_id;
    u32 lowest = me;
    for (u32 i = 0; i < inc->remote_count && i < BCNET_MAX_PLAYERS; i++) {
        bc_remote_player *r = &inc->remotes[i];
        if (r->state.map_id == map_id && r->player_id < lowest) {
            lowest = r->player_id;
        }
    }
    return lowest == me;
}

/* Last frame's answer, so code that is not enemy_sync can ask.
 *
 * The spawn patch needs it, and it fires from deep inside game code at whatever point in the frame
 * an enemy happens to throw something — far from anywhere `inc` is in scope. Caching the frame's
 * answer in a file-scope word is the same shape as bc_map_id (world_internal.h), and for the same
 * reason. Defaults to owning nothing, so nothing is ever suppressed before the first sync. */
static u32 s_owns_here = 0;

u32 enemy_owns_objects_here(void) {
    return s_owns_here;
}

/* Which markers we treat as enemies.
 *
 * An allowlist, not a heuristic. There are 274 marker types covering enemies, scenery, doors,
 * signs and collectibles, and no field reliably separates them. Being wrong here means replicating
 * or killing something that is not an enemy, which surfaces far from the cause — so this starts
 * narrow and widens as each type is observed working.
 *
 * These cover Spiral Mountain and Mumbo's Mountain, which is where testing actually happens.
 */
static const u16 k_enemy_markers[] = {
    /* Spiral Mountain — Bottles' tutorial targets, the only enemies a fresh save meets. */
    MARKER_128_COLLYWOBBLE_THE_CAULIFLOWER_A,
    MARKER_129_BAWL_THE_ONION_A,
    MARKER_12A_TOPPER_THE_CARROT_A,
    MARKER_1E6_TOPPER_THE_CARROT_B,
    MARKER_1E7_BAWL_THE_ONION_B,
    MARKER_1E8_COLLYWOBBLE_THE_CAULIFLOWER_B,
    MARKER_135_QUARRIE,

    /* Mumbo's Mountain */
    MARKER_5_GRUBLIN,
    MARKER_1E2_GRUBLIN_HOOD,
    MARKER_A_CHIMPY,
    MARKER_50_BEEHIVE,

    /* Treasure Trove Cove */
    MARKER_13_SNIPPET,
    MARKER_16B_SNIPPET_UPSIDEDOWN,
    MARKER_DD_BLACK_SNIPPET,
    MARKER_DE_BLACK_SNIPPET_UPSIDEDOWN,
    MARKER_15_CLAM,
    MARKER_14_SNACKER,
    MARKER_21A_SEAMAN_GRUBLIN,

    /* Bubblegloop Swamp */
    MARKER_C1_FLIBBIT_RED,
    MARKER_C5_FLIBBIT_YELLOW,
    MARKER_C7_YUMBLIE,
    MARKER_FC_CROCTUS,

    /* Freezeezy Peak */
    MARKER_200_TWINKLY_BLUE,
    MARKER_201_TWINKLY_GREEN,
    MARKER_202_TWINKLY_ORANGE,
    MARKER_203_TWINKLY_RED,
    MARKER_205_TWINKLY_MUNCHER,
    MARKER_B1_SIR_SLUSH,

    /* Gobi's Valley */
    MARKER_AA_HISTUP,
    MARKER_C0_TRUNKER,

    /* Mad Monster Mansion and elsewhere */
    MARKER_99_TEEHEE,
    MARKER_296_TEEHEE_PURPLE,
    MARKER_127_BAT,

    /* Rusty Bucket Bay */
    MARKER_1B2_CLUCKER_A,
    MARKER_1D0_CLUCKER_B,
    MARKER_2E_GRIMLET,
    MARKER_1B7_BOOM_BOX,

    /* Mad Monster Mansion */
    MARKER_48_NAPPER,
    MARKER_254_PORTRAIT_CHOMPA_A,

    /* Click Clock Wood */
    MARKER_1AE_ZUBBA,
    MARKER_1B5_CATERPILLAR,
    MARKER_1F9_SNARE_BEAR,

    /* Gruntilda's Lair */
    MARKER_1EA_GRUNTLING_RED,
    MARKER_1F1_GRUNTLING_BLACK,
    MARKER_295_GRUNTLING_BLUE,

    /* Treasure Trove Cove lockups, and hazards that move under their own power — a sawblade or a
     * whipcrack in a different place on each screen is as unfair as an enemy in the wrong spot. */
    MARKER_A4_LOCKUP_SLOW,
    MARKER_F6_LOCKUP_MEDIUM,
    MARKER_F7_LOCKUP_FAST,
    MARKER_28_CLANKER_SAWBLADE,
    MARKER_1A7_CLANKER_WHIPCRACK,
    MARKER_1C5_WHIPCRACK,

    /* Assorted, appearing across several worlds */
    MARKER_4_TERMITE,
    MARKER_C9_FLOTSAM,
    MARKER_65_SHRAPNEL,
    MARKER_6A_GLOOP,
    MARKER_250_ICECUBE_A,
    MARKER_25F_ICECUBE_B,

    /* Bosses (Phase 8).
     *
     * The plan expected each of these to be bespoke work, on the assumption that one peer would
     * simulate them and the rest would be shown the result. That is not the architecture we
     * ended up with: every peer runs the fight, damage is pooled through the shared collision
     * dispatcher, and death is announced. A boss is therefore mostly just a tougher enemy, and
     * position correction plus pooled damage covers it.
     *
     * What is NOT covered is each boss's arena state machine — the cutscenes, camera takeovers
     * and phase transitions. Those run independently on each machine. In practice they are driven
     * by the same flags and the same damage, so they stay broadly in step; where they do not, the
     * symptom is a cutscene playing at slightly different moments, not a fight that cannot be won.
     *
     * Grunty is included, but position and damage alone would not be enough for her: the final
     * fight is a scripted six-phase sequence, and boss.c replicates the phase machine itself. */
    MARKER_7_CONGA,
    MARKER_A5_NIPPER,
    MARKER_16C_NIPPER,
    MARKER_C8_MR_VILE,
    MARKER_BC_GOBI_1,
    MARKER_BF_GOBI_2,
    MARKER_C3_GOBI_3,
    MARKER_1A1_BOSS_BOOM_BOX_LARGEST,
    MARKER_1A2_BOSS_BOOM_BOX_LARGE,
    MARKER_1A3_BOSS_BOOM_BOX_MEDIUM,
    MARKER_1A4_BOSS_BOOM_BOX_SMALL,
    MARKER_49_MOTZHAND,
    MARKER_20B_WOZZA,
    /* The final fight. Grunty and her Jinjos sync like any other enemy for position and damage;
     * their phase transitions ride the boss channel instead (boss.c). */
    MARKER_25E_GRUNTILDA_FINAL_BOSS,
    MARKER_27B_BOSS_JINJO_ORANGE,
    MARKER_27C_BOSS_JINJO_GREEN,
    MARKER_27D_BOSS_JINJO_PINK,
    MARKER_27E_BOSS_JINJO_YELLOW,
    MARKER_285_JINJONATOR,

    /*
     * Projectiles and other actors spawned during play are absent for a different reason: their
     * spawn order diverges between machines, so they have no shared identity (docs §3). They need
     * owner-assigned ids in a spawn event. */
};

#define ENEMY_MARKER_COUNT (sizeof(k_enemy_markers) / sizeof(k_enemy_markers[0]))

/* How far apart the owner's copy and ours may drift before we snap to theirs.
 *
 * Not a lerp toward the owner every frame: both machines are running the same AI, so small
 * differences are the two simulations tracking each other and correcting them constantly would
 * fight the local AI and read as jitter. Only a real divergence — an enemy chasing a different
 * player, or one that has been knocked somewhere — is worth overriding. */
#define ENEMY_SNAP_DISTANCE 220.0f

/* Enemies we know are dead, so a local copy that has not caught up yet can be removed on sight.
 * Keyed by net id, which is per-map, so this is cleared on a map change. */
#define ENEMY_MAX_TRACKED 64u

static u32 s_dead_ids[ENEMY_MAX_TRACKED];
static u32 s_dead_count = 0;

/* Enemies that were alive here last frame. Anything that drops out of this set has died locally —
 * which is how a death is detected without needing to know each enemy's death path. */
static u32 s_live_ids[ENEMY_MAX_TRACKED];
static u32 s_live_count = 0;

static u32 s_tracked_map = 0xFFFFFFFFu;

/* Drives the tier phasing. Its own counter rather than the frame number from main.c, so it only
 * advances when enemies are actually being synced — a session that turns enemy_sync off and back
 * on resumes from where it stopped instead of jumping an arbitrary distance through the phase. */
static u32 s_tick = 0;

/* One owned enemy considered for publishing this frame. The actor index is safe to hold here
 * because it is used within the same call: nothing spawns an actor between the gather and the
 * publish, and it is actor_new reallocating the array that makes a held Actor* the §13 bug. */
typedef struct {
    u32 net_id;
    s32 actor_index;
    f32 dist2;
} EnemyCandidate;

/* Projectiles.
 *
 * The owner publishes each one it has, carrying the actor type so a receiver can spawn something
 * it has never seen. Receivers throw away whatever their own copy of the enemy threw and adopt the
 * owner's instead — that is the only way the same shot exists, in the same place, for everybody.
 *
 * A projectile that stops appearing in the frame has hit something or expired, and is removed.
 */
static u32 marker_is_enemy(ActorMarker *marker) {
    if (marker == NULL) {
        return 0;
    }
    for (u32 i = 0; i < ENEMY_MARKER_COUNT; i++) {
        if ((u32)marker->id == (u32)k_enemy_markers[i]) {
            return 1;
        }
    }
    return 0;
}

static u32 id_in(const u32 *list, u32 count, u32 id) {
    for (u32 i = 0; i < count; i++) {
        if (list[i] == id) {
            return 1;
        }
    }
    return 0;
}

static void id_add(u32 *list, u32 *count, u32 id) {
    if (*count < ENEMY_MAX_TRACKED && !id_in(list, *count, id)) {
        list[(*count)++] = id;
    }
}

void enemy_mark_dead(u32 net_id) {
    id_add(s_dead_ids, &s_dead_count, net_id);
}

/* Observe every hit on a synced enemy and tell everyone. Entry hook, so the arguments are
 * readable. Our own replays are skipped, or one hit would echo between peers indefinitely. */
RECOMP_HOOK("marker_callCollisionFunc")
void banjocoop_on_marker_collision(ActorMarker *this, ActorMarker *other,
                                  enum marker_collision_func_type_e type) {
    if (s_replaying_hit || this == NULL || suBaddieActorArray == NULL) {
        return;
    }
    /* Only the damage and death handlers matter; the passive one fires constantly. */
    if (type != MARKER_COLLISION_FUNC_0 && type != MARKER_COLLISION_FUNC_2_DIE) {
        return;
    }
    if (!marker_is_enemy(this)) {
        return;
    }
    bc_enemy_report_hit(bkrecomp_get_marker_spawn_index(this), (u32)type);
}

void enemy_apply_hit(u32 net_id, u32 collision_type) {
    if (suBaddieActorArray == NULL) {
        return;
    }
    for (s32 i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (actor->despawn_flag || !marker_is_enemy(actor->marker)) {
            continue;
        }
        if (bkrecomp_get_marker_spawn_index(actor->marker) != net_id) {
            continue;
        }
        /* Run the game's own damage path rather than reaching into the enemy's state. Whatever
         * "being hit" means for this enemy — losing health, being knocked back, dying — it means
         * the same thing here as it does for a local hit. */
        s_replaying_hit = 1;
        marker_callCollisionFunc(actor->marker, playerMarker,
                                 (enum marker_collision_func_type_e)collision_type);
        s_replaying_hit = 0;
        return;
    }
}

void enemy_reset(void) {
    s_dead_count = 0;
    s_live_count = 0;
    s_tracked_map = 0xFFFFFFFFu;
    s_tick = 0;
    projectile_reset();
}

/* Squared distance from a position to the nearest remote player standing in this map.
 *
 * Remote players only: our own screen is not what the object frame is for. When nobody else is
 * here every object culls, which is the correct answer — publishing a map's enemies to an empty
 * map is the largest single waste this change removes, and it is also the common case under
 * free-roam.
 *
 * Squared throughout, so no object ever costs a square root. */
static f32 nearest_remote_dist2(bc_incoming *inc, u32 map_id, const f32 *pos) {
    /* Larger than any squared distance BK can produce, so "nobody here" culls by falling through
     * the ordinary comparisons rather than by a special case at every call site. */
    f32 best = 1.0e30f;
    for (u32 i = 0; i < inc->remote_count && i < BCNET_MAX_PLAYERS; i++) {
        bc_remote_player *r = &inc->remotes[i];
        if (r->state.map_id != map_id) {
            continue;
        }
        f32 dx = r->state.pos[0] - pos[0];
        f32 dy = r->state.pos[1] - pos[1];
        f32 dz = r->state.pos[2] - pos[2];
        f32 d2 = (dx * dx) + (dy * dy) + (dz * dz);
        if (d2 < best) {
            best = d2;
        }
    }
    return best;
}

/* Insertion sort, nearest first. At most ENEMY_MAX_TRACKED entries once a frame, and nearly
 * sorted from one frame to the next because enemies move slowly relative to the sort — which is
 * the case insertion sort is best at and a general-purpose sort is worst at. */
static void sort_by_distance(EnemyCandidate *list, u32 count) {
    for (u32 i = 1; i < count; i++) {
        EnemyCandidate key = list[i];
        u32 j = i;
        while (j > 0 && list[j - 1].dist2 > key.dist2) {
            list[j] = list[j - 1];
            j--;
        }
        list[j] = key;
    }
}

void enemy_sync(bc_incoming *inc, bc_outgoing *out, u32 map_id) {
    out->objects.count = 0;
    out->objects.map_id = map_id;

    if (!inc->connected || suBaddieActorArray == NULL) {
        enemy_reset();
        return;
    }

    /* Net ids are per-map, so nothing learned about one map means anything in another. */
    if (map_id != s_tracked_map) {
        enemy_reset();
        s_tracked_map = map_id;
    }

    const u32 owner = owns_enemies_here(inc, map_id);
    s_owns_here = owner;
    s_tick++;

    /* An object frame describing another map is stale across a transition; applying it would drive
     * whatever is standing here to another map's coordinates. */
    bc_object_frame *frame =
        (!owner && inc->objects.count != 0 && inc->objects.map_id == map_id) ? &inc->objects : NULL;

    u32 seen[ENEMY_MAX_TRACKED];
    u32 seen_count = 0;

    /* Owned enemies worth considering, gathered before any are written to the frame. The publish
     * decision needs to compare them against each other — nearest first, within a budget — which
     * cannot be done while walking the actor array in whatever order it happens to be in. */
    EnemyCandidate candidates[ENEMY_MAX_TRACKED];
    u32 candidate_count = 0;
    u32 tier_counts[BC_TIER_COUNT] = {0, 0, 0, 0};

    for (s32 i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (actor->despawn_flag) {
            continue;
        }
        ActorMarker *marker = actor->marker;
        if (!marker_is_enemy(marker)) {
            continue;
        }

        u32 net_id = bkrecomp_get_marker_spawn_index(marker);
        id_add(seen, &seen_count, net_id);

        /* Somebody already killed this one. Our copy is behind; remove it. */
        if (id_in(s_dead_ids, s_dead_count, net_id)) {
            marker_despawn(marker);
            continue;
        }

        if (owner) {
            if (candidate_count < ENEMY_MAX_TRACKED) {
                EnemyCandidate *c = &candidates[candidate_count++];
                c->net_id = net_id;
                c->actor_index = i;
                c->dist2 = nearest_remote_dist2(inc, map_id, actor->position);
                tier_counts[bc_tier_of(c->dist2)]++;
            }
        } else if (frame != NULL) {
            /* Correct only on real divergence — see ENEMY_SNAP_DISTANCE. */
            for (u32 j = 0; j < frame->count && j < BCNET_MAX_OBJECTS; j++) {
                bc_object_state *obj = &frame->objects[j];
                if (obj->net_id != net_id) {
                    continue;
                }
                f32 dx = obj->pos[0] - actor->position[0];
                f32 dy = obj->pos[1] - actor->position[1];
                f32 dz = obj->pos[2] - actor->position[2];
                if ((dx * dx) + (dy * dy) + (dz * dz) >
                    (ENEMY_SNAP_DISTANCE * ENEMY_SNAP_DISTANCE)) {
                    actor->position[0] = obj->pos[0];
                    actor->position[1] = obj->pos[1];
                    actor->position[2] = obj->pos[2];
                    actor->yaw = obj->yaw;
                }
                break;
            }
        }
    }

    /* Publish, nearest first, at whatever rate each enemy's distance earns it.
     *
     * Position and animation only — health and AI state stay local, because every peer is running
     * the same AI and does not need to be told.
     *
     * Two things happen here that did not before. Order: the frame used to be filled in actor
     * array order, so which enemies made the budget was decided by where the game happened to put
     * them. Rate: an enemy nobody is near is corrected by nobody, so publishing it every frame
     * bought nothing. The budget is capped short of BCNET_MAX_OBJECTS so projectiles — which
     * cannot be throttled at all, see projectile.c — always have room. */
    if (owner) {
        sort_by_distance(candidates, candidate_count);

        const u32 budget = BCNET_MAX_OBJECTS - BC_PROJECTILE_RESERVE;
        for (u32 i = 0; i < candidate_count && out->objects.count < budget; i++) {
            EnemyCandidate *c = &candidates[i];
            if (!bc_tier_publishes(c->net_id, c->dist2, s_tick)) {
                continue;
            }
            Actor *actor = &suBaddieActorArray->data[c->actor_index];
            bc_object_state *obj = &out->objects.objects[out->objects.count++];
            obj->net_id = c->net_id;
            obj->flags = BC_OBJ_ACTIVE;
            /* Cleared explicitly. The staging buffer is reused frame to frame and only `count` is
             * reset, so a slot that carried a projectile last frame still holds its actor id — and
             * a non-zero actor id is precisely how a receiver decides an object is something it
             * must spawn (projectile.c). Leaving it would have receivers spawn phantom oranges
             * wherever an enemy happened to land in a slot a projectile used to occupy. */
            obj->actor_id = 0;
            obj->pos[0] = actor->position[0];
            obj->pos[1] = actor->position[1];
            obj->pos[2] = actor->position[2];
            obj->yaw = actor->yaw;
            obj->anim_id = 0;
            obj->anim_frame = 0.0f;
            if (actor->anctrl != NULL) {
                obj->anim_id = (u32)anctrl_getIndex(actor->anctrl);
                obj->anim_frame = anctrl_getAnimTimer(actor->anctrl);
            }
        }
    }

    /* Anything alive here last frame and gone now died locally — whoever landed the killing blow
     * sees it first. Telling everyone else is what lets players watch each other's kills and gang
     * up on the same enemy. Generic on purpose: it needs no knowledge of any enemy's death path. */
    for (u32 i = 0; i < s_live_count; i++) {
        u32 id = s_live_ids[i];
        if (id_in(seen, seen_count, id) || id_in(s_dead_ids, s_dead_count, id)) {
            continue;
        }
        enemy_mark_dead(id);
        bc_enemy_report_death(id);
    }

    for (u32 i = 0; i < seen_count; i++) {
        s_live_ids[i] = seen[i];
    }
    s_live_count = seen_count;

    u32 enemies_sent = out->objects.count;
    projectile_sync(inc, out, owner, frame);

    /* The tuning instrument for the distance constants in tier.h.
     *
     * They are guesses, and the only way to replace them with measured values is to watch which
     * band a map's enemies actually fall into while two people play it. Printed on the same once-a-
     * second cadence as the heartbeat in main.c, and only by the peer that is publishing —
     * everybody else has nothing to say here.
     *
     * Reading it: `culled` climbing while enemies are still on screen means BC_TIER_FAR is too
     * small. Nothing ever culled in a full map means all three are too large. `sent` well under
     * the budget is the change working. */
    if (owner && (s_tick % 60u) == 0) {
        recomp_printf(
            "[banjocoop] objtier near=%u mid=%u far=%u culled=%u sent=%u/%u (proj=%u)\n",
            tier_counts[BC_TIER_INDEX_NEAR], tier_counts[BC_TIER_INDEX_MID],
            tier_counts[BC_TIER_INDEX_FAR], tier_counts[BC_TIER_INDEX_CULLED], enemies_sent,
            BCNET_MAX_OBJECTS - BC_PROJECTILE_RESERVE, out->objects.count - enemies_sent);
    }
}
