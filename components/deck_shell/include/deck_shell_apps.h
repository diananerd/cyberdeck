#pragma once

/* deck_shell_apps — bundled demo apps (F29).
 *
 * App ids:
 *   1 = task_manager   (F29.2)
 *   4 = counter        (F29.4)
 *   7 = net_hello      (F29.3)
 *   9 = settings       (F27.3, registered in deck_shell_settings.c)
 *
 * deck_shell_dl2_boot calls _register, which wires intent resolvers
 * for each app. Launcher cards in deck_shell_dl2.c push via
 * deck_shell_intent_navigate.
 */

#include "deck_error.h"

#ifdef __cplusplus
extern "C" {
#endif

deck_err_t deck_shell_apps_register(void);

#ifdef __cplusplus
}
#endif
