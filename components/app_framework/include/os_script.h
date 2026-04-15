/*
 * CyberDeck — Script Runtime Interface (G4)
 *
 * Defines the vtable that a script interpreter (MicroPython, Lua, etc.) must
 * implement to be used by the OS as a script runtime. The OS does not include
 * any interpreter — this is a forward-declaration only, enabling future
 * runtime registration without OS changes.
 *
 * Register a runtime with os_script_register_runtime().
 * Look up by name with os_script_get_runtime("lua").
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle returned by the runtime's load function. */
typedef void *script_handle_t;

/** Arguments passed to a script function call. */
typedef struct {
    int    argc;
    char **argv;   /* NULL-terminated array of string arguments */
} script_args_t;

/**
 * @brief Runtime vtable — implemented by an interpreter module.
 *
 * Fields:
 *   name      Unique identifier, e.g. "lua", "micropython".
 *   file_ext  Primary file extension handled, e.g. ".lua", ".py".
 *   load      Load a script from disk; return opaque handle.
 *   call      Call a named function within a loaded script.
 *   unload    Free all resources for a loaded script.
 */
typedef struct {
    const char *name;
    const char *file_ext;

    esp_err_t (*load  )(const char *path, script_handle_t *out);
    esp_err_t (*call  )(script_handle_t h, const char *fn,
                        const script_args_t *args);
    void      (*unload)(script_handle_t h);
} script_runtime_t;

/**
 * @brief Register a script runtime with the OS.
 * @param rt  Pointer to a statically-allocated runtime vtable.
 *            The pointer must remain valid for the lifetime of the OS.
 */
void os_script_register_runtime(const script_runtime_t *rt);

/**
 * @brief Look up a registered runtime by name.
 * @param name  Runtime identifier (e.g. "lua").
 * @return Pointer to the runtime vtable, or NULL if not registered.
 */
const script_runtime_t *os_script_get_runtime(const char *name);

/**
 * @brief Return how many runtimes are currently registered.
 */
int os_script_runtime_count(void);

#ifdef __cplusplus
}
#endif
