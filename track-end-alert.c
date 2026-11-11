/* SPDX-License-Identifier: GPL-3.0-only
 *
 * track-end-alert: a red banner on the deck's screen, shown for 2 seconds, when a playing
 * track has N seconds left (N = 10, 15, 30, 45 or 60, chosen in the Mod Menu).
 *
 * Uses only core services (rows, deck_info, lib_tracks, one surface): no hooks, so "firmware": ["any"].
 *   - Mod Menu > Alerte fin de morceau: a checkbox, the delay (choice) and a test button.
 *   - tick() reads each deck's position; the length comes from the rekordbox library
 *     (this core's deck_info has none), read by a thread into a table; the alert fires once per track and deck,
 *     and re-arms when the track is ejected or the playhead goes back above the delay.
 *   - the banner is a surface on AZ_LAYER_TOP, shown on every view, hidden after 2 s.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "azmod-mod.h"

#define MOD_ID "mvcai-track-end-alert"
#define MAX_DECKS 4
#define ALERT_MS 2000          /* how long the banner stays */
#define REARM_MARGIN_S 2.0     /* the playhead must go this far above the delay to re-arm */

static const az_core_v1 *C;    /* set once in init() */

/* The delay choices, in seconds; the row stores the index. */
static const char *const LEAD_LABELS[] = { "10 s", "15 s", "30 s", "45 s", "60 s" };
static const int LEAD_SECS[] = { 10, 15, 30, 45, 60 };
#define N_LEADS 5
#define DEFAULT_LEAD 2         /* 30 s */

static int h_enabled = -1, h_lead = -1;   /* row handles; -1 = not registered */
static az_surface *banner;                 /* NULL = the core refused the surface */
static int banner_on;                      /* what we last told surface_show */
static int test_request;                   /* set by the test button, read by tick */

/* Per deck: may it fire, and until when is its alert shown (0 = none). */
static int armed[MAX_DECKS];
static long until_ms[MAX_DECKS];
static int shown_lead_s;                   /* the delay printed on the banner */
static double last_pos[MAX_DECKS];         /* playhead at the last tick, -1 = none */
static uint32_t logged_id[MAX_DECKS];      /* what we last logged about each deck */
static int logged_known[MAX_DECKS];

static const az_rect BANNER_RECT = { 100, 30, 1080, 170 };
#define ALL_VIEWS (AZ_VIEW_BIT(AZ_VIEW_OTHER) | AZ_VIEW_BIT(AZ_VIEW_SOURCE) |  \
                   AZ_VIEW_BIT(AZ_VIEW_BROWSE) | AZ_VIEW_BIT(AZ_VIEW_SEARCH) | \
                   AZ_VIEW_BIT(AZ_VIEW_DECK) | AZ_VIEW_BIT(AZ_VIEW_MENU))

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- track lengths --------------------------------------------------------------------
 * This core's deck_info() always reports len_s = -1, so the length comes from the rekordbox
 * library: a thread of ours reads every track's length (lib_tracks, playlist 0) into this
 * table, keyed by the track's content id. The thread is the only writer; tick() only reads,
 * and every slot is an atomic word, so no lock is needed. Slots are never removed. */
#define CACHE_BITS 14
#define CACHE_SLOTS (1u << CACHE_BITS)
#define CACHE_MAX 12000                 /* stop filling past this: probing stays short */
#define SCAN_RETRY_MS 20000L            /* ask for a new scan at most this often */
static uint32_t cache_id[CACHE_SLOTS];  /* 0 = free slot */
static uint32_t cache_ms[CACHE_SLOTS];  /* the track's length in milliseconds */
static uint32_t cache_n;                /* entries, written by the scan thread only */
static int scan_req;                    /* tick -> thread: scan the libraries */
static int scan_busy;                   /* thread -> tick: a scan is running */
static int scan_thread_ok;              /* the thread started */
static long last_scan_req_ms = -SCAN_RETRY_MS;

static uint32_t slot_of(uint32_t id)
{
    return (id * 2654435761u) >> (32 - CACHE_BITS);
}

