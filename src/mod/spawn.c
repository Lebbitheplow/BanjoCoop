/* Stop a non-owner ever creating its own copy of a projectile.
 *
 * Every peer runs enemy AI (docs/symbols.md §18), so every peer's Chimpy throws its own orange —
 * aimed at whoever is nearest *there*. Only the owner's is real; the rest have to go. Until now
 * they went by being spawned and then despawned a frame or two later, which is a visible flicker:
 * world_apply runs the deferred despawns near the top of the frame hook (main.c) but
 * projectile_sync, which queues them, runs after it, so a projectile noticed this frame survives
 * until the next one.
 *
 * Preventing the spawn is what next.md §5 asked for, and this is the only place it can be done.
 *
 * ---- why this is a patch and not a hook -----------------------------------------------------
 *
 * The decision needs the Actor the spawn produced, and only a RECOMP_PATCH can see a return value:
 * return hooks get a clobbered context (§14). This is BanjoCoop's only patch, and patches are
 * exclusive — any other mod patching __actor_spawnWithYaw_s32 conflicts with this one. It is worth
 * the slot because it is the single funnel: actor_spawnWithYaw_s32 / _f32 / _s16,
 * spawn_child_actor and func_803055E0 all route through it, so one patch covers every way an enemy
 * can throw something. Verified against the base recomp's 145 patches — it patches marker_init,
 * __codeA5BC0_initProp2Ptr and a long list of *_draw functions, but nothing on the spawn path.
 *
 * ---- why suppression is a despawn and never a NULL -------------------------------------------
 *
 * The obvious implementation is to return NULL for a suppressed actor. It would crash.
 * __chSnowman_spawnSnowball (core2/ch/snowman.c) writes straight through the result with no check.
 * chConga does check, but that is one caller's habit, not a contract — §13 is explicit that no
 * function pointer here is NULL-checked, and this is the same rule for return values.
 *
 * So the actor is created exactly as normal and then immediately despawned. The caller gets a
 * valid Actor to write velocities into and never knows; the actor update loop skips despawn_flag
 * actors, so its AI never runs; and the end-of-frame flush frees it. One wasted actor slot for a
 * fraction of a frame, against a guaranteed crash the moment a Sir Slush throws a snowball.
 */

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "prop.h"
#include "actor.h"

#include "banjocoop/protocol.h"
#include "enemy.h"
#include "projectile.h"
#include "world.h"

/* The spawn table this function walks. Data symbols, resolved by address from
 * bk.us.rev0.datasyms.toml exactly as suBaddieActorArray and sCubeList already are. */
extern s32 sSpawnableActorSize;
extern ActorSpawn *sSpawnableActorList;
/* -> src/core2/code_98CB0.c. Returns 1; the original calls it, so we do too rather than fold it
 * away — a patch that quietly drops a call is a patch that behaves differently from the function
 * it replaced. */
extern s32 dummy_func_80320248(void);

/* Set while the world layer is applying somebody else's change. -> world.c */
extern u32 bc_online;

/* Whether this actor is one we should not have made.
 *
 * Every condition here is a reason to leave the spawn alone, and the order is cheapest first. The
 * default in every uncertain case is to allow it: a projectile that should have been suppressed
 * costs a frame of flicker, which is what this file exists to remove, while one wrongly suppressed
 * is a shot that never happens and possibly a fight that cannot be won.
 */
static u32 suppressed(Actor *actor) {
    if (!bc_online) {
        return 0; /* single player spawns everything, as it should */
    }
    /* We are the one publishing this map's projectiles — ours are the real ones. */
    if (enemy_owns_objects_here()) {
        return 0;
    }
    /* This is the adopted copy of the owner's projectile, spawned by projectile.c a moment ago.
     * Suppressing it would leave no projectile at all on any client. */
    if (projectile_is_adopting()) {
        return 0;
    }
    return marker_is_projectile(actor->marker);
}

/* Reimplemented verbatim from core2/code_7AF80.c, plus the suppression check.
 *
 * The body has to be copied because a patch replaces the function outright; keep it in step if the
 * decomp's ever changes. Everything above the marked line is the original. */
RECOMP_PATCH Actor *__actor_spawnWithYaw_s32(enum actor_e arg0, s32 pos[3], s32 rot) {
    s32 i;
    Actor *spawned = NULL;

    arg0 = (!dummy_func_80320248()) ? (ACTOR_4_BIGBUTT) : (arg0);
    for (i = 0; i < sSpawnableActorSize; i++) {
        /* Cast because actorId is a signed s16 and arg0 an enum; the decomp compares them
         * directly, which the mod's stricter warning set rejects. Same comparison either way —
         * every real actor id is positive and well inside 16 bits. */
        if ((s32)arg0 == (s32)sSpawnableActorList[i].infoPtr->actorId) {
            spawned = sSpawnableActorList[i].spawnFunc(pos, rot, sSpawnableActorList[i].infoPtr,
                                                       sSpawnableActorList[i].unk8);
            break;
        }
    }

    /* ---- ours from here ---- */

    /* marker_despawn dereferences the marker, and a dynamically spawned actor is exactly the case
     * where one can be absent (§13). */
    if (spawned != NULL && spawned->marker != NULL && suppressed(spawned)) {
        marker_despawn(spawned->marker);
    }

    return spawned;
}
