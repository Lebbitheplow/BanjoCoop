/* Shared world state — the incoming half of the Phase 3 replication layer.
 *
 * Holds the registry (what the session believes is already collected) and applies the events the
 * host accepted for us. Nothing here decides anything: adjudication is the host's job, in
 * transport.cpp. By the time an event reaches bc_apply_event it has already been ruled on.
 *
 * Everything in this file runs with bc_applying set, so the outgoing hooks in world.c stay quiet
 * while game code reacts to what we are applying.
 */

#include "world_internal.h"
#include "enemy.h"
#include "ui.h"
#include "boss.h"
#include "api.h"
#include "carry.h"
#include "modes.h"

#define SPRITE_PROP_ASSET_BIAS 0x572

#define WORLD_MAX_PENDING_DESPAWN 8

static u8 s_note_bits[WORLD_MAX_MAPS][WORLD_NOTE_BYTES];

/* Jiggies collected by somebody else, so the local actor can be removed. The jiggyscore bitfield
 * alone cannot tell us this: it is also set by our own pickup, where the game plays out the whole
 * collection sequence and we must not interfere. */
static u8 s_remote_jiggy[WORLD_JIGGY_BYTES];
static u8 s_remote_honeycomb[WORLD_HONEYCOMB_BYTES];
static u8 s_remote_token[WORLD_TOKEN_BYTES];

static ActorMarker *s_pending_despawn[WORLD_MAX_PENDING_DESPAWN];
static u32 s_pending_count = 0;


/* ---- registry ------------------------------------------------------------------------------- */

static u32 bit_get(const u8 *bits, u32 index) {
    return (bits[index >> 3] >> (index & 7u)) & 1u;
}

static void bit_set(u8 *bits, u32 index) {
    bits[index >> 3] |= (u8)(1u << (index & 7u));
}

u32 bc_note_collected(u32 map_id, u32 note_index) {
    if (map_id >= WORLD_MAX_MAPS || note_index >= WORLD_NOTES_PER_MAP) {
        return 0;
    }
    return bit_get(s_note_bits[map_id], note_index);
}

void bc_note_mark(u32 map_id, u32 note_index) {
    if (map_id >= WORLD_MAX_MAPS || note_index >= WORLD_NOTES_PER_MAP) {
        return;
    }
    bit_set(s_note_bits[map_id], note_index);
}

u32 bc_jiggy_is_remote(u32 jiggy_id) {
    if (jiggy_id >= WORLD_MAX_JIGGIES) {
        return 0;
    }
    return bit_get(s_remote_jiggy, jiggy_id);
}

u32 bc_honeycomb_is_remote(u32 honeycomb_id) {
    if (honeycomb_id >= WORLD_MAX_HONEYCOMBS) {
        return 0;
    }
    return bit_get(s_remote_honeycomb, honeycomb_id);
}

u32 bc_token_is_remote(u32 token_id) {
    if (token_id >= WORLD_MAX_TOKENS) {
        return 0;
    }
    return bit_get(s_remote_token, token_id);
}

void bc_registry_clear(void) {
    for (u32 i = 0; i < WORLD_MAX_MAPS; i++) {
        for (u32 j = 0; j < WORLD_NOTE_BYTES; j++) {
            s_note_bits[i][j] = 0;
        }
    }
    for (u32 i = 0; i < WORLD_JIGGY_BYTES; i++) {
        s_remote_jiggy[i] = 0;
    }
    for (u32 i = 0; i < WORLD_HONEYCOMB_BYTES; i++) {
        s_remote_honeycomb[i] = 0;
    }
    for (u32 i = 0; i < WORLD_TOKEN_BYTES; i++) {
        s_remote_token[i] = 0;
    }
    s_pending_count = 0;
    bc_sync_reset();
}

u32 bc_map_has_notes(u32 map_id) {
    if (map_id >= WORLD_MAX_MAPS) {
        return 0;
    }
    for (u32 b = 0; b < WORLD_NOTE_BYTES; b++) {
        if (s_note_bits[map_id][b] != 0) {
            return 1;
        }
    }
    return 0;
}

const u8 *bc_note_bits_raw(u32 *size) {
    *size = (u32)sizeof(s_note_bits);
    return (const u8 *)s_note_bits;
}

