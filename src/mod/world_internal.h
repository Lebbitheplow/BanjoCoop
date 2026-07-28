/* Shared between the two halves of the world layer: world.c (notice a local change and queue it)
 * and world_apply.c (apply a change the host accepted, and keep the registry).
 *
 * Not part of the module's interface — world.h is. This exists only because the outgoing and
 * incoming halves need the same game symbols and the same registry.
 */

#ifndef BANJOCOOP_WORLD_INTERNAL_H
#define BANJOCOOP_WORLD_INTERNAL_H

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "prop.h"
#include "bkrecomp_api.h"

#include "banjocoop/protocol.h"

/* Declared here rather than included: none of these are in functions.h, though all are present in
 * bk.us.rev0.syms.toml and resolve by name at mod load. */
extern enum map_e map_get(void);
extern enum level_e level_get(void);
extern void jiggyscore_setCollected(s32 index, s32 value);
extern u32 jiggyscore_isCollected(enum jiggy_e jiggy_id);
extern u8 *jiggyscore_getPtr(void);
extern enum jiggy_e chjiggy_getJiggyId(Actor *this);
/* The other two permanent score bitfields. Empty honeycombs and Mumbo tokens are world progress
 * that lives in neither the flag arrays nor the note data, which is why hooking flags and notes
 * alone silently missed them. */
extern void honeycombscore_set(enum honeycomb_e index, s32 value);
extern bool honeycombscore_get(enum honeycomb_e index);
extern u8 *honeycombscore_get_ptr(void);
extern void mumboscore_set(enum mumbotoken_e index, s32 value);
extern bool mumboscore_get(enum mumbotoken_e index);
extern void mumboscore_getSizeAndPtr(s32 *size, u8 **addr);
/* Returns a Mumbo token actor's token id. -> src/core2/code_59A80.c */
extern enum mumbotoken_e func_802E0CB0(Actor *this);
extern bool fileProgressFlag_get(enum file_progress_e index);
extern void fileProgressFlag_set(enum file_progress_e index, s32 set);
extern s32 fileProgressFlag_getN(enum file_progress_e offset, s32 num_bits);
extern void fileProgressFlag_setN(enum file_progress_e start, s32 set, s32 length);
extern s32 levelSpecificFlags_get(s32 index);
extern void levelSpecificFlags_set(s32 index, s32 value);
extern s32 levelSpecificFlags_getN(s32 index, s32 num_bits);
extern void levelSpecificFlags_setN(s32 index, s32 value, s32 num_bits);
extern s32 mapSpecificFlags_get(s32 index);
extern void mapSpecificFlags_set(s32 index, s32 value);
extern void item_inc(enum item_e item);
extern void item_adjustByDiffWithoutHud(enum item_e item, s32 diff);
extern s32 item_getCount(enum item_e item);
extern s32 item_adjustByDiffWithHud(enum item_e item, s32 diff);
extern Actor *actorArray_findActorFromMarkerId(enum marker_e marker_id);
extern void item_set(s32 item, s32 value);

/* Map -> level lookup, so the notes we have recorded can be totalled per level.
 * Mirrors MapInfo in patches/note_saving.c. -> func_8030AD00 */
typedef struct {
    s16 map_id;
    s16 level_id;
    char *name;
} BcMapInfo;
extern BcMapInfo *func_8030AD00(enum map_e map_id);
/* Awards the health upgrade once six honeycomb pieces are held. -> core2/gc/pauseMenu.c */
extern void gcpausemenu_80314AC8(s32 arg0);
/* True while the Vile minigame is scoring, in which case notes must not touch the HUD counter.
 * Mirrored from __baMarker_resolveMusicNoteCollision so a granted note behaves like a real one. */
extern bool func_802FADD4(enum item_e item_id);

#define WORLD_MAX_MAPS 512u
/* Busiest map is Mumbo's Mountain at 85 static notes. Rounded up to a byte boundary with room to
 * spare, since a mod that adds notes would otherwise silently corrupt the neighbouring map. */
#define WORLD_NOTES_PER_MAP 96u
#define WORLD_NOTE_BYTES (WORLD_NOTES_PER_MAP / 8u)

#define WORLD_MAX_JIGGIES 0x65u
#define WORLD_JIGGY_BYTES ((WORLD_MAX_JIGGIES + 7u) / 8u)

/* Bounds copied from the stores themselves: HONEYCOMB_COUNT in core2/honeycombscore.c and
 * MUMBO_TOKEN_COUNT in core2/mumboscore.c. Both setters ignore out-of-range ids, and so do we. */
