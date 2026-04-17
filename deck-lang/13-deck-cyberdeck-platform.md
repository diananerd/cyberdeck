# Deck on CyberDeck — Reference Platform Implementation
**Version 1.0 — ESP-IDF v6.0.0 on ESP32-S3 / Waveshare ESP32-S3-Touch-LCD-4.3**

---

## 1. Purpose and Scope

This document specifies the **CyberDeck reference implementation** of the Deck Service Driver Interface (`12-deck-service-drivers`). It is one concrete instantiation of the SDI; other platforms (a future RP2350 port, an ESP32-C6 variant, a desktop simulator) implement the same interface against their own SoC HAL.

It covers:

- ESP-IDF v6.0.0 integration (every concrete `CONFIG_*` key, every binding to the right `esp_*` component)
- The SDI driver implementations on this board (one C module per driver, mapped 1:1 to a `cap_*` component)
- Hardware: pin assignments, I²C bus arbitration, SD card mount, RGB LCD with bounce buffer + PSRAM, GT911 touch
- Memory architecture: PSRAM 80 MHz Octal, internal SRAM regions, DMA-capable buffers, frame buffer in PSRAM
- Security: Secure Boot v2 (RSA-3072), Flash Encryption (XTS-AES-256), Anti-rollback, HMAC-based NVS encryption
- Power management: DFS, Light sleep with tickless idle, RTC GPIO wake sources, deep sleep for background fetch, brownout
- Networking: WiFi STA with WIFI_PS_MIN_MODEM, mbedTLS 4.0 with PSA Crypto, baked-in TLS roots, esp-mqtt as managed component
- OTA: dual track — firmware via `esp_https_ota` partition swap with anti-rollback, app bundles via shared `svc_downloader` writing to SD
- Build system: CMake 3.22.1+, Python 3.10+, GCC 15.1.0, Picolibc, partition.csv, sdkconfig.defaults
- Migration concerns inherited from v6.0 (legacy I²C driver removed, mbedTLS 4.0 PSA mandatory, etc.)
- Boot sequence with timestamps from `app_main()` to first render
- Service architecture: every `svc_*` C module mapped to its FreeRTOS task layout
- The minimal set of OS-bundled (built-in, non-SD) apps
- Crash reporting via ESP-IDF coredump + the runtime's `MSG_PANIC` path
- All recommended `CONFIG_*` values for an interactive embedded UI

**Out of scope here**: language semantics (`01–02`), runtime backend (`11`), abstract SDI (`12`), and cross-SoC component / publishing strategy (`14`). This doc is the canonical reference for one platform.

### Companion documents

| Doc | Role |
|---|---|
| `CLAUDE.md` | Developer notes for this repo |
| `GROUND-STATE.md` | Audit of the existing C codebase (forward-compatible context) |
| `11-deck-implementation` | Portable runtime (no ESP-IDF here) |
| `12-deck-service-drivers` | Hardware-agnostic SDI contract |
| `14-deck-components` | How drivers / runtime are packaged & published |

### Conventions

- All capability and driver paths come from `12-deck-service-drivers §2`.
- All ESP-IDF symbols use the `esp_*` / `CONFIG_*` prefix verbatim.
- Pin names use the `HAL_*` macros from `components/board/include/hal_pins.h`.
- All `CONFIG_*` values listed are confirmed against ESP-IDF v6.0.0 documentation.

---

## 2. Hardware Platform

### 2.1 Board: Waveshare ESP32-S3-Touch-LCD-4.3

| Subsystem | Detail |
|---|---|
| SoC | ESP32-S3 (Xtensa LX7 dual-core, 240 MHz max) |
| Flash | 8 MB, QIO mode, 80 MHz |
| PSRAM | 8 MB, **octal SPIRAM**, 80 MHz |
| Internal SRAM | 512 KB total (DRAM + IRAM split managed by linker) |
| RTC SRAM | 16 KB (deep-sleep retention) |
| Display | 4.3" RGB IPS, 800×480, 16-bit RGB565 parallel |
| Touch | GT911 capacitive over I²C, 5-point (we use single-touch) |
| I/O Expander | CH422G over I²C (backlight, touch reset, LCD reset, SD CS, USB_SEL) |
| RTC chip | PCF85063A over I²C (battery-backed time) |
| SD card | microSD over SPI (CS via expander) |
| USB | One USB-C native (D±/Vbus on GPIO 19/20), one USB-C UART via CH343 |
| UART1 | Available for optional external Bluetooth Classic module (GPIO 15/16) |
| Battery ADC | GPIO 1 (ADC1_CH0), 1:2 voltage divider |
| RGB LCD signals | Parallel pins including GPIO 0 (DATA6) — therefore GPIO 0 is **not** a runtime button |

### 2.2 What this board cannot do

- **No Bluetooth Classic on the SoC.** ESP32-S3 has BLE only (NimBLE). A2DP requires the optional UART1 module.
- **No physical buttons** beyond BOOT/RESET (reserved for flash). Navigation is purely touch.
- **No microSD card-detect pin.** Mount status is inferred by attempting to mount; hot-swap is detected by polling.
- **No on-board ambient sensors.** Sensor capabilities register as `:unavailable` unless extension hardware is added.

The full pin table is §22.

---

## 3. ESP-IDF v6.0 Migration Constraints

The project's `dependencies.lock` pins ESP-IDF v6.0.0 with `idf_component_manager v3.0`. v6.0 introduced several breaking changes that this platform implementation MUST address. The current C codebase (per `GROUND-STATE.md`) was written against ESP-IDF v5.x conventions.

### 3.1 Mandatory migrations

| Concern | v5.x → v6.0 change | Action |
|---|---|---|
| **I²C** | Legacy `driver/i2c.h` removed; only new `driver/i2c_master.h` survives. We currently set `CONFIG_I2C_SUPPRESS_DEPRECATE_WARN=y` — that flag is itself gone in v6.0. | Rewrite `hal_ch422g`, `hal_touch` (via `esp_lcd_touch_gt911`), `hal_rtc` to use `i2c_master_bus_handle_t` + `i2c_master_dev_handle_t`. See §17. |
| **mbedTLS / PSA Crypto** | mbedTLS 4.0 + PSA Crypto API mandatory. Legacy `mbedtls_sha*`, `mbedtls_aes_*` direct calls removed. | Use `psa_crypto_init()` once at boot. All AES/SHA/RSA via PSA. `cap_crypto_aes` wraps `psa_cipher_*`. |
| **LCD config** | `psram_trans_align`, `sram_trans_align` removed → `dma_burst_size`. `color_space`, `rgb_endian` → `rgb_ele_order`. `bits_per_pixel` removed (derived from `in_color_format`). GPIO fields require `gpio_num_t`. | Update `hal_lcd_init` config struct. See §15. |
| **`esp_lcd_panel_disp_off()` removed** | Use `esp_lcd_panel_disp_on_off(panel, false)`. | Replace in any backlight/display power-down paths. |
| **`esp_vfs_fat_sdmmc_unmount()` removed** | Use `esp_vfs_fat_sdcard_unmount()`. | One-line change in `svc_storage`. |
| **`esp_sleep_get_wakeup_cause()` removed** | Use `esp_sleep_get_wakeup_causes()` returning a bitmap. | Update all wake-source detection. |
| **`gpio_deep_sleep_wakeup_enable()` removed** | Use `gpio_wakeup_enable_on_hp_periph_powerdown_sleep()`. | Future deep-sleep paths. |
| **WiFi provisioning** | Internal `wifi_provisioning` component removed. Use external `espressif/network_provisioning ^1.1.0`. | Add via `idf.py add-dependency espressif/network_provisioning^1.1.0` when in-app WiFi onboarding is built. |
| **ESP-MQTT** | Moved out of ESP-IDF; now `espressif/mqtt`. | `idf.py add-dependency espressif/mqtt^1.0` before `cap_mqtt`. |
| **cJSON** | Built-in component removed. | `idf.py add-dependency espressif/cjson^1.0` if needed. |
| **WolfSSL** | Removed from `esp-tls`. | We never used it; no action. |
| **WiFi event task name** | `tiT` → `tcpip`. | Adjust any task-name string filters in logs. |
| **GCC 15.1.0** | New warnings: unterminated string init, header-guard mismatch, self-move, dangling reference. Treated as errors by default. | Fix all warnings (clean build is acceptance criterion). Temporary workaround: `CONFIG_COMPILER_DISABLE_GCC15_WARNINGS=y`. |
| **Picolibc default** | Replaces Newlib. `<sys/signal.h>` → `<signal.h>`. `<sys/dirent.h>` no longer pulls function prototypes; include `<dirent.h>` directly. | Audit C sources. |
| **Compiler warnings as errors by default** | New default. | Keep enabled. |
| **Orphan sections forbidden in linker** | New default. | Audit `.lf` linker fragments. |
| **FreeRTOS in flash by default** | `CONFIG_FREERTOS_IN_IRAM` opt-in. | Leave default (flash) unless a real-time path requires otherwise. |
| **CMake / Python** | CMake ≥ 3.22.1, Python ≥ 3.10. | Document in build instructions; CI must enforce. |
| **`esp_netif_next()` removed** | Use `esp_netif_next_unsafe()` or `esp_netif_find_if()`. | Only relevant if walking netif lists. |
| **BluFi protocol** | Bumped from 0x03 → 0x04 (mbedTLS PSA migration). | Not currently used; no action. |

### 3.2 New v6.0 features we adopt

| Feature | Where used |
|---|---|
| `network_provisioning` component | Settings WiFi onboarding (when implemented) |
| PSA Crypto + mbedTLS 4.0 | All TLS, signature verification for OTA and app bundles, `cap_crypto_aes` |
| HMAC-based NVS encryption (`CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`) | Per-device-keyed NVS without flash encryption (default in v1) |
| `esp_sleep_get_wakeup_causes()` | `svc_power` distinguishes multi-source wakes |
| LCD `dma_burst_size = 64` | Matches 64-byte data cache lines on ESP32-S3 |
| `ESP_TIMER_ISR_AFFINITY` pinned to Core 0 | Frees Core 1 for LVGL |
| Anti-rollback (16-bit `secure_version`) | All firmware images carry an ascending version |
| `LOG_VERSION_2` | Smaller binary, hierarchical log filtering |

---

## 4. Build System

### 4.1 Project layout

