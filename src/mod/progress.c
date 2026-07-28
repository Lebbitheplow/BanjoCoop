/* Progression mirror — Phase 6.
 *
 * The host's save is authoritative and every client mirrors it. This is what makes a joining
 * player's world match the host's: worlds open, jiggies gone, doors paid.
 *
 * It is a *continuous* mirror, not a replay on join, and that is the whole design. A one-shot
 * burst can be dropped, mistimed, or raced against the client's own save load — and when it
 * fails, the symptom is a player standing in front of a world that will not open, with no way to
 * recover short of reconnecting. Resending the whole thing once a second is self-healing: whatever
 * went wrong, the next one fixes it. It is about seventy bytes.
 *
 * What is mirrored is exactly the four permanent stores. What is NOT mirrored is per-player
 * inventory — health, lives, eggs, feathers — because copying the host's would be actively wrong.
 *
 * Everything is applied through the game's own setters rather than by writing the arrays. Several
 * of these stores are CRC-checked (`fileProgressFlag_set` recomputes two of them), so a raw byte
 * copy would produce a save the game rejects.
 */

#include "world_internal.h"
#include "progress.h"

/* -> src/core2/code_9E370.c */
extern ActorArray *suBaddieActorArray;

/* The lair's world entrances.
 *
 * Each decides whether it is open exactly once, in `func_80388524`'s `!volatile_initialized`
 * block (src/lair/code_0.c:1081) — the MM door swings by setting `yaw = 270`, the CC bars and GV
 * gate despawn themselves, and so on. It never looks at the flag again.
 *
 * That is a problem unique to mirrored progression: a client can walk into a lobby before the
 * host's mirror arrives, so the door latches shut, and then the flag turns on behind it. The
 * player is left standing in front of a closed world holding every flag that should open it —
 * which reads exactly like the sync being broken, and was the last thing actually wrong with it.
 *
 * Clearing `volatile_initialized` makes the game re-run its own opening logic. We deliberately do
 * not reimplement what "open" means for ten different doors; the game already knows. */
static const u16 k_lair_entrances[] = {
    ACTOR_20E_MM_ENTRANCE_DOOR,   ACTOR_20F_RBB_ENTRANCE_DOOR,
    ACTOR_210_BGS_ENTRANCE_DOOR,  ACTOR_211_TCC_ENTRANCE_CHEST_LID,
    ACTOR_212_CC_ENTRANCE_BARS,   ACTOR_226_GV_ENTRANCE,
    ACTOR_228_MMM_ENTRANCE_DOOR,  ACTOR_234_CCW_ENTRANCE_DOOR,
    ACTOR_235_FP_ENTANCE_DOOR,    ACTOR_2E5_DOOR_OF_GRUNTY,
};

#define LAIR_ENTRANCE_COUNT (sizeof(k_lair_entrances) / sizeof(k_lair_entrances[0]))

/* Walk the current map's notes, reporting each one's index and whether it is still alive.
 *
 * The alive bit IS the save state: note_saving clears it at map load for anything the save says
 * is collected, and the game clears it again on pickup. So this reads "which notes are gone" out
 * of the world, which is the only way to get at it on 1.0.1 (docs §16).
 *
 * `total` comes back so both peers can log how many notes they counted. If those disagree, the
 * two machines are numbering notes differently and every index in the mirror means something
 * different on each side — which looks exactly like the wrong notes vanishing. */
typedef void (*NoteVisitor)(u32 index, Prop *prop, void *ctx);

