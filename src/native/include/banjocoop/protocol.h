/* BanjoCoop wire protocol — shared by the MIPS-side mod and the native transport library.
 *
 * This header is compiled twice, by two different compilers, for two different architectures:
 *
 *   - MIPS mod code:  clang -target mips -nostdinc   (big-endian, no libc headers at all)
 *   - native library: gcc x86-64, C++17              (little-endian, full libc)
 *
 * Two consequences drive the design below.
 *
 * 1. No <stdint.h> on the MIPS side. Types come from ultra64.h there instead.
 *
 * 2. RDRAM stores 32-bit words in HOST-NATIVE byte order, not big-endian. This is easy to get
 *    wrong. N64Recomp's accessors are:
 *
 *        #define MEM_W(offset, reg) (*(int32_t*)(rdram + (addr - 0xFFFFFFFF80000000)))
 *        #define MEM_B(offset, reg) (*(int8_t*) (rdram + ((addr ^ 3) - ...)))
 *
 *    MEM_W is a plain native load with no byteswap, and the `^ 3` on byte access is the
 *    giveaway: the layout is word-swapped, so each aligned 32-bit word reads natively while
 *    byte order *within* a word is reversed relative to the original big-endian data.
 *
 *    Consequences:
 *      - The native library reads and writes these structs as ordinary native uint32_t/float.
 *        No byteswapping when touching rdram.
 *      - The WIRE format is explicitly big-endian (network order), converted field by field in
 *        transport.cpp. That costs a dozen bswaps per packet and makes the protocol independent
 *        of both peers' endianness, rather than silently assuming both are little-endian.
 *      - Every field stays 4 bytes wide so the two compilers cannot disagree about padding, and
 *        so no field ever straddles a word boundary.
 */

#ifndef BANJOCOOP_PROTOCOL_H
#define BANJOCOOP_PROTOCOL_H

#ifdef MIPS
#include "ultra64.h"
typedef u32 bc_u32;
typedef s32 bc_s32;
typedef f32 bc_f32;
#else
#include <stdint.h>
typedef uint32_t bc_u32;
typedef int32_t bc_s32;
typedef float bc_f32;
#endif

/* Bumped on any incompatible change to the structs or message set below. Peers with mismatched
 * values are rejected at handshake rather than left to desync mysteriously later.
 *
 * 2: Phase 3 — added level_id to bc_player_state, plus the world-event channel below. */
#define BCNET_PROTOCOL_VERSION 2u

#define BCNET_MAX_PLAYERS 8u
#define BCNET_NAME_LEN 32u

/* ENet channels. State is unreliable-sequenced (a dropped frame is superseded by the next one);
 * events must not be lost, so they get the reliable channel. */
#define BCNET_CHANNEL_STATE 0u
#define BCNET_CHANNEL_EVENT 1u
#define BCNET_CHANNEL_COUNT 2u

/* Message types. */
#define BCNET_MSG_HELLO 1u   /* client -> host: identify and request to join */
#define BCNET_MSG_WELCOME 2u /* host -> client: accepted, here is your player id */
#define BCNET_MSG_REJECT 3u  /* host -> client: refused, with a reason */
#define BCNET_MSG_STATE 4u   /* both ways: per-frame player state */
#define BCNET_MSG_PEERS 5u   /* host -> client: roster changed */
#define BCNET_MSG_EVENT 6u   /* both ways: one bc_event (see below), reliable channel */
#define BCNET_MSG_OBJECTS 7u /* owner -> others: simulated object state, unreliable channel */
#define BCNET_MSG_PROGRESS 8u /* host -> others: the authoritative progression mirror, reliable */
#define BCNET_MSG_SAVEFILE 9u /* host -> joining client: the host's save file, verbatim, reliable */

/* BK's EEPROM is 2 KB. Sent whole rather than diffed — it is smaller than one frame of player
 * state and only travels once per join. */
#define BCNET_MAX_SAVE_BYTES 4096u

#define BCNET_MSG_CHAT 10u /* any peer -> everyone, via the host, reliable */

/* Chat. Short on purpose — this is a line of banter over a shoulder, not a messaging app, and it
 * has to fit on screen next to a game. */
