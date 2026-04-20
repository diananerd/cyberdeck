/* deck_shell_deck_apps — F28 Phase 2: actual .deck source apps.
 *
 * Scans the apps SPIFFS partition root for *.deck files and loads each
 * through deck_runtime_app_load. Successful loads are registered with
 * the shell intent registry using sequential app_ids from
 * DECK_APPS_BASE_ID so they can be launched from the launcher.
 *
 * What a loaded .deck app can do today:
 *   - log.info / log.warn / log.error
 *   - run a state machine (with @machine.before / .after hooks)
 *   - call any DL2 builtin (nvs, fs, text, math, time, list, map, ...)
 *   - dispatch @on resume every time the launcher card is tapped
 *
 * What's still missing (Phase 2+):
 *   - bridge.ui.* builtins so a .deck app can render its own screen
 *   - app-side wiring to native events (EVT_WIFI_*, EVT_BATTERY_*)
 *   - migration (NVS-version tracking) and assets (binary blobs)
 *
 * Filter rules: only top-level .deck files at the root are loaded.
 * Files under subdirectories (conformance/, system/) are skipped so the
 * conformance harness keeps its private namespace. `hello.deck` is
 * treated like any other user app — no special casing.
 */

#include "deck_shell_deck_apps.h"
#include "deck_shell_intent.h"
#include "deck_bridge_ui.h"
#include "deck_interp.h"
#include "drivers/deck_sdi_fs.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "shell.deck_apps";

#define DECK_SRC_CAP_BYTES   (24 * 1024)   /* generous ceiling for DL2 apps */

typedef struct {
    uint16_t             app_id;
    char                 path[96];
    deck_runtime_app_t  *handle;
    char                 id_copy[64];      /* copy out of arena in case loader frees */
    char                 name_copy[64];
} loaded_slot_t;

static loaded_slot_t s_slots[DECK_APPS_MAX_LOADED];
static uint32_t      s_n_loaded = 0;

uint32_t deck_shell_deck_apps_count(void)
{
    return s_n_loaded;
}

void deck_shell_deck_apps_info(uint32_t idx, deck_shell_deck_app_info_t *out)
{
    if (!out) return;
    if (idx >= s_n_loaded) {
        out->app_id = 0;
        out->id = out->name = out->path = NULL;
        return;
    }
    loaded_slot_t *s = &s_slots[idx];
    out->app_id = s->app_id;
    out->id     = s->id_copy[0] ? s->id_copy : NULL;
    out->name   = s->name_copy[0] ? s->name_copy : NULL;
    out->path   = s->path;
}

/* ---------- activity adapter ----------
 *
 * Each .deck app runs inside its own activity on the bridge UI stack so
 * bridge.ui.render() can paint into a dedicated LVGL screen without
 * destroying the launcher. The bridge-ui activity push sequence is:
 *
 *   prev.on_pause → new.on_create → lv_scr_load(new_scr) → new.on_resume
 *
 * bridge.ui.render targets `lv_scr_act()`, so dispatching @on resume from
 * on_create would paint onto the OUTGOING screen (launcher) just before
 * lv_scr_load swaps it out — the user sees a flash then an empty screen.
 * We dispatch from on_resume instead, by which point the new screen is
 * active and render() targets the correct lv_obj.
 *
 *   on_create → cache the app handle (intent_data) on the activity
 *   on_resume → dispatch @on resume (first view + return-from-child)
 *   on_pause  → dispatch @on pause  (covers both push-child and pop paths;
 *               bridge-UI fires pause before destroy on pop)
 *   on_destroy→ NULL (pause already ran; @on terminate is reserved for
 *               full module unload via deck_runtime_app_unload)
 */
/* Resolve the current activity to its deck_runtime_app_t handle by
 * looking up the loaded-slot table by app_id. This used to cache the
 * handle in act->state, but `deck_bridge_ui_activity_recreate_all` (used
 * by rotation) resets act->state to NULL while re-invoking on_create
 * with intent_data=NULL — leaving the cached pointer unreachable.
 * Looking up the slot map every call is O(N) with N≤8, negligible. */
