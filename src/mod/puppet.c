/* Remote player puppets.
 *
 * Banjo is not an actor in BK — the player is special-cased globals plus dedicated update and
 * collision code — so there is no second Banjo to spawn. A puppet is instead a purpose-built
 * actor that renders Banjo's model and is driven *entirely* from network state. It deliberately
 * has no movement code, no physics, and no collision authority: everything it does comes from
 * bc_incoming.
 *
 * Registration works because the engine's spawnable-actor table is built at runtime by repeated
 * calls to spawnableActorList_add() and searched linearly by actorId
 * (bk-decomp src/core2/code_7AF80.c:1223,1283). A mod can therefore add its own entry without
 * patching any table, as long as it does so after spawnableActorList_new() has reset the list —
 * which happens on every map load.
 */

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "prop.h"
#include "core2/anctrl.h"

#include "banjocoop/protocol.h"
#include "puppet.h"
#include "carry.h"

/* -> src/core2/code_5C870.c */
extern s32 getGameMode(void);
/* assetcache_release is in the syms but not functions.h */
extern void assetcache_release(void *ptr);

/* Diagnostic switch. actor_draw resolves the model via assetcache_get(marker->modelId) and
 * dereferences the result with NO null check (code_9E370.c:204-205), so an unloadable model is a
 * segfault on the first frame the puppet is drawn. Verify the asset is retrievable before we ever
 * spawn something that will be drawn. */
#define PUPPET_VERBOSE 1

/* Actor and marker ids.
 *
 * These are NOT free-choice values, and picking them badly corrupts the heap silently. Two
 * separate fixed-size arrays are indexed by them, with no bounds checking:
 *
 *   modelCache[actor->modelCacheIndex]  where modelCacheIndex = actorId (code_9E370.c:809)
 *       allocated with exactly AssetCacheSize == 0x3D5 entries (code_A5BC0.c:1390).
 *       marker_loadModelBin WRITES modelInfo->modelPtr through this pointer, so an out-of-range
 *       actor id corrupts whatever follows the array on the heap. The damage surfaces later and
 *       elsewhere — originally as a crash in the *player's* baMarker_update, nowhere near here.
 *
 *   sMarkerToBitfield[marker->id]  (code_7AF80.c:2031)
 *       only 0x1BC entries, read unguarded. A garbage value that survives the != 0xFF check
 *       leads to codeA5BC0_getActorPosition(marker->propPtr, ...), and a dynamically spawned
 *       marker has propPtr == NULL.
 *
 * So: actor id must be < 0x3D5 and marker id < 0x1BC, both unused by vanilla. Note the 10-bit
 * width of the modelCacheIndex/id bitfields (max 0x3FF) is NOT the relevant bound — the array
 * sizes are.
 *
 * Highest vanilla actor id is 0x3CB, so 0x3CC..0x3D4 is the only free actor range.
 */
/* One actor id per transformation, because a puppet's model is fixed by the ActorInfo it was
 * spawned from — changing form means respawning as a different actor type.
 *
 * 0x3CC..0x3D4 is the whole free range (see above), which is nine ids for seven forms. That is
 * the constraint that decides the design: there is no room for a spare per form, so the table
 * below is exactly as long as the transformation enum and no longer.
 */
#define PUPPET_ACTOR_ID_BASE 0x3CC
#define PUPPET_MARKER_ID 0x1ba

/* Indexed by enum transformation_e. Entry 0 is unused — the enum starts at 1. */
#define PUPPET_FORM_COUNT 8u

static const u16 k_form_models[PUPPET_FORM_COUNT] = {
    ASSET_34E_MODEL_BANJOKAZOOIE_HIGH_POLY, /* 0 — unused, kept so the enum indexes directly */
    ASSET_34E_MODEL_BANJOKAZOOIE_HIGH_POLY, /* TRANSFORM_1_BANJO */
    ASSET_34F_MODEL_BANJO_TERMITE,          /* TRANSFORM_2_TERMITE */
    ASSET_36F_MODEL_BANJO_PUMPKIN,          /* TRANSFORM_3_PUMPKIN */
    ASSET_359_MODEL_BANJO_WALRUS,           /* TRANSFORM_4_WALRUS */
    ASSET_374_MODEL_BANJO_CROC,             /* TRANSFORM_5_CROC */
    ASSET_362_MODEL_BANJO_BEE,              /* TRANSFORM_6_BEE */
    ASSET_356_MODEL_BANJO_WISHYWASHY,       /* TRANSFORM_7_WISHWASHY */
};

