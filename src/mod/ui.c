/* The player list — Phase 7.
 *
 * A small always-on overlay: who is in the session, which world they are in, and the connection
 * state. This is the thing that makes a session feel inhabited rather than like a game that
 * happens to have a second character in it, and it is the first piece of BanjoCoop a player sees
 * working.
 *
 * Built once and then only ever re-texted. `recompui_create_*` allocates elements, so rebuilding
 * the list every frame would leak steadily for the whole session; a fixed row per player slot and
 * `recompui_set_text` costs nothing.
 *
 * There is no `sprintf` here — the mod is built `-nostdinc` against no libc — so the small string
 * helpers below do the formatting by hand.
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

RECOMP_IMPORT(".", u32 bcnet_player_name(u32 player_id, char *out, u32 max));

/* Map name lookup, so the list can say "Mumbo's Mountain" rather than a number.
 * Mirrors MapInfo in patches/note_saving.c. -> func_8030AD00 */
typedef struct {
    s16 map_id;
    s16 level_id;
    char *name;
} UiMapInfo;
extern UiMapInfo *func_8030AD00(enum map_e map_id);

#define UI_ROW_CHARS 64u

/* Toasts. Few and short-lived: this is a glance, not a feed. */
#define UI_TOAST_SLOTS 4u
#define UI_TOAST_FRAMES 240u /* ~4 seconds */

/* Chat. Opened with D-pad up, sent with Start, closed with B — chosen because none of them do
 * anything useful while you are stationary and typing, and because the overlay only captures
 * input while it is actually open. A capture left switched on would take the controller away
 * from the game entirely, which is much worse than not having chat. */
#define UI_CHAT_ROWS BCNET_CHAT_HISTORY

static RecompuiContext s_context = 0;
static RecompuiResource s_rows[BCNET_MAX_PLAYERS];
static RecompuiResource s_status_row = 0;
static u32 s_built = 0;
static u32 s_visible = 0;

static RecompuiResource s_toasts[UI_TOAST_SLOTS];
static u32 s_toast_expiry[UI_TOAST_SLOTS];
static u32 s_toast_next = 0;

static RecompuiContext s_chat_context = 0;
static RecompuiResource s_chat_rows[UI_CHAT_ROWS];
static RecompuiResource s_chat_input = 0;
static u32 s_chat_built = 0;
static u32 s_chat_open = 0;
static u32 s_chat_seen = 0;

extern int bakey_pressed(s32 button);

/* Refreshed about twice a second. Ping and position move constantly, but nothing here is worth
 * re-laying-out the interface at 60 Hz for. */
#define UI_REFRESH_FRAMES 30u

/* ---- string building ------------------------------------------------------------------------ */

u32 ui_str_put(char *dst, u32 at, const char *src, u32 max) {
    if (src == NULL) {
        return at;
    }
    while (*src != '\0' && at < max - 1) {
        dst[at++] = *src++;
    }
    return at;
}

u32 ui_str_put_u32(char *dst, u32 at, u32 value, u32 max) {
    char tmp[12];
    u32 n = 0;
    if (value == 0) {
        tmp[n++] = '0';
    }
    while (value != 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n > 0 && at < max - 1) {
        dst[at++] = tmp[--n];
    }
    return at;
}

/* ---- building the overlay -------------------------------------------------------------------- */

static void build(void) {
    s_context = recompui_create_context();
    recompui_open_context(s_context);

    RecompuiResource root = recompui_context_root(s_context);
    /* Top-right, out of the way of the note and jiggy counters in the other corners. */
    recompui_set_position(root, POSITION_ABSOLUTE);
    recompui_set_top(root, 16.0f, UNIT_DP);
    recompui_set_right(root, 16.0f, UNIT_DP);
    recompui_set_display(root, DISPLAY_FLEX);
    recompui_set_flex_direction(root, FLEX_DIRECTION_COLUMN);
    recompui_set_align_items(root, ALIGN_ITEMS_FLEX_END);
    recompui_set_row_gap(root, 2.0f, UNIT_DP);

    s_status_row = recompui_create_label(s_context, root, "BanjoCoop", LABELSTYLE_SMALL);
    for (u32 i = 0; i < BCNET_MAX_PLAYERS; i++) {
        s_rows[i] = recompui_create_label(s_context, root, "", LABELSTYLE_SMALL);
        recompui_set_visibility(s_rows[i], VISIBILITY_HIDDEN);
    }

    /* Toasts sit under the player list, in the same column. */
    for (u32 i = 0; i < UI_TOAST_SLOTS; i++) {
        s_toasts[i] = recompui_create_label(s_context, root, "", LABELSTYLE_SMALL);
        recompui_set_visibility(s_toasts[i], VISIBILITY_HIDDEN);
        s_toast_expiry[i] = 0;
    }

    /* The overlay must never take input — the game is being played underneath it. */
    recompui_set_context_captures_input(s_context, 0);
    recompui_set_context_captures_mouse(s_context, 0);
    recompui_close_context(s_context);
    s_built = 1;
}

/* ---- toasts ---------------------------------------------------------------------------------- */

static void toast(const char *text) {
    if (!s_built) {
        return;
    }
    recompui_open_context(s_context);
    u32 slot = s_toast_next % UI_TOAST_SLOTS;
    s_toast_next++;
    recompui_set_text(s_toasts[slot], text);
    recompui_set_visibility(s_toasts[slot], VISIBILITY_VISIBLE);
    s_toast_expiry[slot] = UI_TOAST_FRAMES;
    recompui_close_context(s_context);
}