/* Length in seconds of a library track, or -1 when not in the table (any thread). */
static double cache_len_s(uint32_t id)
{
    if (id == 0) return -1.0;
    uint32_t s = slot_of(id);
    for (uint32_t k = 0; k < CACHE_SLOTS; k++) {
        uint32_t i = (s + k) & (CACHE_SLOTS - 1);
        uint32_t cur = __atomic_load_n(&cache_id[i], __ATOMIC_ACQUIRE);
        if (cur == id) return (double)__atomic_load_n(&cache_ms[i], __ATOMIC_RELAXED) / 1000.0;
        if (cur == 0) return -1.0;
    }
    return -1.0;
}

/* Scan thread only. */
static void cache_put(uint32_t id, uint32_t ms)
{
    if (id == 0 || ms == 0) return;
    uint32_t s = slot_of(id);
    for (uint32_t k = 0; k < CACHE_SLOTS; k++) {
        uint32_t i = (s + k) & (CACHE_SLOTS - 1);
        uint32_t cur = __atomic_load_n(&cache_id[i], __ATOMIC_RELAXED);
        if (cur == id) {
            __atomic_store_n(&cache_ms[i], ms, __ATOMIC_RELAXED);
            return;
        }
        if (cur == 0) {
            if (cache_n >= CACHE_MAX) return;
            __atomic_store_n(&cache_ms[i], ms, __ATOMIC_RELAXED);
            __atomic_store_n(&cache_id[i], id, __ATOMIC_RELEASE);   /* publishes the length */
            cache_n++;
            return;
        }
    }
}

static int on_lib_track(const az_track *t, void *user)
{
    (void)user;
    if (!t) return 0;
    const double l = t->len_s;
    if (l > 0.0 && l < 1e5) cache_put(t->id, (uint32_t)(l * 1000.0 + 0.5));
    return 0;
}

static void *scan_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (__atomic_exchange_n(&scan_req, 0, __ATOMIC_RELAXED)) {
            __atomic_store_n(&scan_busy, 1, __ATOMIC_RELAXED);
            for (int stick = 0; stick < 2; stick++) {
                int r = C->lib_tracks(MOD_ID, stick, 0, on_lib_track, NULL);
                C->log(MOD_ID, "library scan, USB %d: result %d, %u tracks known", stick + 1, r,
                       (unsigned)__atomic_load_n(&cache_n, __ATOMIC_RELAXED));
            }
            __atomic_store_n(&scan_busy, 0, __ATOMIC_RELAXED);
        }
        struct timespec ts = { 0, 500 * 1000 * 1000 };   /* idle: look again in 0.5 s */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static int lead_seconds(void)
{
    int i = h_lead >= 0 ? C->row_value(h_lead) : DEFAULT_LEAD;   /* no rows: the default */
    if (i < 0 || i >= N_LEADS) i = DEFAULT_LEAD;
    return LEAD_SECS[i];
}

static int alerts_enabled(void)
{
    return h_enabled >= 0 ? C->row_value(h_enabled) == 1 : 1;   /* no rows: always on */
}

/* ---- the banner ------------------------------------------------------------------- */

static void draw_centered(az_draw *d, az_rect area, int y, const char *s, int px, uint32_t rgb)
{
    int w = C->text_width ? C->text_width(s, px) : 0;
    int x = area.x + (area.w - w) / 2;
    if (x < area.x) x = area.x;
    C->draw_text(d, x, y, s, px, rgb);
}

static void on_paint(az_draw *d, az_rect area, void *user)
{
    (void)user;
    const uint32_t red = 0xD0101C, white = 0xFFFFFF;
    C->fill_rect(d, area.x, area.y, area.w, area.h, red);
    C->draw_rect(d, area.x, area.y, area.w, area.h, 6, white);

    draw_centered(d, area, area.y + 14, "FIN DE MORCEAU", 64, white);

    /* "Deck 1, 3 - 30 s restantes": every deck whose alert is still shown */
    char decks[40] = "";
    size_t n = 0;
    for (int i = 0; i < MAX_DECKS; i++) {
        if (until_ms[i] == 0) continue;
        int w = snprintf(decks + n, sizeof decks - n, "%s%d", n ? ", " : "", i + 1);
        if (w < 0 || (size_t)w >= sizeof decks - n) break;
        n += (size_t)w;
    }
    char line[96];
    snprintf(line, sizeof line, "Deck %s - %d s restantes", decks, shown_lead_s);
    int px = 40;
    while (px > 8 && C->text_width && C->text_width(line, px) > area.w - 24) px -= 4;
    draw_centered(d, area, area.y + 14 + 96 + 10, line, px, white);
}