#define BCNET_CHAT_LEN 96u
#define BCNET_CHAT_HISTORY 6u

typedef struct {
    bc_u32 from;                  /* player id, stamped by the host */
    bc_u32 length;                /* 0 for an empty slot */
    bc_u32 text[BCNET_CHAT_LEN / 4]; /* packed 4 chars per word — see the note on rdram above */
} bc_chat_line;

typedef struct {
    bc_u32 count;
    bc_chat_line lines[BCNET_CHAT_HISTORY];
} bc_chat_log;

/* Reject reasons. Distinguishing these matters: a version mismatch and a ROM mismatch look
 * identical in-game (silent desync) but need completely different fixes from the user. */
#define BCNET_REJECT_PROTOCOL 1u
#define BCNET_REJECT_MOD_VERSION 2u
#define BCNET_REJECT_ROM_HASH 3u
#define BCNET_REJECT_FULL 4u

/* Player state flags. */
#define BCNET_STATE_ACTIVE 0x1u /* slot is occupied; puppet should be visible */
#define BCNET_STATE_INGAME 0x2u /* player is in gameplay, not a menu/loading screen */

/* Per-frame player state. This is both the staging-buffer layout in rdram and the wire payload.
 *
 * map_id is present from the very first version even though Phase 1 only ever has one map
 * loaded: free-roam is the stated end goal, and retrofitting a map key into a shipped protocol
 * is far more painful than carrying an unused field for a while.
 */
typedef struct {
    bc_u32 map_id;
    bc_u32 level_id;   /* level_get(); the host routes level-scoped events on this */
    bc_u32 flags;      /* BCNET_STATE_* */
    bc_f32 pos[3];     /* player_getPosition */
    bc_f32 vel[3];     /* baphysics_get_velocity */
    bc_f32 yaw;        /* player_getYaw */
    bc_u32 transform;  /* player_getTransformation — selects the puppet's model */
    bc_u32 anim_id;    /* from the player's AnimCtrl */
    bc_f32 anim_frame; /* ditto; drives puppet animation playback */
} bc_player_state;

/* ---- world events (Phase 3) ---------------------------------------------------------------
 *
 * Player state above is a continuous signal: dropping a frame is harmless because the next one
 * supersedes it. World changes are the opposite — a lost "this note is gone" is a permanent
 * divergence — so they travel as discrete events on the reliable channel instead.
 *
 * The host is authoritative. A client that sees a local world change sends the event to the
 * host; the host decides whether it happened, records it, and relays to whoever needs it. Only
 * the host ever adjudicates, which is what keeps two players grabbing the same note from
 * counting twice.
 */

#define BC_EV_NOTE_STATIC 1u    /* a = note_index. Claimed, deduped, granted. */
#define BC_EV_NOTE_DYNAMIC 2u   /* a unused. Enemy-dropped notes; relayed, not deduped. */
/* 3 is retired. It was NOTE_REVOKE — the host telling the loser of a simultaneous grab to hand
 * the point back. That was wrong: notes are shared progress, so both players should count the note
 * once, and the loser's own pickup already is that count. Deducting dropped them to zero. The
 * receiver-side "have I already recorded this note" check is what prevents double-counting; the
 * host simply drops the duplicate claim. Do not reuse 3 for something else. */
#define BC_EV_JIGGY 4u          /* a = jiggy_id, b = value. Idempotent bit set. */
#define BC_EV_FLAG_FILEPROG 5u  /* a = index, b = value. Permanent progress; goes to everyone. */
#define BC_EV_FLAG_LEVEL 6u     /* a = index, b = value. Routed to peers in the same level. */
#define BC_EV_FLAG_MAP 7u       /* a = index, b = value. Routed to peers in the same map. */
#define BC_EV_HASH 8u           /* a = world hash, b = flag hash. Desync detector. */
/* Multi-bit flag writes. The game has setN variants that write a run of bits directly instead of
 * looping over the single-bit setter, so hooking the latter alone misses them entirely — the
 * jiggy podiums, the sandcastle, and GV's pyramid state all go through these.
 * a = start index, b = value, c = bit length. */