/* How fast a puppet converges on its last received position, per frame.
 *
 * State arrives at 30 Hz while the game runs at 60, so raw assignment would visibly stutter.
 * This is exponential smoothing rather than the snapshot interpolation the plan calls for
 * (buffer 2-3 packets, render slightly in the past). Smoothing is a fraction of the complexity
 * and looks correct at LAN latency; it degrades into "lags slightly behind" rather than
 * "jitters" as latency rises, because nothing is ever extrapolated. Revisit if it looks soft
 * under the 150 ms test. */
#define PUPPET_LERP 0.35f

/* Distance beyond which we snap instead of sliding. Without this, a warp, a map change, or a
 * respawn produces a puppet gliding across the level in a straight line through the geometry. */
#define PUPPET_SNAP_DISTANCE 300.0f

/* Holds an ActorMarker, never an Actor*.
 *
 * actor_new grows the actor array with realloc (code_9E370.c:773), so any Actor* we cached is
 * invalidated the moment some other actor spawns. That produced both halves of a nasty bug: writes
 * through the stale pointer silently went nowhere (the puppet appeared frozen) and reading
 * actor->anctrl out of freed memory eventually segfaulted inside anctrl_getIndex.
 *
 * ActorMarker is the engine's stable handle for exactly this reason — the marker pool is a fixed
 * 0xE0-entry allocation (code_A5BC0.c:2223) that is never reallocated. Resolve the Actor fresh
 * every frame via marker_getActor(). */
typedef struct {
    ActorMarker *marker;
    u32 active;
    u32 player_id;
    u32 map_id;
    /* Which form this puppet was spawned as. A puppet's model is fixed by the ActorInfo it came
     * from, so changing form means respawning — there is no way to swap a model in place. */
    u32 form;
} Puppet;

static Puppet s_puppets[BCNET_MAX_PLAYERS];

/* Set once our actor type is in the spawnable list for the current map. */
static u32 s_registered = 0;

/* The engine calls this every frame for each puppet. It must do nothing: all puppet state comes
 * from the network in puppet_sync(). Registering a no-op is simpler and safer than leaving the
 * pointer NULL, which the engine would have to be trusted to check. */
static void puppet_update_noop(Actor *this) {
    (void)this;
}

/* Animation table. `startAnimation` in ActorInfo is an INDEX INTO THIS ARRAY, not an asset id —
 * actor_new does `&actor->unk18[actor->state]` (bk-decomp src/core2/code_9E370.c:893).
 *
 * Entry 0 must stay {0, 0.0f}: actor_new only creates actor->anctrl when the selected entry's
 * index is non-zero, so a puppet starting at state 0 would have no AnimCtrl and could never be
 * animated. We start at state 1 for that reason. Beyond providing a valid AnimCtrl this table is
 * not really used — puppet_apply_state overwrites the animation index every frame from the
 * network. */
static ActorAnimationInfo s_puppet_animations[] = {
    {0, 0.0f},
    {ASSET_6F_ANIM_BSSTAND_IDLE, 1.0f},
};

#define PUPPET_START_ANIM_STATE 1

/* Filled in at registration, one per form. They differ only in actorId and modelId; everything
 * else is the same puppet. */
static ActorInfo s_puppet_info[PUPPET_FORM_COUNT];

static const ActorInfo s_puppet_template = {
    PUPPET_MARKER_ID,                       /* markerId */
    PUPPET_ACTOR_ID_BASE,                   /* actorId — overwritten per form */
    ASSET_34E_MODEL_BANJOKAZOOIE_HIGH_POLY, /* modelId — overwritten per form */
    PUPPET_START_ANIM_STATE,                /* startAnimation (index into animations) */
    s_puppet_animations,                    /* animations */
    puppet_update_noop,                     /* update_func */
    puppet_update_noop,                     /* update2_func — a no-op rather than NULL. The
                                             * engine calls actorUpdate2Func without a NULL
                                             * check (code_9E370.c:546); it is currently gated
                                             * by marker->unk2C_2, which marker_init clears, but
                                             * relying on that bitfield staying 0 is exactly the
                                             * assumption that made draw_func crash. */
    actor_draw,                             /* draw_func — must not be NULL. actor_new passes it
                                             * straight to marker_init as the marker's drawFunc
                                             * and the renderer calls it unconditionally, so NULL
                                             * segfaults the moment the puppet is drawn. */
    0,                                      /* unk18 */
    0x7FFF,                                 /* draw_distance — never cull; a vanishing co-op
                                             * partner reads as a bug, not a distance fade */
    1.0f,                                   /* shadow_scale */
    0                                       /* unk20 */
};

/* spawnableActorList_new() empties the table on every map load, so our entry has to be re-added
 * each time. A return hook rather than a patch: the base recomp is free to patch this too. */