void bc_defer_despawn(ActorMarker *marker) {
    for (u32 i = 0; i < s_pending_count; i++) {
        if (s_pending_despawn[i] == marker) {
            return;
        }
    }
    if (s_pending_count < WORLD_MAX_PENDING_DESPAWN) {
        s_pending_despawn[s_pending_count++] = marker;
    }
}

void bc_run_deferred_despawns(void) {
    for (u32 i = 0; i < s_pending_count; i++) {
        if (s_pending_despawn[i] != NULL) {
            marker_despawn(s_pending_despawn[i]);
        }
        s_pending_despawn[i] = NULL;
    }
    s_pending_count = 0;
}

/* ---- applying ------------------------------------------------------------------------------- */

/* ---- note identity ---------------------------------------------------------------------------
 *
 * A note's network id is its position in a fixed walk of the cube array: cube 0..cubeCnt, then the
 * two fallback cubes, counting music-note sprite props as we go.
 *
 * That walk gives the same numbering on every machine because `sCubeList` is built from the same
 * static level data everywhere — no runtime API, no spawn order, no dependency on which
 * BanjoRecomp version is running. (BanjoRecomp's own note bookkeeping computes an equivalent
 * index, but it only exposes it to mods on `main`, not in the 1.0.1 release; see docs/symbols.md
 * §16 before reaching for a `bkrecomp_*` export again.)
 *
 * Collected notes are still counted. Their props stay in the array with only the alive bit
 * cleared, so skipping them would renumber every note after them and desync the two peers.
 */

u32 bc_prop_is_note(Prop *prop) {
    if (prop->is_3d || prop->is_actor) {
        return 0;
    }
    return (u32)prop->spriteProp.sprite_index + SPRITE_PROP_ASSET_BIAS ==
           (u32)ASSET_6D6_MODEL_MUSIC_NOTE;
}

/* One walk, two questions. With `find` set, reports that prop's index through `out_index`; with
 * `find` NULL, returns the prop holding index `target`. */
static Prop *note_walk_cube(Cube *cube, Prop *find, u32 target, u32 *counter, u32 *out_index) {
    if (cube == NULL) {
        return NULL;
    }
    for (u32 i = 0; i < cube->prop2Cnt; i++) {
        Prop *prop = &cube->prop2Ptr[i];
        if (!bc_prop_is_note(prop)) {
            continue;
        }
        u32 index = (*counter)++;
        if (find != NULL) {
            if (prop == find) {
                *out_index = index;
                return prop;
            }
        } else if (index == target) {
            return prop;
        }
    }
    return NULL;
}

static Prop *note_walk(Prop *find, u32 target, u32 *out_index) {
    /* The cube list is torn down and rebuilt across a map transition. Matching map ids are not
     * enough to prove it is populated right now — an event can land in the gap. */
    if (sCubeList.cubes == NULL || sCubeList.cubeCnt <= 0) {
        return NULL;
    }

    u32 counter = 0;
    for (s32 i = 0; i < sCubeList.cubeCnt; i++) {
        Prop *hit = note_walk_cube(&sCubeList.cubes[i], find, target, &counter, out_index);
        if (hit != NULL) {
            return hit;
        }
    }
    /* The two fallback cubes hold props that did not land in the grid; note_saving searches them
     * too, and a note in one of them is otherwise unreachable. */
    Prop *hit = note_walk_cube(sCubeList.unk3C, find, target, &counter, out_index);
    if (hit != NULL) {
        return hit;
    }
    return note_walk_cube(sCubeList.unk40, find, target, &counter, out_index);
}

s32 bc_note_index_of(Prop *prop) {
    u32 index = 0;
    if (prop == NULL || note_walk(prop, 0, &index) == NULL) {
        return -1;
    }
    return (s32)index;
}

/* Remove a note from our world without collecting it. Clearing the sprite prop's alive bit is how
 * the game itself retires a collected note (core2/ba/marker.c:845), so this leaves exactly the
 * state a local pickup would have left, minus the sound and the score. */
static u32 despawn_note(u32 note_index) {
    Prop *prop = note_walk(NULL, note_index, NULL);
    if (prop == NULL) {
        return 0;
    }
    prop->spriteProp.unk8_4 = FALSE;
    return 1;
}

