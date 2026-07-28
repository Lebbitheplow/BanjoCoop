/* Shared world state — the outgoing half of the Phase 3 replication layer.
 *
 * Hooks on the game's own state-change functions notice that the local player changed the world
 * and queue an event describing it. The incoming half — applying what the host accepted, and the
 * registry it is applied against — lives in world_apply.c.
 *
 * Every hook checks bc_applying first. Applying a remote change runs game code that looks exactly
 * like a local change, and without that check two peers would bounce a single flag write off each
 * other forever.
 */

#include "world.h"
#include "world_internal.h"
#include "enemy.h"
#include "progress.h"
#include "ui.h"
#include "boss.h"
#include "carry.h"
#include "modes.h"

u32 bc_map_id = 0;
u32 bc_level_id = 0;
u32 bc_online = 0;
u32 bc_is_host = 0;
u32 bc_local_player_id = 0;
u32 bc_applying = 0;

static bc_outgoing g_outgoing;
static u32 s_frame = 0;

/* Roster size last frame, so the host can spot someone arriving. */
static u32 s_last_remote_count = 0;

/* Desync detector. Hashes are compared over several consecutive samples before anything is
 * reported: peers hash at slightly different moments, so a single mismatch during a burst of
 * changes is expected and means nothing. */
#define WORLD_HASH_PERIOD 300u /* frames; ~5s at 60fps */

bc_outgoing *world_outgoing(void) {
    return &g_outgoing;
}

void bc_queue_event(u32 kind, u32 map_id, u32 level_id, u32 a, u32 b, u32 c) {
    if (!bc_online || bc_applying) {
        return;
    }
    if (g_outgoing.queue.count >= BCNET_EVENT_QUEUE) {
        /* A dropped world change is a permanent desync, so this is loud rather than silent. If it
         * ever fires in practice the queue is too small for a map-load flag burst. */
        g_outgoing.queue.dropped++;
        return;
    }
    bc_event *ev = &g_outgoing.queue.events[g_outgoing.queue.count++];
    ev->kind = kind;
    ev->map_id = map_id;
    ev->level_id = level_id;
    ev->a = a;
    ev->b = b;
    ev->c = c;
}

/* For hooks with no better source of truth than "wherever the player is this frame". */
static void queue_here(u32 kind, u32 a, u32 b) {
    bc_queue_event(kind, bc_map_id, bc_level_id, a, b, 0);
}

/* All entry hooks from here down. Only entry hooks may read arguments: the hook dispatcher calls
 * the hook with whatever recomp_context exists at the hook site, and at a function's return the
 * argument registers are long gone (librecomp/src/mod_hooks.cpp:28). */

/* ---- collectibles ---------------------------------------------------------------------------
 *
 * The note pickup. BanjoRecomp `RECOMP_PATCH`es this function, so it is not available to patch —
 * but hooks attach by symbol and apply to the patched version, which is all we need.
 *
 * BanjoRecomp's `main` branch also fires a `bkrecomp_note_collected_event` carrying a ready-made
 * note index, which would be tidier. It does not exist in the 1.0.1 release we run against, so we
 * derive the index ourselves — see the note-identity section of world_apply.c.
 */
RECOMP_HOOK("__baMarker_resolveMusicNoteCollision") void banjocoop_on_note_collected(Prop *prop) {
    if (bc_applying || prop == NULL) {
        return;
    }

    /* Enemy-dropped notes are actor props, and are deliberately NOT reported.
     *
     * They have no cross-machine identity — each peer's own enemy drops its own note. Relaying
     * the score therefore counted one dropped note once per player: whoever collected theirs
     * granted everyone else a note as well, on top of the one they were about to collect
     * themselves. Two players clearing the same enemies drifted steadily upward.
     *
     * Now that damage is pooled, an enemy dies on every machine at the same moment and drops on
     * every machine, so each player collecting their own copy arrives at the same total without
     * anything being sent. The failure mode flips from guaranteed over-counting to a possible
     * under-count if one player leaves theirs lying, which the desync detector will surface. */
    if (prop->is_actor) {
        return;
    }
    if (prop->is_3d) {
        return;
    }

    s32 index = bc_note_index_of(prop);
    if (index < 0) {
        return;
    }
    bc_note_mark(bc_map_id, (u32)index);
    queue_here(BC_EV_NOTE_STATIC, (u32)index, 0);
}

