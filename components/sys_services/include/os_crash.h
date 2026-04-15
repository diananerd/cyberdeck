/*
 * CyberDeck — Crash Log (F4)
 *
 * Records unclean resets (panic, watchdog, brownout) to /sdcard/.cyberdeck/crash.log.
 * Call os_crash_init() from app_main after the SD card is mounted.
 * If the last reset was caused by a crash, appends a timestamped entry to the log.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize crash logging.
 *
 * Reads esp_reset_reason(). If the previous boot ended with a crash
 * (panic, watchdog, brownout), appends a record to
 * /sdcard/.cyberdeck/crash.log. No-op if SD is not mounted or last
 * reset was clean.
 */
void os_crash_init(void);

#ifdef __cplusplus
}
#endif