static void walk_map_notes(NoteVisitor visit, void *ctx, u32 *total) {
    u32 counter = 0;
    if (sCubeList.cubes == NULL || sCubeList.cubeCnt <= 0) {
        *total = 0;
        return;
    }
    for (s32 c = 0; c <= sCubeList.cubeCnt + 1; c++) {
        Cube *cube;
        if (c < sCubeList.cubeCnt) {
            cube = &sCubeList.cubes[c];
        } else if (c == sCubeList.cubeCnt) {
            cube = sCubeList.unk3C;
        } else {
            cube = sCubeList.unk40;
        }
        if (cube == NULL) {
            continue;
        }
        for (u32 i = 0; i < cube->prop2Cnt; i++) {
            Prop *prop = &cube->prop2Ptr[i];
            if (!bc_prop_is_note(prop)) {
                continue;
            }
            u32 index = counter++;
            if (visit != NULL) {
                visit(index, prop, ctx);
            }
        }
    }
    *total = counter;
}

static void note_collect_bits(u32 index, Prop *prop, void *ctx) {
    bc_u32 *bits = (bc_u32 *)ctx;
    if (index < (BC_PROGRESS_NOTE_WORDS * 32u) && !prop->spriteProp.unk8_4) {
        bits[index >> 5] |= (1u << (index & 31u));
    }
}

typedef struct {
    const bc_u32 *bits;
    u32 map_id;
    u32 cleared;
} NoteApplyCtx;

static void note_apply_bits(u32 index, Prop *prop, void *ctx) {
    NoteApplyCtx *a = (NoteApplyCtx *)ctx;
    if (index >= (BC_PROGRESS_NOTE_WORDS * 32u)) {
        return;
    }
    u32 host_collected = (a->bits[index >> 5] >> (index & 31u)) & 1u;
    if (!host_collected) {
        return;
    }
    /* Record it either way, so the arrival sweep and the note total agree with the mirror. */
    bc_note_mark(a->map_id, index);
    if (prop->spriteProp.unk8_4) {
        prop->spriteProp.unk8_4 = FALSE;
        a->cleared++;
    }
}

static void refresh_lair_entrances(void) {
    if (suBaddieActorArray == NULL) {
        return;
    }
    for (s32 i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (actor->despawn_flag) {
            continue;
        }
        for (u32 e = 0; e < LAIR_ENTRANCE_COUNT; e++) {
            if ((u32)actor->modelCacheIndex == (u32)k_lair_entrances[e]) {
                actor->volatile_initialized = 0;
                break;
            }
        }
    }
}

/* The four stores, packed in this order into the word array. Counts are bit counts, taken from
 * the stores themselves: file progress runs to FILEPROG_123_CHEAT_ENTERED, and the other three
 * are the bounds their own setters enforce. */
#define PROG_FILEPROG_BITS 0x124u
#define PROG_JIGGY_BITS WORLD_MAX_JIGGIES
#define PROG_HONEYCOMB_BITS WORLD_MAX_HONEYCOMBS
#define PROG_TOKEN_BITS WORLD_MAX_TOKENS

#define PROG_TOTAL_BITS (PROG_FILEPROG_BITS + PROG_JIGGY_BITS + PROG_HONEYCOMB_BITS + PROG_TOKEN_BITS)

/* A build that outgrows the packet should fail here rather than silently truncate somebody's
 * progress — a truncated mirror would clear real flags on every client. */
#if (PROG_TOTAL_BITS > (BC_PROGRESS_WORDS * 32u))
#error "bc_progress is too small for the progression bitfields; raise BC_PROGRESS_WORDS"
#endif

static void put_bit(bc_u32 *words, u32 index, u32 value) {
    u32 w = index >> 5;
    u32 b = index & 31u;
    if (value) {
        words[w] |= (1u << b);
    } else {
        words[w] &= ~(1u << b);
    }
}

static u32 get_bit(const bc_u32 *words, u32 index) {
    return (words[index >> 5] >> (index & 31u)) & 1u;
}

static u32 s_published = 0;
static u32 s_last_set_count = 0xFFFFFFFFu;
static u32 s_received = 0;
static u32 s_last_note_total = 0xFFFFFFFFu;
static u32 s_client_note_total = 0xFFFFFFFFu;

