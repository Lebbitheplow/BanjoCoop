#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"
#include "recompconfig.h"

#include "core2/ba/physics.h"
#include "core2/anctrl.h"

#include "puppet.h"
#include "world.h"
#include "enemy.h"
#include "ui.h"
#include "revive.h"
#include "api.h"
#include "carry.h"
#include "modes.h"

#include "banjocoop/protocol.h"

/* Bumped on any change that makes builds incompatible with each other. */
#define BANJOCOOP_MOD_VERSION 27u

/* Defined in the decomp but absent from functions.h, so declared here.
 *   player_getYaw  -> src/core2/playerutils.c
 *   map_get        -> src/core2/code_AD5B0.c */
extern f32 player_getYaw(void);
extern enum map_e map_get(void);
extern enum level_e level_get(void);
/* -> src/core2/code_7060.c */
extern AnimCtrl *player_getAnimCtrlPtr(void);

/* Imports from our own native library (banjocoop_net.so).
 *
 * "." is DependencySelf — the mod's own exports, which includes anything listed under
 * `native_libraries` in the manifest. ("*" would mean the base recomp instead.) */
RECOMP_IMPORT(".", u32 bcnet_init(u32 mod_version, const char *name, const u8 *rom_hash));
RECOMP_IMPORT(".", u32 bcnet_host(u32 port));
RECOMP_IMPORT(".", u32 bcnet_join(const char *address, u32 port));
/* Cloudflare tunnel variants: host over a bundled cloudflared tunnel, poll for the generated join
 * code, and join by that code. See src/native/src/tunnel.cpp. */
RECOMP_IMPORT(".", u32 bcnet_host_tunnel(u32 port));
RECOMP_IMPORT(".", u32 bcnet_get_join_code(char *out, u32 max));
RECOMP_IMPORT(".", u32 bcnet_join_tunnel(const char *code));
RECOMP_IMPORT(".", void bcnet_shutdown(void));
RECOMP_IMPORT(".", void bcnet_pump(bc_player_state *local, bc_outgoing *outgoing, bc_incoming *incoming));
RECOMP_IMPORT(".", u32 bcnet_status(void));
RECOMP_IMPORT(".", u32 bcnet_reject_reason(void));
RECOMP_IMPORT(".", void bcnet_set_sim(u32 latency_ms, u32 jitter_ms, u32 loss_permille));
RECOMP_IMPORT(".", void bcnet_set_save_path(const char *path));
RECOMP_IMPORT(".", u32 bcnet_take_host_save(char *out, u32 max));

/* Mirrors bcnet::Status. */
#define BC_STATUS_OFFLINE 0
#define BC_STATUS_HOSTING 1
#define BC_STATUS_CONNECTING 2
#define BC_STATUS_CONNECTED 3
#define BC_STATUS_FAILED 4
#define BC_STATUS_REJECTED 5

#define NETMODE_OFFLINE 0
#define NETMODE_HOST 1
#define NETMODE_JOIN 2

/* Mirrors the "connection" config enum. */
#define CONNECTION_DIRECT 0
#define CONNECTION_TUNNEL 1

/* Longest join code we will read back from the tunnel (a trycloudflare hostname is well under
 * this; a named-tunnel host has room too). */
#define JOIN_CODE_MAX 128u

/* Staging buffers. Static so their addresses are stable for the whole session — the native side
 * receives them as MIPS pointers each frame and translates through TO_PTR. */
static bc_player_state g_local;
static bc_incoming g_incoming;

static u32 g_initialised = 0;
static u32 g_last_status = 0xFFFFFFFF;
static u32 g_frame = 0;

/* Set once the tunnel join code has been logged, so it is announced exactly once per host session. */
static u32 g_join_code_logged = 0;

/* So turning enemy sync off mid-session drops what it had learned, rather than leaving stale
 * per-map enemy ids around. */
static u32 g_enemy_sync_was_on = 0;

/* Last simulation settings pushed to the transport, so we only cross the boundary when they
 * actually change rather than every time we poll. */
static u32 g_sim_latency = 0xFFFFFFFF;
static u32 g_sim_jitter = 0xFFFFFFFF;
static u32 g_sim_loss = 0xFFFFFFFF;