RECOMP_HOOK("jiggyscore_setCollected") void banjocoop_on_jiggy(s32 index, s32 value) {
    if (bc_applying || index <= 0 || (u32)index >= WORLD_MAX_JIGGIES) {
        return;
    }
    /* Only a change is worth sending. The game re-asserts jiggy state in a few places, and
     * without this the reliable channel would carry the same bit repeatedly. */
    if ((jiggyscore_isCollected((enum jiggy_e)index) != 0) == (value != 0)) {
        return;
    }
    queue_here(BC_EV_JIGGY, (u32)index, value != 0 ? 1u : 0u);
}

/* Empty honeycombs and Mumbo tokens keep their own score bitfields, exactly parallel to
 * jiggyscore. Hooking notes, jiggies and flags does not touch either of them — which is why a hex
 * piece collected in Spiral Mountain went nowhere. */

RECOMP_HOOK("honeycombscore_set") void banjocoop_on_honeycomb(s32 index, s32 value) {
    if (bc_applying || index <= 0 || (u32)index >= WORLD_MAX_HONEYCOMBS) {
        return;
    }
    if ((honeycombscore_get((enum honeycomb_e)index) != 0) == (value != 0)) {
        return;
    }
    queue_here(BC_EV_HONEYCOMB, (u32)index, value != 0 ? 1u : 0u);
}

RECOMP_HOOK("mumboscore_set") void banjocoop_on_mumbo_token(s32 index, s32 value) {
    if (bc_applying || index <= 0 || (u32)index >= WORLD_MAX_TOKENS) {
        return;
    }
    if ((mumboscore_get((enum mumbotoken_e)index) != 0) == (value != 0)) {
        return;
    }
    queue_here(BC_EV_MUMBO_TOKEN, (u32)index, value != 0 ? 1u : 0u);
}

/* A Jinjo was rescued.
 *
 * The marker id is the identity — one Jinjo of each colour per level — and the collision handler
 * is the only place that knows it happened. An entry hook, so the marker argument is readable.
 *
 * Note the bit arithmetic the game uses: `1 << (marker->id + 6)` with ids 0x5A..0x5E, which lands
 * on bits 0..4 only because MIPS masks shift counts to five bits. Everything downstream mirrors
 * that quirk rather than trying to correct it. */
RECOMP_HOOK("__chJinjo_802CDBA8") void banjocoop_on_jinjo(ActorMarker *this, ActorMarker *other) {
    if (bc_applying || this == NULL) {
        return;
    }
    queue_here(BC_EV_JINJO, (u32)this->id, 0);
}

/* Entering a level calls this, which zeroes the level's note counter before note_saving restores
 * whatever the save file holds. Notes collected by other players this session are deliberately
 * not written to that save file, so without correcting afterwards the shared total collapses to
 * whatever the local player had personally saved.
 *
 * Entry hook, because the level argument is only readable at entry — but the correction itself is
 * deferred to the frame hook. Doing it here would just be undone by the reset we are standing in
 * front of. */
RECOMP_HOOK("itemscore_levelReset") void banjocoop_on_level_reset(s32 level) {
    bc_request_note_recount((u32)level);
}

/* ---- flags -----------------------------------------------------------------------------------
 *
 * Split by category exactly as the plan's §1.8 requires, because the three categories have
 * genuinely different scopes:
 *
 *   file-progress  permanent, save-backed, world-wide       -> everyone
 *   level-specific one level's state                        -> peers in that level
 *   map-specific   one map's switches, doors, opened gates  -> peers in that map
 *
 * Volatile flags are deliberately NOT replicated. Despite the plan listing them, reading through
 * their uses they are mostly per-player session state (in-minigame, dialog shown, camera mode)
 * rather than world state, and replicating those would actively break the receiving player. The
 * handful that are world-scoped need naming individually; that allowlist is future work.
 *
 * Every hook compares against the current value first. Game code re-sets flags to the value they
 * already hold constantly, and without this filter the reliable channel would carry a steady
 * stream of no-ops.
 */

RECOMP_HOOK("fileProgressFlag_set") void banjocoop_on_fileprog_flag(s32 index, s32 value) {
    if (bc_applying) {
        return;
    }
    if ((fileProgressFlag_get((enum file_progress_e)index) != 0) == (value != 0)) {
        return;
    }
    queue_here(BC_EV_FLAG_FILEPROG, (u32)index, value != 0 ? 1u : 0u);
}

