#pragma once

/* deck_shell_intent — UI intent navigation.
 *
 * An "intent" identifies a destination screen + optional payload. The
 * shell resolves intents to concrete activity pushes via a registry
 * keyed by app_id. F27.2 ships the registry + back/home wiring; the
 * launcher and individual apps register their handlers in F28-F29.
 */

#include <stddef.h>
#include <stdint.h>

#include "deck_error.h"
#include "deck_bridge_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t app_id;
    uint16_t screen_id;
    void    *data;       /* heap-owned by caller, passed to on_create */
    size_t   data_size;  /* informational; activity decides how to read */
} deck_shell_intent_t;

/* Resolver: given an intent, push the matching activity. */
typedef deck_err_t (*deck_shell_intent_resolver_t)(
    const deck_shell_intent_t *intent);

/* Register an intent resolver for `app_id`. At most one resolver per
 * app_id; second registration replaces the first (returns OK). */
deck_err_t deck_shell_intent_register(uint16_t app_id,
                                       deck_shell_intent_resolver_t resolver);

/* Route an intent — looks up the resolver and invokes it. Returns
 * UNRESOLVED_SYMBOL if no resolver is registered. */
deck_err_t deck_shell_intent_navigate(const deck_shell_intent_t *intent);

/* Default back / home / tasks callbacks wired into the navbar — they
 * call deck_bridge_ui_activity_pop / _pop_to_home / launch the task
 * manager app respectively, but no-op while the lockscreen has
 * navigation locked. */
void deck_shell_navbar_back(void);
void deck_shell_navbar_home(void);
void deck_shell_navbar_tasks(void);

/* Lock / unlock the back+home gestures. Used by lockscreen + system
 * dialogs that must not be dismissed by accident. */
void deck_shell_nav_lock(bool lock);
bool deck_shell_nav_is_locked(void);

#ifdef __cplusplus
}
#endif