/* How often to re-read the debug sim settings. Polling exists so the latency matrix can be
 * stepped through from the mod menu mid-session; once a second is far more often than a human
 * changes a slider, and reading three config values is trivial next to a frame of gameplay. */
#define SIM_POLL_FRAMES 60u

static void refresh_net_sim(void) {
    u32 latency = (u32)recomp_get_config_double("sim_latency_ms");
    u32 jitter = (u32)recomp_get_config_double("sim_jitter_ms");
    u32 loss = (u32)recomp_get_config_double("sim_loss_permille");

    if (latency == g_sim_latency && jitter == g_sim_jitter && loss == g_sim_loss) {
        return;
    }
    g_sim_latency = latency;
    g_sim_jitter = jitter;
    g_sim_loss = loss;

    bcnet_set_sim(latency, jitter, loss);

    if (latency != 0 || jitter != 0 || loss != 0) {
        recomp_printf("[banjocoop] netsim: %ums +/-%ums, %u.%u%% loss\n", latency, jitter,
                      loss / 10u, loss % 10u);
    } else {
        recomp_printf("[banjocoop] netsim off\n");
    }
}

/* Build fingerprint, compared between peers at handshake.
 *
 * This is deliberately NOT a ROM hash. No mod-facing API exposes the ROM, and it would add
 * little: the runtime already validates the ROM against the recomp's expected hash at startup,
 * so two BanjoRecompiled installs of the same version necessarily run the same rev0 ROM. Peer
 * ROM divergence is structurally impossible here, unlike in SM64CoopDX where players run
 * assorted patched ROMs.
 *
 * What this does catch is protocol/build skew between peers running different builds of
 * BanjoCoop itself, alongside the explicit mod-version check.
 *
 * KNOWN GAP: the genuine desync risk in this architecture is peers having *different sets of
 * other mods* loaded, which changes game behaviour underneath the replication layer. There is no
 * API to enumerate loaded mods yet; when there is, hash their ids+versions in here. Until then a
 * mismatched mod list will produce confusing desyncs with no diagnostic. */
#define BC_BUILD_FINGERPRINT_BYTE (u8)((BCNET_PROTOCOL_VERSION * 31u) + BANJOCOOP_MOD_VERSION)
static const u8 k_build_fingerprint[20] = {
    BC_BUILD_FINGERPRINT_BYTE, BC_BUILD_FINGERPRINT_BYTE, BC_BUILD_FINGERPRINT_BYTE,
    BC_BUILD_FINGERPRINT_BYTE, BC_BUILD_FINGERPRINT_BYTE, BC_BUILD_FINGERPRINT_BYTE,
    BC_BUILD_FINGERPRINT_BYTE, BC_BUILD_FINGERPRINT_BYTE, BC_BUILD_FINGERPRINT_BYTE,
    BC_BUILD_FINGERPRINT_BYTE, BC_BUILD_FINGERPRINT_BYTE, BC_BUILD_FINGERPRINT_BYTE,
    BC_BUILD_FINGERPRINT_BYTE, BC_BUILD_FINGERPRINT_BYTE, BC_BUILD_FINGERPRINT_BYTE,
    BC_BUILD_FINGERPRINT_BYTE, BC_BUILD_FINGERPRINT_BYTE, BC_BUILD_FINGERPRINT_BYTE,
    BC_BUILD_FINGERPRINT_BYTE, BC_BUILD_FINGERPRINT_BYTE,
};

static void banjocoop_start(void) {
    /* Hand the native side our save path. Only the game knows where it is, and only the native
     * side can read or write a file — the host reads its own to send, a client uses the directory
     * to know where to put the host's. */
    char *save_path = (char *)recomp_get_save_file_path();
    if (save_path != NULL) {
        bcnet_set_save_path(save_path);
    }

    char *name = recomp_get_config_string("player_name");
    bcnet_init(BANJOCOOP_MOD_VERSION, name != NULL ? name : "player", k_build_fingerprint);
    if (name != NULL) {
        recomp_free_config_string(name);
    }

    u32 mode = recomp_get_config_u32("netmode");
    u32 port = (u32)recomp_get_config_double("port");
    u32 connection = recomp_get_config_u32("connection");

    g_join_code_logged = 0;

    if (mode == NETMODE_HOST) {
        if (connection == CONNECTION_TUNNEL) {
            recomp_printf("[banjocoop] hosting over Cloudflare tunnel (local port %u)\n", port);
            bcnet_host_tunnel(port);
        } else {
            recomp_printf("[banjocoop] hosting on port %u\n", port);
            bcnet_host(port);
        }
    } else if (mode == NETMODE_JOIN) {
        char *address = recomp_get_config_string("address");
        if (address != NULL) {
            if (connection == CONNECTION_TUNNEL) {
                recomp_printf("[banjocoop] joining tunnel '%s'\n", address);
                bcnet_join_tunnel(address);
            } else {
                recomp_printf("[banjocoop] joining %s:%u\n", address, port);
                bcnet_join(address, port);
            }
            recomp_free_config_string(address);
        }
    }
}