RECOMP_HOOK_RETURN("spawnableActorList_new") void banjocoop_register_puppet_actor(void) {
    for (u32 form = 0; form < PUPPET_FORM_COUNT; form++) {
        s_puppet_info[form] = s_puppet_template;
        s_puppet_info[form].actorId = PUPPET_ACTOR_ID_BASE + form;
        s_puppet_info[form].modelId = k_form_models[form];
        spawnableActorList_add(&s_puppet_info[form], actor_new, 0);
    }
    s_registered = 1;

    /* The old actors died with the previous map; drop our stale pointers so we do not write
     * through them. */
    for (u32 i = 0; i < BCNET_MAX_PLAYERS; i++) {
        s_puppets[i].marker = NULL;
        s_puppets[i].active = 0;
    }
}

static void puppet_despawn(Puppet *p) {
    if (p->marker != NULL) {
        marker_despawn(p->marker);
    }
    p->marker = NULL;
    p->active = 0;
}

/* 0 = unchecked, 1 = model is retrievable, 2 = model is NOT retrievable (do not spawn). */
static u32 s_model_checked = 0;

static u32 puppet_model_ok(void) {
    void *model_bin;

    if (s_model_checked != 0) {
        return s_model_checked == 1;
    }

    model_bin = assetcache_get(ASSET_34E_MODEL_BANJOKAZOOIE_HIGH_POLY);
    if (model_bin == NULL) {
        s_model_checked = 2;
        recomp_printf("[banjocoop] model 0x34E not in asset cache - puppets disabled\n");
        return 0;
    }
    assetcache_release(model_bin);
    s_model_checked = 1;
    recomp_printf("[banjocoop] model 0x34E ok (flag=%d)\n",
                  asset_getFlag(ASSET_34E_MODEL_BANJOKAZOOIE_HIGH_POLY));
    return 1;
}

/* Which form to render a remote player as.
 *
 * Falls back to plain Banjo when the form's model is not in the asset cache — which happens
 * whenever the other player is transformed in a world whose assets we do not have loaded. The
 * plan calls for pinning every form's assets at startup so this never happens, but the asset
 * cache API that needs does not exist in the 1.0.1 runtime (docs §16), and rendering the wrong
 * character is much better than the segfault an unloadable model produces: actor_draw
 * dereferences assetcache_get's result with no null check. */
static u32 puppet_form_for(bc_remote_player *remote) {
    u32 form = remote->state.transform;
    if (form == 0 || form >= PUPPET_FORM_COUNT) {
        return TRANSFORM_1_BANJO;
    }
    void *model = assetcache_get((enum asset_e)k_form_models[form]);
    if (model == NULL) {
        return TRANSFORM_1_BANJO;
    }
    assetcache_release(model);
    return form;
}

static void puppet_spawn(Puppet *p, bc_remote_player *remote) {
    f32 pos[3];

    if (!puppet_model_ok()) {
        return;
    }
    u32 form = puppet_form_for(remote);
    pos[0] = remote->state.pos[0];
    pos[1] = remote->state.pos[1];
    pos[2] = remote->state.pos[2];

    {
        /* Take the marker immediately and discard the Actor* — it is only valid until the next
         * actor spawn reallocs the array. */
        Actor *spawned = actor_spawnWithYaw_f32((s32)(PUPPET_ACTOR_ID_BASE + form), pos,
                                                (s32)remote->state.yaw);
        if (spawned == NULL) {
            recomp_printf("[banjocoop] failed to spawn puppet for p%u\n", remote->player_id);
            return;
        }
        p->marker = spawned->marker;
        if (p->marker == NULL) {
            recomp_printf("[banjocoop] puppet for p%u spawned with no marker\n", remote->player_id);
            return;
        }
    }

    p->active = 1;
    p->player_id = remote->player_id;
    p->map_id = remote->state.map_id;
    p->form = form;
#if PUPPET_VERBOSE
    recomp_printf("[banjocoop] spawned puppet p%u marker=%p pos=(%.0f,%.0f,%.0f)\n",
                  remote->player_id, p->marker,
                  remote->state.pos[0], remote->state.pos[1], remote->state.pos[2]);
#endif
}

