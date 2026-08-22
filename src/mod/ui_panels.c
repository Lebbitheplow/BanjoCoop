/* Chat and the lobby — the two panels that take input.
 *
 * Split from the player list because they are a different kind of thing: the list is glanced at
 * and never touched, while these take the controller away from the game while they are open. That
 * is the whole hazard here. A capture left switched on makes the game unplayable, so every path
 * that closes a panel releases it, and both are shut automatically when the situation that
 * justified them ends — the lobby once connected, chat once disconnected.
 */

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"
#include "recompconfig.h"
#include "recompui.h"

#include "banjocoop/protocol.h"
#include "ui.h"
#include "ui_internal.h"

extern int bakey_pressed(s32 button);

RECOMP_IMPORT(".", u32 bcnet_player_name(u32 player_id, char *out, u32 max));
RECOMP_IMPORT(".", u32 bcnet_host(u32 port));
RECOMP_IMPORT(".", u32 bcnet_join(const char *address, u32 port));
RECOMP_IMPORT(".", u32 bcnet_host_tunnel(u32 port));
RECOMP_IMPORT(".", u32 bcnet_get_join_code(char *out, u32 max));
RECOMP_IMPORT(".", u32 bcnet_join_tunnel(const char *code));

/* Mirrors the "connection" config enum. */
#define CONNECTION_DIRECT 0
#define CONNECTION_TUNNEL 1
#define JOIN_CODE_MAX 128u

#define UI_CHAT_ROWS BCNET_CHAT_HISTORY
#define UI_ROW_CHARS 64u

static RecompuiContext s_chat_context = 0;
static RecompuiResource s_chat_rows[UI_CHAT_ROWS];
static RecompuiResource s_chat_input = 0;
static u32 s_chat_built = 0;
static u32 s_chat_open = 0;
static u32 s_chat_seen = 0;

/* ---- chat and lobby ---------------------------------------------------------------------------
 *
 * One context, shown only while the player has it open. It is the only thing here that captures
 * input, and it releases it the moment it closes — a capture left switched on takes the controller
 * away from the game entirely, which is far worse than having no chat at all.
 */

static void build_chat(void) {
    s_chat_context = recompui_create_context();
    recompui_open_context(s_chat_context);

    RecompuiResource root = recompui_context_root(s_chat_context);
    recompui_set_position(root, POSITION_ABSOLUTE);
    recompui_set_left(root, 16.0f, UNIT_DP);
    recompui_set_bottom(root, 16.0f, UNIT_DP);
    recompui_set_display(root, DISPLAY_FLEX);
    recompui_set_flex_direction(root, FLEX_DIRECTION_COLUMN);
    recompui_set_row_gap(root, 2.0f, UNIT_DP);
    recompui_set_min_width(root, 400.0f, UNIT_DP);

    for (u32 i = 0; i < UI_CHAT_ROWS; i++) {
        s_chat_rows[i] = recompui_create_label(s_chat_context, root, "", LABELSTYLE_SMALL);
        recompui_set_visibility(s_chat_rows[i], VISIBILITY_HIDDEN);
    }
    s_chat_input = recompui_create_textinput(s_chat_context, root);

    recompui_close_context(s_chat_context);
    recompui_hide_context(s_chat_context);
    s_chat_built = 1;
}

/* Pack a line into words. Everything crossing into rdram has to be 4-byte (see protocol.h), so the
 * text travels packed and both ends agree on the order. */
static void chat_pack(const char *text, bc_chat_line *out) {
    u32 n = 0;
    while (text[n] != '\0' && n < BCNET_CHAT_LEN - 1) {
        n++;
    }
    for (u32 i = 0; i < BCNET_CHAT_LEN / 4; i++) {
        out->text[i] = 0;
    }
    for (u32 i = 0; i < n; i++) {
        out->text[i >> 2] |= ((u32)(u8)text[i]) << (24u - 8u * (i & 3u));
    }
    out->length = n;
}

static void chat_unpack(const bc_chat_line *line, char *out, u32 max) {
    u32 n = line->length;
    if (n > max - 1) {
        n = max - 1;
    }
    for (u32 i = 0; i < n; i++) {
        out[i] = (char)((line->text[i >> 2] >> (24u - 8u * (i & 3u))) & 0xFFu);
    }
    out[n] = '\0';
}