/* Once cloudflared has minted the tunnel URL, surface it in the log so a host using the config
 * menu (rather than the in-game lobby) can read off the code to share. Polled because the URL
 * arrives a few seconds after hosting starts. */
static void poll_join_code(void) {
    if (g_join_code_logged) {
        return;
    }
    char code[JOIN_CODE_MAX];
    if (bcnet_get_join_code(code, JOIN_CODE_MAX) != 0) {
        g_join_code_logged = 1;
        recomp_printf("[banjocoop] tunnel join code (share this): %s\n", code);
    }
}

static void report_status_change(void) {
    u32 status = bcnet_status();
    if (status == g_last_status) {
        return;
    }
    g_last_status = status;

    switch (status) {
        case BC_STATUS_HOSTING:
            recomp_printf("[banjocoop] hosting\n");
            break;
        case BC_STATUS_CONNECTING:
            recomp_printf("[banjocoop] connecting...\n");
            break;
        case BC_STATUS_CONNECTED:
            recomp_printf("[banjocoop] connected as player %u\n", g_incoming.local_player_id);
            break;
        case BC_STATUS_FAILED:
            recomp_printf("[banjocoop] connection failed\n");
            break;
        case BC_STATUS_REJECTED: {
            u32 reason = bcnet_reject_reason();
            /* Naming the reason matters: every one of these looks identical in-game otherwise. */
            const char *text = "unknown";
            if (reason == BCNET_REJECT_PROTOCOL) {
                text = "protocol version mismatch";
            } else if (reason == BCNET_REJECT_MOD_VERSION) {
                text = "mod version mismatch - both players need the same BanjoCoop version";
            } else if (reason == BCNET_REJECT_ROM_HASH) {
                text = "build mismatch - both players need the same BanjoCoop build";
            } else if (reason == BCNET_REJECT_FULL) {
                text = "server is full";
            }
            recomp_printf("[banjocoop] rejected: %s\n", text);
            break;
        }
        default:
            break;
    }
}

/* baphysics_update runs once per frame during gameplay. Entry hook rather than a patch: hooks
 * stack across mods, and the base recomp already patches 146 functions. */