```
cyberdeck/
├── CMakeLists.txt                 # top-level idf.py project
├── partitions.csv                 # see §5
├── sdkconfig.defaults             # see §4.3
├── deck-lang/                     # specs (this doc + siblings)
├── components/                    # local C components
│   ├── board/                     # board HAL (pins, lcd, touch, expander, battery, rtc, sd)
│   ├── ui_engine/                 # LVGL bridge: activity stack, common widgets, themes, effects
│   ├── sys_services/              # svc_*: WiFi, settings, time, downloader, OTA, event bus
│   ├── app_framework/             # app_manager, app_registry, app_state, intent
│   ├── apps/                      # built-in C apps (transitional; will become Deck apps; see §14)
│   ├── deck_runtime/              # GIT SUBMODULE — portable runtime per 11
│   ├── deck_bridge/               # GIT SUBMODULE — C glue mapping SDI → runtime
│   ├── deck_drivers_*/            # GIT SUBMODULES — one per cap_* driver bundle
│   └── deck_os_surface/           # the .deck-os blob, embedded as binary
└── main/main.c                    # app_main() entry; orchestrates init order
```

Deck-related components are pulled as **git submodules** from their own repositories — see `14-deck-components §3`.

### 4.2 CMake requirements

```cmake
cmake_minimum_required(VERSION 3.22.1)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cyberdeck)
```

Each component declares dependencies via `idf_component_register(... REQUIRES ... PRIV_REQUIRES ...)`. The `.deck-os` blob is embedded via:

```cmake
target_add_binary_data(${COMPONENT_LIB} "cyberdeck.deck-os" TEXT)
```

### 4.3 sdkconfig.defaults (target values)

```kconfig
# ─── Target / CPU ─────────────────────────────────────────────────────────────
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y

# ─── Flash ────────────────────────────────────────────────────────────────────
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y

# ─── PSRAM (Octal, 80 MHz) ────────────────────────────────────────────────────
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096    # alloc <=4 KB stays in internal SRAM
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536 # 64 KB reserved internally for DMA-only allocs

# ─── Cache ────────────────────────────────────────────────────────────────────
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y        # matches dma_burst_size=64 in LCD config

# ─── FreeRTOS (SMP, dual-core) ────────────────────────────────────────────────
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2       # CHECK_PATTERN
CONFIG_FREERTOS_USE_TIMERS=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y         # required for light sleep
CONFIG_FREERTOS_TIMER_TASK_STACK_SIZE=3584
CONFIG_FREERTOS_IDLE_TASK_STACKSIZE=1536

# Watchdogs
CONFIG_ESP_TASK_WDT_EN=y
CONFIG_ESP_TASK_WDT_INIT=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10
CONFIG_ESP_TASK_WDT_PANIC=n                 # warn first; production may flip to y
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y
CONFIG_ESP_INT_WDT=y
CONFIG_ESP_INT_WDT_TIMEOUT_MS=300

# ─── Power Management ─────────────────────────────────────────────────────────
CONFIG_PM_ENABLE=y
CONFIG_PM_DFS_INIT_AUTO=y
CONFIG_PM_PROFILING=n                       # enable temporarily during power tuning

# ─── Brownout ─────────────────────────────────────────────────────────────────
CONFIG_ESP_BROWNOUT_DET=y
CONFIG_ESP_BROWNOUT_DET_LVL_SEL_3=y         # ~2.6 V threshold

# ─── Console ──────────────────────────────────────────────────────────────────
CONFIG_ESP_CONSOLE_UART=y                   # UART0 via CH343
CONFIG_ESP_CONSOLE_UART_NUM=0
CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200

# ─── Logging ──────────────────────────────────────────────────────────────────
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_VERSION_2=y                      # smaller binary; hierarchical filtering
CONFIG_LOG_COLORS=y

# ─── Partition Table ──────────────────────────────────────────────────────────
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# ─── lwIP tuning ──────────────────────────────────────────────────────────────
CONFIG_LWIP_MAX_SOCKETS=10
CONFIG_LWIP_TCP_RECVMBOX_SIZE=12
CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=4096

# ─── mbedTLS / PSA Crypto ─────────────────────────────────────────────────────
CONFIG_MBEDTLS_PSA_CRYPTO_ENABLED=y
CONFIG_MBEDTLS_HARDWARE_AES=y               # hardware AES on ESP32-S3
CONFIG_MBEDTLS_HARDWARE_SHA=y               # hardware SHA on ESP32-S3
CONFIG_MBEDTLS_HARDWARE_MPI=y               # hardware bignum (RSA) accel
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y             # save ~20 KB; on-demand TLS buffer alloc
CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=8192     # 8 KB record (default 16 KB)
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y         # baked-in roots
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y  # curated bundle (~38 certs / 93% web coverage)
CONFIG_MBEDTLS_TLS_PROTO_TLS1_2=y
CONFIG_MBEDTLS_TLS_PROTO_TLS1_3=y

# ─── HTTP / OTA ───────────────────────────────────────────────────────────────
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y
CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD=y
CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y
CONFIG_BOOTLOADER_APP_SEC_VER_SIZE_EFUSE_FIELD=16

# ─── NVS Encryption (HMAC-based, independent of flash encryption) ─────────────
CONFIG_NVS_ENCRYPTION=y
CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC=y
CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=0          # HMAC eFuse block 0

# ─── Secure Boot V2 (RSA-3072) — production only (eFuse irreversible) ─────────
# CONFIG_SECURE_BOOT=y
# CONFIG_SECURE_BOOT_V2_ENABLED=y
# CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y
# CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y

# ─── Flash Encryption — production only ───────────────────────────────────────
# CONFIG_SECURE_FLASH_ENC_ENABLED=y
# CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y

# ─── LVGL 8.4 + esp_lvgl_port ─────────────────────────────────────────────────
CONFIG_LV_COLOR_SCREEN_TRANSP=y
CONFIG_LV_MEM_CUSTOM=y                      # LVGL allocates via heap_caps (PSRAM)
CONFIG_LV_MEMCPY_MEMSET_STD=y
CONFIG_LV_USE_LOG=y
CONFIG_LV_LOG_PRINTF=y
CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y
CONFIG_LV_USE_FONT_COMPRESSED=y
CONFIG_LV_FONT_MONTSERRAT_18=y
CONFIG_LV_FONT_MONTSERRAT_24=y
CONFIG_LV_FONT_MONTSERRAT_32=y
CONFIG_LV_FONT_MONTSERRAT_40=y
CONFIG_LV_USE_DEMO_WIDGETS=n
CONFIG_LV_USE_DEMO_BENCHMARK=n
CONFIG_LV_USE_DEMO_STRESS=n
CONFIG_LV_USE_DEMO_MUSIC=n
CONFIG_LV_USE_PERF_MONITOR=n

# esp_lvgl_port (RGB panel + bounce buffer)
CONFIG_EXAMPLE_LVGL_PORT_TASK_CORE=1                 # LVGL on Core 1
CONFIG_EXAMPLE_LVGL_PORT_TASK_PRIORITY=4
CONFIG_EXAMPLE_LVGL_PORT_TASK_STACK_SIZE_KB=8
CONFIG_EXAMPLE_LVGL_PORT_TASK_MAX_DELAY_MS=20
CONFIG_EXAMPLE_LVGL_PORT_TASK_MIN_DELAY_MS=2
CONFIG_EXAMPLE_LVGL_PORT_TICK=10                     # 10 ms LVGL tick
CONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_ENABLE=y         # double-buffer anti-tear
CONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_MODE=1           # mode 1 = 2 buffers in PSRAM
CONFIG_EXAMPLE_LVGL_PORT_BUF_PSRAM=y                 # frame buffer in PSRAM
CONFIG_EXAMPLE_LVGL_PORT_BUF_HEIGHT=100              # 100-line bounce buffer (160 KB internal)
CONFIG_EXAMPLE_LVGL_PORT_ROTATION_DEGREE=0           # landscape default

# ─── Coredump (deferred — see §21.4) ──────────────────────────────────────────
# CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
# CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
# CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y
```

### 4.4 Build artifacts and flash workflow

See `CLAUDE.md` for the operational flash workflow (USB native vs CH343 ports). Build outputs:

```
build/cyberdeck.bin                 # main app; flashed to ota_0 or ota_1
build/cyberdeck.elf                 # symbols (espcoredump.py + gdb)
build/partition_table/partition-table.bin
build/bootloader/bootloader.bin
build/deck_os_surface/cyberdeck.deck-os.bin   # embedded surface text
```

---

## 5. Partition Table

`partitions.csv`:

```
# Name,    Type, SubType,   Offset,    Size,      Flags
nvs,       data, nvs,       0x9000,    0x5000,
otadata,   data, ota,       0xe000,    0x2000,
ota_0,     app,  ota_0,     0x10000,   0x370000,
ota_1,     app,  ota_1,     0x380000,  0x370000,
nvs_keys,  data, nvs_keys,  0x6F0000,  0x1000,    encrypted
storage,   data, fat,       0x6F1000,  0x10F000,
```

### 5.1 Per-partition rationale

| Partition | Why this size |
|---|---|
| `nvs` (20 KB) | OS settings, granted permissions, schema versions, theme. ~50 keys per app × ~30 apps. HMAC NVS encryption operates over this transparently. |
| `otadata` (8 KB) | ESP-IDF requirement for OTA boot select. |
| `ota_0` (3520 KB) | Slot A. The compiled image (runtime + bridge + built-in apps) is ~2 MB; 3.5 MB leaves headroom for added native modules. |
| `ota_1` (3520 KB) | Slot B (mirror). Required for safe firmware OTA via partition swap. |
| `nvs_keys` (4 KB) | NVS encryption key partition. With HMAC-based encryption (default), not strictly required — the key is derived from an eFuse HMAC block. Reserved for forward compatibility. |
| `storage` (1084 KB) | FAT FS mounted at `/spiffs`. Holds VM snapshots, crash logs, OTA staging metadata, system asset cache, downloader queue journal. |

Coredump partition is **deferred** — crashes are persisted via the runtime's `MSG_PANIC` path to `/spiffs/crashes/`. A future reorg may add a dedicated coredump partition.

### 5.2 NVS namespacing

Single `nvs` partition. Per-app namespaces constructed by the `cap_nvs` driver as `app_<truncated>` (NVS limits namespace names to 15 chars). Reserved namespaces:

