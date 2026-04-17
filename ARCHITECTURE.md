# ARCHITECTURE.md — CyberDeck firmware (DL1 Core target)

**Estado al commit:** `v0.2.0-dev` post-wipe. Stack legacy C removido; scaffolding de los componentes DL1 en su sitio. Este documento refleja la arquitectura **objetivo** de DL1 Core — los componentes `deck_*` aún son placeholders hasta F1 del [DEVELOPMENT-PLAN-DL1.md](DEVELOPMENT-PLAN-DL1.md).

---

## Stack de capas

```
┌─────────────────────────────────────────────────────────────┐
│  apps/  (bundles .deck en SPIFFS)                           │
│    · hello/manifest.deck + main.deck   (F2+)                │
│    · launcher single-app stub          (F8)                 │
│    · conformance/*.deck                (F9)                 │
├─────────────────────────────────────────────────────────────┤
│  components/deck_shell/       Shell single-app DL1          │
│    · boot sequence                                          │
│    · discover + load one .deck de SPIFFS                    │
│    · HOME / BACK event routing                              │
│    · app lifecycle (launch/suspend/resume/terminate)        │
├─────────────────────────────────────────────────────────────┤
│  components/deck_runtime/     Interpreter Deck              │
│    · tipos + refcount allocator + string interning          │
│    · lexer  (WS-sensitive, literales, operators, strings)   │
│    · parser (LL(1), AST para subset DL1)                    │
│    · loader (10 stages: lex/parse/type/cap-bind/link)       │
│    · evaluator (tree-walking, value + frame stacks)         │
│    · effect dispatcher (sync, sin continuations en DL1)     │
│    · @machine runtime (states, transitions, hooks)          │
├─────────────────────────────────────────────────────────────┤
│  components/deck_sdi/         Service Driver Interface      │
│    · registry + vtables                                     │
│    · DL1 drivers:                                           │
│       storage.nvs       (read + write)                      │
│       storage.fs        (read-only aceptable)               │
│       system.info       (device_id, heap, levels)           │
│       system.time       (monotonic + wall opcional)         │
│       system.shell      (minimal: push/pop de una app)      │
├─────────────────────────────────────────────────────────────┤
│  components/board/            Hardware abstraction          │
│    · I2C bus + CH422G expander                              │
│    · LCD / touch / backlight    (dormant en DL1)            │
│    · RTC (PCF85063A)                                        │
│    · Battery ADC                                            │
│    · SD card (dormant en DL1; DL2+ usa SD, DL1 usa SPIFFS)  │
├─────────────────────────────────────────────────────────────┤
│  ESP-IDF v6.0 + FreeRTOS + xtensa-esp32s3 toolchain         │
└─────────────────────────────────────────────────────────────┘
```

Regla de dependencias: una capa solo puede referenciar la inmediatamente inferior (o ESP-IDF directamente para primitivos). `deck_runtime` no conoce `board`; `deck_shell` no conoce los drivers SDI por detrás del contrato.

---

## Componentes activos

| Componente | Estado | Contenido |
|---|---|---|
| `components/board/` | Production (pre-DL1) | HAL intacto del hardware CyberDeck; decoupled de LVGL vía `hal_lcd_set_vsync_cb()`. `hal_gesture` removido (se rehace en DL2 con la bridge UI). |
| `components/deck_sdi/` | Placeholder | Lista para F1. Cada driver DL1 llega como `include/drivers/*.h` + `src/drivers/*.c`. |
| `components/deck_runtime/` | Placeholder | Lista para F2–F7. Divide por subsistema: tipos, lexer, parser, loader, eval, dispatcher, machine. |
| `components/deck_shell/` | Placeholder | Lista para F8. Thin layer sobre runtime + SDI. |
| `main/` | Bootstrap | `main.c` hace init NVS + USB/JTAG console, log device id + heap, idle heartbeat. Se completa en F1 (registro SDI + self-tests) y F8 (arranque del shell). |

## Componentes explícitamente ausentes

Removidos en el wipe del clean-slate refactor. **No retornan** — sus responsabilidades se cubren por el runtime Deck + SDI:

- `components/app_framework/` — el lifecycle + registro de apps pasa al `deck_shell` + `@machine` runtime.
- `components/apps/` — las apps viven como `.deck` files, no como componentes C.
- `components/sys_services/` — los servicios (settings, event bus, wifi, battery, downloader, ota, time) se reintroducen como drivers SDI + capabilities Deck, nivel por nivel (DL1 solo requiere nvs + time; DL2 añade wifi/http/battery; DL3 añade downloader/ota/mqtt/ble).
- `components/ui_engine/` — la UI se reintroduce en DL2+ como bridge DVC (un nuevo componente `deck_bridge_dvc/` que renderiza árboles DVC emitidos por el runtime a widgets LVGL).
- `components/os_core/` — sus piezas (event, process, service registry, storage, db, settings, defer, poller, task) se reintroducen donde corresponda: event en el runtime, process/task en shell, db como driver SDI (DL3), etc.

---

## Flujo de control (DL1 target — F8 en adelante)

```
Boot
  └─ ESP-IDF brings up PSRAM, heap, FreeRTOS
  └─ app_main() (main/main.c)
        ├─ nvs_flash_init()
        ├─ usb_serial_jtag_driver_install()
        ├─ deck_sdi_registry_init()           (F1)
        │    └─ register 5 DL1 drivers
        ├─ deck_runtime_init()                (F2+)
        │    └─ alloc limits, intern table
        └─ deck_shell_init()                  (F8)
             └─ discover /spiffs/apps/*/manifest.deck
             └─ deck_shell_launch("sys.hello")
                  ├─ loader stages 0-9
                  ├─ evaluator runs @on launch
                  └─ @machine enters initial state
                        └─ effect dispatcher routes capabilities to SDI
                              └─ SDI driver calls ESP-IDF primitives
```

Eventos HOME/BACK (en DL1: vía UART char, GPIO boot button si está wired, o API test harness) llegan al shell, se convierten en atoms Deck (`:home`, `:back`) y se encolan al runtime como eventos del bus.

---

## Decisiones de diseño registradas en DL1

1. **Clean-slate.** No hay shims de compat con el framework anterior. El código legacy solo vive como referencia técnica en `GROUND-STATE.md` y en la historia de git.
2. **SDI es el único punto de contacto** entre runtime y hardware. Ningún código del runtime incluye headers de `board/` o ESP-IDF directamente — siempre pasa por un driver SDI.
3. **DL1 no requiere display.** La placa CyberDeck tiene pantalla, pero en DL1 el LCD queda dormant (no inicializado). Decisión: simplifica el runtime, prueba el boundary de conformancia. UI llega en DL2 vía bridge DVC.
4. **Apps en SPIFFS, no SD.** DL1 spec permite fs read-only. SPIFFS embebido es suficiente para hello + conformance tests. SD está reservada para DL2+ con fs read/write.
5. **Event bus en el runtime, no en el shell.** El bus de eventos (HOME/BACK/os.app_launched/os.app_terminated) vive dentro del runtime como parte del scheduler de efectos, no como servicio separado. Simplifica la superficie de shell.
6. **Single-app.** DL1 corre una sola app a la vez. No hay activity stack, no hay task switching, no hay suspend/resume persistente. Lo reintroduce DL2 (2-3 apps concurrentes) y DL3 (4+ con VM snapshot).

---

## Presupuestos DL1

| Recurso | Budget | Medido en |
|---|---|---|
| Flash — runtime (deck_sdi + deck_runtime + deck_shell) | ≤ 120 KB | `idf.py size-components` |
| Heap — idle después de bootstrap | ≤ 64 KB | `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` delta |
| App cargada (código + estado) | ≤ 48 KB | Tracked por `deck_alloc_used()` |
| Platform image total | ≤ 256 KB flash, ≤ 128 KB SRAM | Build output + heap_init |

La plataforma CyberDeck es DL3-capable (8 MB flash + 8 MB PSRAM), pero el runtime DL1 debe caber en el budget DL1 para que la misma implementación pueda claim-ear DL1 en placas más chicas (ESP32-C3 + 4 MB) sin recompilar con flags distintos.

---

## Referencias

- Plan detallado DL1: [DEVELOPMENT-PLAN-DL1.md](DEVELOPMENT-PLAN-DL1.md)
- Plan general v0.1 → v1.0: [DEVELOPMENT-PLAN.md](DEVELOPMENT-PLAN.md)
- Spec de niveles: [deck-lang/16-deck-levels.md](deck-lang/16-deck-levels.md)
- Audit del estado anterior (histórico, útil como referencia de mecanismos): [GROUND-STATE.md](GROUND-STATE.md)
- CLAUDE.md raíz: convenciones del repo, flash workflow, hardware notes.