static u32 count_set(const bc_u32 *w) {
    u32 n = 0;
    for (u32 i = 0; i < BC_PROGRESS_WORDS; i++) {
        bc_u32 v = w[i];
        while (v != 0) {
            n += (v & 1u);
            v >>= 1;
        }
    }
    return n;
}

void progress_publish(bc_outgoing *out) {
    bc_u32 *w = out->progress.words;
    for (u32 i = 0; i < BC_PROGRESS_WORDS; i++) {
        w[i] = 0;
    }

    u32 at = 0;
    for (u32 i = 0; i < PROG_FILEPROG_BITS; i++, at++) {
        put_bit(w, at, fileProgressFlag_get((enum file_progress_e)i) ? 1u : 0u);
    }
    for (u32 i = 0; i < PROG_JIGGY_BITS; i++, at++) {
        put_bit(w, at, jiggyscore_isCollected((enum jiggy_e)i) ? 1u : 0u);
    }
    for (u32 i = 0; i < PROG_HONEYCOMB_BITS; i++, at++) {
        put_bit(w, at, honeycombscore_get((enum honeycomb_e)i) ? 1u : 0u);
    }
    for (u32 i = 0; i < PROG_TOKEN_BITS; i++, at++) {
        put_bit(w, at, mumboscore_get((enum mumbotoken_e)i) ? 1u : 0u);
    }

    /* Refuse to publish an empty mirror.
     *
     * This mirror CLEARS bits the host does not have, so publishing one before the host's save is
     * loaded would wipe every client's progression. A real save always has flags set — even a
     * brand new file has some — so all-zero means "not ready", not "no progress". Cheap insurance
     * against the one failure mode here that destroys data rather than merely failing to sync. */
    u32 set_count = count_set(w);
    if (set_count == 0) {
        out->progress.valid = 0;
        return;
    }

    /* The current map's notes, straight from the world. */
    for (u32 i = 0; i < BC_PROGRESS_NOTE_WORDS; i++) {
        out->progress.note_bits[i] = 0;
    }
    out->progress.note_map = bc_map_id;
    u32 total_notes = 0;
    walk_map_notes(note_collect_bits, out->progress.note_bits, &total_notes);
    if (total_notes != s_last_note_total) {
        s_last_note_total = total_notes;
        recomp_printf("[banjocoop] host map %u has %u note props\n", bc_map_id, total_notes);
    }

    out->progress.valid = 1;

    if (!s_published || set_count != s_last_set_count) {
        s_published = 1;
        s_last_set_count = set_count;
        recomp_printf("[banjocoop] publishing progression mirror: %u bits set\n", set_count);
        /* Name the flags that actually gate world entry. "16 bits set" says nothing about whether
         * the two that matter are among them, and that ambiguity is what made this hard to pin
         * down from a log. -> lair/code_0.c:1025 */
        recomp_printf("[banjocoop]   host MM_OPEN=%u MM_PUZZLE=%u jiggies=%u\n",
                      fileProgressFlag_get((enum file_progress_e)0x31) ? 1u : 0u,
                      fileProgressFlag_get((enum file_progress_e)0x5D) ? 1u : 0u,
                      (u32)item_getCount(ITEM_26_JIGGY_TOTAL));
        recomp_printf("[banjocoop]   host fileprog set:");
        for (u32 i = 0; i < PROG_FILEPROG_BITS; i++) {
            if (fileProgressFlag_get((enum file_progress_e)i)) {
                recomp_printf(" %x", i);
            }
        }
        recomp_printf("\n");
    }
}

/* Applying is a diff, for two reasons: the setters are not free (file progress recomputes two
 * CRCs on every call), and writing only what differs keeps this silent in the normal case where
 * the two saves already agree.
 *
 * It mirrors in both directions — a bit the host does not have is cleared here. That is what
 * "the host's save is authoritative" means. A client's own pickup still reaches the host as an
 * event first, so the mirror confirms it rather than undoing it.
 */
