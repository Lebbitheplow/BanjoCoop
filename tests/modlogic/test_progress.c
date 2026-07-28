/* Does the progression mirror actually carry a host's save to a client?
 *
 * This compiles the real src/mod/progress.c against fake game accessors, so the answer comes from
 * running the actual shipped logic rather than from reading it. The mod's code only loads when a
 * game session starts, which needs a controller — so this is the only way to check the mirror
 * without asking somebody to play Banjo-Kazooie.
 */

#include <stdio.h>
#include <string.h>

#include "world_internal.h"
#include "progress.h"

StubSave *g_stub = NULL;

static StubSave s_host;
static StubSave s_client;

int fileProgressFlag_get(enum file_progress_e i) { return g_stub->fileprog[(int)i]; }
void fileProgressFlag_set(enum file_progress_e i, s32 v) { g_stub->fileprog[(int)i] = (u8)(v != 0); }
u32 jiggyscore_isCollected(enum jiggy_e i) { return g_stub->jiggy[(int)i]; }
void jiggyscore_setCollected(s32 i, s32 v) { g_stub->jiggy[i] = (u8)(v != 0); }
int honeycombscore_get(enum honeycomb_e i) { return g_stub->honeycomb[(int)i]; }
void honeycombscore_set(enum honeycomb_e i, s32 v) { g_stub->honeycomb[(int)i] = (u8)(v != 0); }
int mumboscore_get(enum mumbotoken_e i) { return g_stub->token[(int)i]; }
void mumboscore_set(enum mumbotoken_e i, s32 v) { g_stub->token[(int)i] = (u8)(v != 0); }
s32 item_getCount(s32 item) { (void)item; return 0; }

/* A lair with a Mumbo's Mountain door that has already latched itself shut — exactly the state a
 * client is in when it walks into the lobby before the host's mirror arrives. */
struct StubCubeList sCubeList = {0, 0, 0, 0};
u32 bc_map_id = 2;
u32 bc_is_host = 0;

/* One map's worth of note props, plus a non-note prop between them so the walk has to skip
 * something — a walk that counted everything would still pass a test where every prop is a note. */
#define TEST_NOTES 8
static Prop s_props[TEST_NOTES * 2];
static Cube s_cube;

u32 bc_prop_is_note(Prop *prop) { return !prop->is_3d && !prop->is_actor; }

static u8 s_note_marks[96];
void bc_note_mark(u32 map_id, u32 i) { (void)map_id; if (i < 96) s_note_marks[i] = 1; }
u32 bc_note_collected(u32 map_id, u32 i) { (void)map_id; return i < 96 ? s_note_marks[i] : 0; }

static void build_map(void) {
    for (int i = 0; i < TEST_NOTES * 2; i++) {
        int is_note = (i % 2) == 0;
        s_props[i].is_3d = is_note ? 0 : 1;   /* odd ones are model props, not notes */
        s_props[i].is_actor = 0;
        s_props[i].spriteProp.unk8_4 = 1;     /* alive */
    }
    s_cube.prop2Cnt = TEST_NOTES * 2;
    s_cube.prop2Ptr = s_props;
    sCubeList.cubes = &s_cube;
    sCubeList.cubeCnt = 1;
    sCubeList.unk3C = 0;
    sCubeList.unk40 = 0;
}

/* note index N is prop 2N, because every other prop is not a note */
static Prop *note_prop(int n) { return &s_props[n * 2]; }

static Actor s_actors[2];
static ActorArray s_array;
ActorArray *suBaddieActorArray = NULL;

static int g_failures = 0;

static void check(int cond, const char *what) {
    printf("  [%s] %s\n", cond ? " ok " : "FAIL", what);
    if (!cond) {
        g_failures++;
    }
}

/* The real thing: FILEPROG_31_MM_OPEN and FILEPROG_5D_MM_PUZZLE_PIECES_PLACED are what actually
 * gate entry to Mumbo's Mountain (lair/code_0.c:1025). If those two do not survive the trip, a
 * client stands in front of a closed mountain — which is exactly the reported symptom. */
#define FILEPROG_31_MM_OPEN 0x31
#define FILEPROG_5D_MM_PUZZLE 0x5D

