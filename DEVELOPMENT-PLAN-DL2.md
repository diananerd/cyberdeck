# DEVELOPMENT-PLAN-DL2.md — Ruta a conformance DL2 Standard

**Alcance:** llevar el firmware desde `v0.6.0` (DL1 rock solid) hasta `v0.10.0` — runtime Deck **DL2 Standard** al 100% de conformancia, ejecutando apps reales con UI (bridge DVC sobre LVGL), WiFi, HTTP, activity stack, lockscreen, y un Task Manager funcional.

**Punto de partida:** DL1 runtime interpretado, 66 checks verdes, allocator rock-solid (delta 0 en 100 reruns), fuzz 200-iter 0-crashes. Base estable para apilar DL2 encima.

**Estrategia:** apilar. NO borramos DL1 — DL2 es un strict superset (spec 16 §3.1). Todo lo que funcionaba en DL1 sigue funcionando; DL2 añade capabilities, lenguaje y OS surface. Fases atómicas, cada una con su ciclo test/build/flash/debug/fix/next y su commit hardware-verificado.

**"Done" sigue siendo hardware-verificado.** Cada commit debe flashear y correr en la placa sin regresión.

---

## Índice

1. [Deliverables de v0.10.0 DL2](#1-deliverables-de-v0100-dl2)
2. [Scope DL2 (recap de spec 16 §5)](#2-scope-dl2-recap-de-spec-16-5)
3. [Arquitectura objetivo](#3-arquitectura-objetivo)
4. [Plan de fases atómicas](#4-plan-de-fases-atómicas)
5. [Criterios de release v0.10.0](#5-criterios-de-release-v0100)
6. [Riesgos y mitigaciones DL2](#6-riesgos-y-mitigaciones-dl2)
7. [Orden de batalla sugerido](#7-orden-de-batalla-sugerido)

---

## 1. Deliverables de v0.10.0 DL2

1. **Runtime Deck DL2** con funciones `fn`, lambdas, recursión, list/tuple/map literals, string interpolation, `Result T E`, `do` blocks, `is`, pipe operators, effect annotations, stdlib amplio.
2. **SDI DL2 completo:** `storage.fs` con write/create/delete/mkdir, `network.wifi`, `network.http`, `system.battery`, `system.time` con NTP, `system.security`, `bridge.ui`.
3. **Bridge UI DVC funcional:** serialización/deserialización de snapshots, decoder LVGL, componentes básicos (TEXT, PASSWORD, CHOICE, TOGGLE, CONFIRM, LOADING), statusbar + navbar + activity stack.
4. **Shell DL2:** lockscreen con PIN, intent navigation, rotation, settings sub-screens.
5. **App model DL2:** `@use` de OS capabilities, `@use.optional`, `@requires` completo, `@permissions`, `@config`, `@errors`, `@flow`, `@on` lifecycle (launch/resume/pause/suspend/terminate/low_memory/network_change), `@migration`, `@type` records.
6. **Apps DL2 bundle:** Launcher full grid, Task Manager funcional, 1+ user app real con WiFi + HTTP.
7. **Conformance suite DL2:** ≥ 150 checks verdes en hardware (sobre los 66 DL1). Incluye networking, UI snapshot roundtrip, multi-app concurrency, memory pressure handling, permission flow.
8. **Footprint dentro de spec 16 §5.9:** ≤ 280 KB flash runtime, ≤ 512 KB SRAM, ≤ 2 MB total platform image, 2–3 concurrent apps.
9. **Todo hardware-verificado.** DL2 conformance JSON snapshot committed en `tests/conformance/reports/dl2-sample.json`.

**Explícitamente fuera de scope DL2 (diferido a DL3):**
- `Stream T` type + `for`/`when` en content bodies → DL3
- OTA dual-track, `crypto.aes`, full Settings app, Bluetooth → DL3
- `@task`, `@handles`, `@stream` → DL3
- Files app + Bluesky kitchen-sink → DL3

---

## 2. Scope DL2 (recap de spec 16 §5)

**Lenguaje añadido (§5.1):**
- Union types `T1 | T2`
- `Result T E`
- Named record types `@type`
- Effect annotations `!alias`
- `where` bindings
- Functions puras + con efectos
- Recursion + tail calls
- Lambdas
- Pattern matching avanzado (records, variants con payload, guards)
- `do` blocks
- Pipe operators `|>`, `|>?`
- `is` operator
- Stdlib: list/map/tuple helpers (`len`, `head`, `map`, `filter`, `reduce`, …)
- Result/Optional helpers (`ok`, `err`, `unwrap`, `map_ok`, `and_then`, …)
- `type_of`, `is_int`, …
- `@private`

**App model añadido (§5.2):**
- `@use` de OS capabilities + `@use.optional`
- `@requires` completo (deck_os, runtime, capabilities)
- `@permissions`, `@config`, `@errors`
- `@machine.before/after` hooks
- `@flow`, `@flow.step`
- `@on` lifecycle: launch, resume, pause, suspend, terminate, low_memory, network_change
- `@migration`, `@test`, `@assets`

**OS surface añadido (§5.3):**
- `fs` con write/create/delete/mkdir
- `http` + `api_client` + `cache`
- `crypto.aes`
- `network.wifi` (scan/connect/status)
- `system.battery`, `system.time` con NTP
- `system.security` (PIN, lock/unlock, permissions)
- `system.tasks` (list_processes, kill_process)
- `display.notify`, `display.screen`, `system.locale`
- System events: battery, wifi, settings, rotation, network_change, permission_change, memory_pressure

**Runtime añadido (§5.4):**
- Scheduler con timers + condition tracking
- Stream buffers (ring, distinct, throttle, debounce)
- Continuation table para async effect dispatch
- Memory pressure monitor → `os.memory_pressure`
- IPC framing runtime ↔ bridge UI
- DVC wire format + render arena
- Single-evaluator + time-sliced multi-app

**SDI DL2 (§5.5):** `storage.fs` r/w, `display.panel`, `display.touch`, `network.wifi`, `network.http`, `system.battery`, `system.time` (NTP), `system.security`, `bridge.ui`.

**Bridge UI DL2 (§5.6):** DVC components TEXT / PASSWORD / CHOICE / DATE / TOGGLE / RANGE / CONFIRM / LOADING. UI services confirm/loading/keyboard/choice. Statusbar (time/WiFi/battery), Navbar (BACK/HOME), activity stack (max 4).

**Shell DL2 (§5.7):** Lockscreen PIN numpad básico, statusbar service, navbar service, intent navigation, settings sub-screen conventions, display rotation.

**Apps DL2 (§5.8):** Launcher full, Task Manager funcional, user apps con WiFi/HTTP/NVS/FS/cache/crypto.

---

## 3. Arquitectura objetivo

```
┌───────────────────────────────────────────────────────────────┐
│ apps/ (SPIFFS + SD)                                           │
│   · launcher.deck (full grid)                                 │
│   · taskman.deck (functional)                                 │
│   · net_hello.deck (WiFi + HTTP demo)                         │
│   · user apps (.deck bundles)                                 │
├───────────────────────────────────────────────────────────────┤
│ deck_shell/     — DL2 multi-app shell                         │
│   · activity stack (max 4)                                    │
│   · lockscreen PIN                                            │
│   · statusbar service                                         │
│   · navbar service                                            │
│   · intent navigation + rotation                              │
├───────────────────────────────────────────────────────────────┤
│ deck_runtime/   — Interpreter DL2                             │
│   · fn + lambda + recursion                                   │
│   · list/tuple/map literals + stdlib                          │
│   · Result / Optional + helpers                               │
│   · @use capability binding + effects                         │
│   · async dispatcher + continuation table                     │
│   · scheduler + timers                                        │
│   · memory pressure monitor                                   │
│   · DVC wire format emitter                                   │
├───────────────────────────────────────────────────────────────┤
│ deck_bridge_ui/ — NUEVO: LVGL decoder de DVC trees            │
│   · DVC component set (TEXT, PASSWORD, CHOICE, …)             │
│   · UI services (confirm, loading, keyboard, choice)          │
│   · Activity stack management                                 │
├───────────────────────────────────────────────────────────────┤
│ deck_sdi/       — Service Driver Interface DL2                │
│   · storage.nvs, storage.fs (r+w)                             │
│   · network.wifi, network.http                                │
│   · system.info, system.time (NTP), system.battery            │
│   · system.security (PIN, perms)                              │
│   · system.shell, bridge.ui                                   │
├───────────────────────────────────────────────────────────────┤
│ board/          — Hardware abstraction (unchanged)            │
│   · LCD, touch, CH422G, RTC, SD, backlight, WiFi PHY          │
└───────────────────────────────────────────────────────────────┘
```

Cambio principal vs DL1: **se re-activa LVGL** sobre bridge DVC (NO volvemos al `ui_engine` legacy). La UI es render-side de snapshots emitidos por Deck apps; las apps no tocan LVGL directamente.

---

## 4. Plan de fases atómicas

Cada fase = milestone con sub-pasos atómicos; cada sub-paso = 1 commit hardware-verificado.

### F21 — Language DL2 foundations

**Milestone → v0.7.0**

Habilita las construcciones fundamentales antes de stdlib. Cada sub-paso pasa por build + flash + nuevos tests `.deck` positivos y negativos.

- **F21.1** — `fn` keyword + parser + interp: funciones puras con parámetros y body. Recursion básica. 5+ tests.
- **F21.2** — Lambdas (`fn (a, b) -> body` o similar), closures minimal (captura immutable).
- **F21.3** — Tail-call optimization para recursión profunda (spec 16 §6.4).
- **F21.4** — List literal `[1, 2, 3]`: lexer TOK_LBRACK/RBRACK, parser AST_LIT_LIST, interp constructor, pattern matching sobre listas.
- **F21.5** — Tuple literal `(a, b)`: distinguir de paréntesis de agrupación por coma.
- **F21.6** — Map literal `{k: v}`: lexer TOK_LBRACE/RBRACE + parser + interp + pattern matching.
- **F21.7** — String interpolation `"hello ${name}"`: lexer produce segmentos; parser emite AST_CONCAT; interp evalúa.
- **F21.8** — `do` blocks para sequencing.
- **F21.9** — `is` operator (atom/state test).
- **F21.10** — Pipe operators `|>` y `|>?`.
- **F21.11** — `where` bindings.

**Criterios de salida F21:** runtime acepta programas con fn + lambdas + list/tuple/map + interpolation + pipe sin regresión. ≥ 20 tests `.deck` nuevos verdes. rerun_sanity_x100 sigue delta 0.

### F22 — Types + Result + stdlib

**Milestone → v0.7.1**

- **F22.1** — `Result T E` type (atom `:ok`/`:err` + payload) + pattern matching sobre variants.
- **F22.2** — `@type` records: declaración + field access + `with` update.
- **F22.3** — Union types `T1 | T2` (para Result y matches).
- **F22.4** — Stdlib list: `list.len`, `list.head`, `list.tail`, `list.map`, `list.filter`, `list.reduce`.
- **F22.5** — Stdlib map: `map.get`, `map.put`, `map.keys`, `map.values`.
- **F22.6** — Stdlib tuple: acceso por índice `.0`, `.1`.
- **F22.7** — Result/Optional helpers: `ok`, `err`, `unwrap`, `map_ok`, `and_then`.
- **F22.8** — Type inspection: `type_of`, `is_int`, `is_str`, etc.
- **F22.9** — `@private` para encapsulación módulo.

**Criterios:** ≥ 30 tests `.deck` stdlib verdes. Tail-call + stdlib walking fundamentales para fases siguientes.

### F23 — Effects + @use capabilities

**Milestone → v0.7.2**

- **F23.1** — Parser acepta `!alias` en firma de función.
- **F23.2** — Loader verifica: cada `!alias` debe estar declarada en `@use`.
- **F23.3** — `@use` bindings completos: local modules + OS capabilities.
- **F23.4** — `@use.optional`.
- **F23.5** — `@requires` completo (deck_os, runtime semver, capability list).
- **F23.6** — `@permissions` declarativo.
- **F23.7** — `@errors` declarativo.
- **F23.8** — Tests: app que declara `!http` sin `@use http` → `E_CAPABILITY_MISSING`.

**Criterios:** contrato de efectos enforced por el loader. Tests negativos exhaustivos.

### F24 — Async dispatcher + scheduler

**Milestone → v0.7.3**

- **F24.1** — Continuation table: cada effect async recibe un continuation id; `os.resume` lo reanuda.
- **F24.2** — Scheduler con timers: `os.schedule_in(ms, callback)`, `os.cancel`.
- **F24.3** — Stream buffers: ring buffer de N, distinct, throttle, debounce (primitivos para watches futuros).
- **F24.4** — Memory pressure monitor: emite evento `os.memory_pressure` si heap_free < threshold.
- **F24.5** — IPC framing esbozo: API `bridge.push_snapshot(bytes)` (sin UI aún).

**Criterios:** async dispatch end-to-end con un callback dummy. Scheduler ejecuta timers sin drift. Memory pressure test simula presión y verifica evento.

### F25 — SDI DL2 drivers

**Milestone → v0.8.0**

Cada driver un sub-paso. Cada uno con selftest ampliado.

- **F25.1** — `storage.fs` gana `write`, `create`, `delete`, `mkdir`. VFS SPIFFS + eventual SD. Ahora FS no es read-only.
- **F25.2** — `network.wifi` driver: scan + connect + status + eventos. Reutilizar stack ESP-IDF WiFi.
- **F25.3** — `network.http` driver: GET + POST + headers + body + TLS (usar esp_http_client).
- **F25.4** — `system.battery` driver: ADC vía board HAL, evento low-battery.
- **F25.5** — `system.time` driver con SNTP: sync wall clock vía NTP pool cuando WiFi connected.
- **F25.6** — `system.security` driver: PIN hash (SHA-256 salt), lock/unlock, permission storage en NVS.
- **F25.7** — `bridge.ui` driver: recibe bytes DVC snapshot, decodea a LVGL widgets. Skeleton sin componentes aún.
- **F25.8** — `display.panel`, `display.touch` drivers: pull LCD init + GT911 touch del board/ actual (ya hay código en `components/board/`).

**Criterios:** cada driver con selftest verde. WiFi connect a red conocida end-to-end. HTTP GET a `https://httpbin.org/get` exitoso.

### F26 — Bridge UI DVC

**Milestone → v0.8.5**

- **F26.1** — DVC wire format spec + encoder en runtime (serializa snapshots como binary blob).
- **F26.2** — Render arena + decoder en `deck_bridge_ui/`.
- **F26.3** — Componentes básicos: `TEXT`, `LABEL`, `BUTTON`.
- **F26.4** — Componentes input: `PASSWORD`, `CHOICE`, `TOGGLE`, `RANGE`.
- **F26.5** — Overlays: `CONFIRM`, `LOADING`, `KEYBOARD` (TEXT_UPPER).
- **F26.6** — Statusbar service (time/WiFi/battery minimal).
- **F26.7** — Navbar service (BACK/HOME botones).
- **F26.8** — Activity stack: push/pop/pop_to_home. Lifecycle hooks (on_create, on_resume, on_pause, on_destroy).
- **F26.9** — Rotación: recreate_all.

**Criterios:** app Deck que emite un snapshot con label + botón, al tapear el botón incrementa contador. Statusbar actualiza batería/wifi dinámicamente.

### F27 — Shell DL2

**Milestone → v0.9.0**

- **F27.1** — Lockscreen PIN (re-usar o recrear como app Deck DL2).
- **F27.2** — Intent navigation: `ui_intent_navigate(intent)`.
- **F27.3** — Settings sub-screens conventions (screen_id namespacing).
- **F27.4** — Display rotation con settings persist en NVS.

**Criterios:** boot muestra lockscreen → unlock → launcher (real, multi-app).

### F28 — App model DL2 completo

**Milestone → v0.9.1**

- **F28.1** — `@machine.before` / `@machine.after` hooks.
- **F28.2** — `@flow` + `@flow.step` sugar (transforma a `@machine` durante loading).
- **F28.3** — `@on` lifecycle completo: `resume`, `pause`, `suspend`, `terminate`, `low_memory`, `network_change`.
- **F28.4** — `@migration` (stub DL2; ejecuta desde version N−1 a N leyendo NVS).
- **F28.5** — `@assets` bundle.

**Criterios:** app con lifecycle completo ejecuta todos los hooks bajo los eventos correspondientes. Tests específicos por hook.

### F29 — Apps DL2

**Milestone → v0.9.5**

- **F29.1** — Launcher DL2: grid completo, iconos desde `@assets` o LVGL symbols.
- **F29.2** — Task Manager: lista apps running, kill dialog con confirm, memoria por app.
- **F29.3** — `net_hello.deck`: WiFi connect + HTTP GET + display del response.
- **F29.4** — `counter.deck`: ejemplo simple que mantiene estado en @machine + UI button.

**Criterios:** las 4 apps corren desde launcher, Task Manager las gestiona, se recuperan de rotación.

### F30 — Conformance DL2

**Milestone → v0.9.9 RC**

Extender la batería de conformance. Nuevo scope ≥ 150 checks.

- **F30.1** — Harness: registra también `deck_level=2`, reporta features DL2 detectadas.
- **F30.2** — Tests lang DL2: fn, lambda, recursion, list/tuple/map literals, interpolation, pipe, do, is, where (≥ 30 tests).
- **F30.3** — Tests stdlib: list.map/filter/reduce, map.*, Result/Optional helpers (≥ 20 tests).
- **F30.4** — Tests error paths DL2: `!alias` sin `@use`, `@requires` capability missing, `@permissions` denegada.
- **F30.5** — Tests bridge UI: snapshot roundtrip, component decoding, statusbar updates.
- **F30.6** — Tests multi-app: push/pop 4 apps simultáneas, memoria por app bounded.
- **F30.7** — Tests networking: WiFi scan, WiFi connect, HTTP GET, HTTP POST (mock o httpbin), timeout handling.
- **F30.8** — Tests lifecycle: todos los `@on` hooks invocados en el orden correcto.
- **F30.9** — Stress DL2: multi-app load bajo memory pressure, interpretes simultáneos, rotation churn.
- **F30.10** — Conformance report: JSON snapshot DL2 (`tests/conformance/reports/dl2-sample.json`).

**Criterios:** ≥ 150 checks verdes. Todos los tests DL1 siguen verdes (strict superset).

### F31 — Release v0.10.0 DL2 certified

**Milestone → v0.10.0**

- Medir size con `idf.py size-components` — verificar ≤ 280 KB runtime flash.
- Boot time + idle heap budgets.
- CHANGELOG, README, ARCHITECTURE, tests/conformance/README actualizados.
- version.txt → 0.10.0.
- Tag v0.10.0.

---

## 5. Criterios de release v0.10.0

Checklist obligatorio:

- ✅ `idf.py build` limpio, sin warnings
- ✅ Boot arranca al launcher full sin panic
- ✅ Lockscreen PIN funcional
- ✅ Launcher muestra ≥ 4 apps instalables
- ✅ Task Manager kill-switch opera
- ✅ `net_hello.deck` ejecuta WiFi + HTTP round-trip en red real
- ✅ Runtime flash ≤ 280 KB
- ✅ Heap idle ≤ 512 KB SRAM
- ✅ `system.info.deck_level` responde `2`
- ✅ App con `requires.deck_level: 3` obtiene `E_LEVEL_BELOW_REQUIRED` limpio
- ✅ Conformance suite DL2 — ≥ 150 tests verdes
- ✅ rerun_sanity_x100 delta 0 (regression DL1)
- ✅ Fuzz 500+ iter — 0 crashes
- ✅ Reporte JSON committed en `tests/conformance/reports/dl2-sample.json`
- ✅ README + CHANGELOG actualizados

---

## 6. Riesgos y mitigaciones DL2

| Riesgo | Mitigación |
|---|---|
| **WiFi + LVGL + Deck runtime saturan heap** | Presupuesto de 512 KB SRAM + PSRAM opcional. Instrumentar heap en cada driver. Si excedemos, escalar a PSRAM (LV_USE_PSRAM) o reducir buffer sizes. |
| **Continuation table complejidad** | Implementar incrementalmente: F24.1 arranca con 1 pending continuation; escalar solo cuando un test lo exija. |
| **DVC wire format vs LVGL impedance** | Definir la spec wire primero (F26.1), generar ambos extremos de la spec (runtime emitter + bridge decoder) en paralelo con tests round-trip antes de cualquier UI visible. |
| **Multi-app concurrency bugs** | Single-evaluator-thread retenido del DL1. Tests stress multi-app deben correr bajo churn + rotación + memory pressure simultáneamente. |
| **Spec 16 §5 amplio — scope creep** | Cada sub-fase tiene criterio de salida explícito con tests. No avanzar hasta el criterio cumplido en hardware. |
| **Breaking DL1 al apilar features** | Toda la suite DL1 (66 checks) debe seguir verde después de cada sub-commit. Re-correr la suite completa en cada flash. |
| **WiFi connect inestable en tests** | Tests de networking gated por env var `CYBERDECK_TEST_SSID`/`CYBERDECK_TEST_PSK`. En boot sin credenciales, skip graciosamente. |
| **Render arena fragmentation** | Arena dedicada por snapshot, liberada al recibir el siguiente. Medir fragmentación en stress rotación. |
| **Tiempo total a v0.10.0** | Estimado ~200-400 sesiones atómicas. Cada `v0.X.Y` intermedio es release interna verificada. Tag `v0.10.0` solo cuando §5 checklist completo. |

---

## 7. Orden de batalla sugerido

El camino más corto a "algo visible" es:

1. **F21** (lang foundations) + **F22** (types/stdlib) — 2–3 semanas de sprints. Sin UI, pero el runtime gana toda la expresividad. Validado por conformance.
2. **F25.7** (bridge.ui skeleton) + **F25.8** (display/touch) + **F26.1–F26.3** (DVC wire + TEXT component) — primer "Hello DL2" VISIBLE en pantalla.
3. **F23** (effects/@use) — antes de agregar network, para que el loader enforce el contrato.
4. **F25.2–F25.3** (WiFi + HTTP) → **F29.3** (`net_hello.deck`) — primer app de red visible.
5. **F24** (async dispatcher) — necesario antes de streams/watches reales.
6. **F26.4–F26.9** (UI completo) + **F27** (shell) + **F28** (app model) — el sistema operativo toma forma.
7. **F29** (apps) → **F30** (conformance) → **F31** (release).

Este orden prioriza **feedback visible temprano** y **contratos enforced antes de expandir surface**. Si algo en F26 ó F25 resulta más difícil de lo esperado, siempre podemos inyectar fases F21.x adicionales sin romper el stack.

---

**Próximo paso concreto:** arrancar F21.1 (`fn` keyword + parser + interp + 5 tests) cuando decidamos iniciar DL2.