RECOMP_HOOK("baphysics_update") void banjocoop_frame(void) {
    if (!g_initialised) {
        g_initialised = 1;
        banjocoop_start();
        refresh_net_sim();
    }

    g_frame++;

    if ((g_frame % SIM_POLL_FRAMES) == 0) {
        refresh_net_sim();
    }

    /* Gather local state. These are plain native reads/writes: rdram stores 32-bit words in host
     * order, and every field of bc_player_state is 4 bytes wide. */
    f32 pos[3];
    f32 vel[3];
    player_getPosition(pos);
    baphysics_get_velocity(vel);

    g_local.map_id = (bc_u32)map_get();
    g_local.level_id = (bc_u32)level_get();
    g_local.flags = BCNET_STATE_ACTIVE | BCNET_STATE_INGAME;
    g_local.pos[0] = pos[0];
    g_local.pos[1] = pos[1];
    g_local.pos[2] = pos[2];
    g_local.vel[0] = vel[0];
    g_local.vel[1] = vel[1];
    g_local.vel[2] = vel[2];
    g_local.yaw = player_getYaw();
    g_local.transform = (bc_u32)player_getTransformation();

    /* Animation asset index + playback timer, so puppets reproduce the sender's exact pose
     * rather than inferring an animation from velocity. */
    {
        AnimCtrl *anctrl = player_getAnimCtrlPtr();
        if (anctrl != NULL) {
            g_local.anim_id = (bc_u32)anctrl_getIndex(anctrl);
            g_local.anim_frame = anctrl_getAnimTimer(anctrl);
        }
    }

    /* Stamp where we are before pumping: the world hooks fire from deep inside game code during
     * the frame and read this rather than calling map_get() from wherever they happen to be. */
    world_begin_frame(g_local.map_id, g_local.level_id);

    /* The single boundary crossing for the frame. Carries the world events our hooks queued out,
     * and brings back both remote player state and the events the host accepted for us. */
    bcnet_pump(&g_local, world_outgoing(), &g_incoming);

    report_status_change();
    poll_join_code();

    /* Spawn/update/despawn the remote-player puppets. */
    if (g_incoming.connected) {
        puppet_sync(&g_incoming, g_local.map_id);
    } else {
        puppet_clear_all();
    }
    /* Called even when offline: it is what notices the disconnect and drops the session's
     * replicated world state. */
    world_apply(&g_incoming);

    /* Enemies. On by default: players are meant to be able to gang up on one together and watch
     * each other's kills, which only works if it is running for everybody. No peer's AI is ever
     * silenced, so the failure mode if this is wrong is enemies disagreeing about where they are,
     * not enemies frozen or unkillable. */
    u32 enemy_on = recomp_get_config_u32("enemy_sync") != 0;
    if (enemy_on) {
        enemy_sync(&g_incoming, world_outgoing(), g_local.map_id);
    } else if (g_enemy_sync_was_on) {
        enemy_reset();
        world_outgoing()->objects.count = 0;
    }
    g_enemy_sync_was_on = enemy_on;

    /* If the host's save has arrived, adopt it. This is the part that makes the host's save
     * genuinely the session's save: recomp_change_save_file reloads the cartridge save from disk,
     * so from here the client is playing the host's file rather than its own.
     *
     * Their original save is untouched on disk — it comes back by disconnecting and reloading it.
     * Done once; the name is cleared when taken. */
    {
        char host_save[32];
        if (bcnet_take_host_save(host_save, sizeof(host_save)) != 0) {
            recomp_printf("[banjocoop] adopting the host's save file ('%s')\n", host_save);
            recomp_change_save_file(host_save);
        }
    }

    api_begin_frame(&g_incoming);
    modes_update(&g_incoming, g_local.map_id);
    carry_update(&g_incoming, g_local.map_id);
    revive_begin_frame(g_local.map_id);
    revive_update(&g_incoming, g_local.map_id);

    ui_update(&g_incoming, world_outgoing(), g_local.map_id, g_frame);

    /* Heartbeat, always — including when nothing is connected.
     *
     * Its absence is itself the diagnosis: if this line is missing from a log, the frame hook is
     * not running and no amount of replication code was ever going to help. Everything else here
     * only prints on a change or while connected, which makes "silent" ambiguous. */
    if ((g_frame % 60u) == 0) {
        recomp_printf("[banjocoop] hb frame=%u status=%u connected=%u remotes=%u map=%u\n",
                      g_frame, bcnet_status(), g_incoming.connected, g_incoming.remote_count,
                      g_local.map_id);
    }

    /* Temporary Phase 1 telemetry — replaced by the recompui overlay and, in Phase 2, by actual
     * puppets rendered from this data. */
    if ((g_frame % 60) == 0 && g_incoming.connected) {
        /* Log our OWN outgoing position too. Without it, "the other player looks frozen" is
         * ambiguous between "their state never changed" and "we are not applying what arrives". */
        recomp_printf("[banjocoop] p%u ping=%ums remotes=%u local map=%u pos=(%.1f, %.1f, %.1f)\n",
                      g_incoming.local_player_id, g_incoming.ping_ms, g_incoming.remote_count,
                      g_local.map_id, g_local.pos[0], g_local.pos[1], g_local.pos[2]);
        for (u32 i = 0; i < g_incoming.remote_count; i++) {
            bc_remote_player *r = &g_incoming.remotes[i];
            recomp_printf("[banjocoop]   remote p%u map=%u pos=(%.1f, %.1f, %.1f)\n",
                          r->player_id, r->state.map_id,
                          r->state.pos[0], r->state.pos[1], r->state.pos[2]);
        }
    }
}