static deck_runtime_app_t *adapter_handle(deck_bridge_ui_activity_t *act,
                                           void *intent_data)
{
    /* intent_data (on initial push) is authoritative if present. */
    if (intent_data) return intent_data;
    if (!act) return NULL;
    for (uint32_t i = 0; i < s_n_loaded; i++) {
        if (s_slots[i].app_id == act->app_id) return s_slots[i].handle;
    }
    return NULL;
}

static void deck_app_on_create(deck_bridge_ui_activity_t *act, void *intent_data)
{
    /* Cache only — actual @on resume dispatch happens in on_resume, once
     * the new LVGL screen is live. */
    (void)adapter_handle(act, intent_data);
}

static void deck_app_on_resume(deck_bridge_ui_activity_t *act, void *intent_data)
{
    deck_runtime_app_t *app = adapter_handle(act, intent_data);
    if (!app) return;
    (void)deck_runtime_app_dispatch(app, "resume", NULL);
}

static void deck_app_on_pause(deck_bridge_ui_activity_t *act, void *intent_data)
{
    deck_runtime_app_t *app = adapter_handle(act, intent_data);
    if (!app) return;
    (void)deck_runtime_app_dispatch(app, "pause", NULL);
}

/* on_destroy runs after on_pause during a pop (activity-stack ordering:
 * top.on_pause → resume prev → top.on_destroy). We intentionally do NOT
 * dispatch "pause" again here — on_pause already fired — and we do NOT
 * dispatch "terminate" because the app handle stays loaded in its slot
 * for re-launch. Terminate is reserved for deck_runtime_app_unload, used
 * on shutdown / uninstall flows. Leaving on_destroy NULL is the right
 * signal for "nothing extra to do". */

static const deck_bridge_ui_lifecycle_t s_deck_app_cbs = {
    .on_create  = deck_app_on_create,
    .on_resume  = deck_app_on_resume,
    .on_pause   = deck_app_on_pause,
    .on_destroy = NULL,
};

/* Intent hook (F28 Phase 2 cierre) — when a user taps a rendered
 * `bridge.ui.trigger` widget with intent_id>0, the bridge UI decoder
 * calls this hook. We resolve the current-top activity back to a loaded
 * .deck app handle and dispatch `@on trigger_<intent_id>` on it.
 *
 * The .deck app declares its handlers as distinct @on events:
 *   @on trigger_1: ...
 *   @on trigger_42: ...
 *
 * Out-of-stack taps (e.g. trigger on launcher which isn't a .deck app)
 * are silently ignored — the intent_id name collision with the C-side
 * launcher apps is impossible because the launcher doesn't emit triggers
 * with non-zero intent_id from DVC (it builds its UI imperatively). */