| Namespace | Owner |
|---|---|
| `cyberdeck` | OS (theme, brightness, locale, WiFi creds, PIN hash) |
| `factory` | Factory data (battery cal, device_id, HMAC key purpose) |
| `apps_perms` | Granted/denied capability per app |
| `apps_schema` | Migration schema version per app |
| `apps_trust` | Per-app signature verification cache |
| `apps_dict` | Reverse index from full app id → namespace name (collision detection) |
| `app_<id>` | Per-app persistent state |

---

## 6. App Bundle and Manifest Model

### 6.1 The manifest IS `app.deck`

There is no separate `manifest.json`. The app's identity, version, permissions, and deep-link handlers all live in `app.deck` itself, in the standard top-level annotations:

```deck
@app
  name:    "Bluesky"
  id:      "social.bsky.app"
  version: "1.0.0"
  icon:    "BS"

@use
  network.http   as http
  storage.local  as store
  notifications  as notif

@permissions
  network.http   reason: "Connect to Bluesky"
  storage.local  reason: "Cache your timeline"
  notifications  reason: "Notify on mentions"

@handles
  "bsky://profile/{handle}"
  "bsky://post/{rkey}"
```

The bundle discovery walker (`os_app_discover`) reads each `app.deck`'s leading annotation block (the lexer can stop at the first `fn` / `@machine` / `@flow`), parses the `@app`, `@permissions`, `@handles` declarations, and registers the app. No JSON parser, no duplicated metadata, no risk of a `manifest.json` falling out of sync with the source it describes.

The full type-checked load (`11-deck-implementation §9`) happens at app launch, not at discovery.

### 6.2 SD-resident bundle layout

A bundle is one directory under `/sdcard/apps/`. The directory name MUST equal the app's `app.id` (the discovery scanner enforces this).

```
/sdcard/apps/{app_id}/
  app.deck                # entry source AND manifest (mandatory)
  *.deck                  # additional source modules referenced via @use ./...
  assets/                 # bundled assets per @assets
    icon.png
    icon@2x.png
    splash.png
    certs/*.pem
    *.{png,jpg,wav,ttf}
  signature.bin           # Ed25519 over the bundle's canonical hash (optional)
  files/                  # per-app data sandbox (created on first launch by cap_storage_local)
  cache/                  # per-app cache (clearable on memory pressure)
  {app_id}.db             # SQLite database (created on demand by cap_db)
```

`files/`, `cache/`, and the SQLite file are created by the OS on first access — they are not part of the distributable bundle.

### 6.3 Distributable archive format

For OTA delivery and developer-side packaging, the bundle is wrapped as a **zstd-compressed tar**:

```
{app_id}-{version}.tar.zst
  containing the directory layout above (excluding files/, cache/, *.db)
```

`tar` is universally tooled; `zstd` compresses well on the small working set the device can spare. The reference packing tool is `tools/pack_app.py` (`14-deck-components §5`).

### 6.4 Canonical bundle hash

For signature and OTA verification:

```
1. List all files in the unpacked bundle (excluding signature.bin), sorted lexicographically.
2. For each file: append `path\n`, `len(file)\n`, `sha256(file)\n` to a hashing buffer.
3. The bundle hash is sha256 of the buffer.
```

`signature.bin` (when present) is Ed25519 over this hash. The signature key fingerprint is recorded in the `apps_trust` NVS namespace at install time so subsequent updates can verify they come from the same publisher.

### 6.5 Trust levels

1. **Official** — signature verified against the project's baked-in public key. Permissions still apply per `@permissions`.
2. **Community** — signature verified against a key in `system.security.trusted_keys`. Same as Official; user can revoke a key.
3. **Unsigned** — `signature.bin` absent or invalid. App may still run, but `@requires_permission` capabilities always show the dialog (no caching).

For v1, signature is **optional** and the default trust level is `unsigned`. Production policy will require signing.

### 6.6 Atomic install / upgrade

See §13.2 for the atomic-rename protocol used by `cap_ota_app`.

---

## 7. OS-Bundled Apps (Built-In, Not on SD)

### 7.1 The minimal set

Only the apps **necessary for the OS to function** are baked into the firmware image. Everything else is SD-resident (or installed via app OTA into SD).

| Slot | Annex | App | `app.id` | Why bundled |
|---|---|---|---|---|
| 0 | A | Launcher | `system.launcher` | Cannot reach any other app without it |
| 1 | B | Task Manager | `system.taskman` | Needed when the foreground app is unresponsive — must work without SD |
| 2 | C | Settings | `system.settings` | Configures WiFi, BLE, UI, security, OTA — required to bring up the system |
| 3 | D | Files | `system.files` | Browse / move / inspect SD contents; needed to repair an SD without USB MSC |

All four apps are **system-privileged** (their `app.id` starts with `system.`) and may declare the privileged capabilities `system.shell`, `system.apps`, `system.tasks`, `system.crashes`, `system.security`. Each bundled app has its own annex in `deck-lang/`:

- `annex-a-launcher.md`
- `annex-b-task-manager.md`
- `annex-c-settings.md`
- `annex-d-files.md`

Future bundled apps would slot in alphabetically (E, F, G…). The `XX` annex (`annex-xx-bluesky.md`) is the kitchen-sink demo — large, SD-resident, no fixed slot.

### 7.2 Why these four and not more

- A user with no SD card inserted can still reach the Launcher, see "no apps installed", and use Settings to configure WiFi to download apps. The OS is usable in a degraded mode.
- A user whose foreground Deck app is panicking can swipe to the Task Manager, kill it, and return to the Launcher.
- A user who needs to change theme, brightness, network, or perform a firmware OTA can do so without SD.
- A user who needs to inspect, move, copy, or delete files on the SD (or `/spiffs` system area) can do so via Files without needing an external machine — useful for recovering from a bad app install or freeing space.

Anything beyond this set is SD-resident. The bundled set is intentionally tiny so the firmware image stays small and so non-essential apps live on user-replaceable media.

### 7.3 Where the bundled apps' source lives

```
components/
  apps_bundled/
    system_launcher/
      app.deck
      assets/icon.png
      flows/*.deck
      machines/*.deck
    system_taskman/
      app.deck
      ...
    system_settings/
      app.deck
      flows/*.deck
      ...
    system_files/
      app.deck
      flows/*.deck
      ...
```

At build time, these are embedded into the firmware image as binary blobs (`target_add_binary_data` per app directory). The `os_app_discover` scanner registers them at boot **before** scanning the SD card. They never appear under `/sdcard/apps/` and cannot be uninstalled.

A bundled app is upgraded only via firmware OTA — the new firmware image carries the new bundled-app sources. SD-resident apps continue to use the per-app OTA path (§13.2).

### 7.4 Crash Reporter

Crash inspection is a **tab inside Settings** (`system.settings`), not a standalone bundled app. The tab uses the `system.crashes` capability. The user's mental model is "Settings → Diagnostics → Crashes."

The `@on crash_report` hook (`02-deck-app §11`) fires on `system.settings`. The Loader rule that restricts `@on crash_report` to apps whose `app.id == "system.crash_reporter"` is amended to also accept `app.id == "system.settings"` for v1. Full specification in `annex-c-settings.md`.

### 7.5 Lockscreen

The lockscreen is **not an app**. It is a UI surface owned by the bridge (`10-deck-bridge-ui §10`). It is rendered by the bridge UI service, validates PINs through the `system.security` driver, and never holds Deck state. This is intentional: the lockscreen must be available even if no Deck VM is running.

---

## 8. The UI Engine Is a Service Driver

The bridge UI engine — LVGL on this board — is a **service driver** (`deck.driver.bridge.ui` in `12-deck-service-drivers §6.3`), not part of the Deck runtime. The runtime emits a DVC tree (`11-deck-implementation §18`) and hands it to the UI driver via `bridge.render(tree)`. Apps never see this driver directly; the runtime is the only consumer.

This separation is what lets the same Deck app render against:

- LVGL on this board (the reference)
- A future SDL implementation on a desktop simulator
- A possible terminal-mode renderer for CI (see `14-deck-components §6.2`)
- A hypothetical e-ink renderer on a different board

The UI driver is a service, on the same footing as `storage.fs` or `network.wifi`. It is composed into the platform at boot like any other driver. The capability surface declared in `03-deck-os` does not include it because apps cannot call it directly — they describe content, the runtime translates to DVC, the driver renders.

The implications for this board:

- LVGL is in `bridge_ui_lvgl.c` under `components/deck_drivers_display_lvgl/`.
- The LVGL task lives on Core 1 at priority 4.
- All LVGL calls are mutex-protected (`lvgl_mux`, recursive).
- VSYNC notifications are IRAM-safe ISR callbacks that wake the LVGL task.

A different platform that implements `deck.driver.bridge.ui` against a different GUI library (raylib, Slint, a custom canvas) substitutes only this driver — every app keeps working.

---

## 9. Memory Architecture

### 9.1 Regions and budgets

| Region | Total | Use |
|---|---|---|
| **Internal SRAM (DRAM)** | ~250 KB usable | FreeRTOS stacks, ESP-IDF runtime control, WiFi/lwIP working buffers, DMA-required buffers, runtime heap (small allocations) |
| **Internal SRAM (IRAM)** | ~80 KB | Hot code (interrupt handlers, LVGL fast renderer; FreeRTOS itself is in flash by v6.0 default) |
| **RTC SRAM** | 16 KB | Reserved for deep-sleep wake context (not yet wired) |
| **PSRAM** | 8 MB | LVGL framebuffers (~1.5 MB for double-buffer), Deck VM heaps, snapshots, asset cache |

### 9.2 Allocation policy

- **≤ 4 KB**: internal SRAM by default (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`).
- **> 4 KB**: PSRAM by default. Runtime tags snapshot section and render arena with `MALLOC_CAP_SPIRAM`.
- **DMA-required**: MUST be in internal SRAM (`MALLOC_CAP_DMA`). PSRAM is not DMA-capable for most ESP32-S3 peripherals.
- **64 KB internal reserve** (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536`) keeps a hard floor for DMA / WiFi / lwIP even when PSRAM is full.

### 9.3 The `MALLOC_CAP_*` flags we use