int main(void) {
    bc_outgoing out;
    bc_incoming in;
    memset(&out, 0, sizeof(out));
    memset(&in, 0, sizeof(in));
    memset(&s_host, 0, sizeof(s_host));
    memset(&s_client, 0, sizeof(s_client));

    printf("test: progression mirror carries a host's save to a client\n");

    /* A host that has opened Mumbo's Mountain. */
    g_stub = &s_host;
    s_host.fileprog[FILEPROG_31_MM_OPEN] = 1;
    s_host.fileprog[FILEPROG_5D_MM_PUZZLE] = 1;
    s_host.fileprog[0x123] = 1; /* the very last flag, to catch a short walk */
    s_host.jiggy[1] = 1;
    s_host.jiggy[0x64] = 1; /* last jiggy */
    s_host.honeycomb[0x18] = 1;
    s_host.token[125] = 1;

    progress_publish(&out);
    check(out.progress.valid == 1, "host publishes a valid mirror");

    /* Hand it to a client with a completely empty save. */
    in.progress = out.progress;
    g_stub = &s_client;
    u32 changed = progress_apply(&in);
    check(changed > 0, "client applies changes from an empty save");

    check(s_client.fileprog[FILEPROG_31_MM_OPEN] == 1, "MM_OPEN reaches the client");
    check(s_client.fileprog[FILEPROG_5D_MM_PUZZLE] == 1, "MM puzzle-complete reaches the client");
    check(s_client.fileprog[0x123] == 1, "the last file-progress flag reaches the client");
    check(s_client.jiggy[1] == 1, "first jiggy reaches the client");
    check(s_client.jiggy[0x64] == 1, "last jiggy reaches the client");
    check(s_client.honeycomb[0x18] == 1, "last honeycomb reaches the client");
    check(s_client.token[125] == 1, "last Mumbo token reaches the client");

    /* Every store must land in its own region: a bit-cursor slip would smear one store's bits into
     * the next, which is far worse than not syncing at all. */
    check(memcmp(s_host.fileprog, s_client.fileprog, STUB_FILEPROG) == 0,
          "the whole file-progress array matches the host exactly");
    check(memcmp(s_host.jiggy + 1, s_client.jiggy + 1, STUB_JIGGY - 1) == 0,
          "the whole jiggy array matches the host exactly");
    check(memcmp(s_host.honeycomb + 1, s_client.honeycomb + 1, STUB_HONEYCOMB - 1) == 0,
          "the whole honeycomb array matches the host exactly");
    check(memcmp(s_host.token + 1, s_client.token + 1, STUB_TOKEN - 1) == 0,
          "the whole token array matches the host exactly");

    /* Steady state: applying the same mirror again must be a no-op. */
    changed = progress_apply(&in);
    check(changed == 0, "re-applying the same mirror changes nothing");

    /* Authoritative in both directions: a client's stray flag is cleared. */
    s_client.fileprog[0x40] = 1;
    changed = progress_apply(&in);
    check(changed == 1 && s_client.fileprog[0x40] == 0,
          "a flag the host does not have is cleared on the client");

    /* Notes: the host's dead notes must reach the client, and land on the SAME notes. */
    build_map();
    memset(s_note_marks, 0, sizeof(s_note_marks));
    bc_map_id = 2;
    note_prop(1)->spriteProp.unk8_4 = 0; /* host collected notes 1, 4 and 7 */
    note_prop(4)->spriteProp.unk8_4 = 0;
    note_prop(7)->spriteProp.unk8_4 = 0;

    g_stub = &s_host;
    bc_outgoing nout;
    memset(&nout, 0, sizeof(nout));
    progress_publish(&nout);
    check(nout.progress.note_map == 2, "mirror carries the map its notes describe");

    /* Client: a fresh map where every note is still standing. */
    build_map();
    memset(s_note_marks, 0, sizeof(s_note_marks));
    bc_incoming nin;
    memset(&nin, 0, sizeof(nin));
    nin.progress = nout.progress;
    g_stub = &s_client;
    progress_apply(&nin);

    check(note_prop(1)->spriteProp.unk8_4 == 0, "note 1 is removed on the client");
    check(note_prop(4)->spriteProp.unk8_4 == 0, "note 4 is removed on the client");
    check(note_prop(7)->spriteProp.unk8_4 == 0, "note 7 is removed on the client");
    check(note_prop(0)->spriteProp.unk8_4 == 1 && note_prop(2)->spriteProp.unk8_4 == 1 &&
              note_prop(3)->spriteProp.unk8_4 == 1 && note_prop(5)->spriteProp.unk8_4 == 1 &&
              note_prop(6)->spriteProp.unk8_4 == 1,
          "no other note is touched - the right ones vanish, not just the right number");

    /* A mirror for a different map must not touch this one's notes. */
    build_map();
    nin.progress.note_map = 99;
    progress_apply(&nin);
    check(note_prop(1)->spriteProp.unk8_4 == 1, "another map's note state is ignored");
    sCubeList.cubes = 0;
    sCubeList.cubeCnt = 0;

    /* A door that already decided it was shut must be told to look again, or the client stands in
     * front of a closed world holding every flag that opens it. */
    s_actors[0].modelCacheIndex = ACTOR_20E_MM_ENTRANCE_DOOR;
    s_actors[0].volatile_initialized = 1;
    s_actors[1].modelCacheIndex = 0x999; /* something unrelated, must be left alone */
    s_actors[1].volatile_initialized = 1;
    s_array.cnt = 2;
    s_array.data = s_actors;
    suBaddieActorArray = &s_array;

    g_stub = &s_client;
    s_client.fileprog[FILEPROG_31_MM_OPEN] = 0; /* force a change on the next apply */
    changed = progress_apply(&in);
    check(changed > 0, "a re-opened flag counts as a change");
    check(s_actors[0].volatile_initialized == 0,
          "the MM entrance door is told to re-check whether it is open");
    check(s_actors[1].volatile_initialized == 1, "unrelated actors are left alone");
    suBaddieActorArray = NULL;

    /* The guard that stops an unloaded host wiping everyone. */
    memset(&s_host, 0, sizeof(s_host));
    g_stub = &s_host;
    bc_outgoing empty;
    memset(&empty, 0, sizeof(empty));
    progress_publish(&empty);
    check(empty.progress.valid == 0, "an all-zero save is never published");

    if (g_failures == 0) {
        printf("\nmod logic tests passed\n");
        return 0;
    }
    printf("\n%d check(s) failed\n", g_failures);
    return 1;
}
