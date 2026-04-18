# Changelog

Todas las versiones notables del firmware CyberDeck. Formato inspirado en Keep-a-Changelog.

## [0.6.0] — 2026-04-17 — DL1 rock solid

Profundiza aún más DL1: latency percentiles, fuzz testing, edge cases avanzados, SDI drivers bajo stress. **66 named checks** en hardware.

### Added

**Latency percentiles (F16):**
- `deck_test_t.samples[5]` + `n_samples` — cada positive test corre 5 veces, sort + compute min/p50/p99/max.
- Log per-test muestra distribución + OUTLIER flag si max > 2*p50.
- JSON gana `deck_outliers`.

**Fuzz testing (F17):**
- `stress.fuzz_random_inputs` — 200 iter con xorshift32 seed 0xDECAFBAD. 100 iter bytes aleatorios puros + 100 iter bit-flips de sanity.deck.
- Silencia runtime logs durante el loop (restore al final).
- Asserta 0 crashes, 0 "other" return codes, heap drift bounded.

**Edge cases lang adicionales (F18):**
- `edge.match_when` — match guards `when n < x`
- `edge.match_deep` — match con 20 arms
- `edge.int_limits` — int64 max/min
- `edge.float_special` — floats negativos/pequeños, suma imprecisa
- `edge.unicode` — UTF-8 bytes en string literal (día/ñ/á)
- `edge.long_ident` — identifier de 70+ chars
- `edge.deep_let` — 51 let encadenados

**SDI drivers stress (F19):**
- `stress.sdi_nvs_churn` — 20 × set/get/del, avg 1.5ms/iter, 0 leak
- `stress.sdi_fs_read_hammer` — 100 reads, avg 196us, 0 leak
- `stress.sdi_time_monotonic` — 1000 reads, 0 regressions, avg 1.1us/call

### Stats en hardware (v0.6.0)

- **66 named checks verdes**: 5 C-side suites + 49 .deck tests (35 positivos + 14 negativos) + 12 stress/memory/perf/fuzz/SDI.
- Suite runtime: ~230 ms (5 samples por positive test).
- Fuzz: 200 iter, 0 crashes, 0 unexpected return codes.
- rerun_sanity_x100: live delta 0 bytes (allocator rock solid).
- NVS churn: 20 iter clean, FS hammer: 100 iter clean, Time monotonic: 1000 iter 0 regressions.
- `deck_alloc_peak`: 15.8 KB (vs budget 64 KB).

## [0.5.0] — 2026-04-17 — DL1 hardened

Endurece DL1 más allá del happy path: cobertura exhaustiva de errores, edge cases, concurrencia, corrupt-input rejection, heap pressure, y regression guards build+runtime.

### Added

**Builtins + fixes de parser que completan DL1:**
- Parser acepta `not` (keyword) además de `!` como unary NOT.
- Parser acepta `and`/`or` (keywords) además de `&&`/`||` como binarios.
- Parser tolera NEWLINEs extras antes de INDENT (permite comments en bloques).
- Builtin `fs.list(path)` — devuelve string con entries separadas por `\n`.
- Builtins `os.resume`/`os.suspend`/`os.terminate` — no-ops DL1 single-app que retornan unit.
- `deck_alloc_set_limit` — API pública para pressure-test sin reinit.

**14 tests .deck negativos (F12.1 + F12.2):**
- `errors.{parse_error, unresolved_symbol, capability_missing, level_unknown, incompatible_edition, incompatible_surface, type_error_missing_id}`
- `errors.{divide_by_zero_int, modulo_by_zero_int, divide_by_zero_float, str_minus_int}`

**8 tests .deck de edge cases (F12.3):**
- `edge.{empty_strings, long_string, escapes, comments, nested_let, nested_match, string_intern, double_neg}`

**Instrumentación por test (F13.1):**
- `deck_test_t` gana `duration_us`, `heap_delta`, `alloc_delta`.
- Log per-test muestra `us, heap±N, alloc±N`.
- JSON gana `deck_total_us`, `deck_max_us`, `deck_slowest`.

**Regression guards (F13.2):**
- `perf.boot_time_budget` — asserta boot→conformance ≤ 2 s (baseline ~302 ms).
- `perf.flash_size_reasonable` — heap total libre > 2 MB.
- `tools/assert_bin_size.cmake` — custom build target falla si `cyberdeck.bin > 500 KB`.

**Stress tests duros (F14.1–F14.4):**
- `stress.log_hook_concurrent` — task en core 1 logea 500 Hz mientras el main ejecuta sanity; sentinel aún capturado, sin panic.
- `stress.corrupt_inputs_rejected` — 5 patrones adversariales (bin garbage, truncated, null-mid, invalid UTF-8, empty) rechazados estructuralmente.
- `stress.rerun_sanity_x100` — 100 load+eval de sanity.deck; **live+0 heap+0** (allocator/interner estancos).
- `stress.heap_pressure_recovers` — squeeze de `deck_alloc_set_limit` fuerza NO_MEMORY; runtime se recupera, sanity post-restore PASS.

### Changed
- Log-capture hook (`conf_vprintf_hook`) es thread-safe vía SemaphoreMutex — requerido para tolerar concurrencia real.
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 3584 → 6144 bytes (stack overflow detectado por canary en F14.1).
- `FS_LIST_BUF` 1024 → 4096 bytes (SPIFFS list saturaba con 38+ archivos).
- `CONFIG_SPIFFS_OBJ_NAME_LEN` 32 → 64 (paths `/conformance/*.deck` no cabían).

### Stats en hardware (v0.5.0)

- **55 named checks verdes** (5 suites + 42 .deck + 8 stress); 200+ sub-cases con internos.
- Runtime flash: ≈ 37 KB (budget 120 KB).
- cyberdeck.bin: 315 KB (budget 500 KB).
- Boot→conformance: ~302 ms.
- Suite runtime: 179 ms (avg 4.3 ms/test, max 24 ms).
- Live allocs residual: 66 (stable across runs).
- rerun_sanity_x100: delta 0 bytes.

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