static int on_banner_input(const az_event *ev, void *user)
{
    (void)ev;
    (void)user;
    return 0;                          /* a pure notice: the touch is not ours */
}

/* Show or hide the surface to match the alerts that are live, repaint when it changes. */
static void sync_banner(int any, int changed)
{
    if (!banner) return;
    if (any != banner_on) {
        banner_on = any;
        if (any) C->surface_place(banner, BANNER_RECT);
        C->surface_show(banner, any);
    }
    if (any && changed) C->surface_redraw(banner);
}

/* ---- the Mod Menu rows ------------------------------------------------------------- */

static void on_test(void *user)
{
    (void)user;
    __atomic_store_n(&test_request, 1, __ATOMIC_RELAXED);
}

/* ---- lifecycle --------------------------------------------------------------------- */

static void tick(void)
{
    if (!AZ_HAS(C, deck_info) || !AZ_HAS(C, deck_count) || !AZ_HAS(C, surface_show)) return;

    const long now = now_ms();
    const int on = alerts_enabled();
    const int lead = lead_seconds();
    int changed = 0;

    if (!on) {
        /* switched off: drop every alert and make sure nothing fires when it comes back */
        for (int i = 0; i < MAX_DECKS; i++) {
            if (until_ms[i]) changed = 1;
            until_ms[i] = 0;
            armed[i] = 0;
        }
        __atomic_store_n(&test_request, 0, __ATOMIC_RELAXED);
        sync_banner(0, changed);
        return;
    }

    if (__atomic_exchange_n(&test_request, 0, __ATOMIC_RELAXED)) {
        until_ms[0] = now + ALERT_MS;      /* the test shows deck 1 */
        shown_lead_s = lead;
        changed = 1;
    }

    int n = C->deck_count();
    if (n < 0) n = 0;                       /* unknown: no alert, never a guess */
    if (n > MAX_DECKS) n = MAX_DECKS;

    for (int i = 0; i < n; i++) {
        az_deck_info in;
        memset(&in, 0, sizeof in);
        in.size = (int)sizeof in;
        if (C->deck_info(i, &in) != 0 || !in.loaded) {
            armed[i] = 1;                   /* empty deck: the next track may alert */
            last_pos[i] = -1.0;
            logged_id[i] = 0;
            continue;
        }
        const double pos = in.pos_s;
        /* also false for NaN: an unknown position skips this round */
        if (!(pos >= 0.0 && pos < 1e6)) continue;
        const int moved = last_pos[i] >= 0.0 && pos > last_pos[i] && pos - last_pos[i] < 2.0;
        last_pos[i] = pos;

        /* the length: the core's own when it has one, else the library's */
        double len = in.len_s;
        const char *src = "deck";
        if (!(len > 0.0 && len < 1e6)) {
            len = cache_len_s(in.content_id);
            src = "library";
        }
        const int known = len > 0.0 && len < 1e6;
        if (logged_id[i] != in.content_id || logged_known[i] != known) {
            logged_id[i] = in.content_id;
            logged_known[i] = known;
            C->log(MOD_ID, "deck %d: track %u, length %s (%s)", i + 1, (unsigned)in.content_id,
                   known ? "known" : "unknown", src);
        }
        if (!known) {
            /* not in the table yet: ask the thread to read the libraries again, now and then */
            if (scan_thread_ok && in.content_id != 0 &&
                !__atomic_load_n(&scan_busy, __ATOMIC_RELAXED) &&
                now - last_scan_req_ms >= SCAN_RETRY_MS) {
                last_scan_req_ms = now;
                __atomic_store_n(&scan_req, 1, __ATOMIC_RELAXED);
            }
            continue;
        }
        const double remaining = len - pos;
        const int playing = in.playing == 1 || moved;

        if (remaining > (double)lead + REARM_MARGIN_S) {
            armed[i] = 1;                   /* new track, or the playhead went back */
        } else if (armed[i] && playing && remaining > 0.0 && remaining <= (double)lead) {
            armed[i] = 0;
            until_ms[i] = now + ALERT_MS;
            shown_lead_s = lead;
            changed = 1;
            C->log(MOD_ID, "deck %d: %d s left", i + 1, lead);
        }
    }

    int any = 0;
    for (int i = 0; i < MAX_DECKS; i++) {
        if (until_ms[i] == 0) continue;
        if (now - until_ms[i] >= 0) {       /* expired */
            until_ms[i] = 0;
            changed = 1;
        } else {
            any = 1;
        }
    }
    sync_banner(any, changed);
}

