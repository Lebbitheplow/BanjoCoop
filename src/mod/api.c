/* BanjoCoop's public API — the implementation behind banjocoop_api.h.
 *
 * The plan singles this out as what turned SM64CoopDX from a mod into a platform, and the reason
 * is worth stating: everything else in this project is BanjoCoop deciding what is worth
 * replicating. This lets another mod decide for itself, and inherit the parts that were hard —
 * reliable delivery, ordering, attribution that cannot be spoofed, and routing by map or level.
 *
 * Kept deliberately small. A wide API is a promise to keep it working; five functions and one
 * event are enough to build on and cheap to honour.
 */

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "banjocoop/protocol.h"
#include "world_internal.h"
#include "api.h"

/* Filled in each frame from the incoming snapshot, so the exports can answer without the caller
 * needing anything from us. */
static u32 s_connected = 0;
static u32 s_local_id = 0;
static u32 s_player_count = 0;
static u32 s_is_host = 0;

void api_begin_frame(bc_incoming *inc) {
    s_connected = inc->connected;
    s_local_id = inc->local_player_id;
    s_player_count = inc->connected ? (inc->remote_count + 1u) : 0u;
    s_is_host = inc->is_host;
}

RECOMP_EXPORT u32 banjocoop_is_connected(void) {
    return s_connected;
}

RECOMP_EXPORT u32 banjocoop_local_player_id(void) {
    return s_local_id;
}

RECOMP_EXPORT u32 banjocoop_player_count(void) {
    return s_player_count;
}

RECOMP_EXPORT u32 banjocoop_is_host(void) {
    return s_is_host;
}

/* Scope rides in the event's own map and level fields rather than in the payload.
 *
 * That is not a trick to save a word — it is what lets the host's existing router handle these
 * without knowing they are somebody else's messages. A message for everyone claims no map and no
 * level; one scoped to a map claims that map. The router already decides who wants an event by
 * exactly that test, so a mod gets free-roam-correct delivery without having to know free-roam
 * exists.
 *
 * Two payload words rather than three, because the tag has to travel too and three fields is what
 * an event carries. Two values plus a tag covers "something happened to object N", which is what
 * this is for; anything larger wants its own state, not a message.
 */
RECOMP_EXPORT void banjocoop_send(u32 tag, u32 scope, u32 a, u32 b) {
    if (!s_connected) {
        return; /* single player is not an error, it is just quiet */
    }
    u32 map = (scope == BANJOCOOP_SCOPE_MAP) ? bc_map_id : 0u;
    u32 level = (scope == BANJOCOOP_SCOPE_LEVEL) ? bc_level_id : 0u;
    bc_queue_event(BC_EV_CUSTOM, map, level, tag, a, b);
}

RECOMP_DECLARE_EVENT(banjocoop_on_message(u32 from, u32 tag, u32 a, u32 b));

void api_deliver(const bc_event *ev) {
    /* Never fired for our own — a mod that wanted to react to its own call would just call it. */
    if (ev->origin == s_local_id) {
        return;
    }
    banjocoop_on_message(ev->origin, ev->a, ev->b, ev->c);
}