static void chat_refresh(bc_incoming *inc) {
    recompui_open_context(s_chat_context);
    for (u32 i = 0; i < UI_CHAT_ROWS; i++) {
        if (i >= inc->chat.count) {
            recompui_set_visibility(s_chat_rows[i], VISIBILITY_HIDDEN);
            continue;
        }
        char body[BCNET_CHAT_LEN];
        char name[BCNET_NAME_LEN];
        char line[UI_ROW_CHARS + BCNET_CHAT_LEN];
        u32 at = 0;
        chat_unpack(&inc->chat.lines[i], body, sizeof(body));
        if (bcnet_player_name(inc->chat.lines[i].from, name, sizeof(name)) == 0) {
            name[0] = 'p';
            name[1] = (char)('0' + (inc->chat.lines[i].from % 10u));
            name[2] = '\0';
        }
        at = ui_str_put(line, at, name, sizeof(line));
        at = ui_str_put(line, at, ": ", sizeof(line));
        at = ui_str_put(line, at, body, sizeof(line));
        line[at] = '\0';
        recompui_set_text(s_chat_rows[i], line);
        recompui_set_visibility(s_chat_rows[i], VISIBILITY_VISIBLE);
    }
    recompui_close_context(s_chat_context);
}

static void chat_update(bc_incoming *inc, bc_outgoing *out) {
    if (!inc->connected) {
        if (s_chat_open) {
            s_chat_open = 0;
            recompui_set_context_captures_input(s_chat_context, 0);
            recompui_hide_context(s_chat_context);
        }
        return;
    }
    if (!s_chat_built) {
        build_chat();
    }

    if (!s_chat_open && bakey_pressed(BUTTON_D_UP)) {
        s_chat_open = 1;
        chat_refresh(inc);
        recompui_show_context(s_chat_context);
        /* Only now does anything here take the controller. */
        recompui_set_context_captures_input(s_chat_context, 1);
        return;
    }

    if (s_chat_open) {
        if (bakey_pressed(BUTTON_START)) {
            /* The returned buffer is ours to free — see the note above the import. */
            char *typed = recompui_get_input_text(s_chat_input);
            if (typed != NULL) {
                if (typed[0] != '\0') {
                    chat_pack(typed, &out->outgoing_chat);
                    recompui_set_input_text(s_chat_input, "");
                }
                recomp_free(typed);
            }
        }
        if (bakey_pressed(BUTTON_START) || bakey_pressed(BUTTON_B)) {
            s_chat_open = 0;
            recompui_set_context_captures_input(s_chat_context, 0);
            recompui_hide_context(s_chat_context);
            return;
        }
        chat_refresh(inc);
        return;
    }

    /* Closed, but show it briefly when something new arrives so a line is never missed. */
    if (inc->chat.count != s_chat_seen) {
        s_chat_seen = inc->chat.count;
        chat_refresh(inc);
        recompui_show_context(s_chat_context);
    }
}

/* ---- lobby ------------------------------------------------------------------------------------
 *
 * Host or join without leaving the game. The mod's config options already do this, but they mean
 * pausing, opening a menu, typing an address and restarting the session — a lobby is the same
 * thing where you can see it, which is what the plan asks for.
 *
 * Shown only while disconnected: there is nothing to choose once you are in a session, and an
 * input-capturing panel over live gameplay is exactly the hazard the chat box is careful about.
 */

static RecompuiContext s_lobby_context = 0;
static RecompuiResource s_lobby_address = 0;
static RecompuiResource s_lobby_status = 0;
static u32 s_lobby_built = 0;
static u32 s_lobby_open = 0;
/* Set after a tunnel host is started: the lobby stays up (rather than closing on "connected", which
 * a host is the moment it hosts) to show the join code as soon as cloudflared reports it. */
static u32 s_showing_code = 0;
static u32 s_code_shown = 0;