#define WORLD_MAX_HONEYCOMBS 0x19u
#define WORLD_HONEYCOMB_BYTES ((WORLD_MAX_HONEYCOMBS + 7u) / 8u)
#define WORLD_MAX_TOKENS 126u
#define WORLD_TOKEN_BYTES ((WORLD_MAX_TOKENS + 7u) / 8u)

/* The empty honeycomb and the health honeycomb share an update function; only the empty one is
 * scored. -> MARKER_53_EMPTY_HONEYCOMB */
#define WORLD_MARKER_EMPTY_HONEYCOMB 0x53

/* Where the local player is this frame. The collection hooks fire from deep inside game code and
 * read these rather than working out where they are from scratch. */
extern u32 bc_map_id;
extern u32 bc_level_id;
extern u32 bc_online;
/* Whether we are the authority this frame. The host derives shared world state the clients cannot
 * see for themselves — notes already collected in its save file, in particular. */
extern u32 bc_is_host;
extern u32 bc_local_player_id;

/* Set while a remote change is being applied, so the hooks do not re-broadcast it. The one rule
 * everything else depends on: without it two peers bounce a single flag write off each other
 * forever. */
extern u32 bc_applying;

/* The cube list. Needed by note identity (which note did the player just take) and by the arrival
 * sweep (clear notes we already know are gone out of the map we walked into).
 * Layout copied verbatim from patches/prop_extension_patches.c. */
extern struct {
    Cube *cubes;
    f32 margin;
    s32 min[3];
    s32 max[3];
    s32 stride[2];
    s32 cubeCnt;
    s32 unk2C;
    s32 width[3];
    Cube *unk3C;
    Cube *unk40;
    s32 unk44;
} sCubeList;

/* True for a music-note sprite prop. A sprite prop's asset id is its sprite_index biased by
 * 0x572 (core2/ba/marker.c:840); the game identifies notes this way in its own collision path. */
u32 bc_prop_is_note(Prop *prop);

/* Cheap pre-filter so the per-level recount can skip maps with nothing recorded. */
u32 bc_map_has_notes(u32 map_id);

/* Raw note registry, for hashing. */
const u8 *bc_note_bits_raw(u32 *size);

/* Drop the session-sync state (sweep bookkeeping, any dump in flight). Called by
 * bc_registry_clear so a disconnect resets both halves together. */
void bc_sync_reset(void);

/* Queue an outgoing event. Silently does nothing while offline or while applying. */
void bc_queue_event(u32 kind, u32 map_id, u32 level_id, u32 a, u32 b, u32 c);

/* The registry — our record of which shared objects are gone. Deliberately separate from
 * BanjoRecomp's saved note data: that belongs to the player's save file, and a co-op session must
 * not silently edit it. This is session state, matching the plan's host-registry model. */
u32 bc_note_collected(u32 map_id, u32 note_index);
void bc_note_mark(u32 map_id, u32 note_index);
void bc_registry_clear(void);

/* A note's cross-machine id: its ordinal in a fixed walk of the cube array. Returns -1 if the
 * prop is not a note or the cube list is not loaded. */
s32 bc_note_index_of(Prop *prop);

/* A despawn cannot happen inside the hook that discovers it is needed: our hooks run at function
 * entry, so freeing the actor there would leave the function we are hooking to run on freed
 * memory. Deferring to the frame hook sidesteps that entirely. */
u32 bc_jiggy_is_remote(u32 jiggy_id);
u32 bc_honeycomb_is_remote(u32 honeycomb_id);
u32 bc_token_is_remote(u32 token_id);
void bc_defer_despawn(ActorMarker *marker);
void bc_run_deferred_despawns(void);

void bc_apply_event(const bc_event *ev);

/* Entering a level wipes its note counter and restores it from the save file. Our replicated
 * notes are deliberately not in that save file, so the count has to be rebuilt from the registry
 * afterwards or a session's shared notes vanish the moment anyone re-enters a level. */
void bc_request_note_recount(u32 level_id);
void bc_run_pending_recount(void);

/* Clear notes we already know are collected out of the map we just arrived in. Retries itself
 * until the cube list is populated, so it does not race the map load. */
void bc_run_arrival_sweep(void);


/* Desync detection. FNV-1a over the state that is supposed to be identical on every peer. */
u32 bc_world_hash(void);
u32 bc_flag_hash(void);
void bc_check_hashes(const bc_event *ev);

#endif
