/* Session synchronisation — everything that reconciles a peer's world with the session's, as
 * opposed to applying one discrete change (that is world_apply.c).
 *
 * Three jobs, all of them about state that was decided while somebody was not listening:
 *
 *   - arrival sweep: a map we walk into respawns notes we already know are gone
 *   - note recount:  entering a level wipes the counter and restores it from a save file that
 *                    deliberately does not contain other players' notes
 *   - (the late-join state dump that used to live here is gone: the continuous progression
 *     mirror in progress.c supersedes it, and one mechanism is better than two)
 *
 * The desync detector lives here too, since it exists to catch these going wrong.
 */

#include "world_internal.h"

/* Last map we cleared already-collected notes out of; the sentinel forces a sweep on arrival. */
static u32 s_swept_map = 0xFFFFFFFFu;

/* Peers hash at slightly different moments, so a lone mismatch during a burst of changes means
 * nothing. Only a divergence that survives several samples is real. */
#define WORLD_HASH_STRIKES 3u
static u32 s_hash_strikes = 0;
static u32 s_hash_reported = 0;

void bc_sync_reset(void) {
    s_hash_strikes = 0;
    s_hash_reported = 0;
    s_swept_map = 0xFFFFFFFFu;
}

/* ---- arrival sweep ---------------------------------------------------------------------------
 *
 * Entering a map spawns its notes fresh, including ones we already know are gone. Rather than
 * depend on the host's arrival replay landing at exactly the right moment, we clear them from our
 * own registry in a single pass. This is the belt to the replay's braces: it needs no network, and
 * it retries until the cube list is actually populated, which the replay cannot do.
 */

static void sweep_cube(Cube *cube, u32 map_id, u32 *counter) {
    if (cube == NULL) {
        return;
    }
    for (u32 i = 0; i < cube->prop2Cnt; i++) {
        Prop *prop = &cube->prop2Ptr[i];
        if (!bc_prop_is_note(prop)) {
            continue;
        }
        u32 index = (*counter)++;

        /* The host's saved notes are no longer announced from here. They ride the progression
         * mirror instead (progress.c), which is continuous and therefore immune to join order,
         * dropped events and queue limits — the same reasoning that made flags reliable. This
         * sweep now only does what it was for: clearing notes we already know are gone out of a
         * map we have just walked into. */
        if (bc_note_collected(map_id, index)) {
            prop->spriteProp.unk8_4 = FALSE;
        }
    }
}

void bc_run_arrival_sweep(void) {
    if (bc_map_id == s_swept_map) {
        return;
    }
    /* Not loaded yet — leave the map unmarked so this retries next frame. */
    if (sCubeList.cubes == NULL || sCubeList.cubeCnt <= 0) {
        return;
    }

    u32 counter = 0;
    for (s32 i = 0; i < sCubeList.cubeCnt; i++) {
        sweep_cube(&sCubeList.cubes[i], bc_map_id, &counter);
    }
    sweep_cube(sCubeList.unk3C, bc_map_id, &counter);
    sweep_cube(sCubeList.unk40, bc_map_id, &counter);

    s_swept_map = bc_map_id;
}

/* ---- note recount across level transitions --------------------------------------------------- */

static u32 s_recount_level = 0;
static u32 s_recount_pending = 0;

void bc_request_note_recount(u32 level_id) {
    s_recount_level = level_id;
    s_recount_pending = 1;
}

void bc_run_pending_recount(void) {
    if (!s_recount_pending) {
        return;
    }
    s_recount_pending = 0;

    u32 total = 0;
    for (u32 map = 0; map < WORLD_MAX_MAPS; map++) {
        if (!bc_map_has_notes(map)) {
            continue;
        }
        /* Notes are counted per level, but recorded per map, so every map has to be mapped back
         * to its level before it can be totalled. */
        BcMapInfo *info = func_8030AD00((enum map_e)map);
        if (info == NULL || (u32)info->level_id != s_recount_level) {
            continue;
        }
        for (u32 i = 0; i < WORLD_NOTES_PER_MAP; i++) {
            if (bc_note_collected(map, i)) {
                total++;
            }
        }
    }

    /* Only ever raise the count. The registry holds what this session replicated; the value the
     * game just restored holds what the local player saved previously, and the two are not the
     * same set. Taking the larger keeps saved progress intact while adding the session's shared
     * notes. Reconciling them properly needs host-authoritative saves — Phase 6. */
    s32 current = item_getCount(ITEM_C_NOTE);
    if ((s32)total > current) {
        recomp_printf("[banjocoop] level %u note total %d -> %u from registry\n", s_recount_level,
                      current, total);
        item_set(ITEM_C_NOTE, (s32)total);
    }
}

/* ---- desync detector -------------------------------------------------------------------------
 *
 * Per the plan's verification section this is worth more than any other single piece of tooling,
 * because a replication bug otherwise shows up hours later as "the door did not open for me" with
 * nothing to point at.
 */

#define FNV_OFFSET 2166136261u
#define FNV_PRIME 16777619u

static u32 fnv_bytes(u32 hash, const u8 *data, u32 len) {
    for (u32 i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

u32 bc_world_hash(void) {
    u32 note_bytes = 0;
    const u8 *note_bits = bc_note_bits_raw(&note_bytes);
    u32 h = fnv_bytes(FNV_OFFSET, note_bits, note_bytes);
    u8 *jiggies = jiggyscore_getPtr();
    if (jiggies != NULL) {
        /* Collected bits only. The adjacent spawned bits are legitimately per-machine. */
        h = fnv_bytes(h, jiggies, 0xD);
    }

    u8 *honeycombs = honeycombscore_get_ptr();
    if (honeycombs != NULL) {
        h = fnv_bytes(h, honeycombs, WORLD_HONEYCOMB_BYTES);
    }

    /* Mumbo tokens have no plain pointer accessor, only the size+ptr pair. */
    s32 token_size = 0;
    u8 *tokens = NULL;
    mumboscore_getSizeAndPtr(&token_size, &tokens);
    if (tokens != NULL && token_size > 0) {
        h = fnv_bytes(h, tokens, (u32)token_size);
    }
    return h;
}

u32 bc_flag_hash(void) {
    u32 h = FNV_OFFSET;
    /* Read through the accessor rather than the raw array: the base recomp patches
     * fileProgressFlag_get, and hashing the underlying bytes would miss that.
     * Highest vanilla flag is FILEPROG_123_CHEAT_ENTERED. */
    for (u32 i = 0; i <= 0x123; i++) {
        h ^= fileProgressFlag_get((enum file_progress_e)i) ? 1u : 0u;
        h *= FNV_PRIME;
    }
    return h;
}

void bc_check_hashes(const bc_event *ev) {
    u32 mine_world = bc_world_hash();
    u32 mine_flags = bc_flag_hash();

    if (ev->a == mine_world && ev->b == mine_flags) {
        s_hash_strikes = 0;
        return;
    }

    /* Peers hash at slightly different moments, so a single mismatch while changes are in flight
     * is normal. Only a divergence that survives several samples is real. */
    s_hash_strikes++;
    if (s_hash_strikes < WORLD_HASH_STRIKES || s_hash_reported) {
        return;
    }
    s_hash_reported = 1;
    recomp_printf("[banjocoop] DESYNC: world %08X vs %08X, flags %08X vs %08X\n",
                  mine_world, ev->a, mine_flags, ev->b);
}