| Flag | Where |
|---|---|
| `MALLOC_CAP_INTERNAL` | DMA buffers, WiFi RX/TX queues, FreeRTOS task stacks |
| `MALLOC_CAP_SPIRAM` | LVGL framebuffer, Deck VM heap (when > 4 KB), snapshot blobs, render arena, large strings |
| `MALLOC_CAP_DMA` | LCD bounce buffer, SD SPI buffers, audio I2S buffers |
| `MALLOC_CAP_8BIT` | Default for byte-addressable buffers |
| `MALLOC_CAP_32BIT` | Aligned-only buffers |
| `MALLOC_CAP_CACHE_ALIGNED` | DMA buffers shared between cores |
| `MALLOC_CAP_RTCRAM` | Future deep-sleep state |

### 9.4 LVGL framebuffer placement

800×480 RGB565 = **768 KB** per buffer. Two buffers = **1.5 MB** in PSRAM (`CONFIG_EXAMPLE_LVGL_PORT_BUF_PSRAM=y`).

The bounce buffer pattern:

- LVGL renders into the active PSRAM framebuffer (CPU writes via cache).
- The `esp_lcd_rgb_panel` driver reads from a small **bounce buffer** in internal SRAM (160 KB = 800 × 100 lines × 2 bytes).
- A high-priority ISR copies PSRAM → bounce buffer in chunks of `dma_burst_size = 64` bytes (matches the 64-byte cache line).
- DMA streams the bounce buffer out to the LCD pixel pins.

Result: stable 30 FPS sustained without LCD pixel-clock starvation. Cost: ~2% CPU on the bounce ISR.

### 9.5 Memory pressure thresholds

| Symbol | Value | Behavior |
|---|---|---|
| `MEM_LOW_SRAM_BYTES` | 64 KB | `os.memory_pressure (level: :low)` |
| `MEM_CRITICAL_SRAM_BYTES` | 32 KB | `:critical`; eviction eligible |
| `MEM_LOW_PSRAM_BYTES` | 1 MB | `:low` |
| `MEM_CRITICAL_PSRAM_BYTES` | 512 KB | `:critical` |
| `MEM_HYSTERESIS_FACTOR` | 1.5 | Free must rise above threshold × 1.5 to clear |
| `MEM_SAMPLE_INTERVAL_MS` | 1000 | `svc_memory_monitor` polls every 1 s |
| `MEM_EVICT_COOLDOWN_MS` | 2000 | Min time between consecutive evictions |

Algorithm and eviction policy: see `09-deck-shell §19` (semantics) and §10 of `09-deck-shell` (concrete platform numbers — this section is the source of truth on this board).

---

## 10. FreeRTOS Layout

ESP-IDF v6.0's FreeRTOS is based on **Vanilla FreeRTOS v10.5.1** with dual-core SMP modifications. Stack sizes are in **bytes**, critical sections require **spinlocks**, and **floating-point usage auto-pins a task to its current core**.

### 10.1 Task layout

| Task | Core | Priority | Stack | Owner |
|---|---|---|---|---|
| `IDLE0` / `IDLE1` | per-core | 0 | 1.5 KB | ESP-IDF; watchdog-monitored |
| `main` | 0 | 1 | 8 KB | `app_main()`; bootstrap |
| `esp_timer` | 0 | 22 | 4 KB | esp_idf; do not touch |
| `IPC0` / `IPC1` | per-core | 24 | 1 KB | esp_idf; inter-processor calls |
| `tcpip` | 0 | 18 | 4 KB | esp_idf (renamed from `tiT` in v6.0) |
| `wifi` | 0 | 23 | 6.5 KB | esp_idf |
| `BTU_TASK` (NimBLE) | 0 | 4 | 4 KB | esp_idf (only if BLE used) |
| `BTC_TASK` (NimBLE) | 0 | 19 | 3 KB | esp_idf (only if BLE used) |
| `cyberdeck_event_task` | 0 | 5 | 4 KB | `svc_event` |
| `lvgl_task` | 1 | 4 | 8 KB | `bridge_ui_lvgl` |
| `deck_runtime_task[N]` | 1 | 3 | 8 KB | `deck_bridge`; one per running VM (max 4) |
| `svc_poller_task` | 0 | 4 | 4 KB | `os_poller` |
| `svc_downloader_task` | 0 | 5 | 8 KB | `svc_downloader` |
| `svc_battery_task` | 0 | 3 | 2 KB | `svc_battery` (folds memory monitor) |
| `svc_time_task` | 0 | 3 | 4 KB | `svc_time` |

### 10.2 Why Core 1 is reserved for the user thread

- **Core 1**: LVGL + Deck VMs. UI responsiveness isolated from network stack.
- **Core 0**: WiFi, BLE, lwIP, event loop, `svc_*` background tasks.

Bridge calls from a Deck VM (Core 1) to a service (Core 0) cross core boundaries via the runtime mailbox (`11-deck-implementation §19`) — explicit message passing, never shared mutable state.

### 10.3 FPU policy

LVGL uses `float` for color blending and transformations. Per ESP-IDF rules, this **auto-pins** the LVGL task to its current core (Core 1). We pin it explicitly so behavior is deterministic from boot.

We do **not** use `float` in any ISR (forbidden in ESP-IDF FreeRTOS) or in any `svc_*` task on Core 0.

### 10.4 Watchdog subscriptions

`CONFIG_ESP_TASK_WDT_TIMEOUT_S=10`. Default subscribers: `IDLE0`, `IDLE1`. Additional:

- `lvgl_task` — feeds at every render cycle (~16–33 ms)
- `deck_runtime_task[N]` — feeds at top of every main loop tick

`svc_*` tasks are not subscribed (they may legitimately block on long I/O); they have per-task internal timeouts.

### 10.5 Power management locks

With `CONFIG_PM_ENABLE=y` and `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`, the system enters light sleep automatically when idle. Per-task locks gate this:

- `lvgl_task` holds `ESP_PM_NO_LIGHT_SLEEP` while a render is in flight.
- `deck_runtime_task[N]` holds `ESP_PM_NO_LIGHT_SLEEP` during dispatch and during `@on launch` / `@on resume`.
- `svc_downloader_task` holds `ESP_PM_NO_LIGHT_SLEEP` while a download is active.
- `svc_wifi` holds `ESP_PM_APB_FREQ_MAX` during connection handshake; once associated, switches to `WIFI_PS_MIN_MODEM` and releases the lock.
- ESP-IDF auto-managed peripherals (SPI master, I²C, I²S, SDMMC, UART, GPTimer, WiFi, BT) handle their own locks transparently.

CPU current with PM enabled: ~120 mA (active) → ~12 mA (idle, WiFi connected). Backlight is the dominant load when on.

---

## 11. Power Management

### 11.1 Modes

| Mode | Trigger | Wake sources |
|---|---|---|
| Active | User input within `screen_timeout` | n/a |
| Idle (DFS) | No CPU work for ≥ 100 ms | Any FreeRTOS event |
| Light sleep | Display off + nothing scheduled in next 100 ms | Touch (when wired), timer, RTC alarm, USB activity |
| Deep sleep | Long idle + opt-in via Settings | RTC alarm, touch threshold, EXT0/EXT1 GPIO |

Default policy in v1: **never enter deep sleep automatically.** Snapshot restore from cold boot is slow and WiFi reconnect adds 3–5 s.

### 11.2 Light sleep specifics

ESP-IDF enters light sleep automatically when:

- `CONFIG_PM_ENABLE=y` and `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`
- All FreeRTOS tasks blocked
- No timer due within 100 ms
- No PM lock held (§10.5)

Latency cost: 0.2–40 µs added interrupt latency per ESP-IDF docs.

### 11.3 Display timeout

Configurable; default 30 s. Backlight off via `hal_backlight_off()` (clears CH422G OUT bit 2). After `auto_lock_timeout` (default 2 min), `app_manager_lock()` pushes the lockscreen.

### 11.4 Battery and charging

`svc_battery_task` samples ADC1_CH0 (GPIO 1) every 5 s. Voltage → percent via lookup table calibrated at factory time (NVS namespace `factory`). Charging detection: USB Vbus polled via the expander.

Events:

- `EVT_BATTERY_UPDATED` on percent change
- `EVT_BATTERY_LOW` at 20%
- `EVT_BATTERY_CRITICAL` at 5% — auto-snapshot foreground VM, dim display 50%
- At 2% — clean shutdown (snapshot all VMs, then `esp_restart`)

### 11.5 Background fetch (deferred)

`cap_background_fetch` would implement `background_fetch` via deep sleep + RTC alarm. Requires bootloader-mode detection for "background boot" path. Deferred.

---

## 12. Security

### 12.1 Layered defense

| Layer | Mechanism | Status |
|---|---|---|
| Boot integrity | Secure Boot v2 (RSA-3072) | **Optional**; opt-in for production |
| Image confidentiality | Flash Encryption (XTS-AES-256) | **Optional**; opt-in for production |
| Anti-rollback | 16-bit `secure_version` + `SECURE_VERSION` eFuse | **Enabled** in v1 default |
| Per-app data isolation | Path-prefix sandbox + per-app NVS namespace | **Enabled** by SDI driver wrappers |
| Capability gating | `@requires_permission` checks at load | **Enabled**; grants in NVS `apps_perms` |
| Restricted capabilities (`system.*`) | Only `app.id` starting `system.` may declare | **Enabled** at load |
| Lockscreen PIN | Argon2id with per-device salt | **Enabled**; rate-limited |
| TLS | mbedTLS 4.0 + PSA Crypto, hardware AES/SHA/MPI | **Enabled** by default |
| App signing | Ed25519 over canonical bundle hash | **Optional** in v1 |
| JTAG | `DIS_PAD_JTAG` eFuse for production | **Deferred** |
| NVS encryption | HMAC-based (eFuse block 0) | **Enabled** in v1 default |

### 12.2 Secure Boot v2 specifics

ESP32-S3 uses **RSA-PSS with 3072-bit keys exclusively** (no ECDSA). Up to three public-key digests in `BLOCK_KEY0..2`. Per-key revocation via `KEY_REVOKE0..2` eFuses.

Conservative revocation: revoke an old key only after OTA migration to a new key has succeeded fleet-wide. **Aggressive** mode (`CONFIG_SECURE_BOOT_ENABLE_AGGRESSIVE_KEY_REVOKE`) can brick devices on any failed verify — do not enable.

### 12.3 Flash Encryption specifics