static void build_lobby(void) {
    s_lobby_context = recompui_create_context();
    recompui_open_context(s_lobby_context);

    RecompuiResource root = recompui_context_root(s_lobby_context);
    recompui_set_position(root, POSITION_ABSOLUTE);
    recompui_set_left(root, 50.0f, UNIT_PERCENT);
    recompui_set_top(root, 30.0f, UNIT_PERCENT);
    recompui_set_display(root, DISPLAY_FLEX);
    recompui_set_flex_direction(root, FLEX_DIRECTION_COLUMN);
    recompui_set_row_gap(root, 8.0f, UNIT_DP);
    recompui_set_min_width(root, 420.0f, UNIT_DP);

    recompui_create_label(s_lobby_context, root, "BanjoCoop", LABELSTYLE_LARGE);
    s_lobby_status = recompui_create_label(s_lobby_context, root,
                                           "Start: host. Paste an address/code below, then B to join.",
                                           LABELSTYLE_SMALL);
    s_lobby_address = recompui_create_textinput(s_lobby_context, root);
    recompui_set_input_text(s_lobby_address, "127.0.0.1");

    recompui_close_context(s_lobby_context);
    recompui_hide_context(s_lobby_context);
    s_lobby_built = 1;
}

/* Close the lobby and hand the controller back to the game. */
static void lobby_close(void) {
    s_lobby_open = 0;
    s_showing_code = 0;
    s_code_shown = 0;
    recompui_set_context_captures_input(s_lobby_context, 0);
    recompui_hide_context(s_lobby_context);
}

static void lobby_update(bc_incoming *inc) {
    if (recomp_get_config_u32("show_lobby") == 0) {
        return;
    }

    /* Code-display mode: after hosting over a tunnel, keep the lobby up to show the join code. This
     * runs even though `inc->connected` is set (a host is "connected" the instant it hosts), which
     * is exactly why it is handled before the auto-close below. */
    if (s_showing_code) {
        if (!s_code_shown) {
            char code[JOIN_CODE_MAX];
            if (bcnet_get_join_code(code, JOIN_CODE_MAX) != 0) {
                /* No libc here, so the line is assembled with the mod's own string helper. */
                char line[JOIN_CODE_MAX + 48];
                u32 at = 0;
                at = ui_str_put(line, at, "Join code (share it): ", sizeof(line));
                at = ui_str_put(line, at, code, sizeof(line));
                at = ui_str_put(line, at, "   -   Z to close", sizeof(line));
                recompui_set_text(s_lobby_status, line);
                s_code_shown = 1;
            }
        }
        if (bakey_pressed(BUTTON_Z) || bakey_pressed(BUTTON_D_DOWN)) {
            lobby_close();
        }
        return;
    }

    /* Only while there is a choice to make. */
    if (inc->connected) {
        if (s_lobby_open) {
            lobby_close();
        }
        return;
    }
    if (!s_lobby_built) {
        build_lobby();
    }

    if (!s_lobby_open && bakey_pressed(BUTTON_D_DOWN)) {
        s_lobby_open = 1;
        recompui_show_context(s_lobby_context);
        recompui_set_context_captures_input(s_lobby_context, 1);
        return;
    }
    if (!s_lobby_open) {
        return;
    }

    u32 port = (u32)recomp_get_config_double("port");
    u32 connection = recomp_get_config_u32("connection");

    if (bakey_pressed(BUTTON_START)) {
        if (connection == CONNECTION_TUNNEL) {
            /* Stay open to show the code cloudflared is about to report. */
            recompui_set_text(s_lobby_status, "starting Cloudflare tunnel...");
            bcnet_host_tunnel(port);
            s_showing_code = 1;
            s_code_shown = 0;
            return;
        }
        recompui_set_text(s_lobby_status, "hosting...");
        bcnet_host(port);
    } else if (bakey_pressed(BUTTON_B)) {
        char *addr = recompui_get_input_text(s_lobby_address);
        if (addr != NULL) {
            recompui_set_text(s_lobby_status, "joining...");
            if (connection == CONNECTION_TUNNEL) {
                bcnet_join_tunnel(addr);
            } else {
                bcnet_join(addr, port);
            }
            recomp_free(addr);
        }
    } else if (!bakey_pressed(BUTTON_Z)) {
        return;
    }

    lobby_close();
}


void ui_panels_update(bc_incoming *inc, bc_outgoing *out) {
    chat_update(inc, out);
    lobby_update(inc);
}
