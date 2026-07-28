/* Stub of the mod's internal header, so the pure logic in src/mod/progress.c can be compiled and
 * tested on the host without a ROM, the game, or the MIPS toolchain.
 *
 * This exists because the mod's code only loads when a game session starts, which needs a
 * controller — so its behaviour cannot be observed by launching the runtime. Without this, the
 * only way to check a change to the progression mirror was to ask a human to play the game, which
 * is a terrible way to find an off-by-one.
 *
 * It fakes exactly the game accessors progress.c uses, backed by plain arrays.
 */

#ifndef BANJOCOOP_STUB_WORLD_INTERNAL_H
#define BANJOCOOP_STUB_WORLD_INTERNAL_H

#include <stdint.h>
#include <stdio.h>

typedef uint32_t u32;
typedef int32_t s32;
typedef uint8_t u8;
typedef uint16_t u16;
#define FALSE 0
#define TRUE 1
typedef int bool_t;

/* The real headers make these distinct enums; ints are enough to exercise the indexing. */
enum file_progress_e { FILEPROG_DUMMY = 0 };
enum jiggy_e { JIGGY_DUMMY = 0 };
enum honeycomb_e { HONEYCOMB_DUMMY = 0 };
enum mumbotoken_e { MUMBOTOKEN_DUMMY = 0 };

#include "banjocoop/protocol.h"

#define WORLD_MAX_JIGGIES 0x65u
#define WORLD_MAX_HONEYCOMBS 0x19u
#define WORLD_MAX_TOKENS 126u

/* The fake save. Two of them, so a test can act as host and client at once. */
#define STUB_FILEPROG 0x124
#define STUB_JIGGY 0x65
#define STUB_HONEYCOMB 0x19
#define STUB_TOKEN 126

typedef struct {
    u8 fileprog[STUB_FILEPROG];
    u8 jiggy[STUB_JIGGY];
    u8 honeycomb[STUB_HONEYCOMB];
    u8 token[STUB_TOKEN];
} StubSave;

extern StubSave *g_stub;

int fileProgressFlag_get(enum file_progress_e i);
void fileProgressFlag_set(enum file_progress_e i, s32 v);
u32 jiggyscore_isCollected(enum jiggy_e i);
void jiggyscore_setCollected(s32 i, s32 v);
int honeycombscore_get(enum honeycomb_e i);
void honeycombscore_set(enum honeycomb_e i, s32 v);
int mumboscore_get(enum mumbotoken_e i);
void mumboscore_set(enum mumbotoken_e i, s32 v);
s32 item_getCount(s32 item);

/* Minimal prop/cube surface, so the note-mirror path compiles in the harness. A note prop here is
 * just "is it still alive", which is exactly what the real code reads. */
typedef struct { u32 unk8_4; } StubSpriteProp;
typedef struct { StubSpriteProp spriteProp; u32 is_3d; u32 is_actor; } Prop;
typedef struct { u32 prop2Cnt; Prop *prop2Ptr; } Cube;

extern struct StubCubeList {
    Cube *cubes;
    s32 cubeCnt;
    Cube *unk3C;
    Cube *unk40;
} sCubeList;

u32 bc_prop_is_note(Prop *prop);
void bc_note_mark(u32 map_id, u32 note_index);
u32 bc_note_collected(u32 map_id, u32 note_index);
extern u32 bc_map_id;
extern u32 bc_is_host;

/* Minimal actor surface, so the entrance-refresh path compiles in the harness. */
typedef struct { s32 despawn_flag; s32 modelCacheIndex; s32 volatile_initialized; } Actor;
typedef struct { s32 cnt; s32 max_cnt; Actor *data; } ActorArray;
#define ACTOR_20E_MM_ENTRANCE_DOOR 0x20E
#define ACTOR_20F_RBB_ENTRANCE_DOOR 0x20F
#define ACTOR_210_BGS_ENTRANCE_DOOR 0x210
#define ACTOR_211_TCC_ENTRANCE_CHEST_LID 0x211
#define ACTOR_212_CC_ENTRANCE_BARS 0x212
#define ACTOR_226_GV_ENTRANCE 0x226
#define ACTOR_228_MMM_ENTRANCE_DOOR 0x228
#define ACTOR_234_CCW_ENTRANCE_DOOR 0x234
#define ACTOR_235_FP_ENTANCE_DOOR 0x235
#define ACTOR_2E5_DOOR_OF_GRUNTY 0x2E5
#define ITEM_26_JIGGY_TOTAL 0x26

#define recomp_printf(...) ((void)0)

#endif