- Mode: **Release** for production (`CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y`).
- Algorithm: XTS-AES-256.
- Always encrypts: bootloader, partition table, all app partitions, otadata, `nvs_keys`.
- One-time first-boot encryption: "up to a minute for large partitions."
- Once enabled, **cannot be disabled** without burning further eFuses.

### 12.4 NVS encryption (HMAC-based)

`CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC=y` and `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=0`:

- Uses ESP32-S3's HMAC peripheral with an eFuse key (block 0).
- Derives NVS encryption keys on-demand. The key never leaves the hardware accelerator.
- Works **without** flash encryption — useful in development.
- ~2.5× slower NVS integer operations on cache miss (negligible for typical usage).

Provision the HMAC key at factory time:

```bash
espefuse.py burn-key BLOCK_KEY0 my_hmac_key.bin HMAC_UP
```

Once burned, the key is unreadable from software.

### 12.5 Anti-rollback

`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y` + `CONFIG_BOOTLOADER_APP_SEC_VER_SIZE_EFUSE_FIELD=16` give a 16-bit `secure_version` in `esp_app_desc_t`. The bootloader rejects images below the eFuse threshold. After successful boot validation, the bootloader commits the new version to eFuse (irreversible).

Use sparse versioning — increment by 1 per release. 65,536 releases ceiling (1 release/day = 180 years).

### 12.6 PIN hashing

Argon2id parameters tuned for ESP32-S3:

- Memory: 4 MB (PSRAM)
- Iterations: 3
- Parallelism: 1
- Salt: 16 bytes per device (NVS `cyberdeck:pin_salt`)
- Output: 32 bytes

A PIN check takes ~250 ms — acceptable on the lockscreen.

### 12.7 What v1 does NOT enforce

- Code signing for apps (default trust = `unsigned`)
- Encrypted SD card
- Secure Boot enabled by default (production opt-in)
- Coredump encryption

The v1 threat model is "single-user device not exposed to physical attackers." Production hardening is a separate roadmap.

---

## 13. OTA — Dual Track

There are **two independent OTA pipelines**, with separate drivers (`12-deck-service-drivers §8`), separate UI flows, and separate trust models:

- **Firmware OTA** — replaces the entire OS image (interpreter, bridge, all built-in code, `system.launcher` / `system.taskman` / `system.settings`). Uses `esp_https_ota` partition swap + anti-rollback. Requires reboot.
- **App OTA** — replaces one SD-resident app bundle. Uses the shared `cap_downloader` (same one used for podcasts and large media downloads). No reboot. Atomic via `.staging/` + rename.

This separation is intentional. A user-app update should never depend on a working firmware OTA pipeline, and a firmware update should never touch SD-resident user data.

### 13.1 Firmware OTA — `cap_ota_firmware`

#### Flow

```
ota.firmware.check()
  → fetch manifest URL via esp_http_client
  → returns { available_version, secure_version, url, sha256, size, min_runtime }

ota.firmware.download(url)
  → esp_https_ota_begin() opens HTTPS (TLS via baked-in roots + per-app certs)
  → esp_https_ota_perform() loop: read chunks, write to inactive ota_X partition
       fires EVT_OTA_PROGRESS every 1% → bridge fires os.download_progress
  → on complete: esp_https_ota_finish() validates length and checksum
  → Secure Boot v2 signature is verified by the bootloader on next boot

ota.firmware.apply()
  → esp_ota_set_boot_partition(inactive)
  → mark slot PENDING_VALIDATION (otadata)
  → esp_restart()

After reboot — validation window (30 s):
  → new firmware boots; bootloader sees PENDING_VALIDATION
  → runtime starts a 30 s timer
  → on user touch OR explicit ota.confirm() → esp_ota_mark_app_valid_cancel_rollback()
       slot is COMMITTED
  → on timer expiry → esp_ota_mark_app_invalid_rollback_and_reboot()
       bootloader rolls back to previous slot
  → on panic during boot: panic counter in RTC SRAM; auto-rollback after 3 strikes
```

Restricted to apps with `app.id` starting `system.` (i.e. `system.settings` in v1).

#### Power-loss safety

| Failure point | Recovery |
|---|---|
| Mid-download | `/spiffs/ota/pending.meta` records progress; resume via HTTP Range |
| Between `apply` and reboot | otadata atomic write per ESP-IDF |
| During validation window | Auto-rollback on next boot (otadata still PENDING) |
| Panic loop | Bootloader 3-strike RTC counter |

### 13.2 App OTA — `cap_ota_app`

#### Flow

```
ota.app.install(app_id, url)
  → cap_downloader.enqueue(url, dest=/sdcard/.staging/{app_id}.tar.zst)
      svc_downloader_task pulls from queue, downloads via esp_http_client + Range
      progress callback fires (DECK_APP_OTA_PHASE_DOWNLOADING)

  → on download complete:
       SHA-256 verify against manifest    (DECK_APP_OTA_PHASE_VERIFYING)
       Ed25519 verify if opts.verify_signature (PSA Crypto)

  → unpack archive (DECK_APP_OTA_PHASE_UNPACKING)
       streaming tar+zstd to /sdcard/.staging/{app_id}/

  → read new app.deck @app block; check compatibility:
       min_deck_runtime <= current_runtime_version
       deck_os_version  >= manifest.deck_os_version

  → if app currently running:
       send MSG_SUSPEND to its VM (deadline 500 ms)
       snapshot to /spiffs/snapshots/{app_id}.snap
       free the VM slot

  → atomic swap (DECK_APP_OTA_PHASE_SWAPPING):
       if apps/.old/{app_id} exists, delete it
       rename apps/{app_id} → apps/.old/{app_id}
       rename .staging/{app_id} → apps/{app_id}

  → run migrations (DECK_APP_OTA_PHASE_MIGRATING)
       runtime walks @migration chain in the new app

  → on success:
       fire EVT_APP_INSTALLED → os.app_installed to launcher
       after confidence period (next successful launch),
       delete apps/.old/{app_id} asynchronously

  → on any failure:
       roll back: rename apps/.old/{app_id} → apps/{app_id}
       restore snapshot if app was running
       notify caller via DeckResult :err
```

#### Why we share the downloader

`cap_downloader` is the **same** service used by:

- `cap_ota_app` (downloading new app bundles to SD staging)
- `cap_ota_firmware` for very large firmware images (when partial download is enabled)
- A music app downloading podcast episodes
- A photos app caching remote images
- Any future media-fetching app

Reasons:

- **One queue, one rate limit, one pool.** A user updating an app shouldn't have podcast downloads racing the bandwidth.
- **One persistence model.** Downloads survive reboot via a queue journal in `/spiffs/downloader/queue.json`. After power loss, in-flight downloads resume.
- **One UI surface.** A unified downloads list in Settings.
- **Smaller footprint.** No duplicated HTTP Range / TLS / progress reporting logic.

The downloader exposes a custom builtin (`downloader.*`) to apps that have a `download` permission (typically multimedia apps). Internal `cap_ota_*` consumers bypass the permission check (privileged).

#### Atomicity guarantees

- `apps/{app_id}` is **never** in a half-modified state visible to apps. Either old or new is in place; staging lives at `.staging/{app_id}` (leading `.` excluded by the discovery scanner).
- The atomic primitive is FAT FS rename within the same directory.
- On any error during swap, rollback is a straight rename of `.old/{app_id}` back.

### 13.3 Manifest format (downloader-side)

Both pipelines expect a manifest at the URL given to `check()`:

```json
{
  "schema": 1,
  "kind": "firmware",                  // "firmware" | "app"
  "app_id": "social.bsky.app",         // for kind=app only
  "version": "1.0.0",
  "secure_version": 12,                // for kind=firmware (anti-rollback)
  "min_deck_runtime": 1,
  "deck_os_version": 1,
  "url": "https://example.com/cyberdeck-1.0.0.bin",
  "size": 2097152,
  "sha256": "abcd1234...",
  "signed_by": "official",             // "official" | "community" | <fingerprint>
  "signature": "base64-ed25519-sig",
  "release_notes_url": "https://example.com/release-notes/1.0.0"
}
```

The manifest itself is signed (`signature` covers everything else). `signed_by` maps to a public key registered in NVS `apps_trust`.

For app updates, the manifest is **separate from `app.deck`**. `app.deck` is the in-bundle metadata; the OTA manifest is the network-distribution descriptor with hash, signature, and rollout policy.

### 13.4 Validation window

After `ota.firmware.apply()` and reboot, the new firmware enters validation. Confirmation sources:

- Any user touch event within 30 s
- Explicit `ota.firmware.confirm()` call
- A successful network round-trip (configurable; off by default to avoid auto-confirm on a hostile network)

Rollback sources:

- Validation timer expiry (30 s) without confirm
- Any panic during boot (3-strike RTC counter)
- Explicit `ota.firmware.rollback()`

If the user does not interact within 30 s but the system is otherwise stable, we **roll back** — we'd rather miss a fix than break a device the user is not present to triage.

---

## 14. Service Architecture (svc_* and cap_*)

### 14.1 Module catalog

| Module | Task | Backs SDI driver |
|---|---|---|
| `svc_wifi` | uses esp_wifi's task | `deck.driver.network.wifi` |
| `svc_settings` | passive (NVS-backed cache) | (consumed by many) |
| `svc_event` | `cyberdeck_event_task` | (internal event bus) |
| `svc_battery` | `svc_battery_task` (also memory monitor) | `deck.driver.system.battery` |
| `svc_time` | `svc_time_task` | `deck.driver.system.time` |
| `svc_storage` | passive | (consumed by storage drivers) |
| `svc_downloader` | `svc_downloader_task` | `deck.driver.network.downloader` |
| `svc_notifications` | shares `svc_poller_task` | `deck.driver.network.notifications` |
| `svc_http` | passive (per-session) | `deck.driver.network.http` |
| `svc_ota` | passive (uses downloader) | `deck.driver.ota.firmware` |
| `os_app_storage` | passive | `deck.driver.storage.local`, `.fs`, `.db` |
| `os_app_nvs` | passive | `deck.driver.storage.nvs` |
| `os_app_discover` | passive (called on mount) | (internal) |
| `os_process` | passive | `deck.driver.system.tasks` |
| `os_task` | passive (factory) | (used by Deck task pool) |
| `os_poller` | shares `svc_poller_task` | `deck.driver.network.notifications` |
| `os_defer` | passive | (utility) |