u32 progress_apply(bc_incoming *inc) {
    if (!inc->progress.valid) {
        return 0;
    }

    const bc_u32 *w = inc->progress.words;

    /* Same guard from the receiving end: never let an empty mirror wipe local progression. */
    if (count_set(w) == 0) {
        return 0;
    }

    if (!s_received) {
        s_received = 1;
        recomp_printf("[banjocoop] first progression mirror from host: %u bits set\n",
                      count_set(w));
        recomp_printf("[banjocoop]   mirror says MM_OPEN=%u MM_PUZZLE=%u; we have %u/%u\n",
                      get_bit(w, 0x31), get_bit(w, 0x5D),
                      fileProgressFlag_get((enum file_progress_e)0x31) ? 1u : 0u,
                      fileProgressFlag_get((enum file_progress_e)0x5D) ? 1u : 0u);
    }
    u32 changed = 0;
    u32 at = 0;

    for (u32 i = 0; i < PROG_FILEPROG_BITS; i++, at++) {
        u32 want = get_bit(w, at);
        if ((fileProgressFlag_get((enum file_progress_e)i) ? 1u : 0u) != want) {
            fileProgressFlag_set((enum file_progress_e)i, (s32)want);
            changed++;
        }
    }
    for (u32 i = 0; i < PROG_JIGGY_BITS; i++, at++) {
        u32 want = get_bit(w, at);
        if (i == 0) {
            continue; /* jiggy 0 is not a jiggy; its setter ignores it */
        }
        if ((jiggyscore_isCollected((enum jiggy_e)i) ? 1u : 0u) != want) {
            jiggyscore_setCollected((s32)i, (s32)want);
            changed++;
        }
    }
    for (u32 i = 0; i < PROG_HONEYCOMB_BITS; i++, at++) {
        u32 want = get_bit(w, at);
        if (i == 0) {
            continue;
        }
        if ((honeycombscore_get((enum honeycomb_e)i) ? 1u : 0u) != want) {
            honeycombscore_set((enum honeycomb_e)i, (s32)want);
            changed++;
        }
    }
    for (u32 i = 0; i < PROG_TOKEN_BITS; i++, at++) {
        u32 want = get_bit(w, at);
        if (i == 0) {
            continue;
        }
        if ((mumboscore_get((enum mumbotoken_e)i) ? 1u : 0u) != want) {
            mumboscore_set((enum mumbotoken_e)i, (s32)want);
            changed++;
        }
    }

    /* Notes for the map we are standing in. Continuous, like everything else here: a client that
     * arrived before the host, or joined late, or missed an event, converges within a second. */
    if (inc->progress.note_map == bc_map_id) {
        NoteApplyCtx ctx;
        ctx.bits = inc->progress.note_bits;
        ctx.map_id = bc_map_id;
        ctx.cleared = 0;
        u32 total = 0;
        walk_map_notes(note_apply_bits, &ctx, &total);
        if (total != s_client_note_total) {
            s_client_note_total = total;
            recomp_printf("[banjocoop] client map %u has %u note props\n", bc_map_id, total);
        }
        if (ctx.cleared != 0) {
            recomp_printf("[banjocoop] mirrored %u collected notes from the host in map %u\n",
                          ctx.cleared, bc_map_id);
        }
    }

    if (changed != 0) {
        /* A world-open flag may have just turned on behind a door that already decided it was
         * shut. Let the doors look again. */
        refresh_lair_entrances();
        recomp_printf("[banjocoop]   after apply: MM_OPEN=%u MM_PUZZLE=%u (entrances refreshed)\n",
                      fileProgressFlag_get((enum file_progress_e)0x31) ? 1u : 0u,
                      fileProgressFlag_get((enum file_progress_e)0x5D) ? 1u : 0u);
    }

    return changed;
}

void progress_reset(void) {
    s_last_note_total = 0xFFFFFFFFu;
    s_client_note_total = 0xFFFFFFFFu;
    s_published = 0;
    s_last_set_count = 0xFFFFFFFFu;
    s_received = 0;
}
