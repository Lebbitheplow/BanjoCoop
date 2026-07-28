/* Bosses and the Gruntilda fight — Phase 8.
 *
 * Ordinary enemies need none of this. Their behaviour is a consequence of position and damage,
 * both of which already agree on every machine, so letting each peer simulate them and correcting
 * the result is enough.
 *
 * A boss is not that. A boss is a scripted sequence, and a peer that falls a phase behind is
 * fighting a different fight in the same room — one player watching a death animation while
 * another is still being chased. Two things fix that, and neither needed the bespoke per-boss work
 * the plan expected:
 *
 *   - `subaddie_set_state` is the generic state-machine driver for most actors in the game, so
 *     replicating that one call keeps every boss's phases in step without knowing anything about
 *     any particular boss.
 *   - Gruntilda drives her own six-phase sequence through `chfinalboss_setPhase`, plus a handful
 *     of discrete moments — a Jinjo statue appearing, the Jinjonator's strike, her defeat — that
 *     no state change describes. Those are replicated explicitly.
 *
 * The final fight matters more than the rest put together: a co-op mod that cannot finish the game
 * has not finished its job.
 */

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "prop.h"
#include "actor.h"
#include "bkrecomp_api.h"

#include "banjocoop/protocol.h"
#include "boss.h"
#include "enemy.h"

extern ActorArray *suBaddieActorArray;
extern void subaddie_set_state(Actor *actor, u32 state);
extern void chfinalboss_setPhase(ActorMarker *marker, s32 phase_id);
extern void chfinalboss_spawnStatue(s32 statue_id);
extern void chfinalboss_setJinjoStatueActivated(s32 state);
extern void chfinalboss_setBossDefeated(void);
extern void chjinjonator_attack(ActorMarker *marker, s32 hit_count, s32 mirrored);
extern void chjinjonator_finalAttack(ActorMarker *marker);

/* Which markers are bosses.
 *
 * Deliberately much smaller than the enemy list. Replicating every actor's state change would
 * flood the reliable channel — `subaddie_set_state` fires constantly across seventy-odd actor
 * types — and ordinary enemies do not need it. This is only the fights where being a phase apart
 * would be visible. */
static const u16 k_boss_markers[] = {
    MARKER_7_CONGA,
    MARKER_A5_NIPPER,
    MARKER_16C_NIPPER,
    MARKER_C8_MR_VILE,
    MARKER_BC_GOBI_1,
    MARKER_BF_GOBI_2,
    MARKER_C3_GOBI_3,
    MARKER_1A1_BOSS_BOOM_BOX_LARGEST,
    MARKER_1A2_BOSS_BOOM_BOX_LARGE,
    MARKER_1A3_BOSS_BOOM_BOX_MEDIUM,
    MARKER_1A4_BOSS_BOOM_BOX_SMALL,
    MARKER_48_NAPPER,
    MARKER_49_MOTZHAND,
    MARKER_20B_WOZZA,
    /* The final fight. */
    MARKER_25E_GRUNTILDA_FINAL_BOSS,
    MARKER_27B_BOSS_JINJO_ORANGE,
    MARKER_27C_BOSS_JINJO_GREEN,
    MARKER_27D_BOSS_JINJO_PINK,
    MARKER_27E_BOSS_JINJO_YELLOW,
    MARKER_276_STONE_JINJO,
    MARKER_27A_JINJO_STATUE_BASE,
    MARKER_27F_JINJONATOR_STATUE_BASE,
    MARKER_285_JINJONATOR,
};

#define BOSS_MARKER_COUNT (sizeof(k_boss_markers) / sizeof(k_boss_markers[0]))

/* Set while applying somebody else's phase change, so our own hook does not send it back. */
static u32 s_applying_boss = 0;

u32 bc_marker_is_boss(ActorMarker *marker) {
    if (marker == NULL) {
        return 0;
    }
    for (u32 i = 0; i < BOSS_MARKER_COUNT; i++) {
        if ((u32)marker->id == (u32)k_boss_markers[i]) {
            return 1;
        }
    }
    return 0;
}

static Actor *find_boss(u32 net_id) {
    if (suBaddieActorArray == NULL) {
        return NULL;
    }
    for (s32 i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (actor->despawn_flag || !bc_marker_is_boss(actor->marker)) {
            continue;
        }
        if (bkrecomp_get_marker_spawn_index(actor->marker) == net_id) {
            return actor;
        }
    }
    return NULL;
}