#define BC_EV_FLAG_FILEPROG_N 9u
#define BC_EV_FLAG_LEVEL_N 10u
/* The other two score bitfields. Empty honeycombs and Mumbo tokens are permanent world progress
 * kept in their own stores — neither a flag nor a note — so they need their own events.
 * a = id, b = value. Idempotent bit sets, like BC_EV_JIGGY. */
#define BC_EV_HONEYCOMB 11u
#define BC_EV_MUMBO_TOKEN 12u
/* An enemy died. a = net id. Routed to peers in the same map.
 *
 * Reliable, and separate from the object frame, because death is a discrete fact: a dropped
 * "it's gone" is never superseded, it just leaves that enemy alive forever on one machine. Any
 * peer may raise it — whoever lands the killing blow sees it die locally first. */
#define BC_EV_ENEMY_DEAD 13u
/* A Jinjo was rescued. a = its marker id (MARKER_5A..5E), which is also its identity: there is
 * exactly one of each colour per level. Routed to peers in the same level.
 *
 * Kept out of the progression mirror on purpose. Jinjo credit is `item_adjustByDiffWithHud(
 * ITEM_12_JINJOS, 1 << n)` — an ADD, not a bit set — so crediting one twice carries into the next
 * bit and corrupts the mask. It has to be applied exactly once, guarded on the bit not already
 * being held, which is an event's job rather than a mirror's. */
#define BC_EV_JINJO 14u
/* Somebody hit an enemy. a = net id, b = which collision handler fired. Routed to peers in the
 * same map.
 *
 * This is what makes ganging up work. Every peer simulates enemies independently, so a hit landed
 * on one machine is invisible to the others — an enemy needing three hits could be worked on by
 * two players forever and never die, because each machine only ever saw its own player's hits.
 * Replaying the hit everywhere pools the damage without needing to read or write any enemy's
 * health, which has no generic accessor. */
#define BC_EV_ENEMY_HIT 15u
/* A boss changed phase. a = net id, b = the state it moved to. Routed to peers in the same map.
 *
 * Bosses are not health bars, they are scripted sequences, and a peer a phase behind is fighting
 * a different fight in the same room. Ordinary enemies do not need this — their state is a
 * consequence of position and damage, both of which already agree — but a boss's phase is decided
 * by script and timers that drift.
 *
 * `subaddie_set_state` is the generic state-machine driver for 70-odd actors, so replicating that
 * one call covers every boss's phase transitions without knowing anything about any of them. */
#define BC_EV_BOSS_STATE 16u

/* The Gruntilda fight specifically. a = which moment, b and c its arguments.
 *
 * Her phases do not go through the generic setter — `chfinalboss_setPhase` drives a six-phase
 * sequence of its own — and the fight has discrete moments no state change describes: a Jinjo
 * statue appearing, the Jinjonator's strike, Grunty finally going down. Without these the fight
 * simply cannot be finished together, which would make the whole mod pointless. */
#define BC_EV_FIGHT 17u

#define BC_FIGHT_PHASE 1u          /* b = ch_finalboss_phase_e */
#define BC_FIGHT_SPAWN_STATUE 2u   /* b = ch_bossjinjo_e */
#define BC_FIGHT_STATUE_ACTIVE 3u  /* b = 0/1 */
#define BC_FIGHT_DEFEATED 4u
#define BC_FIGHT_JINJONATOR 5u     /* b = hit count, c = mirrored */
#define BC_FIGHT_FINAL_BLOW 6u

/* Somebody else's mod said something. a/b/c are entirely its own business.
 *
 * This is the difference between a mod and a platform. Everything above is BanjoCoop deciding
 * what is worth replicating; this lets another mod decide for itself, without touching any of it.
 * A mod declares a tag, sends values under it, and receives everyone else's — the replication,
 * ordering, attribution and routing are already solved.
 *
 * Reliable and attributed, like every other event, because a mod author debugging "my thing did
 * not arrive" is a far worse experience than a few extra bytes. */
#define BC_EV_CUSTOM 18u
/* One player picked another up, or put them down. a = the carried player, b = 1 or 0.
 * Routed to peers in the same map. */
