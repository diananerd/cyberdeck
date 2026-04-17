# DL1 Conformance Suite

Batería de conformance DL1 que corre al boot (`deck_conformance_run`).

## Qué verifica

| Categoría | Elementos | Fuente |
|---|---|---|
| **C-side suites** (5) | allocator+intern, lexer (35), parser (43), loader (18), interp+machine (32) | self-tests internos del runtime |
| **.deck tests positivos** (17) | lang.{literals,arith,compare,logic,strings,let,if,match}, os.{math,text,time,info,nvs,fs,conv}, app.machine, sanity | `apps/conformance/*.deck` |
| **.deck tests negativos** (3) | errors.{level_below_required,pattern_not_exhaustive,type_mismatch} | `apps/conformance/err_*.deck` |
| **Stress/memory C-side** (3) | heap_idle_budget, no_residual_leak, rerun_sanity_x10 | checks post-suite |

Total: **28 named checks** (148+ sub-cases sumando los casos internos).

## Cómo funciona

- El harness `components/deck_conformance/` aggrega selftests y corre cada `.deck` bundled en SPIFFS.
- Cada `.deck` test emite un sentinel canónico `DECK_CONF_OK:<name>` desde `@on launch` solo si sus aserciones pasan.
- Un hook sobre `esp_log_set_vprintf` tee todo `ESP_LOGI` a un buffer durante la ejecución del test; el runner busca el sentinel con `strstr`.
- Tests negativos pasan si `deck_runtime_run_on_launch` devuelve exactamente `expected_err`.
- El reporte final se emite como línea JSON por UART + se persiste en `/deck/reports/dl1-<monotonic_ms>.json`.

## Snapshot

`reports/dl1-sample.json` es el estado objetivo (todos verdes). Formato:

```json
{
  "deck_level":1, "deck_os":1, "runtime":"0.2.0", "edition":2026,
  "suites_total":5,  "suites_pass":5,  "suites_fail":0,
  "deck_tests_total":20, "deck_tests_pass":20, "deck_tests_fail":0,
  "stress_total":3, "stress_pass":3, "stress_fail":0,
  "heap_used_during_suite":11372,
  "intern_count":267, "intern_bytes":8822,
  "deck_alloc_peak":672, "deck_alloc_live":26,
  "monotonic_ms":800
}
```

## Gaps conocidos DL1 → DL2

Limitaciones del runtime que reducen la cobertura `.deck`-side:

- **Funciones de usuario** (`fn`). No hay keyword ni parser. Por eso los stress de recursión 400 frames / 10 000 iteraciones no se escriben en Deck; se cubren con re-runs desde C.
- **List / tuple / map literales** (`[1,2,3]`, `(a,b)`, `{k: v}`). Parser solo tiene `TOK_LPAREN`/`TOK_RPAREN` para agrupar expresiones; no hay literales compuestos.
- **String interpolation** (`"${x}"` o `"{x}"`). Lexer sin soporte. El operador `<>` de concat sí funciona.
- **Keywords `and`/`or`** como binarios. El lexer los reconoce pero el parser solo mapea `&&`/`||`. (Fix trivial pendiente, no bloqueante.)
- **`fs.list`** como builtin Deck — hay API SDI, falta binding.
- **`os.resume`/`os.suspend`/`os.terminate`** — no hay builtins aún.

Estos gaps son DL1 spec-compliant en lo mínimo verificable (spec 16 §4), y quedan abiertos para DL2 o como fixes menores pre-v0.4.0.
