# DL1 Conformance Suite

Batería de conformance DL1 que corre al boot (`deck_conformance_run`) y valida DL1 más allá del happy path: error paths exhaustivos, edge cases, concurrencia, corrupt-input rejection, heap pressure y regression guards.

## Qué verifica

| Sección | Elementos | Fuente |
|---|---|---|
| **C-side suites** (5) | allocator+intern, lexer (35), parser (43), loader (18), interp+machine (32) | self-tests internos del runtime |
| **.deck tests positivos** (28) | sanity, lang.{literals,arith,compare,logic,strings,let,if,match,and_or_kw}, os.{math,text,time,info,nvs,fs,fs.list,conv,lifecycle}, app.machine, edge.{empty_strings,long_string,escapes,comments,nested_let,nested_match,string_intern,double_neg} | `apps/conformance/*.deck` |
| **.deck tests negativos** (14) | errors.{level_below_required,pattern_not_exhaustive,type_mismatch,parse_error,unresolved_symbol,capability_missing,level_unknown,incompatible_edition,incompatible_surface,type_error_missing_id,divide_by_zero_int,modulo_by_zero_int,divide_by_zero_float,str_minus_int} | `apps/conformance/err_*.deck` |
| **Stress / memory / perf** (8) | heap_idle_budget, no_residual_leak, rerun_sanity_x100, boot_time_budget, flash_size_reasonable, log_hook_concurrent, corrupt_inputs_rejected, heap_pressure_recovers | checks post-suite (C-side) |

Total: **55 named checks** (200+ sub-cases con los internos de lexer/parser/loader/interp).

## Cómo funciona

- El harness `components/deck_conformance/` aggrega selftests y corre cada `.deck` bundled en SPIFFS.
- Cada `.deck` test emite un sentinel canónico `DECK_CONF_OK:<name>` desde `@on launch` solo si sus aserciones pasan.
- Un hook sobre `esp_log_set_vprintf` tee todo `ESP_LOGI` a un buffer durante la ejecución del test; el runner busca el sentinel con `strstr`. **El hook es thread-safe** (protegido con SemaphoreMutex) para tolerar concurrencia de tasks de sistema.
- Tests negativos pasan si `deck_runtime_run_on_launch` devuelve exactamente `expected_err`.
- Cada test registra timing (us) + heap delta + alloc delta; agregados visibles en el JSON (`deck_total_us`, `deck_max_us`, `deck_slowest`).
- El reporte final se emite como línea JSON por UART + se persiste en `/deck/reports/dl1-<monotonic_ms>.json`.

## Stress coverage

| Test | Qué asserta |
|---|---|
| `memory.heap_idle_budget` | heap_free_internal ≥ 200 KB tras correr toda la batería |
| `memory.no_residual_leak` | `deck_alloc_live ≤ 150` tras toda la batería (growing linearly con tests — lo que cuenta es el delta de rerun_x100) |
| `stress.rerun_sanity_x100` | 100 load+eval de sanity.deck → live delta ±20, heap delta ≤ 512 bytes. **En baseline: live+0 heap+0** (allocator/interner estancos). |
| `perf.boot_time_budget` | Boot→conformance < 2 s (baseline ~302 ms) |
| `perf.flash_size_reasonable` | Heap total libre > 2 MB (PSRAM mapeado) |
| `stress.log_hook_concurrent` | Task de ruido en core 1 logea 500 Hz mientras el main ejecuta sanity; sentinel aún capturado, sin panic |
| `stress.corrupt_inputs_rejected` | 5 patrones adversariales (bin garbage, truncated, null-mid, invalid UTF-8, empty) → todos rechazados con LOAD_* sin crash |
| `stress.heap_pressure_recovers` | Squeeze de `deck_alloc_set_limit(used+64)` → test falla con NO_MEMORY, runtime NO panic, sanity post-restore PASS |

## Regression guards build-time

- `tools/assert_bin_size.cmake` rechaza el build si `cyberdeck.bin > 500 000 bytes` (baseline ~315 KB)
- Integrado como custom target `cyberdeck_bin_size_guard` que depende de `gen_project_binary`

## Snapshot

`reports/dl1-sample.json` — estado canónico todos-verdes v0.5.0:

```json
{
  "deck_level": 1, "deck_os": 1, "runtime": "0.2.0", "edition": 2026,
  "suites_total": 5,   "suites_pass": 5,   "suites_fail": 0,
  "deck_tests_total": 42, "deck_tests_pass": 42, "deck_tests_fail": 0,
  "deck_total_us": 179155, "deck_max_us": 24138, "deck_slowest": "app.machine",
  "stress_total": 8, "stress_pass": 8, "stress_fail": 0,
  "heap_used_during_suite": 23440,
  "intern_count": 409, "intern_bytes": 18586,
  "deck_alloc_peak": 1608, "deck_alloc_live": 66,
  "monotonic_ms": 1721
}
```

## Fixes de runtime descubiertos por la batería

- **Parser: `not` como unary** (F11.1) — lexer emitía TOK_KW_NOT pero parser solo mapeó TOK_BANG
- **Parser: `and`/`or` como binarios** (F11.1) — misma gap; test `lang.and_or_kw` expuso
- **Parser: NEWLINEs extras antes de INDENT** (F12.3) — comments en bloques rompían `@on launch:` porque el parser solo consumía 1 NEWLINE
- **fs.list builtin expuesto a Deck** (F11.2)
- **os.{resume,suspend,terminate} builtins** (F11.3)
- **Log capture hook thread-safe** (F14.1) — SemaphoreMutex requerido para tolerar concurrencia
- **Stack main task 3584 → 6144 bytes** (F14.1) — insuficiente con harness+runtime+fopen+concurrent-noise
- **`deck_alloc_set_limit` público** (F14.4) — permite pressure-test sin reinit del allocator

## Gaps DL1 → DL2

- **Funciones de usuario** (`fn`). No hay keyword ni parser. Por eso los stress de recursión no se escriben en Deck; se cubren con re-runs desde C.
- **List / tuple / map literales** (`[1,2,3]`, `(a,b)`, `{k: v}`). Parser solo tiene `TOK_LPAREN`/`TOK_RPAREN` para agrupar expresiones.
- **String interpolation** (`"${x}"` o `"{x}"`). Lexer sin soporte. El operador `<>` de concat sí funciona.
- **Guards `when`** en match. Lexer tiene el keyword, evaluator lo consume, pero no se ejercita activamente en tests (no encontramos el path de uso aún).
- **`time.duration`/`time.to_iso`** — builtins existen, no tienen coverage `.deck` dedicada.

Estos gaps quedan abiertos para DL2.
