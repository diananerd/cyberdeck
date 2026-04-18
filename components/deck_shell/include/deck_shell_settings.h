#pragma once

#include "deck_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the settings intent resolver. App id is reserved (9). */
deck_err_t deck_shell_settings_register(void);

#ifdef __cplusplus
}
#endif