Each `cap_*` C module wraps the `svc_*` (and/or relevant ESP-IDF component) and presents the SDI vtable. The full mapping appears in §6 — wait, that section moved. The mapping table is below in §14.3.

### 14.2 Why services live in C, not Deck

Services hold native handles (sockets, file descriptors, HTTP clients, MQTT clients) and run on dedicated FreeRTOS tasks for parallelism with the user-facing Deck VM. Implementing them in Deck would require either:

- A second VM dedicated to system services (extra heap, complexity)
- Cooperative multitasking inside one VM (defeats parallelism)

The chosen design places heavy lifting in C, exposes thin Deck capability surfaces. The SDI (`12-deck-service-drivers`) makes the boundary explicit and portable.

### 14.3 Driver-to-component mapping

| SDI driver | C module | ESP-IDF backing |
|---|---|---|
| `deck.driver.storage.local` | `cap_storage_local.c` | `os_app_storage` on `esp_vfs_fat` |
| `deck.driver.storage.nvs` | `cap_nvs.c` | `nvs_flash` with HMAC encryption |
| `deck.driver.storage.fs` | `cap_fs.c` | `esp_vfs_fat` over SPI SD |
| `deck.driver.storage.db` | `cap_db.c` | SQLite (managed component) |
| `deck.driver.storage.cache` | `cap_cache.c` | In-memory LRU in PSRAM |
| `deck.driver.network.wifi` | `cap_wifi.c` | `esp_wifi` with `WIFI_PS_MIN_MODEM` |
| `deck.driver.network.http` | `cap_http.c` | `esp_http_client` + mbedTLS PSA |
| `deck.driver.network.socket` | `cap_socket.c` | lwIP BSD sockets |
| `deck.driver.network.mqtt` | `cap_mqtt.c` | managed `espressif/mqtt` |
| `deck.driver.network.downloader` | `cap_downloader.c` | `esp_http_client` + HTTP Range + WL atomic writes |
| `deck.driver.network.notifications` | `cap_notifications.c` | `svc_notifications` over SQLite |
| `deck.driver.display.panel` | `cap_panel.c` | `esp_lcd_panel_rgb` + bounce buffer |
| `deck.driver.display.touch` | `cap_touch.c` | `esp_lcd_touch_gt911` over `i2c_master_dev` |
| `deck.driver.display.theme` | `cap_theme.c` | `svc_settings` + `ui_theme` |
| `deck.driver.display.notify` | `cap_notify.c` | `ui_effect_toast` |
| `deck.driver.display.screen` | `cap_screen.c` | `hal_backlight` + `esp_lcd_panel_disp_on_off` |
| `deck.driver.bridge.ui` | `bridge_ui_lvgl.c` | LVGL 8.4 + `esp_lvgl_port` (renders DVC_MARKDOWN per `10-deck-bridge-ui §4.2`) |
| `deck.driver.markdown` | `cap_markdown.c` | md4c parser + per-doc render arena; backs `@builtin md` (pure) and `@capability markdown` (streaming + editor state). Always present; no fallback. |
| `deck.driver.system.info` | `cap_system_info.c` | `esp_chip_info` + NVS factory |
| `deck.driver.system.locale` | `cap_system_locale.c` | NVS-backed |
| `deck.driver.system.time` | `cap_system_time.c` | `esp_sntp` + PCF85063A |
| `deck.driver.system.battery` | `cap_system_battery.c` | `svc_battery` over ADC |
| `deck.driver.system.security` | `cap_system_security.c` | NVS + Argon2id + HMAC eFuse |
| `deck.driver.system.shell` | `cap_system_shell.c` | `app_manager` + `svc_event` |
| `deck.driver.system.apps` | `cap_system_apps.c` | `app_registry` |
| `deck.driver.system.tasks` | `cap_system_tasks.c` | FreeRTOS stats + `heap_caps_*` |
| `deck.driver.system.crashes` | `cap_system_crashes.c` | `/spiffs/crashes/` ring |
| `deck.driver.system.audio` | `cap_system_audio.c` | UART1 (BT module) or PWM via I2S |
| `deck.driver.ota.firmware` | `cap_ota_firmware.c` | `esp_https_ota` + anti-rollback |
| `deck.driver.ota.app` | `cap_ota_app.c` | `cap_downloader` + tar/zst + atomic rename |
| `deck.driver.ble` | `cap_ble.c` | NimBLE |
| `deck.driver.bt_classic` | `cap_bt_classic.c` | UART1 module AT-commands |
| `deck.driver.crypto.aes` | `cap_crypto_aes.c` | PSA Crypto API (HW AES backed) |
| `deck.driver.sensors.*` | `cap_sensors_stub.c` | n/a (returns `:err :unavailable`) |
| `deck.driver.hardware.uart` | `cap_hardware_uart.c` | `driver/uart.h` |
| `deck.driver.background_fetch` | `cap_background_fetch.c` | RTC alarm + deep sleep (deferred) |

### 14.4 Inter-service contention

| Resource | Mechanism |
|---|---|
| I²C bus | New `i2c_master` driver is internally thread-safe per device handle |
| SPI for SD | Internally serialized by FATFS |
| NVS | ESP-IDF NVS API is internally thread-safe |
| WiFi driver | Thread-safe by design |
| HTTP client pool | Each session has its own `esp_http_client`; bounded by `LWIP_MAX_SOCKETS=10` |

Deck VMs cannot directly contend on these — capability calls go through `deck_bridge`, serialized per VM and arbitrating across services using the per-resource locks above.

---

## 15. Display Pipeline

### 15.1 RGB panel config (v6.0-correct)

```c
esp_lcd_rgb_panel_config_t panel_config = {
    .clk_src = LCD_CLK_SRC_DEFAULT,
    .timings = {
        .pclk_hz           = 16 * 1000 * 1000,
        .h_res             = 800,
        .v_res             = 480,
        .hsync_pulse_width = 4,
        .hsync_back_porch  = 8,
        .hsync_front_porch = 8,
        .vsync_pulse_width = 4,
        .vsync_back_porch  = 8,
        .vsync_front_porch = 8,
        .flags.pclk_active_neg = 1,
    },
    .data_width            = 16,                                   /* RGB565 */
    .num_fbs               = 2,                                    /* double-buffer */
    .bounce_buffer_size_px = 800 * CONFIG_EXAMPLE_LVGL_PORT_BUF_HEIGHT,
    .dma_burst_size        = 64,                                   /* matches 64-byte cache line */
    .rgb_ele_order         = LCD_RGB_ELEMENT_ORDER_RGB,            /* v6.0: replaces color_space + rgb_endian */

    .hsync_gpio_num        = HAL_LCD_HSYNC,
    .vsync_gpio_num        = HAL_LCD_VSYNC,
    .de_gpio_num           = HAL_LCD_DE,
    .pclk_gpio_num         = HAL_LCD_PCLK,
    .disp_gpio_num         = -1,
    .data_gpio_nums        = { HAL_LCD_DATA0, HAL_LCD_DATA1, ..., HAL_LCD_DATA15 },

    .flags.fb_in_psram     = 1,
};

esp_lcd_panel_handle_t panel;
esp_lcd_new_rgb_panel(&panel_config, &panel);
esp_lcd_panel_init(panel);
esp_lcd_panel_disp_on_off(panel, true);    /* v6.0: replaces removed esp_lcd_panel_disp_off() */
```

`gpio_num_t` typing is mandatory (v6.0); `HAL_LCD_*` macros must resolve to typed values.

### 15.2 VSYNC callback (IRAM-safe)

```c
IRAM_ATTR static bool on_frame_buf_complete(esp_lcd_panel_handle_t panel,
                                             const esp_lcd_rgb_panel_event_data_t *edata,
                                             void *user_ctx) {
    return ui_engine_notify_vsync_from_isr();
}

esp_lcd_rgb_panel_event_callbacks_t cbs = {
    .on_frame_buf_complete = on_frame_buf_complete,
};
esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, NULL);
```

In v6.0, `on_frame_buf_complete` replaces the v5.x `on_bounce_frame_finish`.

### 15.3 LVGL task and mutex

`bridge_ui_lvgl.c`:

- `lvgl_task` pinned to Core 1 at priority 4 (above WiFi background; below ESP timer at 22).
- Recursive mutex `lvgl_mux` protects all LVGL calls. The bridge wraps every `cap_*` call that touches LVGL.
- `ui_lock(timeout_ms)` / `ui_unlock()` from `ui_engine` are the canonical entry points.

### 15.4 Touch driver (new I²C API)

```c
i2c_master_bus_config_t bus_cfg = {
    .clk_source                  = I2C_CLK_SRC_DEFAULT,
    .i2c_port                    = I2C_NUM_0,
    .scl_io_num                  = HAL_I2C_SCL,
    .sda_io_num                  = HAL_I2C_SDA,
    .glitch_ignore_cnt           = 7,
    .flags.enable_internal_pullup = true,
};
i2c_master_bus_handle_t i2c_bus;
i2c_new_master_bus(&bus_cfg, &i2c_bus);

esp_lcd_panel_io_handle_t tp_io;
esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
esp_lcd_new_panel_io_i2c_v2(i2c_bus, &io_cfg, &tp_io);

esp_lcd_touch_handle_t tp;
esp_lcd_touch_config_t cfg = {
    .x_max = 800, .y_max = 480,
    .rst_gpio_num = -1,                /* via CH422G expander instead */
    .int_gpio_num = -1,                /* polling mode */
    .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
};
esp_lcd_touch_new_i2c_gt911(tp_io, &cfg, &tp);
```

`esp_lcd_new_panel_io_i2c_v2` is the v6.0 entry point that takes an `i2c_master_bus_handle_t` instead of a legacy port number.

---

## 16. Networking

### 16.1 WiFi

`svc_wifi` wraps `esp_wifi`:

- Mode: STA only in v1 (no SoftAP)
- Auto-reconnect: 3× with 5 s backoff; then post `EVT_WIFI_DISCONNECTED { reason: :max_retries }`
- Power save: `WIFI_PS_MIN_MODEM` once associated; `WIFI_PS_NONE` during scan/connect
- Stored credentials: NVS `cyberdeck:wifi_ssid` + `:wifi_psk`, encrypted via HMAC NVS encryption