/* Award a note without the pickup. Notes are shared progress, so a note collected anywhere in the
 * level counts for everyone in that level — but the HUD counter is per-level, so a note collected
 * in a level we are not in must not touch it. */
static void grant_note(u32 level_id) {
    if (level_id != bc_level_id) {
        return;
    }
    if (!func_802FADD4(ITEM_1B_VILE_VILE_SCORE)) {
        item_inc(ITEM_C_NOTE);
    } else {
        item_adjustByDiffWithoutHud(ITEM_C_NOTE, 1);
    }
}

/* A collected object is recorded in TWO places, and replicating only one of them is the mistake
 * this file keeps inviting:
 *
 *   1. a permanent score bit  (jiggyscore / honeycombscore / mumboscore)
 *   2. a separate item counter the pickup bumps alongside it
 *      (ITEM_26_JIGGY_TOTAL, ITEM_13_EMPTY_HONEYCOMB, ITEM_1C_MUMBO_TOKEN, ITEM_C_NOTE)
 *
 * Set only the bit and the object correctly disappears for the other player while their totals
 * never move. Bump only the counter and the object stays collectable, so it gets counted twice.
 * Both, guarded on the bit having actually changed, is the only correct combination — the guard
 * is what makes a re-delivered event harmless.
 *
 * Whenever a new collectible is added here, go and read its case in `core2/ba/marker.c` and
 * mirror *every* side effect it has, not just the obvious one.
 */
