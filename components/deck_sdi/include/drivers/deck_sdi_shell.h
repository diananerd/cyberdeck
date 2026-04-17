#pragma once

/* system.shell — app lifecycle control.
 *
 * Mandatory at DL1 in minimal form: push/pop of a single app. DL2 adds
 * the activity stack + intent navigation; DL3 adds privileged switching.
 *
 * The DL1 stub lands here in F1.6 with launch() returning NOT_SUPPORTED;
 * the real impl replaces it in F8 after the runtime loader is complete.
 *
 * See deck-lang/09-deck-shell.md.
 */

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Launch an app by id. DL1 stub returns NOT_SUPPORTED. */
    deck_sdi_err_t (*launch)(void *ctx, const char *app_id);

    /* Terminate the current app. No-op if none running. */
    deck_sdi_err_t (*terminate)(void *ctx);

    /* Id of the running app, NULL if none. */
    const char *(*current_app_id)(void *ctx);

    /* True if an app is currently running. */
    bool (*is_running)(void *ctx);
} deck_sdi_shell_vtable_t;

deck_sdi_err_t deck_sdi_shell_register_stub(void);

deck_sdi_err_t deck_sdi_shell_launch(const char *app_id);
deck_sdi_err_t deck_sdi_shell_terminate(void);
const char    *deck_sdi_shell_current_app_id(void);
bool           deck_sdi_shell_is_running(void);

deck_sdi_err_t deck_sdi_shell_selftest(void);

#ifdef __cplusplus
}
#endif