/* Grunty herself, wherever she is — the fight events all act on her and there is only ever one. */
static ActorMarker *find_final_boss(void) {
    if (suBaddieActorArray == NULL) {
        return NULL;
    }
    for (s32 i = 0; i < suBaddieActorArray->cnt; i++) {
        Actor *actor = &suBaddieActorArray->data[i];
        if (!actor->despawn_flag && actor->marker != NULL &&
            (u32)actor->marker->id == (u32)MARKER_25E_GRUNTILDA_FINAL_BOSS) {
            return actor->marker;
        }
    }
    return NULL;
}

/* ---- outgoing ---------------------------------------------------------------------------------
 *
 * All entry hooks: only entry hooks may read arguments (docs §14).
 */

RECOMP_HOOK("subaddie_set_state") void banjocoop_on_actor_state(Actor *actor, u32 state) {
    if (s_applying_boss || actor == NULL || !bc_marker_is_boss(actor->marker)) {
        return;
    }
    if ((u32)actor->state == state) {
        return; /* re-asserting the same phase is not a transition */
    }
    bc_boss_report_state(bkrecomp_get_marker_spawn_index(actor->marker), state);
}

RECOMP_HOOK("chfinalboss_setPhase") void banjocoop_on_fight_phase(ActorMarker *marker, s32 phase) {
    if (s_applying_boss) {
        return;
    }
    bc_boss_report_fight(BC_FIGHT_PHASE, (u32)phase, 0);
}

RECOMP_HOOK("chfinalboss_spawnStatue") void banjocoop_on_fight_statue(s32 statue_id) {
    if (s_applying_boss) {
        return;
    }
    bc_boss_report_fight(BC_FIGHT_SPAWN_STATUE, (u32)statue_id, 0);
}

RECOMP_HOOK("chfinalboss_setJinjoStatueActivated") void banjocoop_on_fight_statue_active(s32 on) {
    if (s_applying_boss) {
        return;
    }
    bc_boss_report_fight(BC_FIGHT_STATUE_ACTIVE, on != 0 ? 1u : 0u, 0);
}

RECOMP_HOOK("chfinalboss_setBossDefeated") void banjocoop_on_fight_defeated(void) {
    if (s_applying_boss) {
        return;
    }
    bc_boss_report_fight(BC_FIGHT_DEFEATED, 0, 0);
}

/* The Jinjonator's strikes are the damage that actually ends the fight, and they are driven by
 * the Jinjos each player rescued — so they must land for everyone, not just whoever triggered
 * them. */
RECOMP_HOOK("chjinjonator_attack")
void banjocoop_on_jinjonator(ActorMarker *marker, s32 hit_count, s32 mirrored) {
    if (s_applying_boss) {
        return;
    }
    bc_boss_report_fight(BC_FIGHT_JINJONATOR, (u32)hit_count, mirrored != 0 ? 1u : 0u);
}

RECOMP_HOOK("chjinjonator_finalAttack") void banjocoop_on_jinjonator_final(ActorMarker *marker) {
    if (s_applying_boss) {
        return;
    }
    bc_boss_report_fight(BC_FIGHT_FINAL_BLOW, 0, 0);
}

/* ---- incoming ---------------------------------------------------------------------------------- */

void boss_apply_state(u32 net_id, u32 state) {
    Actor *actor = find_boss(net_id);
    if (actor == NULL) {
        return;
    }
    s_applying_boss = 1;
    subaddie_set_state(actor, state);
    s_applying_boss = 0;
}

void boss_apply_fight(u32 what, u32 a, u32 b) {
    ActorMarker *grunty = find_final_boss();

    s_applying_boss = 1;
    switch (what) {
        case BC_FIGHT_PHASE:
            if (grunty != NULL) {
                chfinalboss_setPhase(grunty, (s32)a);
            }
            break;
        case BC_FIGHT_SPAWN_STATUE:
            chfinalboss_spawnStatue((s32)a);
            break;
        case BC_FIGHT_STATUE_ACTIVE:
            chfinalboss_setJinjoStatueActivated((s32)a);
            break;
        case BC_FIGHT_DEFEATED:
            chfinalboss_setBossDefeated();
            break;
        case BC_FIGHT_JINJONATOR:
            if (grunty != NULL) {
                chjinjonator_attack(grunty, (s32)a, (s32)b);
            }
            break;
        case BC_FIGHT_FINAL_BLOW:
            if (grunty != NULL) {
                chjinjonator_finalAttack(grunty);
            }
            break;
        default:
            break;
    }
    s_applying_boss = 0;
}