#define BC_EV_CARRY 19u
/* Party modes. a/b depend on the moment. Sent to everyone: a round is a session-wide thing, and
 * players scatter across maps in hide and seek by definition. */
#define BC_EV_MODE 20u

#define BC_MODE_START 1u /* a = mode, b = seeker */
#define BC_MODE_FOUND 2u /* a = player found */
#define BC_MODE_END 3u   /* a = winner, or BCNET_MAX_PLAYERS if cancelled */

/* Sized to cover the burst that a map load produces (a run of flag writes as the world sets
 * itself up) without the queue ever being the thing that drops a world change. Overflow is
 * counted and logged rather than silently ignored — a dropped event is a permanent desync, so
 * it must be visible. */
/* Sized against the worst single-frame burst, which is the host announcing a map's already-saved
 * notes on arrival: Mumbo's Mountain has 85 static notes, so a 64-slot queue would silently drop
 * a mostly-completed save's worth of them. Overflow is counted and logged, but not dropping in
 * the first place is better. */
#define BCNET_EVENT_QUEUE 128u

typedef struct {
    bc_u32 kind;     /* BC_EV_* */
    bc_u32 map_id;   /* map the event happened in; routing key for map-scoped kinds */
    bc_u32 level_id; /* level it happened in; routing key for level-scoped kinds */
    /* Who caused it. Stamped by the host from the peer the event arrived on, never by the sender,
     * so it cannot be spoofed — the same reasoning that keeps a client from claiming notes on
     * somebody else's behalf. Receivers use it to say "player2 got a Jiggy!" rather than just
     * silently gaining one. */
    bc_u32 origin;
    bc_u32 a;
    bc_u32 b;
    bc_u32 c;
} bc_event;

typedef struct {
    bc_u32 count;
    bc_u32 dropped; /* cumulative overflow count, for diagnostics */
    bc_event events[BCNET_EVENT_QUEUE];
} bc_event_queue;

/* ---- progression mirror (Phase 6) -----------------------------------------------------------
 *
 * The host's save is authoritative and clients mirror it. This is a *continuous* mirror, not a
 * replay on join, and that distinction is the whole point: a one-shot burst can be dropped,
 * mistimed, or raced against the client's own save load, and when it fails the symptom is a
 * player standing in front of a world that will not open with no way to recover. A mirror
 * resent every second is self-healing — whatever went wrong, the next one fixes it.
 *
 * The payload is the packed bits of the four permanent stores: file progress, jiggies, empty
 * honeycombs and Mumbo tokens. Everything is carried as 4-byte words because rdram's layout makes
 * sub-word access unsafe across the boundary (see the header comment above); the mod packs and
 * unpacks, and the transport moves opaque words without interpreting them.
 *
 * Deliberately NOT mirrored: health, lives, eggs and feathers. Those are per-player inventory —
 * the plan's split — and copying the host's would be actively wrong.
 */

#define BC_PROGRESS_WORDS 20u

/* Notes for the map the host is standing in. 96 slots covers the busiest map (Mumbo's Mountain,
 * 85 static notes).
 *
 * Notes were originally synced by events alone, plus a census of the host's save, plus a sweep on
 * arrival — three mechanisms for one fact. Flags proved that a continuous mirror is what actually
 * survives contact: it self-heals, it does not care about join order, and there is nothing to miss
 * because it is resent every second. Notes get the same treatment.
 *
 * Only the current map travels. Note state is per-map and a whole game's worth would not fit in a
 * packet, but the only map whose notes a player can see is the one they are in. */
#define BC_PROGRESS_NOTE_WORDS 3u

typedef struct {
    bc_u32 valid; /* 0 until the host has published at least once */
    bc_u32 note_map; /* which map note_bits describes */
    bc_u32 note_bits[BC_PROGRESS_NOTE_WORDS];
    bc_u32 words[BC_PROGRESS_WORDS];
} bc_progress;