static void deck_app_intent_hook(uint32_t intent_id,
                                  const deck_bridge_ui_val_t *vals,
                                  uint32_t n_vals)
{
    deck_bridge_ui_activity_t *top = deck_bridge_ui_activity_current();
    if (!top) return;

    loaded_slot_t *slot = NULL;
    for (uint32_t i = 0; i < s_n_loaded; i++) {
        if (s_slots[i].app_id == top->app_id) { slot = &s_slots[i]; break; }
    }
    if (!slot || !slot->handle) return;

    /* Concept #58/#59/#60 — prefer the captured-action path. If the runtime
     * has a binding for this intent_id it evaluates the stored action AST
     * with the bridge-supplied payload exposed as `event.value[s]`. If not
     * (e.g. imperative-builder apps that registered `@on trigger_N`),
     * fall back to the legacy event-dispatch path. */
    enum { INTENT_VALS_CAP = 32 };
    deck_intent_val_t rt_vals[INTENT_VALS_CAP];
    uint32_t rt_n = n_vals <= INTENT_VALS_CAP ? n_vals : INTENT_VALS_CAP;
    for (uint32_t i = 0; i < rt_n; i++) {
        rt_vals[i].key = vals[i].key;
        rt_vals[i].b   = vals[i].b;
        rt_vals[i].i   = vals[i].i;
        rt_vals[i].f   = vals[i].f;
        rt_vals[i].s   = vals[i].s;
        switch (vals[i].kind) {
            case DECK_BRIDGE_UI_VAL_BOOL: rt_vals[i].kind = DECK_INTENT_VAL_BOOL; break;
            case DECK_BRIDGE_UI_VAL_I64:  rt_vals[i].kind = DECK_INTENT_VAL_I64;  break;
            case DECK_BRIDGE_UI_VAL_F64:  rt_vals[i].kind = DECK_INTENT_VAL_F64;  break;
            case DECK_BRIDGE_UI_VAL_STR:  rt_vals[i].kind = DECK_INTENT_VAL_STR;  break;
            case DECK_BRIDGE_UI_VAL_ATOM: rt_vals[i].kind = DECK_INTENT_VAL_ATOM; break;
            default:                       rt_vals[i].kind = DECK_INTENT_VAL_NONE; break;
        }
    }
    deck_err_t rc = deck_runtime_app_intent_v(slot->handle, intent_id,
                                               rt_n ? rt_vals : NULL, rt_n);
    if (rc != DECK_RT_OK) {
        ESP_LOGW(TAG, "intent_v(%u) → %s on %s",
                 (unsigned)intent_id, deck_err_name(rc), slot->path);
    }
    /* Legacy fallback — `@on trigger_N` still dispatched for imperative
     * apps (hello.deck / ping.deck). Declarative apps have no such
     * handler, so the dispatch is a silent no-op for them. */
    char evt[32];
    snprintf(evt, sizeof(evt), "trigger_%u", (unsigned)intent_id);
    (void)deck_runtime_app_dispatch(slot->handle, evt, NULL);
}

/* Intent resolver — pushes an activity for the .deck app and lets the
 * lifecycle callbacks drive @on resume. The activity owns a dedicated
 * LVGL screen so bridge.ui.render() paints there, not over the launcher. */
static deck_err_t deck_app_intent_resolver(const deck_shell_intent_t *intent)
{
    if (!intent) return DECK_RT_INTERNAL;
    loaded_slot_t *slot = NULL;
    for (uint32_t i = 0; i < s_n_loaded; i++) {
        if (s_slots[i].app_id == intent->app_id) { slot = &s_slots[i]; break; }
    }
    if (!slot || !slot->handle) return DECK_LOAD_UNRESOLVED_SYMBOL;

    deck_sdi_err_t pr = deck_bridge_ui_activity_push(
        intent->app_id, intent->screen_id, &s_deck_app_cbs, slot->handle);
    if (pr != DECK_SDI_OK) {
        ESP_LOGW(TAG, "activity_push for %s: %s",
                 slot->path, deck_sdi_strerror(pr));
        return DECK_RT_INTERNAL;
    }
    return DECK_RT_OK;
}

/* Collector: fs.list callback writes matching entries into a bounded
 * array owned by the caller. Subdirectory contents are not descended —
 * the driver lists only the current directory level. */
typedef struct {
    char   names[DECK_APPS_MAX_LOADED][96];
    uint32_t n;
} scan_ctx_t;

static bool scan_cb(const char *name, bool is_dir, void *user)
{
    if (!name || is_dir) return true;
    /* SPIFFS is flat — subdirectories exist only as path prefixes in the
     * listed names ("conformance/sanity.deck" etc). We skip any entry with
     * a slash so conformance + system fixtures stay private; only top-level
     * user apps get loaded into launcher slots. */
    if (strchr(name, '/') != NULL) return true;
    size_t n = strlen(name);
    if (n < 5 || strcmp(name + n - 5, ".deck") != 0) return true;

    scan_ctx_t *sc = user;
    if (sc->n >= DECK_APPS_MAX_LOADED) {
        ESP_LOGW(TAG, "too many .deck files, truncating at %d", DECK_APPS_MAX_LOADED);
        return false;
    }
    snprintf(sc->names[sc->n], sizeof(sc->names[sc->n]), "%s", name);
    sc->n++;
    return true;
}