### 16.2 TLS

mbedTLS 4.0 + PSA Crypto:

- **Baked-in roots** via `esp_crt_bundle` with `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y` (curated ~38 certs, ~93% web coverage). Saves ~150 KB vs full Mozilla bundle.
- **Per-app roots** from `@assets … as: :tls_cert for_domain: "host"` declarations — loaded at app launch, freed on terminate.
- **Per-call override** via `HttpOptions { tls_ca_cert: asset(:name) }`.
- **Hardware accel**: `MBEDTLS_HARDWARE_AES`, `MBEDTLS_HARDWARE_SHA`, `MBEDTLS_HARDWARE_MPI`. RSA-3072 verify drops from ~600 ms (software) to ~50 ms.
- **Dynamic buffers**: `MBEDTLS_DYNAMIC_BUFFER` saves ~20 KB per connection.

### 16.3 HTTP client pool

`esp_http_client` for both `cap_http` and `cap_downloader`. Pool bounded by `LWIP_MAX_SOCKETS=10`. Connection reuse (Keep-Alive) automatic when server doesn't send `Connection: close`. A single `esp_http_client_handle_t` is **not** safe for concurrent use; each parallel request needs its own.

### 16.4 MQTT

`espressif/mqtt` managed component. One client per (broker, app) pair. QoS 0/1/2. TLS via the per-app trust map.

### 16.5 Downloader

`svc_downloader` (a/k/a `cap_downloader`) is the queued large-download service shared by OTA and multimedia (§13.2). Persistent journal at `/spiffs/downloader/queue.json` for survival across reboots.

---

## 17. I²C Bus Arbitration

### 17.1 Bus topology

| Address | Device | Owner |
|---|---|---|
| `0x14` | GT911 (touch) | `hal_touch` (via `esp_lcd_touch_gt911`) |
| `0x38` | CH422G (I/O expander) | `hal_ch422g` |
| `0x51` | PCF85063A (RTC) | `hal_rtc` |

Single I²C bus, GPIO 8/9 at 100 kHz.

### 17.2 New I²C driver (v6.0 mandatory)

```c
i2c_master_bus_config_t bus_cfg = {
    .i2c_port                    = I2C_NUM_0,
    .sda_io_num                  = HAL_I2C_SDA,
    .scl_io_num                  = HAL_I2C_SCL,
    .clk_source                  = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt           = 7,
    .flags.enable_internal_pullup = true,
};
i2c_master_bus_handle_t s_i2c_bus;
i2c_new_master_bus(&bus_cfg, &s_i2c_bus);

i2c_device_config_t ch422g_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = 0x38,
    .scl_speed_hz    = 100000,
};
i2c_master_dev_handle_t s_ch422g;
i2c_master_bus_add_device(s_i2c_bus, &ch422g_cfg, &s_ch422g);
/* similarly for GT911 (0x14) and PCF85063A (0x51) */
```

The new API is **internally thread-safe** per device handle. We no longer need a separate `s_i2c_mutex`.

### 17.3 Touch interrupt handling

GT911 INT pin currently **not wired** (`int_gpio_num = -1`). Touch is polled by the LVGL task at ~10 ms cadence (`CONFIG_EXAMPLE_LVGL_PORT_TASK_MIN_DELAY_MS=2`).

When/if wired, the ISR sets a flag; the LVGL task reads it and performs the I²C transaction in task context (the new API requires task context).

---

## 18. UART, SPI, GPIO, ADC

### 18.1 UART

| Port | Use | Pins | Baud |
|---|---|---|---|
| UART0 | Console (CH343 USB-serial) | TX=GPIO 43, RX=GPIO 44 | 115200 |
| UART1 | Optional BT module | TX=GPIO 15, RX=GPIO 16 | 115200 default; up to 1.5 Mbps for audio |

### 18.2 SPI for SD

`sdspi_host` driver. Frequency capped at `SDMMC_FREQ_DEFAULT` (~20 MHz). Pins: MISO=GPIO 13, MOSI=GPIO 11, SCK=GPIO 12, CS via CH422G OUT bit 4.

### 18.3 GPIO

`gpio_num_t` typed throughout (v6.0). Strapping pins on ESP32-S3:

- **GPIO 0** (BOOT): used by RGB LCD as DATA6 — never as runtime button
- **GPIO 3**: ROM log output level
- **GPIO 45**: SDIO host/slave, download mode
- **GPIO 46**: UART download mode

### 18.4 ADC

ADC1_CH0 (GPIO 1) for battery sensing. Curve Fitting calibration scheme.

```c
adc_oneshot_unit_handle_t adc1;
adc_oneshot_unit_init_cfg_t init = { .unit_id = ADC_UNIT_1, .ulp_mode = ADC_ULP_MODE_DISABLE };
adc_oneshot_new_unit(&init, &adc1);

adc_oneshot_chan_cfg_t cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
adc_oneshot_config_channel(adc1, ADC_CHANNEL_0, &cfg);
```

---

## 19. External Bluetooth Module (UART1)

Optional HC-05 / BT201 family module on UART1. Auto-detected via `AT\r\n` → `OK\r\n` at boot. Used for A2DP audio out only. AT-command set documented in the `cap_system_audio` driver implementation.

---

## 20. Logging

### 20.1 ESP-IDF integration

`bridge.log(level, message)` maps to:

| Deck level | ESP-IDF macro |
|---|---|
| `:debug` | `ESP_LOGD("deck:" app_id, ...)` |
| `:info` | `ESP_LOGI("deck:" app_id, ...)` |
| `:warn` | `ESP_LOGW("deck:" app_id, ...)` |
| `:error` | `ESP_LOGE("deck:" app_id, ...)` |

`CONFIG_LOG_VERSION_2=y` enables hierarchical log filtering and reduces binary size.

Runtime control:

```c
esp_log_level_set("deck:social.bsky.app", ESP_LOG_DEBUG);
esp_log_level_set("svc_*", ESP_LOG_INFO);
```

### 20.2 Persistence (optional)

Off by default. When enabled in Settings > Diagnostics, log lines `:warn` and above are appended to `/sdcard/system/logs/system-{date}.log`. Daily rotation, 30-day retention.

---

## 21. Crash Reporting

### 21.1 Sources

- **ESP-IDF panic handler** — hardware faults, stack overflows, watchdog timeouts
- **Deck runtime `MSG_PANIC`** — Deck-side panics
- **Bridge contract violations** — capability returns wrong type, opaque misuse

### 21.2 Crash log format

```json
{
  "schema": 1,
  "timestamp": "2026-04-16T20:42:31Z",
  "kind": "deck_panic" | "esp_panic" | "watchdog" | "bridge_contract",
  "app_id": "social.bsky.app",
  "message": "stack overflow",
  "stack": "...",
  "vm_state": "...",
  "fw_version": "1.0.0",
  "fw_secure_version": 12,
  "free_sram_at_crash": 28160,
  "free_psram_at_crash": 5242880
}
```

Decoded host-side via `idf.py monitor` with `cyberdeck.elf`.

### 21.3 Storage ring

`/spiffs/crashes/`:

```
crashes.idx                    # 64-byte header (write_idx, head_idx, total_writes)
crash_{N}.log                  # 0..49 slots, ring buffer
```

`cap_system_crashes` exposes this to the Settings → Diagnostics tab.

### 21.4 Coredump (deferred)