static void puppet_apply_state(Puppet *p, bc_remote_player *remote) {
    Actor *actor;

    if (!p->active || p->marker == NULL) {
        return;
    }

    /* Resolve fresh every frame — see the comment on Puppet. */
    actor = marker_getActor(p->marker);
    if (actor == NULL) {
        p->marker = NULL;
        p->active = 0;
        return;
    }

    f32 dx = remote->state.pos[0] - actor->position[0];
    f32 dy = remote->state.pos[1] - actor->position[1];
    f32 dz = remote->state.pos[2] - actor->position[2];
    f32 dist_sq = (dx * dx) + (dy * dy) + (dz * dz);

    if (dist_sq > (PUPPET_SNAP_DISTANCE * PUPPET_SNAP_DISTANCE)) {
        actor->position[0] = remote->state.pos[0];
        actor->position[1] = remote->state.pos[1];
        actor->position[2] = remote->state.pos[2];
    } else {
        actor->position[0] += dx * PUPPET_LERP;
        actor->position[1] += dy * PUPPET_LERP;
        actor->position[2] += dz * PUPPET_LERP;
    }

    actor->yaw = remote->state.yaw;

    /* Drive the animation from the sender's AnimCtrl. anim_id is an asset index and anim_frame is
     * the playback timer, so setting both reproduces the remote player's pose exactly rather than
     * guessing an animation from velocity. */
    if (actor->anctrl != NULL && remote->state.anim_id != 0) {
        if ((u32)anctrl_getIndex(actor->anctrl) != remote->state.anim_id) {
            anctrl_setIndex(actor->anctrl, (enum asset_e)remote->state.anim_id);
        }
        anctrl_setAnimTimer(actor->anctrl, remote->state.anim_frame);
    }
}

/* Whether it is safe to have puppets in the world right now.
 *
 * Spawning an actor requires the actor system and cube list to be set up for gameplay. During the
 * intro, title screen, file select and map transitions they are not — and crucially both peers
 * report the same map_id in those states, so the same-map check alone happily tries to spawn a
 * puppet into a world that does not exist yet. That is the likely cause of the join-time crash:
 * it fires on both sides at once, exactly when the second player connects.
 *
 * GAME_MODE_3_NORMAL is ordinary gameplay; 4 is paused, which is still a live world. Everything
 * else (attract demo, bonus games, cutscene modes) either has no world or one we must not touch.
 *
 * s_registered gates on our actor type actually being in the spawnable list: the list is emptied
 * on every map load and refilled by our return hook, and spawning before that returns NULL. */

static u32 puppets_allowed(void) {
    s32 mode;

    if (!s_registered) {
        return 0;
    }

    mode = getGameMode();
    return (mode == GAME_MODE_3_NORMAL || mode == GAME_MODE_4_PAUSED);
}

void puppet_sync(bc_incoming *inc, u32 local_map_id) {
    u32 seen[BCNET_MAX_PLAYERS];

    if (!puppets_allowed()) {
        /* Drop any puppets we are holding rather than leaving dangling actors across a
         * transition — the actors themselves are freed with the map. */
        for (u32 i = 0; i < BCNET_MAX_PLAYERS; i++) {
            s_puppets[i].marker = NULL;
            s_puppets[i].active = 0;
        }
        return;
    }

    for (u32 i = 0; i < BCNET_MAX_PLAYERS; i++) {
        seen[i] = 0;
    }

    for (u32 i = 0; i < inc->remote_count && i < BCNET_MAX_PLAYERS; i++) {
        bc_remote_player *remote = &inc->remotes[i];
        if (remote->player_id >= BCNET_MAX_PLAYERS) {
            continue;
        }

        Puppet *p = &s_puppets[remote->player_id];

        /* Only show players in the same map. Free-roam (Phase 4) means remote players are
         * routinely somewhere else entirely, and rendering them here would put a puppet at
         * another world's coordinates. */
        if (remote->state.map_id != local_map_id) {
            if (p->active) {
                puppet_despawn(p);
            }
            continue;
        }

        seen[remote->player_id] = 1;

        /* A player who has transformed needs a different model, and a model cannot be swapped in
         * place — so the puppet is replaced. Only on an actual change, or a transformed player
         * would be respawned every frame. */
        if (p->active && p->form != puppet_form_for(remote)) {
            puppet_despawn(p);
        }
        if (!p->active) {
            puppet_spawn(p, remote);
        }
        /* While we are carrying them the game owns this puppet's position — applying network
         * state on top would fight it, and the network state is itself derived from where we are
         * carrying them to. */
        if (!carry_is_carrying(remote->player_id)) {
            puppet_apply_state(p, remote);
        }
    }

    /* Anyone we did not hear about this frame has left, or moved to another map. */
    for (u32 i = 0; i < BCNET_MAX_PLAYERS; i++) {
        if (s_puppets[i].active && !seen[i]) {
            puppet_despawn(&s_puppets[i]);
        }
    }
}

/* The marker of a player's puppet, for anything that wants to hand it to the game — carrying
 * uses this. NULL when they have no puppet here. */
ActorMarker *puppet_marker_for(u32 player_id) {
    if (player_id >= BCNET_MAX_PLAYERS || !s_puppets[player_id].active) {
        return NULL;
    }
    return s_puppets[player_id].marker;
}

void puppet_clear_all(void) {
    for (u32 i = 0; i < BCNET_MAX_PLAYERS; i++) {
        if (s_puppets[i].active) {
            puppet_despawn(&s_puppets[i]);
        }
    }
}