RECOMP_HOOK("levelSpecificFlags_set") void banjocoop_on_level_flag(s32 index, s32 value) {
    if (bc_applying) {
        return;
    }
    if ((levelSpecificFlags_get(index) != 0) == (value != 0)) {
        return;
    }
    queue_here(BC_EV_FLAG_LEVEL, (u32)index, value != 0 ? 1u : 0u);
}

RECOMP_HOOK("mapSpecificFlags_set") void banjocoop_on_map_flag(s32 index, s32 value) {
    if (bc_applying) {
        return;
    }
    if ((mapSpecificFlags_get(index) != 0) == (value != 0)) {
        return;
    }
    queue_here(BC_EV_FLAG_MAP, (u32)index, value != 0 ? 1u : 0u);
}

/* The multi-bit setters. `mapSpecificFlags_setN` loops over `mapSpecificFlags_set` and so is
 * already covered by the hook above, but these two write the run of bits directly and would
 * otherwise be invisible — which is how the jiggy podiums, the sandcastle switches and GV's
 * pyramid state would silently fail to replicate. */

RECOMP_HOOK("fileProgressFlag_setN")
void banjocoop_on_fileprog_flag_n(s32 start, s32 value, s32 length) {
    if (bc_applying || length <= 0 || length > 32) {
        return;
    }
    if (fileProgressFlag_getN((enum file_progress_e)start, length) == value) {
        return;
    }
    bc_queue_event(BC_EV_FLAG_FILEPROG_N, bc_map_id, bc_level_id, (u32)start, (u32)value,
                   (u32)length);
}

RECOMP_HOOK("levelSpecificFlags_setN")
void banjocoop_on_level_flag_n(s32 start, s32 value, s32 length) {
    if (bc_applying || length <= 0 || length > 32) {
        return;
    }
    if (levelSpecificFlags_getN(start, length) == value) {
        return;
    }
    bc_queue_event(BC_EV_FLAG_LEVEL_N, bc_map_id, bc_level_id, (u32)start, (u32)value,
                   (u32)length);
}

/* A jiggy somebody else collected is still sitting in our world: chjiggy_update only consults
 * jiggyscore during its INIT state, which has long since run. Rather than despawn from inside
 * this hook — the hooked function is about to run and would be operating on a freed actor — the
 * marker is queued for the frame hook to deal with. */
RECOMP_HOOK("chjiggy_update") void banjocoop_jiggy_update(Actor *this) {
    if (this == NULL || this->marker == NULL) {
        return;
    }
    u32 id = (u32)chjiggy_getJiggyId(this);
    if (id == 0 || id >= WORLD_MAX_JIGGIES || !bc_jiggy_is_remote(id)) {
        return;
    }
    bc_defer_despawn(this->marker);
}

/* Honeycombs and tokens need the same treatment for the same reason: each checks its score only
 * once, on its first frame, so one collected remotely afterwards just stands there. Leaving it
 * would also let us collect it a second time and double-count the token counter. */

/* Mirrors ActorLocal_EmptyHoneycomb (core2/code_42CB0.c) — `uid` is the first field. */
typedef struct {
    s32 uid;
} BcHoneycombLocal;

RECOMP_HOOK("chHoneycomb_update") void banjocoop_honeycomb_update(Actor *this) {
    if (this == NULL || this->marker == NULL) {
        return;
    }
    /* The health honeycomb shares this update function and is not scored. */
    if (this->marker->id != WORLD_MARKER_EMPTY_HONEYCOMB) {
        return;
    }
    u32 id = (u32)((BcHoneycombLocal *)&this->local)->uid;
    if (id == 0 || id >= WORLD_MAX_HONEYCOMBS || !bc_honeycomb_is_remote(id)) {
        return;
    }
    bc_defer_despawn(this->marker);
}

/* The Mumbo token actor's update function. -> src/core2/code_59A80.c */
RECOMP_HOOK("func_802E0B10") void banjocoop_token_update(Actor *this) {
    if (this == NULL || this->marker == NULL) {
        return;
    }
    u32 id = (u32)func_802E0CB0(this);
    if (id == 0 || id >= WORLD_MAX_TOKENS || !bc_token_is_remote(id)) {
        return;
    }
    bc_defer_despawn(this->marker);
}