/* ---- simulated objects (Phase 5) ------------------------------------------------------------
 *
 * Enemies are the opposite of collectibles. A note is one discrete fact that must never be lost,
 * so it travels reliably as an event. An enemy is a continuous signal — position, facing,
 * animation, every frame — where a dropped packet is superseded by the next one and re-sending it
 * would be worse than useless. So they ride the unreliable channel alongside player state.
 *
 * Ownership: exactly one peer simulates a given object and publishes it; everyone else applies
 * what arrives and does not run its AI. That is the model the plan settled on instead of
 * deterministic lockstep, which is not achievable over recompiled code we do not control.
 *
 * Identity is the hard part and is NOT solved by the same scheme as props. A prop's spawn index is
 * fixed static-level-data order, identical everywhere. Actors spawn as play happens, so their
 * order diverges the moment two players trigger different things — see docs/symbols.md §3. Only
 * objects spawned from a map's static spawn list can use their spawn index directly; anything
 * spawned dynamically needs an id assigned by the owner and carried in a spawn event.
 */

#define BCNET_MAX_OBJECTS 24u /* per packet; a map's live enemies near one player */

#define BC_OBJ_ACTIVE 0x1u /* object exists and should be visible */
#define BC_OBJ_DEAD 0x2u   /* owner says it died; receivers play it out and stop applying state */

typedef struct {
    bc_u32 net_id; /* stable within a map — see the identity note above */
    bc_u32 flags;  /* BC_OBJ_* */
    /* Non-zero for objects the receiver may not have: it spawns this actor type rather than
     * looking for one it already owns. This is what lets a projectile — which exists only because
     * an enemy threw it, moments ago, on one machine — appear on every other machine.
     *
     * Carrying it in the state frame rather than in a separate spawn event means spawn, position
     * and disappearance are all described by the same message. A frame that stops mentioning an
     * object is that object being gone; there is no second mechanism to keep in step. */
    bc_u32 actor_id;
    bc_f32 pos[3];
    bc_f32 yaw;
    bc_u32 anim_id;
    bc_f32 anim_frame;
} bc_object_state;

/* One frame of object state for one map. The map id is carried so a receiver can discard a frame
 * that arrives just after it left, rather than applying another map's coordinates to whatever
 * happens to be standing here. */
typedef struct {
    bc_u32 map_id;
    bc_u32 count;
    bc_object_state objects[BCNET_MAX_OBJECTS];
} bc_object_frame;

/* The outgoing staging buffer. The mod appends events as its hooks fire during the frame; pump()
 * drains the whole queue and resets count to 0. */
typedef struct {
    bc_event_queue queue;
    /* Objects this peer owns and is publishing. Left at count 0 by peers that own nothing. */
    bc_object_frame objects;
    /* Host only: the progression mirror. `valid` stays 0 on clients. */
    bc_progress progress;
    /* A line the player just typed, if any. Cleared by pump once sent. */
    bc_chat_line outgoing_chat;
} bc_outgoing;

/* One remote player as handed back to the mod each frame. */
typedef struct {
    bc_u32 player_id;
    bc_u32 flags; /* BCNET_STATE_* */
    bc_player_state state;
} bc_remote_player;

/* The incoming staging buffer the mod reads after each pump().
 *
 * Fixed-size and inlined rather than a pointer-to-array, so the mod can allocate it statically
 * and the native side never has to translate a second rdram pointer.
 */
typedef struct {
    bc_u32 remote_count;
    bc_u32 local_player_id;
    bc_u32 connected;  /* 0 = offline, 1 = connected */
    bc_u32 ping_ms;    /* to host; 0 when hosting */
    bc_u32 is_host;    /* 1 when this peer is the authority */
    bc_remote_player remotes[BCNET_MAX_PLAYERS];
    /* World events accepted since the last pump, in arrival order. Ordering matters: a flag set
     * followed by a flag clear must not be applied the other way round. */
    bc_event_queue events;
    /* Latest object frame from whoever owns the objects in our map. Only the newest is kept —
     * older ones describe positions that have already been superseded. */
    bc_object_frame objects;
    /* Latest progression mirror from the host. `valid` is 0 until one has arrived. */
    bc_progress progress;
    /* Recent chat, newest last. Carried as packed words rather than bytes because everything in
     * this struct is read straight out of rdram, where sub-word access is wrong (see the header
     * comment). The mod unpacks. */
    bc_chat_log chat;
} bc_incoming;

#endif /* BANJOCOOP_PROTOCOL_H */
