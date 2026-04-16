/*
 * CyberDeck — Task Manager: internal shared definitions
 *
 * Shared between app_taskman.c, taskman_detail.c, taskman_sysview.c.
 * Not part of the public API — only include from within the taskman component.
 */

#pragma once

#include "ui_activity.h"   /* view_cbs_t */
#include "svc_monitor.h"   /* sys_snapshot_t, mon_app_entry_t */
#include "os_process.h"    /* proc_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Sub-screen IDs within APP_ID_TASKMAN */
#define TASKMAN_SCR_OVERVIEW ((uint8_t)0)
#define TASKMAN_SCR_DETAIL   ((uint8_t)1)
#define TASKMAN_SCR_SYSVIEW  ((uint8_t)2)

/* View callbacks — defined in their respective .c files */
extern const view_cbs_t taskman_detail_cbs;
extern const view_cbs_t taskman_sysview_cbs;

/* Shared format helpers */

static inline const char *proc_state_str(proc_state_t state)
{
    switch (state) {
        case PROC_STATE_RUNNING:    return "RUNNING";
        case PROC_STATE_SUSPENDED:  return "SUSPENDED";
        case PROC_STATE_BACKGROUND: return "BACKGROUND";
        case PROC_STATE_STOPPED:    return "STOPPED";
        default:                    return "UNKNOWN";
    }
}

/**
 * Format a byte count as "142K" or "1.2M".
 * @return Number of chars written (snprintf-style).
 */
static inline int fmt_bytes(char *buf, size_t buflen, size_t bytes)
{
    if (bytes >= 1024u * 1024u) {
        uint32_t m10 = (uint32_t)((bytes * 10u) / (1024u * 1024u));
        return snprintf(buf, buflen, "%lu.%luM",
                        (unsigned long)(m10 / 10u),
                        (unsigned long)(m10 % 10u));
    }
    return snprintf(buf, buflen, "%luK", (unsigned long)(bytes / 1024u));
}

/**
 * Format an uptime in seconds as "Xs", "Xm Ys", or "Xh Ym".
 */
static inline void fmt_uptime(char *buf, size_t buflen, uint32_t secs)
{
    if (secs < 60u) {
        snprintf(buf, buflen, "%lus", (unsigned long)secs);
    } else if (secs < 3600u) {
        snprintf(buf, buflen, "%lum %lus",
                 (unsigned long)(secs / 60u),
                 (unsigned long)(secs % 60u));
    } else {
        snprintf(buf, buflen, "%luh %lum",
                 (unsigned long)(secs / 3600u),
                 (unsigned long)((secs % 3600u) / 60u));
    }
}

#ifdef __cplusplus
}
#endif