/* Enemies are the one thing any peer may declare dead: whoever lands the killing blow watches it
 * die locally first, and everyone else needs telling. Guarded like every other outgoing report so
 * applying somebody else's death does not bounce straight back at them. */
void bc_enemy_report_death(u32 net_id) {
    queue_here(BC_EV_ENEMY_DEAD, net_id, 0);
}

void bc_enemy_report_hit(u32 net_id, u32 collision_type) {
    queue_here(BC_EV_ENEMY_HIT, net_id, collision_type);
}

void bc_mode_report(u32 what, u32 a, u32 b) {
    bc_queue_event(BC_EV_MODE, 0, 0, what, a, b);
}

void bc_carry_report(u32 player_id, u32 on) {
    queue_here(BC_EV_CARRY, player_id, on);
}

void bc_boss_report_state(u32 net_id, u32 state) {
    queue_here(BC_EV_BOSS_STATE, net_id, state);
}

void bc_boss_report_fight(u32 what, u32 a, u32 b) {
    bc_queue_event(BC_EV_FIGHT, bc_map_id, bc_level_id, what, a, b);
}

/* ---- frame ---------------------------------------------------------------------------------- */

void world_begin_frame(u32 map_id, u32 level_id) {
    bc_map_id = map_id;
    bc_level_id = level_id;
    s_frame++;
}

void world_apply(bc_incoming *inc) {
    if (!inc->connected) {
        /* Drop the session's world state once, on the edge, rather than every frame we spend
         * offline — this clears several kilobytes. */
        if (bc_online) {
            world_reset();
        }
        bc_online = 0;
        return;
    }
    bc_online = 1;
    bc_is_host = inc->is_host;
    bc_local_player_id = inc->local_player_id;

    /* One guard around the whole batch rather than per event: applying a flag can cause game code
     * to set further flags synchronously, and every one of those is a consequence of the remote
     * change, not a new local one. */
    bc_applying = 1;
    for (u32 i = 0; i < inc->events.count && i < BCNET_EVENT_QUEUE; i++) {
        bc_event *ev = &inc->events.events[i];
        if (ev->kind == BC_EV_HASH) {
            bc_check_hashes(ev);
        } else {
            bc_apply_event(ev);
            /* Announce it after applying, so the toast and the thing it describes land together. */
            ui_toast_event(inc, ev);
        }
    }
    bc_applying = 0;

    /* Now that we are outside the game functions that asked for them. */
    bc_run_deferred_despawns();

    /* After applying, so notes that arrived this frame — including a map-entry snapshot — are
     * already in the registry when the total is rebuilt. */
    bc_run_pending_recount();

    /* Clear notes we already know are gone out of a map we have just walked into. Independent of
     * the host's arrival replay on purpose: it retries until the cube list exists, which a network
     * message cannot. */
    bc_run_arrival_sweep();

    /* Progression. The host publishes its save state continuously and clients mirror it; this
     * replaced a one-shot replay on join, which could be dropped or mistimed and left a player
     * unable to enter a world with no way to recover. */
    if (inc->is_host) {
        progress_publish(&g_outgoing);
    } else {
        bc_applying = 1;
        u32 changed = progress_apply(inc);
        bc_applying = 0;
        if (changed != 0) {
            recomp_printf("[banjocoop] progression mirror: %u bits brought into line\n", changed);
        }
    }
    s_last_remote_count = inc->remote_count;

    if ((s_frame % WORLD_HASH_PERIOD) == 0) {
        queue_here(BC_EV_HASH, bc_world_hash(), bc_flag_hash());
    }

    if (inc->events.dropped != 0 || g_outgoing.queue.dropped != 0) {
        recomp_printf("[banjocoop] WARNING: dropped %u incoming / %u outgoing world events\n",
                      inc->events.dropped, g_outgoing.queue.dropped);
        g_outgoing.queue.dropped = 0;
    }
}

void world_reset(void) {
    bc_online = 0;
    g_outgoing.queue.count = 0;
    g_outgoing.queue.dropped = 0;
    /* So reconnecting counts as the roster growing again and replays state to the new session. */
    s_last_remote_count = 0;
    progress_reset();
    bc_registry_clear();
}
