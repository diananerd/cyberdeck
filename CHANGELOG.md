# Changelog

Todas las versiones notables del firmware CyberDeck. Formato inspirado en Keep-a-Changelog.

## [0.4.0] — 2026-04-17 — DL1 conformance certified

Cierra el arco DL1 Core (F1–F10 de `DEVELOPMENT-PLAN-DL1.md`). El firmware es un runtime Deck DL1 completo que ejecuta apps `.deck` desde la partición SPIFFS `apps`, con toda la arquitectura legacy C removida.

### Added
- **Conformance suite DL1** (`components/deck_conformance/`) — 28 named checks verdes en hardware: 5 suites C-side, 20 `.deck` tests (17 positivos + 3 error-paths), 3 stress/memory bounds.
- `.deck` test runner con captura de `ESP_LOG` vía `esp_log_set_vprintf` hook.
- Sentinel protocol `DECK_CONF_OK:<name>` para aserciones Deck-side.
- Reporte JSON-line por UART + persistido en `/deck/reports/dl1-<monotonic_ms>.json`.
- Tests negativos: `expected_err` en `deck_test_t` — valida que el runtime devuelve el código de error correcto.
- Snapshot canónico `tests/conformance/reports/dl1-sample.json`.
- `tests/conformance/README.md` con inventario + gaps DL1→DL2.

### Changed
- Parser acepta `not` como unary NOT (antes solo `!`). Bug descubierto por el test `lang.logic`.
- `CONFIG_SPIFFS_OBJ_NAME_LEN=64` (antes 32 default) — paths `/conformance/*.deck` no cabían.
- `main.c` delega los selftests fragmentados en una sola llamada a `deck_conformance_run()`.

### Footprint
- `cyberdeck.bin` = 0x4bd90 bytes (≈ 303 KB total)
- Runtime code (libdeck_runtime.a): **≈ 37 KB** flash (cap DL1: 120 KB) ✅
- Heap idle: `deck_alloc_peak=672 bytes` durante toda la suite (cap DL1: 64 KB) ✅
- `deck_alloc_live=26` stable tras 10 reruns de sanity.deck (delta=0)
- Heap internal free idle: **≈ 345 KB** (threshold stress ≥ 200 KB)

### Known gaps (deferred → DL2 o próximos minor DL1)
- Funciones de usuario (`fn`) — parser/lexer sin keyword
- List / tuple / map literales — parser solo tiene grouping `(…)`
- String interpolation (`${…}`) — lexer sin soporte
- Keywords `and`/`or` como operadores binarios — mapeadas solo a `&&`/`||`
- `fs.list` builtin Deck; `os.resume/suspend/terminate` builtins

## [0.3.9] — 2026-04-17 — RC DL1 conformance

Batería DL1 completa con 100% PASS en hardware. Snapshot committed. Criterios v0.4.0 en validación final.

## [0.3.3] — F8

Shell DL1 single-app + bootstrap de `hello.deck` desde SPIFFS al boot.

## [0.3.2] — F7

`@machine` lifecycle ejecutable (states + on enter/leave + transitions).

## [0.3.1] — F6

Dispatcher completo: builtins math, text, bytes, log, time, system.info, nvs, fs routeados desde código Deck.

## [0.3.0] — F5

Primer `.deck` ejecutado en hardware: `hello.deck` imprime por UART. Momento fundacional del runtime.

## [0.2.3] — F4

Loader DL1 de 10 stages + 18 casos self-test (errores estructurados).

## [0.2.2] — F3

Parser LL(1) DL1 — 43 casos verdes.

## [0.2.1] — F2

Lexer, tipos base + refcount allocator + string interning — 35 casos verdes.

## [0.2.0] — F1

SDI 1.0 con 5 drivers DL1 (storage.nvs, storage.fs, system.info, system.time, system.shell).

## [0.2.0-dev] — F0

Clean-slate wipe del framework legacy (app_framework, apps, sys_services, ui_engine). Bootstrap mínimo + esqueleto de componentes deck_sdi/runtime/shell.

## [0.1.0] — Baseline

C app framework legacy (Launcher + Settings + ui_engine + sys_services + board). Reemplazado por el runtime Deck.