void bc_apply_event(const bc_event *ev) {
    switch (ev->kind) {
        case BC_EV_NOTE_STATIC: {
            /* Two separate questions, and conflating them was a bug: "have I counted this note"
             * and "have I removed it from my world" are not the same.
             *
             * A note collected while we were in another map gets recorded but not despawned —
             * there was nothing to despawn, its props did not exist here. Arriving in that map
             * spawns them fresh, so the despawn has to run even though the note is already
             * recorded. Guarding the whole branch on the recorded check left every such note
             * standing, with a correct count beside it. */
            u32 already = bc_note_collected(ev->map_id, ev->a);
            if (!already) {
                bc_note_mark(ev->map_id, ev->a);
                grant_note(ev->level_id);
            }
            u32 removed = 0;
            if (ev->map_id == bc_map_id) {
                removed = despawn_note(ev->a);
            }
            /* Logged because this is the one path with a silent failure mode: the note can be
             * marked and granted correctly while the walk fails to find the prop, leaving a ghost
             * note standing that still looks collectable. `removed=0` while in the same map is
             * exactly that case, and is otherwise indistinguishable from working. */
            recomp_printf("[banjocoop] note %u map %u applied (removed=%u)\n", ev->a, ev->map_id,
                          removed);
            break;
        }

        /* BC_EV_NOTE_DYNAMIC is no longer sent — see the note hook in world.c. Old peers cannot
         * connect (the build fingerprint differs), so nothing will deliver one, but ignoring it
         * costs nothing and is safer than granting a note for a message we no longer understand. */
        case BC_EV_NOTE_DYNAMIC:
            break;

        case BC_EV_JIGGY:
            if (ev->a > 0 && ev->a < WORLD_MAX_JIGGIES) {
                u32 was_set = jiggyscore_isCollected((enum jiggy_e)ev->a) ? 1u : 0u;
                jiggyscore_setCollected((s32)ev->a, (s32)ev->b);
                if (ev->b) {
                    bit_set(s_remote_jiggy, ev->a);
                    if (!was_set) {
                        item_adjustByDiffWithoutHud(ITEM_26_JIGGY_TOTAL, 1);
                    }
                }
            }
            break;

        case BC_EV_HONEYCOMB:
            if (ev->a > 0 && ev->a < WORLD_MAX_HONEYCOMBS) {
                u32 was_set = honeycombscore_get((enum honeycomb_e)ev->a) ? 1u : 0u;
                honeycombscore_set((enum honeycomb_e)ev->a, (s32)ev->b);
                if (ev->b) {
                    bit_set(s_remote_honeycomb, ev->a);
                    if (!was_set) {
                        item_inc(ITEM_13_EMPTY_HONEYCOMB);
                        /* Six pieces is a health upgrade. The pickup path does this itself, so a
                         * replicated piece has to as well or the sixth one silently does nothing
                         * for the player who did not walk into it. */
                        if (item_getCount(ITEM_13_EMPTY_HONEYCOMB) >= 6) {
                            gcpausemenu_80314AC8(0);
                        }
                    }
                }
            }
            break;

        case BC_EV_MUMBO_TOKEN:
            if (ev->a > 0 && ev->a < WORLD_MAX_TOKENS) {
                u32 was_set = mumboscore_get((enum mumbotoken_e)ev->a) ? 1u : 0u;
                mumboscore_set((enum mumbotoken_e)ev->a, (s32)ev->b);
                if (ev->b) {
                    bit_set(s_remote_token, ev->a);
                    if (!was_set) {
                        item_inc(ITEM_1C_MUMBO_TOKEN);
                    }
                }
            }
            break;

        case BC_EV_JINJO: {
            /* Credit is an ADD, so this must happen exactly once per Jinjo. Guarding on the bit
             * not already being held is what makes a re-delivered event harmless — without it the
             * add carries into the next colour's bit and corrupts the whole mask. */
            u32 bit = 1u << ((ev->a + 6u) & 31u);
            if (((u32)item_getCount(ITEM_12_JINJOS) & bit) == 0) {
                item_adjustByDiffWithHud(ITEM_12_JINJOS, (s32)bit);
            }
            /* And take ours away, or walking into it would credit it a second time. */
            Actor *jinjo = actorArray_findActorFromMarkerId((enum marker_e)ev->a);
            if (jinjo != NULL && jinjo->marker != NULL) {
                bc_defer_despawn(jinjo->marker);
            }
            break;
        }

        case BC_EV_ENEMY_HIT:
            /* Pooled damage: somebody else landed a hit, so our copy takes it too. Both players
             * chipping at the same enemy therefore add up, on every machine. */
            if (ev->map_id == bc_map_id) {
                enemy_apply_hit(ev->a, ev->b);
            }
            break;

        case BC_EV_MODE:
            modes_apply(ev->a, ev->b, ev->c);
            break;

        case BC_EV_CARRY:
            /* Only the player being picked up needs to act on it: everyone else already sees the
             * carrier's puppet holding theirs, because the carrier's game is moving it. */
            if (ev->a == bc_local_player_id) {
                carry_set_carried_by(ev->origin, ev->b);
            }
            break;

        case BC_EV_CUSTOM:
            /* Another mod's business, not ours. */
            api_deliver(ev);
            break;

        case BC_EV_BOSS_STATE:
            /* A boss's phase is decided by script, not by anything the other machines can derive,
             * so it has to be told rather than inferred. */
            if (ev->map_id == bc_map_id) {
                boss_apply_state(ev->a, ev->b);
            }
            break;

        case BC_EV_FIGHT:
            /* The final fight. Without this it cannot be finished together at all. */
            if (ev->map_id == bc_map_id) {
                boss_apply_fight(ev->a, ev->b, ev->c);
            }
            break;

        case BC_EV_ENEMY_DEAD:
            /* Only meaningful in the map it came from; net ids are per-map. The enemy layer
             * removes the local copy on its next pass rather than us reaching for the actor here,
             * so there is one place that knows how to find an enemy by net id. */
            if (ev->map_id == bc_map_id) {
                enemy_mark_dead(ev->a);
            }
            break;

        case BC_EV_FLAG_FILEPROG:
            fileProgressFlag_set((enum file_progress_e)ev->a, (s32)ev->b);
            break;

        case BC_EV_FLAG_LEVEL:
            levelSpecificFlags_set((s32)ev->a, (s32)ev->b);
            break;

        case BC_EV_FLAG_MAP:
            mapSpecificFlags_set((s32)ev->a, (s32)ev->b);
            break;

        case BC_EV_FLAG_FILEPROG_N:
            fileProgressFlag_setN((enum file_progress_e)ev->a, (s32)ev->b, (s32)ev->c);
            break;

        case BC_EV_FLAG_LEVEL_N:
            levelSpecificFlags_setN((s32)ev->a, (s32)ev->b, (s32)ev->c);
            break;

        default:
            break;
    }
}