Adding a `coredump` partition would enable full ESP-IDF coredump capture (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`), decoded with `espcoredump.py`. Useful for hard-to-reproduce native crashes; deferred until partition layout reorg.

---

## 22. Hardware Pin Table

Authoritative source: `components/board/include/hal_pins.h`.

| Function | Pin | Direction | Notes |
|---|---|---|---|
| RGB LCD DATA0..15 | GPIO 7,2,3,4,5,6,0,1 (low byte); 39,40,41,42,43,44,45,46 (high byte) | OUT | RGB565; **GPIO 0 is DATA6** |
| RGB LCD HSYNC | GPIO 38 | OUT | |
| RGB LCD VSYNC | GPIO 17 | OUT | |
| RGB LCD DE | GPIO 18 | OUT | |
| RGB LCD PCLK | GPIO 21 | OUT | |
| LCD reset | CH422G OUT bit 3 | n/a | Via I²C expander |
| LCD backlight | CH422G OUT bit 2 | n/a | Via I²C expander |
| Touch reset | CH422G OUT bit 1 | n/a | Via I²C expander |
| Touch INT | (unused in v1) | IN | Wire to GPIO 4 if migrating to interrupt mode |
| I²C SDA | GPIO 8 | I/O | 100 kHz |
| I²C SCL | GPIO 9 | OUT | |
| SD chip select | CH422G OUT bit 4 | n/a | Via I²C expander |
| SD MISO | GPIO 13 | IN | SPI3 |
| SD MOSI | GPIO 11 | OUT | SPI3 |
| SD SCK | GPIO 12 | OUT | SPI3 |
| Battery ADC | GPIO 1 | ADC1_CH0 | Voltage divider 1:2 |
| USB D+ | GPIO 19 | I/O | Native USB; never to CAN |
| USB D− | GPIO 20 | I/O | Same |
| USB_SEL | CH422G OUT bit 5 | n/a | **Must remain LOW** |
| UART0 TX | GPIO 43 | OUT | 115200 to CH343 |
| UART0 RX | GPIO 44 | IN | |
| UART1 TX | GPIO 15 | OUT | BT module; up to 1.5 Mbps |
| UART1 RX | GPIO 16 | IN | |
| BOOT | GPIO 0 | n/a | Held LOW during reset = download mode |

### 22.1 CH422G OUT register (`0x38`)

Always read-modify-write.

| Bit | Name | Notes |
|---|---|---|
| 0 | reserved | |
| 1 | TP_RST | Touch reset, active-low |
| 2 | BL | Backlight, active-high |
| 3 | LCD_RST | LCD reset, active-low |
| 4 | SD_CS | SD chip select, active-low |
| 5 | USB_SEL | 0 = USB native (must be 0) |
| 6 | reserved | |
| 7 | reserved | |

Named constants `HAL_CH422G_BIT_*` in `hal_ch422g.h`.

---

## 23. Boot Sequence

| T (ms) | Phase | What happens |
|---|---|---|
| 0 | ROM | First-stage bootloader |
| 5 | `app_main()` entered | Console UART up; heap_caps_get_free_size logged |
| 15 | Board init | `board_init()` → CH422G probe via `i2c_master_dev`; default OUT register; **USB_SEL=LOW** |
| 30 | LCD init | `hal_lcd_init()` → RGB controller, framebuffer alloc in PSRAM |
| 45 | Touch init | `hal_touch_init()` → GT911 probe over I²C |
| 55 | Backlight on | `hal_backlight_on()` (after touch reset which clobbers expander) |
| 60 | Battery / RTC | `hal_battery_init()`; `hal_rtc_init()` |
| 80 | Storage mount | `svc_storage_init()` → mount `/spiffs`; attempt `/sdcard` mount |
| 200 | App discovery | If `/sdcard` mounted: scan `/sdcard/apps/` for installed bundles (read each `app.deck` @app block) |
| 250 | Settings | `svc_settings_init()` → read NVS namespace `cyberdeck` |
| 260 | Event loop | `svc_event_init()` → `cyberdeck_event_task` started |
| 280 | WiFi | `svc_wifi_init()` → start WiFi driver; auto-connect to last AP |
| 290 | Battery monitor | `svc_battery_init()` (also starts memory monitor) |
| 300 | Time | `svc_time_init()` → register NTP sync |
| 310 | Notifications | `svc_notifications_init()` → load existing notifications |
| 320 | OTA check | `svc_ota_init()` → check OTA bootmode; mark valid or initiate rollback |
| 330 | UI init | `ui_engine_init()` → `lvgl_task` started on Core 1 |
| 400 | Runtime pool | `deck_runtime_pool_init()` → up to 4 VM task slots (empty) |
| 410 | Bridge / drivers | `deck_bridge_init()` → load `.deck-os` surface; register all `cap_*` drivers; verify against surface |
| 420 | App framework | `app_manager_init()` → activity stack |
| 430 | App registry | `app_registry_init()` → register **bundled** apps (Launcher 0, TaskMan 1, Settings 2) FIRST, then SD-discovered apps starting at slot 256 (`APP_ID_DYNAMIC_BASE`) |
| 440 | Launch launcher | Create `deck_runtime_task[0]`; load `system.launcher` source (embedded blob); run loader (~50 ms); run `@on launch`; emit first render |
| 600 | Visible | Launcher screen presented |

Cold boot: ~600 ms with /sdcard, ~400 ms without.

### 23.1 Boot variants

- **Snapshot resume**: if `/spiffs/snapshots/system.launcher.snap` exists, restore instead of cold-load. Saves ~50 ms.
- **OTA validation boot**: per §13.4. New firmware enters validation timer; commits or rolls back.
- **Recovery boot**: if 3 consecutive boots fail to reach T=600ms, switch to recovery mode (read-only mount, USB MSC for `/sdcard` repair).

### 23.2 Fault handling during boot

| Step | Failure | Behavior |
|---|---|---|
| LCD init | I²C / RGB error | Log; continue without display |
| Touch init | GT911 probe fails | Log; continue without touch |
| Backlight | Expander write fails | Log; continue at default brightness |
| `/spiffs` mount | Format error | Format and remount; if fails, abort to recovery |
| `/sdcard` mount | Card absent | Continue; SD-resident apps unavailable |
| WiFi init | Driver fails | Log; network capabilities `:err :unavailable` |
| RTC init | Chip absent / invalid | Use ESP-IDF time (epoch 0); flag as unsynced |
| `.deck-os` parse | Surface error | Abort — build error; not user-fixable |
| Launcher load | App parse / type error | Show error screen with diagnostics |

After 3 abort cycles, enter recovery mode.

---

## 24. Performance Targets

### 24.1 Boot

| Metric | Target |
|---|---|
| Power-on to launcher visible | < 1.0 s with /sdcard, < 0.7 s without |
| Cold app launch (loader) | < 250 ms typical |
| Warm app launch (snapshot restore) | < 100 ms typical |
| Suspend (snapshot write) | < 100 ms typical, < 200 ms p99 |
| Resume from snapshot | < 80 ms typical |

### 24.2 Steady state

| Metric | Target |
|---|---|
| Touch-to-render latency | < 100 ms typical, < 200 ms p99 |
| LVGL render frame time | < 33 ms (30 FPS sustained) |
| Memory pressure check overhead | < 5 ms / sample |
| WiFi reconnect after blackout | < 5 s typical |
| NTP sync after WiFi up | < 3 s typical |
| RSA-3072 verify (HW MPI) | < 50 ms per signature |
| AES-256-CBC encrypt (HW AES) | > 20 MB/s sustained |

### 24.3 Power (projected with PM enabled)

| Mode | Current draw | Battery life (1500 mAh) |
|---|---|---|
| Active, display on, WiFi connected | ~180 mA | ~8 h |
| Active, display off, WiFi connected | ~45 mA | ~33 h |
| Light sleep, display off, WiFi PSP | ~12 mA | ~125 h (5 days) |
| Deep sleep, RTC alarm only | ~50 µA | months |

---

## 25. Build & Release Checklist

This is a **deliverables manifest**, not implementation status.

### 25.1 Tooling

- [ ] `tools/provision.py` — first-flash device provisioning (UUID, factory cal, HMAC eFuse key burn, optional Ed25519 device key)
- [ ] `tools/sign_bundle.py` — Ed25519 signing for app bundles
- [ ] `tools/pack_app.py` — produce a `.tar.zst` app bundle from a directory
- [ ] `tools/decode_panic.py` — `addr2line` wrapper for ELF decoding from log captures

### 25.2 Components (each its own GitHub repo, see `14-deck-components`)

- [ ] `deck-runtime` — portable runtime per `11-deck-implementation`
- [ ] `deck-bridge-lvgl` — LVGL implementation of `deck.driver.bridge.ui`
- [ ] `deck-driver-esp32-storage-fat` — SD card storage drivers
- [ ] `deck-driver-esp32-storage-nvs` — NVS driver
- [ ] `deck-driver-esp32-network` — wifi + http + downloader + sockets
- [ ] `deck-driver-esp32-display-rgb` — RGB LCD panel driver
- [ ] `deck-driver-esp32-touch-gt911` — GT911 touch driver
- [ ] `deck-driver-esp32-ota` — firmware + app OTA
- [ ] `deck-driver-esp32-system` — info, locale, time, battery, security, shell, apps, tasks, crashes
- [ ] `deck-driver-esp32-crypto-psa` — AES via PSA Crypto
- [ ] `deck-driver-esp32-ble-nimble` — BLE central
- [ ] `deck-os-surface-cyberdeck` — `.deck-os` blob for this board
- [ ] `deck-markdown` — md4c-backed implementation of the core markdown surface (`@builtin md`, `@capability markdown`, `DVC_MARKDOWN`/`DVC_MARKDOWN_EDITOR` rendering). First-class part of the standard library; the platform always includes it.
- [ ] `deck-conformance-suite` — runs against any SDI implementation

### 25.3 Platform integration in this repo

- [ ] Migrate I²C usage to `driver/i2c_master.h` (v6.0 mandatory)
- [ ] Migrate any direct mbedTLS calls to PSA Crypto (v6.0 mandatory)
- [ ] Update LCD config: `dma_burst_size`, `rgb_ele_order`, `gpio_num_t`
- [ ] Replace `esp_vfs_fat_sdmmc_unmount` with `esp_vfs_fat_sdcard_unmount`
- [ ] Wire boot sequence per §23
- [ ] Wire memory monitor per §9.5
- [ ] Wire OTA dual track per §13
- [ ] Set up the SDI driver registry (`12-deck-service-drivers §10.1`)

### 25.4 Bundled OS apps (Deck — replacing C apps)

- [ ] `system.launcher` (slot 0) — replaces current `apps/launcher/` C code
- [ ] `system.taskman` (slot 1) — port existing C task manager to Deck
- [ ] `system.settings` (slot 2) — replaces current `apps/settings/` C code; includes Diagnostics tab with crash list and OTA controls

### 25.5 Optional SD-installable apps (Deck)

- [ ] Files (`org.cyberdeck.files`)
- [ ] Calculator, Notes, Music, Podcasts, Books, Tasks per `APPS.md`
- [ ] Bluesky reference per `annex-xx-bluesky`

### 25.6 Conformance

- [ ] Runtime conformance suite passes against the mock bridge (`11-deck-implementation §24`)
- [ ] SDI conformance suite passes for each `cap_*` driver (`12-deck-service-drivers §12`)
- [ ] On-device smoke tests: every capability invoked once at startup

---

## 26. Versioning

This board's contribution to the project's overall versioning model. The full policy lives in `15-deck-versioning`.

### 26.1 Surface API level (`deck_os_version`)

Currently `1`. Bumped by OS Maintainers on each capability addition or signature change. This board's `.deck-os` (`deck-os-surface-cyberdeck`) inherits the canonical surface and adds CyberDeck-specific extensions in the `ext.cyberdeck.*` namespace.

### 26.2 Runtime version

The portable runtime (`deck-runtime` IDF Component) is on its own semver track. The CyberDeck firmware OTA bundles a specific runtime release; users see the runtime version in Settings → Diagnostics → Versions.

### 26.3 SDI version

`12-deck-service-drivers §11`. Initial: 1.0 (`0x0100`). The CyberDeck driver implementations declare their `sdi_version` at registration; mismatched drivers are rejected at boot.

### 26.4 Editions supported

This board's runtime supports edition **2026** (the current edition). Future runtime releases will support `[2026, 2027]` once edition 2027 stabilizes.

### 26.5 Hardware revision

Single board revision in v1. Future board revisions bump `device_model` in `system.info`. The board revision is reported in `system.info.versions().drivers` indirectly via the platform-driver impl_version values.

### 26.6 Anti-rollback (firmware secure_version)

Per §12.5: 16-bit `secure_version` in `esp_app_desc_t`. Bumped on each firmware OTA release. Independent of all other versioning concerns; protects against downgrade attacks.

---

## 27. Out of Scope for v1

- Code signing enforcement
- Encrypted SD card
- Secure Boot enabled by default
- Multi-user accounts
- App store / remote app browse
- BLE central role for accessory keyboards/mice
- Hot reload of Deck source on-device
- Coredump partition
- Multi-display
- ESP-NOW
- LoRa / sub-GHz radios
- ULP-RISC-V usage
- Background fetch via deep-sleep wake