static deck_err_t load_one(loaded_slot_t *slot, uint16_t app_id, const char *path)
{
    char *buf = heap_caps_malloc(DECK_SRC_CAP_BYTES, MALLOC_CAP_INTERNAL);
    if (!buf) return DECK_RT_NO_MEMORY;

    size_t n = DECK_SRC_CAP_BYTES - 1;
    deck_sdi_err_t rr = deck_sdi_fs_read(path, buf, &n);
    if (rr != DECK_SDI_OK) {
        ESP_LOGW(TAG, "fs.read %s: %s", path, deck_sdi_strerror(rr));
        heap_caps_free(buf);
        return DECK_LOAD_LEX_ERROR;
    }
    buf[n] = '\0';

    deck_runtime_app_t *handle = NULL;
    deck_err_t rc = deck_runtime_app_load(buf, (uint32_t)n, &handle);
    heap_caps_free(buf);
    if (rc != DECK_RT_OK) {
        ESP_LOGW(TAG, "app_load %s failed: %s", path, deck_err_name(rc));
        return rc;
    }

    slot->app_id = app_id;
    slot->handle = handle;
    snprintf(slot->path, sizeof(slot->path), "%s", path);

    const char *id   = deck_runtime_app_id(handle);
    const char *name = deck_runtime_app_name(handle);
    if (id)   snprintf(slot->id_copy,   sizeof(slot->id_copy),   "%s", id);
    if (name) snprintf(slot->name_copy, sizeof(slot->name_copy), "%s", name);

    ESP_LOGI(TAG, "loaded app_id=%u id=\"%s\" name=\"%s\" from %s",
             (unsigned)app_id,
             slot->id_copy[0] ? slot->id_copy : "?",
             slot->name_copy[0] ? slot->name_copy : "?",
             path);
    return DECK_RT_OK;
}

deck_err_t deck_shell_deck_apps_scan_and_register(void)
{
    s_n_loaded = 0;

    /* Route trigger taps inside .deck screens back to the running app as
     * @on trigger_<id> events. Hook stays set even if zero apps load —
     * subsequent apps registered later will see it automatically. */
    deck_bridge_ui_set_intent_hook(deck_app_intent_hook);

    scan_ctx_t sc = { .n = 0 };
    deck_sdi_err_t lr = deck_sdi_fs_list("/", scan_cb, &sc);
    if (lr != DECK_SDI_OK) {
        ESP_LOGW(TAG, "fs.list / failed: %s — no .deck apps",
                 deck_sdi_strerror(lr));
        return DECK_RT_OK;  /* non-fatal; booting without user apps is valid */
    }
    if (sc.n == 0) {
        ESP_LOGI(TAG, "no .deck files found at apps root");
        return DECK_RT_OK;
    }

    uint16_t next_id = DECK_APPS_BASE_ID;
    for (uint32_t i = 0; i < sc.n; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/%s", sc.names[i]);

        loaded_slot_t *slot = &s_slots[s_n_loaded];
        memset(slot, 0, sizeof(*slot));
        if (load_one(slot, next_id, path) != DECK_RT_OK) continue;

        deck_err_t rr = deck_shell_intent_register(next_id, deck_app_intent_resolver);
        if (rr != DECK_RT_OK) {
            ESP_LOGW(TAG, "intent_register id=%u failed: %s",
                     (unsigned)next_id, deck_err_name(rr));
            deck_runtime_app_unload(slot->handle);
            slot->handle = NULL;
            continue;
        }
        s_n_loaded++;
        next_id++;
    }

    ESP_LOGI(TAG, "registered %u .deck apps", (unsigned)s_n_loaded);
    return DECK_RT_OK;
}