void ui_toast_event(bc_incoming *inc, const bc_event *ev) {
    /* Only somebody else's doing is worth announcing — you watched your own happen. */
    if (!s_built || ev->origin == inc->local_player_id) {
        return;
    }

    const char *what = NULL;
    switch (ev->kind) {
        case BC_EV_JIGGY:      what = " got a Jiggy!"; break;
        case BC_EV_HONEYCOMB:  what = " found a honeycomb piece"; break;
        case BC_EV_MUMBO_TOKEN: what = " found a Mumbo token"; break;
        case BC_EV_JINJO:      what = " rescued a Jinjo!"; break;
        default: return; /* notes and flags are far too frequent to announce */
    }

    char line[UI_ROW_CHARS];
    char name[BCNET_NAME_LEN];
    u32 at = 0;
    if (bcnet_player_name(ev->origin, name, sizeof(name)) == 0) {
        name[0] = 'p';
        name[1] = (char)('0' + (ev->origin % 10u));
        name[2] = '\0';
    }
    at = ui_str_put(line, at, name, UI_ROW_CHARS);
    at = ui_str_put(line, at, what, UI_ROW_CHARS);
    line[at] = '\0';
    toast(line);
}

static void expire_toasts(void) {
    u32 any = 0;
    for (u32 i = 0; i < UI_TOAST_SLOTS; i++) {
        if (s_toast_expiry[i] != 0) {
            any = 1;
            break;
        }
    }
    if (!any) {
        return;
    }
    recompui_open_context(s_context);
    for (u32 i = 0; i < UI_TOAST_SLOTS; i++) {
        if (s_toast_expiry[i] == 0) {
            continue;
        }
        s_toast_expiry[i]--;
        if (s_toast_expiry[i] == 0) {
            recompui_set_visibility(s_toasts[i], VISIBILITY_HIDDEN);
        }
    }
    recompui_close_context(s_context);
}

/* "player2  Mumbo's Mountain  12ms" — one row, built by hand. */
static void write_row(RecompuiResource row, u32 player_id, u32 map_id, u32 ping_ms, u32 is_self) {
    char line[UI_ROW_CHARS];
    char name[BCNET_NAME_LEN];
    u32 at = 0;

    if (bcnet_player_name(player_id, name, sizeof(name)) == 0) {
        name[0] = 'p';
        name[1] = (char)('0' + (player_id % 10u));
        name[2] = '\0';
    }

    at = ui_str_put(line, at, name, UI_ROW_CHARS);
    if (is_self) {
        at = ui_str_put(line, at, " (you)", UI_ROW_CHARS);
    }
    at = ui_str_put(line, at, "  ", UI_ROW_CHARS);

    UiMapInfo *info = func_8030AD00((enum map_e)map_id);
    if (info != NULL && info->name != NULL) {
        at = ui_str_put(line, at, info->name, UI_ROW_CHARS);
    } else {
        at = ui_str_put(line, at, "map ", UI_ROW_CHARS);
        at = ui_str_put_u32(line, at, map_id, UI_ROW_CHARS);
    }

    if (!is_self && ping_ms != 0) {
        at = ui_str_put(line, at, "  ", UI_ROW_CHARS);
        at = ui_str_put_u32(line, at, ping_ms, UI_ROW_CHARS);
        at = ui_str_put(line, at, "ms", UI_ROW_CHARS);
    }

    line[at] = '\0';
    recompui_set_text(row, line);
    recompui_set_visibility(row, VISIBILITY_VISIBLE);
}

void ui_update(bc_incoming *inc, bc_outgoing *out, u32 local_map, u32 frame) {
    u32 want = recomp_get_config_u32("show_player_list") != 0;

    if (!s_built) {
        if (!want) {
            return; /* never asked for, never built */
        }
        build();
    }

    if (want != s_visible) {
        s_visible = want;
        if (want) {
            recompui_show_context(s_context);
        } else {
            recompui_hide_context(s_context);
        }
    }
    expire_toasts();
    ui_panels_update(inc, out);

    if (!want || (frame % UI_REFRESH_FRAMES) != 0) {
        return;
    }

    recompui_open_context(s_context);

    if (!inc->connected) {
        recompui_set_text(s_status_row, "BanjoCoop - offline");
        for (u32 i = 0; i < BCNET_MAX_PLAYERS; i++) {
            recompui_set_visibility(s_rows[i], VISIBILITY_HIDDEN);
        }
        recompui_close_context(s_context);
        return;
    }

    recompui_set_text(s_status_row, inc->is_host ? "BanjoCoop - hosting" : "BanjoCoop - connected");

    /* Ourselves first, then everyone else, so the list reads the same on every machine. */
    u32 row = 0;
    write_row(s_rows[row++], inc->local_player_id, local_map, 0, 1);
    for (u32 i = 0; i < inc->remote_count && row < BCNET_MAX_PLAYERS; i++) {
        write_row(s_rows[row++], inc->remotes[i].player_id, inc->remotes[i].state.map_id,
                  inc->ping_ms, 0);
    }
    for (; row < BCNET_MAX_PLAYERS; row++) {
        recompui_set_visibility(s_rows[row], VISIBILITY_HIDDEN);
    }

    recompui_close_context(s_context);
}
