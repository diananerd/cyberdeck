#pragma once

/* deck_shell — minimal DL1 single-app shell.
 *
 * Mounts the "apps" SPIFFS partition via the storage.fs SDI driver,
 * discovers the first .deck file under /apps/, reads it, and hands
 * off to the Deck runtime (deck_runtime_run_on_launch).
 *
 * DL1 single-app: no activity stack, no lockscreen, no intent system.
 * The shell runs to completion of @on launch + @machine lifecycle,
 * then returns. main.c proceeds into the idle loop afterwards.
 *
 * Full shell (activity stack, HOME/BACK routing, intent navigation,
 * task switching) lands in DL2.
 */

#include "deck_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Discover + run the first .deck file. Returns OK if an app ran (or
 * no app was found); non-OK propagates load/runtime errors. */
deck_err_t deck_shell_boot(void);

#ifdef __cplusplus
}
#endif