static int init(const az_core_v1 *core)
{
    if (!core || core->abi != AZ_MOD_ABI) return 1;
    C = core;
    if (!AZ_HAS(C, surface_create) || !AZ_HAS(C, deck_info))
        return 2;                           /* needs AZ-MODS 1.2.3 or newer */

    if (AZ_HAS(C, register_row) && AZ_HAS(C, row_value)) {
        const az_row_v1 enabled = {
            .size = sizeof(az_row_v1),
            .key = "enabled",
            .label = "Alerte fin de morceau",
            .kind = AZ_ROW_TOGGLE,
            .flags = AZ_ROW_DEFAULT_ON,
            .help = "Bandeau rouge de 2 s avant la fin",
        };
        h_enabled = C->register_row(MOD_ID, &enabled);

        const az_row_v1 lead = {
            .size = sizeof(az_row_v1),
            .key = "lead",
            .label = "Delai avant la fin",
            .kind = AZ_ROW_CHOICE,
            .parent = h_enabled >= 0 ? "enabled" : NULL,
            .help = "Temps restant qui declenche l'alerte",
            .choices = LEAD_LABELS,
            .n_choices = N_LEADS,
            .default_choice = DEFAULT_LEAD,
        };
        h_lead = C->register_row(MOD_ID, &lead);

        const az_row_v1 test = {
            .size = sizeof(az_row_v1),
            .key = "test",
            .label = "Tester l'alerte",
            .kind = AZ_ROW_ACTION,
            .parent = h_enabled >= 0 ? "enabled" : NULL,
            .help = "Affiche le bandeau 2 s",
            .on_activate = on_test,
        };
        C->register_row(MOD_ID, &test);
    }

    const az_surface_v1 s = {
        .size = sizeof(az_surface_v1),
        .layer = AZ_LAYER_TOP,
        .views = ALL_VIEWS,
        .on_paint = on_paint,
        .on_input = on_banner_input,
    };
    banner = C->surface_create(MOD_ID, &s);
    if (!banner) C->log(MOD_ID, "surface_create refused: no banner");
    if (!banner) return 3;                  /* the banner is the whole point */

    for (int i = 0; i < MAX_DECKS; i++) {
        armed[i] = 1;
        last_pos[i] = -1.0;
    }
    if (AZ_HAS(C, thread_start) && AZ_HAS(C, lib_tracks) &&
        C->thread_start(MOD_ID, "tea-scan", scan_thread, NULL) == 0) {
        scan_thread_ok = 1;
        __atomic_store_n(&scan_req, 1, __ATOMIC_RELAXED);   /* a first scan right away */
    } else {
        C->log(MOD_ID, "no library thread: track length comes from the deck only");
    }
    C->log(MOD_ID, "loaded, delay=%d s, enabled=%d", lead_seconds(), alerts_enabled());
    return 0;
}

/* "any": core services only, no hooks. No stop(): a mod with a surface is turned off by a
 * reload anyway, and the checkbox above switches the alert off live. */
static const char *const FW_TAGS[] = { "any", NULL };

static const az_mod_v1 MOD = {
    .abi = AZ_MOD_ABI,
    .size = sizeof(az_mod_v1),
    .id = MOD_ID,
    .fw_tags = FW_TAGS,
    .init = init,
    .tick = tick,
    .on_frame = NULL,
};

const az_mod_v1 *az_mod_v1_entry(void)
{
    return &MOD;
}
