/* BanjoCoop's public API — for other mods.
 *
 * Copy this header into your own mod. Everything here is resolved from BanjoCoop at load time, so
 * declare BanjoCoop as an optional dependency and check `banjocoop_is_connected()` before relying
 * on any of it — your mod must still work when BanjoCoop is not installed or nobody is connected.
 *
 * What this gives you is the hard part already solved: a reliable, ordered, attributed message
 * that reaches every other player, routed to the ones who care. What you send under your tag is
 * entirely your own business.
 */

#ifndef BANJOCOOP_API_H
#define BANJOCOOP_API_H

#include "modding.h"

/* Session state. All safe to call when BanjoCoop is absent — they return 0. */
RECOMP_IMPORT("banjocoop", u32 banjocoop_is_connected(void));
RECOMP_IMPORT("banjocoop", u32 banjocoop_local_player_id(void));
RECOMP_IMPORT("banjocoop", u32 banjocoop_player_count(void));
/* 1 if this peer is the session's authority. Use it to decide who resolves something, exactly as
 * BanjoCoop does for collectibles — otherwise every player resolves it and you count it N times. */
RECOMP_IMPORT("banjocoop", u32 banjocoop_is_host(void));

/* Send under your own tag. Pick something unlikely to collide — a hash of your mod id is fine.
 *
 * `scope` decides who hears it:
 *   0  everyone in the session
 *   1  only players in the same map
 *   2  only players in the same level
 *
 * Silently does nothing when disconnected, so callers need no special case for single player.
 */
RECOMP_IMPORT("banjocoop", void banjocoop_send(u32 tag, u32 scope, u32 a, u32 b));

#define BANJOCOOP_SCOPE_ALL 0u
#define BANJOCOOP_SCOPE_MAP 1u
#define BANJOCOOP_SCOPE_LEVEL 2u

/* Fired for each message from another player. Never fired for your own.
 *
 * `from` is the sending player's id, stamped by the host from the connection it arrived on — so
 * it can be trusted for anything you would otherwise have to verify yourself.
 */
/* Subscribe with:
 *     RECOMP_CALLBACK("banjocoop", banjocoop_on_message)
 *     void my_handler(u32 from, u32 tag, u32 a, u32 b) { ... }
 */

#endif
