# DEVELOPMENT-PLAN.md — CyberDeck v0.1 → v1.0.0

**Scope:** llevar el firmware desde el estado actual (C-native app framework en `main`, sin runtime Deck) hasta la implementación de referencia DL3 de la especificación Deck, con apps bundled portadas al lenguaje Deck, conformance suite verde, y publicación de componentes IDF.

**Estrategia central:** desarrollo atómico + incremental + paralelizable. Cada commit es una unidad verificable en hardware. Cada minor release cierra un bloque coherente de features. Cada major release cumple un nivel (DL) de conformancia completo.

**Ciclo de trabajo por commit atómico:**

```
edit  →  build (idf.py build)                        # compila, -Werror limpio
      →  flash (idf.py -p /dev/cu.usbmodem1101 flash monitor)
      →  debug (verificación visual + UART logs + JTAG si aplica)
      →  commit (mensaje conciso en español, siguiendo estilo del repo)
      →  release? (si cerramos un bloque coherente, bump y tag)
```

**"Done" = hardware verificado.** Compilar no es done. Correr tests unitarios no es done. Done es: flashea sin errores, arranca, la feature hace lo que dice, no rompe lo anterior.

---

## Índice

1. [Estado de partida](#1-estado-de-partida)
2. [Principios de planificación](#2-principios-de-planificación)
3. [Tracks paralelos de desarrollo](#3-tracks-paralelos-de-desarrollo)
4. [Roadmap de versiones (v0.1 → v1.0.0)](#4-roadmap-de-versiones-v01--v100)
5. [Fase 0 — Fundaciones y planning](#5-fase-0--fundaciones-y-planning)
6. [Fase 1 — SDI y runtime skeleton (v0.2.x)](#6-fase-1--sdi-y-runtime-skeleton-v02x)
7. [Fase 2 — Runtime DL1 funcional (v0.3.x)](#7-fase-2--runtime-dl1-funcional-v03x)
8. [Fase 3 — Conformance DL1 (v0.4.0)](#8-fase-3--conformance-dl1-v040)
9. [Fase 4 — DL2 expansion (v0.5.x)](#9-fase-4--dl2-expansion-v05x)
10. [Fase 5 — Conformance DL2 (v0.6.0)](#10-fase-5--conformance-dl2-v060)
11. [Fase 6 — DL3 core (v0.7.x)](#11-fase-6--dl3-core-v07x)
12. [Fase 7 — Apps bundled en Deck (v0.8.x)](#12-fase-7--apps-bundled-en-deck-v08x)
13. [Fase 8 — Kitchen sink (v0.9.x)](#13-fase-8--kitchen-sink-v09x)
14. [Fase 9 — v1.0.0](#14-fase-9--v100)
15. [Estrategia de testing en hardware](#15-estrategia-de-testing-en-hardware)
16. [Gestión de versiones y tags](#16-gestión-de-versiones-y-tags)
17. [Estrategia de paralelización](#17-estrategia-de-paralelización)
18. [Riesgos y mitigaciones](#18-riesgos-y-mitigaciones)

---

## 1. Estado de partida

**Versión actual:** `0.1.0`

**Lo que existe (ver `GROUND-STATE.md`):**

| Área | Estado | Componente |
|---|---|---|
| App lifecycle (launch/suspend/foreground/terminate) | ✅ Production | `app_framework/` |
| Activity stack con 4 niveles, lockscreen, gestos | ✅ Production | `ui_engine/` |
| Event bus (ESP_EVENT + svc_event) | ✅ Production | `sys_services/` |
| Storage por app (SD + SQLite) | ✅ Production | `os_core/os_app_storage`, `os_db` |
| NVS global (`svc_settings`) | ✅ Production | `sys_services/` |
| UI theme, ui_common widgets (label/btn/list/card/…) | ✅ Works | `ui_engine/` |
| HAL (LCD, touch, CH422G, RTC, SD, battery) | ✅ Production | `board/` |
| SD app discovery + manifest + registro dinámico | ✅ Production | `os_core/` |
| Apps nativas: Launcher, Settings (parcial) | ✅ Works | `apps/` |
| **Deck runtime** | ❌ No existe | — |
| **Deck lexer/parser/evaluator** | ❌ No existe | — |
| **SDI vtable formal** | ⚠️ Implícito (servicios hablan directo) | — |
| **Conformance suite** | ❌ No existe | — |

**Implicaciones:**

1. No partimos desde cero — el C framework está maduro. El trabajo es *reempaquetarlo* detrás del SDI y *conectarle* un evaluador Deck arriba.
2. El mayor bloque nuevo es el runtime Deck (lexer + parser + loader + evaluator + dispatcher). Eso es código 100% nuevo.
3. Las apps bundled ya existen como C nativo. Necesitamos *volver a escribirlas en Deck* como prueba del sistema, pero el C nativo queda como fallback (coexisten durante la migración).
4. SDI se extrae del código existente sin rehacerlo — es una capa de headers + vtable encima de los servicios actuales.

---

## 2. Principios de planificación

### 2.1 Atomicidad

Cada commit hace **una** cosa: una función, un módulo, un cambio de firma, un bug fix aislado. Nunca mezclar refactor + feature nueva. Nunca commit con el árbol en estado no-arrancable.

**Guía del tamaño:** si el mensaje de commit necesita "y también" o "además", parte el commit. Típico objetivo: 50–300 líneas netas por commit.

### 2.2 Incrementalidad

Siempre que sea posible, entregar un "final simple pero completo" antes que un "prototipo grande pero a medias". Ejemplo: un lexer que solo acepta enteros + identificadores pero completo (acepta, rechaza, emite tokens correctos, pasa 20 tests) es preferible a un lexer "para todo el lenguaje pero con bugs silenciosos en strings".

### 2.3 Progresividad (DL1 → DL2 → DL3)

Seguimos el orden de los niveles de `16-deck-levels.md`:

- DL1 primero: runtime mínimo, apps triviales, conformance DL1 verde.
- DL2 segundo: WiFi + HTTP + lockscreen + Task Manager, apps mid-range.
- DL3 último: SQLite + MQTT + BLE + OTA + content bodies + Bluesky.

La estructura de test suite sigue el mismo orden: primero todos los tests DL1 verdes y estables, luego DL2, luego DL3. Nunca se avanza de nivel con tests del nivel inferior en rojo.

### 2.4 Paralelizable pero sincronizado

Varios *tracks* pueden avanzar en paralelo (ver §3). Pero los merges a `main` siempre dejan el firmware verde y hardware-verificado. No se mergea código que compila pero no fue flasheado y verificado visualmente cuando hay cambio de UI o flujo de ejecución.

### 2.5 Siempre flasheable

Todo commit en `main` debe arrancar en la placa. Si un commit introduce una feature a medio terminar (por ejemplo un capability Deck aún sin impl), debe existir detrás de un feature flag (`CONFIG_DECK_EXPERIMENTAL_<X>` en menuconfig) o un stub que devuelve `NOT_IMPLEMENTED` sin crashear.

### 2.6 Release conservador

Un tag `vX.Y.Z` implica:

- Build limpio en el comando oficial (`idf.py build`)
- Flash limpio en USB native
- Arranque limpio en la placa (sin crashes, brownouts, ni watchdog resets)
- Todas las features listadas en el release notes hardware-verificadas
- Conformance suite correspondiente (si aplica) al 100%

Si un criterio falla, no se tagea. Se crea un `rc.N` interno o se resuelve primero.

---

## 3. Tracks paralelos de desarrollo

Cinco tracks pueden avanzar en paralelo. Cada track se lista con su owner primario y sus dependencias con otros tracks.

### Track A — SDI extraction

Extraer la vtable SDI de los servicios existentes. No introduce código nuevo de features — solo formaliza interfaces. Tiene impacto en `board/`, `sys_services/` y `os_core/`.

**Entrada:** estado actual, spec `12-deck-service-drivers.md`.
**Salida:** header `deck_sdi.h`, implementaciones `deck_sdi_<driver>.c` que envuelven servicios existentes.
**Sincroniza con:** Track B (el runtime llama a SDI, no a servicios directamente).

### Track B — Deck runtime

Lexer, parser, loader, evaluator, dispatcher. 100% código nuevo. Se construye en un nuevo componente `components/deck_runtime/`.

**Entrada:** specs `01-04`, `11`.
**Salida:** librería que puede cargar un `.deck`, verificarlo, evaluarlo, y hacer dispatch de efectos a SDI.
**Sincroniza con:** Track A (consume SDI), Track C (proporciona runtime para tests de conformancia).

### Track C — Conformance suite

Batería de apps Deck + harness que corre cada una y verifica comportamiento esperado. Se publica como componente separado pero se ejecuta dentro de la imagen principal mediante un modo especial.

**Entrada:** specs `01–15`, doc de niveles `16`.
**Salida:** conjunto de `.deck` tests + harness + reporte por nivel.
**Sincroniza con:** Track B (necesita runtime para correr).

### Track D — Bridge UI (DVC renderer)

Ampliar `ui_engine/` para decodificar árboles DVC (wire format de content bodies) y renderizarlos como widgets LVGL. Es código nuevo que se apoya en `ui_common` ya existente.

**Entrada:** specs `10-deck-bridge-ui.md`, `11-deck-implementation.md` §DVC format.
**Salida:** `ui_engine/ui_dvc_render.c` + componentes DVC nuevos faltantes.
**Sincroniza con:** Track B (runtime emite DVC, bridge consume).

### Track E — Apps bundled en Deck

Reescribir Launcher, Task Manager, Settings, Files en Deck. Inicialmente una por una, mientras la contraparte en C sigue funcionando.

**Entrada:** annexes A–D, `09-deck-shell.md`.
**Salida:** `.deck` files empacados como bundles IDF + tests de equivalencia.
**Sincroniza con:** Tracks B, D (necesita runtime + DVC renderer maduro).

### Cronograma cruzado

```
Fase:   F1  F2  F3  F4  F5  F6  F7  F8  F9
Track A  ██  ██  .   ░░  .   ░░  .   .   ░░
Track B  .   ██  ██  ██  ░░  ██  ░░  ░░  .
Track C  .   .   ██  ░░  ██  ░░  ██  ░░  ██
Track D  ░░  ░░  .   ██  ░░  ██  ░░  ░░  .
Track E  .   .   .   .   .   ░░  ██  ██  ░░

██ = trabajo principal   ░░ = trabajo incremental   . = quiescent
```

---

## 4. Roadmap de versiones (v0.1 → v1.0.0)

| Versión | Contenido | Nivel cubierto |
|---|---|---|
| **v0.1.0** (actual) | C framework production, sin runtime Deck | — |
| **v0.2.0** | SDI extracción + runtime skeleton (lexer + parser incompleto) | prep |
| **v0.2.x** | Iteraciones sobre SDI y parser (x = .1, .2, …) | prep |
| **v0.3.0** | Runtime DL1 ejecuta primer `.deck` hello-world | DL1 partial |
| **v0.3.x** | Completar surface DL1 + loader stages 0–9 | DL1 partial |
| **v0.4.0** | **Conformance DL1 al 100%.** Release estable. | **DL1** |
| **v0.5.0** | DL2 arranca: @use OS caps, @requires, WiFi/HTTP, @flow, @permissions | DL2 partial |
| **v0.5.x** | Lockscreen, Task Manager, @task, @config, @migration | DL2 partial |
| **v0.6.0** | **Conformance DL2 al 100%.** Release estable. | **DL2** |
| **v0.7.0** | DL3 arranca: SQLite, Stream, content bodies, @handles | DL3 partial |
| **v0.7.x** | MQTT, BLE, sensors, audio, OTA dual-track | DL3 partial |
| **v0.8.0** | Launcher + Task Manager + Settings en Deck | DL3 partial |
| **v0.8.x** | Files en Deck, feature parity completa con C natives | DL3 partial |
| **v0.9.0** | Bluesky kitchen-sink reference app | DL3 near-complete |
| **v0.9.x** | Polish, perf, memory tuning, docs | DL3 near-complete |
| **v1.0.0** | **Conformance DL3 al 100%** + apps bundled todas en Deck + componentes IDF publicados | **DL3** |

**Criterios de release:** cada `vX.Y.0` requiere conformance del nivel al 100% (cuando aplica) y todos los tests hardware verdes. Cada `vX.Y.Z` (patch) es fix + polish sin features nuevas.

---

## 5. Fase 0 — Fundaciones y planning

**Objetivo:** preparar el terreno para los cinco tracks.

**Duración estimada:** 2–3 sesiones.

### 5.1 Tareas atómicas

Cada bullet = un commit.

- [ ] `chore: bump version.txt a 0.2.0-dev`
- [ ] `build: añadir componente vacío components/deck_runtime/`
- [ ] `build: añadir componente vacío components/deck_sdi/`
- [ ] `build: añadir componente vacío components/deck_bridge_dvc/`
- [ ] `docs: añadir ARCHITECTURE.md con diagrama de componentes post-SDI`
- [ ] `ci: añadir script scripts/build-all.sh que compila y reporta tamaños`
- [ ] `ci: añadir script scripts/flash-verify.sh que automatiza flash + captura de boot log`

### 5.2 Criterio de salida

- Firmware sigue arrancando idéntico a v0.1.0 (ningún componente nuevo altera el comportamiento)
- Build-all script mide flash size baseline
- `scripts/flash-verify.sh` captura un "boot log golden" en `tests/boot.golden` para regresión

---

## 6. Fase 1 — SDI y runtime skeleton (v0.2.x)

**Objetivo:** formalizar el SDI y tener un runtime que parsea (pero todavía no evalúa).

**Duración estimada:** 8–12 sesiones.

### 6.1 Track A — SDI extraction

Cada commit = un driver extraído.

- [ ] `sdi: definir struct deck_sdi_nvs_t + wrapping de svc_settings`
- [ ] `sdi: definir deck_sdi_fs_t + wrapping de os_storage`
- [ ] `sdi: definir deck_sdi_system_info_t (device_id, free_heap, app_id)`
- [ ] `sdi: definir deck_sdi_system_time_t (monotonic + wall)`
- [ ] `sdi: definir deck_sdi_shell_t (single-app minimal)`
- [ ] `sdi: añadir registro central deck_sdi_register(driver)`
- [ ] `sdi: documentar vtable con headers comentados y ejemplos`
- [ ] `test: harness manual que lista drivers registrados por UART al boot`

**Criterio parcial:** al boot, por UART vemos `deck_sdi: registered 5 drivers: nvs, fs, system.info, system.time, shell`. Nada del comportamiento actual cambia.

### 6.2 Track B — Runtime skeleton

- [ ] `runtime: crear componente deck_runtime con esqueleto de deck_runtime_init()`
- [ ] `runtime: definir tipos básicos (deck_value_t, deck_type_t, deck_atom_t)`
- [ ] `runtime: implementar string interning table`
- [ ] `runtime: implementar refcount allocator sobre heap_caps`
- [ ] `runtime: stub deck_lexer_init/next/free`
- [ ] `runtime: implementar lexer para literales (int, float, bool, unit)`
- [ ] `runtime: implementar lexer para identificadores, keywords, operadores`
- [ ] `runtime: implementar lexer para strings + interpolación + escapes`
- [ ] `runtime: implementar lexer para indent/dedent (WS-sensitive)`
- [ ] `runtime: añadir 30+ tests de lexer en modo harness UART (tabla de entrada/salida)`
- [ ] `runtime: stub deck_parser_parse_module()`
- [ ] `runtime: parser — expresiones primarias y precedencia de operadores`
- [ ] `runtime: parser — let bindings y where clauses`
- [ ] `runtime: parser — funciones puras`
- [ ] `runtime: parser — match básico (atoms + literales + wildcards)`
- [ ] `runtime: parser — @app header completo`
- [ ] `runtime: parser — @machine básico (states + transitions)`
- [ ] `runtime: parser — imports y módulos`
- [ ] `runtime: añadir 40+ tests de parser con golden AST printouts`

**Criterio parcial:** al boot, un modo especial (activado con build flag) lee `/sdcard/tests/*.deck`, los lexea, los parsea, y reporta pass/fail por UART. Toda la salida del parser es estructuralmente correcta.

### 6.3 Tag v0.2.0

- Build limpio
- Flash limpio
- SDI registra 5 drivers al boot
- Runtime parsea 10 programas Deck de ejemplo (versión DL1 trivial)
- Nada del comportamiento original se ve alterado (launcher, settings, etc.)

---

## 7. Fase 2 — Runtime DL1 funcional (v0.3.x)

**Objetivo:** un programa Deck DL1 se carga, se evalúa, invoca capabilities vía SDI y produce efectos observables.

**Duración estimada:** 12–18 sesiones.

### 7.1 Track B — Loader

- [ ] `loader: stage 0 — lexical pass integrado`
- [ ] `loader: stage 1 — parse + AST construction`
- [ ] `loader: stage 2 — resolve symbols (module-local only)`
- [ ] `loader: stage 3 — type check (DL1 subset: primitivos, composites, optional)`
- [ ] `loader: stage 4 — capability bind (solo math/text/log/time/nvs/fs/system.info)`
- [ ] `loader: stage 5 — pattern match exhaustiveness check`
- [ ] `loader: stage 6 — compat check (edition, deck_level, deck_os, runtime)`
- [ ] `loader: stage 7 — reserved`
- [ ] `loader: stage 8 — freeze module (inmutable imagen lista para eval)`
- [ ] `loader: stage 9 — linkage a runtime + activa @app.entry`
- [ ] `loader: errores estructurados (E_LEVEL_BELOW_REQUIRED, E_CAPABILITY_MISSING, etc.)`

### 7.2 Track B — Evaluator

- [ ] `eval: value stack (512 deep) + frame stack`
- [ ] `eval: evaluar literales y atoms`
- [ ] `eval: evaluar let bindings`
- [ ] `eval: evaluar llamadas a funciones puras`
- [ ] `eval: evaluar aritmética y lógica`
- [ ] `eval: evaluar string concat + interpolación`
- [ ] `eval: evaluar match (con patrones DL1)`
- [ ] `eval: evaluar módulo imports`
- [ ] `eval: invocar effect dispatcher`

### 7.3 Track B — Dispatcher

- [ ] `dispatcher: routing basado en capability name → SDI driver`
- [ ] `dispatcher: convertir deck_value_t ↔ tipos C para cada capability`
- [ ] `dispatcher: integrar builtins puros (math, text, bytes, log)`
- [ ] `dispatcher: integrar time.now, system.info, nvs, fs (read)`
- [ ] `dispatcher: crash handler (longjmp al safe point)`

### 7.4 Track B — @machine básico

- [ ] `machine: instanciar máquina desde AST`
- [ ] `machine: evaluar on enter/leave hooks`
- [ ] `machine: evaluar transiciones (send)`
- [ ] `machine: lifecycle launch → idle → terminate`

### 7.5 Track C — Conformance harness inicial

- [ ] `conformance: infra — harness que carga un `.deck` y verifica output esperado`
- [ ] `conformance: DL1 test 01 — hello world (log.info)`
- [ ] `conformance: DL1 test 02 — aritmética + match`
- [ ] `conformance: DL1 test 03 — NVS round-trip`
- [ ] `conformance: DL1 test 04 — machine state transitions`
- [ ] `conformance: DL1 test 05 — module imports`

### 7.6 Hitos intermedios

- **v0.3.0** — primer `.deck` hello-world imprime por UART desde una app Deck real. Es *el* momento fundacional.
- **v0.3.5** — 20+ tests DL1 pasando.
- **v0.3.9** — todos los tests de surface DL1 pasando excepto match avanzado (que es DL2).

---

## 8. Fase 3 — Conformance DL1 (v0.4.0)

**Objetivo:** conformance DL1 al 100% y release estable.

**Duración estimada:** 4–6 sesiones.

### 8.1 Tareas

- [ ] `conformance: completar batería DL1 (~50 tests cubriendo todo el spec DL1)`
- [ ] `conformance: añadir stress tests (loops largos, recursión, muchas allocs)`
- [ ] `conformance: añadir memory tests (verifica que refcount no filtra)`
- [ ] `conformance: generar reporte de conformancia JSON`
- [ ] `runtime: fix bugs descubiertos por conformance suite (track commits 1-per-fix)`
- [ ] `perf: medir footprint DL1 — flash/heap/boot time`
- [ ] `perf: comparar con target §4.9 de 16-deck-levels.md (≤120KB flash, ≤64KB heap)`
- [ ] `perf: optimizaciones si se excede (inline, dead-code removal, const-propagation)`
- [ ] `docs: actualizar version.txt a 0.4.0`
- [ ] `docs: CHANGELOG.md con features v0.4.0`
- [ ] `release: tag v0.4.0`

### 8.2 Criterio de release v0.4.0

- ✅ 50+ tests DL1 pasan en hardware
- ✅ Flash size del runtime DL1 ≤ 120 KB
- ✅ Heap de runtime en idle ≤ 64 KB
- ✅ Al menos 10 `.deck` programs de ejemplo en `/sdcard/examples/dl1/`
- ✅ C framework original sigue funcionando (Launcher, Settings, etc.)
- ✅ `system.info.deck_level` reporta `1` correctamente
- ✅ Apps que piden `deck_level: 2` obtienen `E_LEVEL_BELOW_REQUIRED`

---

## 9. Fase 4 — DL2 expansion (v0.5.x)

**Objetivo:** runtime + OS surface DL2 completa. El device ya corre apps Deck con WiFi, HTTP, lockscreen, Task Manager en Deck.

**Duración estimada:** 18–24 sesiones.

### 9.1 Track B — Lenguaje DL2

Cada commit = una feature del lenguaje.

- [ ] `lang: union types + Result T E`
- [ ] `lang: named record types (@type)`
- [ ] `lang: effect annotations (!alias) + verificación en loader`
- [ ] `lang: where bindings completo`
- [ ] `lang: recursion + tail calls con trampolining`
- [ ] `lang: lambdas + closure capture`
- [ ] `lang: pattern matching avanzado (records, variantes con payload, guards)`
- [ ] `lang: do blocks`
- [ ] `lang: pipe operators |> y |>?`
- [ ] `lang: is operator`
- [ ] `lang: stdlib — list helpers (len, head, tail, map, filter, reduce, ...)`
- [ ] `lang: stdlib — map helpers`
- [ ] `lang: stdlib — Result/Optional helpers`
- [ ] `lang: stdlib — type inspection (type_of, is_*)`
- [ ] `lang: @private modifier`

### 9.2 Track B — App model DL2

- [ ] `app: @use OS capabilities (completo)`
- [ ] `app: @use.optional`
- [ ] `app: @requires.capabilities versioned`
- [ ] `app: @permissions + runtime permission gate`
- [ ] `app: @config con NVS persistence`
- [ ] `app: @errors domain vocabulary`
- [ ] `app: @machine.before/after transition hooks`
- [ ] `app: @flow + @flow.step (desugar a @machine)`
- [ ] `app: @on full lifecycle (launch, resume, pause, suspend, terminate, low_memory, network_change)`
- [ ] `app: @migration (schema evolution)`
- [ ] `app: @test inline tests`
- [ ] `app: @assets bundle`

### 9.3 Track A — SDI DL2 drivers

- [ ] `sdi: storage.fs write (create, delete, mkdir, write)`
- [ ] `sdi: network.wifi (scan, connect, disconnect, status)`
- [ ] `sdi: network.http sync (GET, POST, headers, TLS)`
- [ ] `sdi: api_client (session with retry + caching)`
- [ ] `sdi: cache (in-memory TTL)`
- [ ] `sdi: crypto.aes (encrypt/decrypt, gen_key)`
- [ ] `sdi: system.battery (level, charging, low event)`
- [ ] `sdi: system.time NTP sync`
- [ ] `sdi: system.security (PIN hash, lock/unlock, permission_get/set)`
- [ ] `sdi: system.tasks (list_processes, kill_process)`
- [ ] `sdi: display.notify (toast → bridge)`
- [ ] `sdi: display.screen (brightness, power)`
- [ ] `sdi: system.locale`

### 9.4 Track B — Runtime DL2

- [ ] `runtime: scheduler (timers + condition tracker)`
- [ ] `runtime: stream buffers (ring, distinct, throttle, debounce)`
- [ ] `runtime: continuation table (async effects)`
- [ ] `runtime: memory pressure monitor → os.memory_pressure event`
- [ ] `runtime: IPC framing runtime ↔ bridge`

### 9.5 Track D — Bridge UI DL2

- [ ] `bridge: DVC decoder (wire format → LVGL tree)`
- [ ] `bridge: DVC_TEXT + DVC_PASSWORD`
- [ ] `bridge: DVC_CHOICE + DVC_DATE`
- [ ] `bridge: DVC_TOGGLE + DVC_RANGE`
- [ ] `bridge: DVC_CONFIRM (modal)`
- [ ] `bridge: DVC_LOADING overlay`
- [ ] `bridge: statusbar minimal desde Deck`
- [ ] `bridge: navbar BACK + HOME desde Deck`
- [ ] `bridge: keyboard service (TEXT_UPPER)`
- [ ] `bridge: activity stack integrado con @machine states`

### 9.6 Track E — Apps bundled parciales

- [ ] `app: escribir Launcher mínimo en Deck (convive con C native)`
- [ ] `app: escribir Task Manager en Deck (reemplaza al C stub)`
- [ ] `app: lockscreen en Deck`

### 9.7 Track C — Tests DL2

- [ ] `conformance: ~40 tests nuevos cubriendo DL2`
- [ ] `conformance: tests de WiFi flow (mock + real)`
- [ ] `conformance: tests de HTTP flow`
- [ ] `conformance: tests de permissions gate`
- [ ] `conformance: tests de @flow desugar equivalence`
- [ ] `conformance: tests de stream buffers`

### 9.8 Hitos intermedios

- **v0.5.0** — primer app DL2 con `@use wifi http` conecta a red y hace GET
- **v0.5.5** — Task Manager en Deck reemplaza al native
- **v0.5.9** — Lockscreen en Deck reemplaza al native

---

## 10. Fase 5 — Conformance DL2 (v0.6.0)

**Objetivo:** conformance DL2 al 100% y release estable.

**Duración estimada:** 6–8 sesiones.

### 10.1 Tareas

- [ ] `conformance: completar batería DL2 (~100 tests acumulados DL1+DL2)`
- [ ] `conformance: stress tests — long running WiFi reconnects, múltiples apps concurrentes`
- [ ] `conformance: tests de migración @migration`
- [ ] `perf: medir footprint DL2 — target ≤ 280KB flash, ≤ 512KB SRAM`
- [ ] `perf: optimizaciones agresivas donde se exceda`
- [ ] `docs: release notes v0.6.0`
- [ ] `release: tag v0.6.0`

### 10.2 Criterio de release v0.6.0

- ✅ 100+ tests DL1+DL2 pasan en hardware
- ✅ Flash del runtime DL2 ≤ 280 KB
- ✅ Heap en idle ≤ 512 KB SRAM
- ✅ Task Manager funcional en Deck
- ✅ Lockscreen funcional en Deck
- ✅ WiFi + HTTP end-to-end verificado
- ✅ `system.info.deck_level` reporta `2`
- ✅ 20+ `.deck` examples en `/sdcard/examples/dl2/`

---

## 11. Fase 6 — DL3 core (v0.7.x)

**Objetivo:** capabilities DL3 del OS + features del lenguaje DL3.

**Duración estimada:** 20–26 sesiones.

### 11.1 Track B — Lenguaje DL3

- [ ] `lang: Stream T type + @stream declarations`
- [ ] `lang: Fragment type`
- [ ] `lang: view content functions`
- [ ] `lang: when/for en content bodies`
- [ ] `lang: stdlib — compose, flip`
- [ ] `lang: random builtin (impuro)`

### 11.2 Track B — App model DL3

- [ ] `app: @use.when conditional activation`
- [ ] `app: @machine.content =`
- [ ] `app: @machine.watch: reactive watches`
- [ ] `app: @machine delegated (machine: / flow:)`
- [ ] `app: @stream declarations + integration with @machine.watch:`
- [ ] `app: content body renderer — group`
- [ ] `app: content body renderer — text, button, input`
- [ ] `app: content body renderer — list, markdown, image`
- [ ] `app: content body renderer — canvas`
- [ ] `app: content body renderer — when / for`
- [ ] `app: content intents (action / toggle / navigate / send / emit)`
- [ ] `app: @task + @task.every scheduler`
- [ ] `app: @doc + @example metadata`
- [ ] `app: @handles deep links + router`

### 11.3 Track A — SDI DL3 drivers

- [ ] `sdi: storage.db (SQLite wrapping existing os_db)`
- [ ] `sdi: storage.cache`
- [ ] `sdi: network.socket (TCP/UDP)`
- [ ] `sdi: network.ws (WebSocket)`
- [ ] `sdi: network.mqtt`
- [ ] `sdi: network.downloader (wrap svc_downloader)`
- [ ] `sdi: network.notifications (HTTP poll + MQTT push)`
- [ ] `sdi: ble (central, GATT)`
- [ ] `sdi: sensors stubs (GPS, temp, accel, light, ...)`
- [ ] `sdi: system.audio (PCM playback, volume, BT audio external module)`
- [ ] `sdi: hardware.gpio/i2c/spi/uart (expose pins for user apps)`
- [ ] `sdi: hardware.button/door_opened events`
- [ ] `sdi: system.apps (list_installed, config_schema)`
- [ ] `sdi: system.crashes`
- [ ] `sdi: system.shell privilegiado`
- [ ] `sdi: background_fetch`
- [ ] `sdi: markdown renderer`
- [ ] `sdi: ota.firmware (dual-bank, anti-rollback, signature verify)`
- [ ] `sdi: ota.app (per-app bundle install + migrations + rollback)`
- [ ] `sdi: bt_classic stub (reporta not-present)`

### 11.4 Track B — Runtime DL3

- [ ] `runtime: VM snapshot (serialize heap + frames)`
- [ ] `runtime: VM restore (deserialize + remap)`
- [ ] `runtime: hot reload con state migration`
- [ ] `runtime: multi-app concurrency (time-sliced cooperative scheduling)`
- [ ] `runtime: IPC completo runtime ↔ bridge con múltiples UI contexts`

### 11.5 Track D — Bridge UI DL3

- [ ] `bridge: DVC_SEARCH + DVC_PIN + DVC_PROGRESS`
- [ ] `bridge: DVC_CANVAS`
- [ ] `bridge: DVC_MARKDOWN`
- [ ] `bridge: DVC_SHARE`
- [ ] `bridge: date picker service`
- [ ] `bridge: badge service`
- [ ] `bridge: progress overlay service`
- [ ] `bridge: share sheet service`
- [ ] `bridge: permission dialog service`
- [ ] `bridge: statusbar completo (BT, SD, audio)`
- [ ] `bridge: navbar completo (+ TASKS long-press)`
- [ ] `bridge: display rotation desde Deck (recreate_all)`
- [ ] `bridge: theme switching desde Deck`

### 11.6 Track C — Tests DL3 parciales

- [ ] `conformance: ~60 tests nuevos cubriendo DL3 core`
- [ ] `conformance: tests de content bodies rendering`
- [ ] `conformance: tests de @stream watches`
- [ ] `conformance: tests de @task scheduler`
- [ ] `conformance: tests de @handles routing`

### 11.7 Hitos intermedios

- **v0.7.0** — primer content body de DL3 se renderiza en LCD desde una app Deck
- **v0.7.3** — SQLite CRUD desde Deck
- **v0.7.5** — MQTT pub/sub desde Deck
- **v0.7.7** — BLE scan + connect desde Deck
- **v0.7.9** — OTA dual-track dry-run verifica rollback

---

## 12. Fase 7 — Apps bundled en Deck (v0.8.x)

**Objetivo:** las 4 apps bundled (Launcher, Task Manager, Settings, Files) están 100% en Deck, con paridad feature-for-feature respecto de las versiones C nativas.

**Duración estimada:** 14–18 sesiones.

### 12.1 Track E — Launcher completo

- [ ] `app/launcher: grid adaptativo (5 cols landscape / 3 cols portrait)`
- [ ] `app/launcher: stream system.apps.list_installed_watch()`
- [ ] `app/launcher: app cards con icon + name + estado`
- [ ] `app/launcher: integración con statusbar`
- [ ] `app/launcher: handles search`
- [ ] `test: launcher Deck renderiza idéntico al C native en hardware`
- [ ] `chore: dejar C native como fallback detrás de CONFIG flag`

### 12.2 Track E — Task Manager

- [ ] `app/taskman: stream system.tasks.cpu_watch()`
- [ ] `app/taskman: stream system.apps.list_running_watch()`
- [ ] `app/taskman: detail screen con live heap/CPU`
- [ ] `app/taskman: kill dialog (DVC_CONFIRM)`
- [ ] `app/taskman: storage info per app`

### 12.3 Track E — Settings

- [ ] `app/settings: pantalla principal con lista de categorías`
- [ ] `app/settings: WiFi scan + connect sub-screen`
- [ ] `app/settings: Security (PIN set/clear)`
- [ ] `app/settings: Display (brightness, theme, rotation)`
- [ ] `app/settings: Audio (master volume, optional BT audio)`
- [ ] `app/settings: Time (NTP sync)`
- [ ] `app/settings: About (device info)`
- [ ] `app/settings: OTA check/download/apply con rollback`
- [ ] `app/settings: crash reporter`

### 12.4 Track E — Files

- [ ] `app/files: browse mounts (sdcard, flash)`
- [ ] `app/files: lista de archivos con tipo`
- [ ] `app/files: copy/move/delete/rename (multi-state machine)`
- [ ] `app/files: preview text/markdown/images`
- [ ] `app/files: integración con @handles de apps que declaran handlers`

### 12.5 Migración

Durante toda la fase se mantiene compat:

- [ ] `chore: feature flag CONFIG_CYBERDECK_USE_DECK_LAUNCHER (default Deck en dev, C native en prod)`
- [ ] `chore: similar para TASKMAN, SETTINGS, FILES`
- [ ] `chore: al final de la fase, Deck se vuelve default; C nativo marcado legacy`

### 12.6 Hitos

- **v0.8.0** — Launcher Deck es default, C native fallback
- **v0.8.3** — Task Manager Deck es default
- **v0.8.6** — Settings Deck es default
- **v0.8.9** — Files Deck es default, feature parity completa

---

## 13. Fase 8 — Kitchen sink (v0.9.x)

**Objetivo:** Bluesky app completo; encuentra bugs finales; perf + polish.

**Duración estimada:** 10–14 sesiones.

### 13.1 Track E — Bluesky

Seguir `annex-xx-bluesky.md` literalmente, paso por paso.

- [ ] `app/bsky: @app header + edition 2026 + requires DL3`
- [ ] `app/bsky: autenticación (OAuth XRPC)`
- [ ] `app/bsky: session persist en nvs + storage.local`
- [ ] `app/bsky: timeline con cache 30s`
- [ ] `app/bsky: notifications con badge stream`
- [ ] `app/bsky: compose con markdown`
- [ ] `app/bsky: search`
- [ ] `app/bsky: @task refresh cada 2 min`
- [ ] `app/bsky: @task token refresh cada 4 min`
- [ ] `app/bsky: image rendering`
- [ ] `app/bsky: deep linking con @handles`

### 13.2 Polish

- [ ] `perf: boot time optimization`
- [ ] `perf: reducir flash size de runtime si excede target DL3`
- [ ] `perf: heap churn analysis durante Bluesky timeline scroll`
- [ ] `a11y: verificar contraste en los 3 temas`
- [ ] `docs: guías "Writing Your First Deck App"`
- [ ] `docs: API reference generada desde specs + @doc annotations`

### 13.3 Hitos

- **v0.9.0** — Bluesky arranca, se autentica, lee timeline
- **v0.9.5** — Bluesky feature-complete según annex
- **v0.9.9** — Bluesky estable sin memory leaks en 24h de uso

---

## 14. Fase 9 — v1.0.0

**Objetivo:** release oficial DL3 conformance-certified.

**Duración estimada:** 6–10 sesiones.

### 14.1 Tareas

- [ ] `conformance: completar batería DL3 completa (~200 tests acumulados)`
- [ ] `conformance: generar reporte oficial firmado con SHA del build`
- [ ] `perf: mediciones finales contra targets de §6.9 de 16-deck-levels.md`
- [ ] `docs: CHANGELOG.md con historial completo`
- [ ] `docs: MIGRATION.md para usuarios que upgrade desde pre-1.0`
- [ ] `components: publicar cada componente en IDF Component Registry`
  - `deck-runtime`
  - `deck-sdi` (headers only)
  - `deck-driver-esp32-nvs`
  - `deck-driver-esp32-fatfs`
  - `deck-driver-esp32-sqlite`
  - `deck-driver-esp32-wifi`
  - `deck-driver-esp32-http`
  - `deck-driver-esp32-mqtt`
  - `deck-driver-esp32-ble`
  - `deck-driver-esp32-ota`
  - `deck-bridge-lvgl`
  - `deck-platform-cyberdeck` (meta-package con declaración `deck.level: 3`)
  - `deck-conformance-suite`
- [ ] `release: tag v1.0.0`
- [ ] `release: GitHub Release con binarios + conformance report + release notes`

### 14.2 Criterio de release v1.0.0

- ✅ **200+ tests DL1+DL2+DL3 pasan en hardware CyberDeck**
- ✅ **Flash del runtime completo ≤ 520 KB**
- ✅ **Heap en idle ≤ 2 MB PSRAM**
- ✅ **Las 4 apps bundled (Launcher, Task Manager, Settings, Files) funcionan en Deck, C nativo eliminado**
- ✅ **Bluesky kitchen-sink funcional al 100%**
- ✅ **`system.info.deck_level` reporta `3`**
- ✅ **Conformance report JSON firmado publicado**
- ✅ **Componentes IDF publicados en registry**
- ✅ **Docs completos publicados**
- ✅ **24h uptime sin leaks ni crashes**

---

## 15. Estrategia de testing en hardware

### 15.1 Tipos de test

| Tipo | Dónde corre | Frecuencia |
|---|---|---|
| **Unit tests internos** (parser, lexer, value ops) | Harness UART al boot, detrás de CONFIG flag | Cada commit que toca runtime |
| **Integration tests** (loader → eval → SDI → observable effect) | Harness UART con `.deck` files en SD | Cada PR / cada commit significativo |
| **Conformance tests** (batería DL1/2/3) | Modo especial de boot que corre toda la suite | Antes de cada release `vX.Y.0` |
| **Hardware smoke tests** (flash, boot, visual verify) | Manual sobre la placa | Cada commit que afecta UI o boot |
| **Long-run tests** (24h, memoria, wifi reconnect) | Manual con board encendido | Antes de cada release estable |

### 15.2 Flash + monitor workflow

Estandarizado en CLAUDE.md:

```bash
# Flash + monitor
idf.py -p /dev/cu.usbmodem1101 flash monitor

# Monitor only (UART console CH343)
idf.py -p /dev/cu.usbmodem58A60705271 monitor
```

Nunca flashear por el puerto UART. Siempre native.

### 15.3 Captura automatizada de boot logs

Script `scripts/flash-verify.sh`:

1. Flash
2. Abre monitor en puerto UART
3. Captura 30 segundos de boot log
4. Compara contra `tests/boot.golden` (expected)
5. Diff si hay regresión

### 15.4 Hardware golden states

Cada release `vX.Y.0` incluye:

- `tests/boot.X.Y.0.golden` — captura del boot log esperado
- `tests/conformance.X.Y.0.json` — reporte de conformancia
- `tests/footprint.X.Y.0.txt` — tamaños flash/heap medidos

Estos archivos se commitean con el tag correspondiente.

### 15.5 Cuándo un test bloquea un release

- Conformance tests del nivel correspondiente deben estar al 100%
- Hardware smoke tests deben pasar
- Boot golden no puede haber regresionado
- Footprint no puede exceder targets

---

## 16. Gestión de versiones y tags

### 16.1 Fuentes de verdad

- `version.txt` en la raíz — versión del firmware
- `deck-lang/15-deck-versioning.md` §X.Y — versiones del spec
- Cada componente en `components/*/idf_component.yml` — versión del componente IDF

### 16.2 Semver aplicado al firmware

- **Major (X)** — cambia cuando se cubre un nivel completo de conformancia. Pre-1.0: 0; post: 1.
- **Minor (Y)** — cambia cuando se cierra una fase y hay release estable. Ej: 0.4.0, 0.6.0, 0.8.0.
- **Patch (Z)** — cambia en bug fixes, polish, docs. No introduce features nuevas.

### 16.3 Pre-release tags

- `v0.3.0-rc.1`, `v0.3.0-rc.2` cuando iteramos sobre conformance antes del release final
- `v0.5.0-alpha.N` para snapshots de desarrollo experimental

### 16.4 Branching

- `main` — siempre arrancable, siempre hardware-verificado
- Feature branches solo si la sesión de trabajo lo justifica (cambio grande, refactor)
- En solo-developer mode (tú+yo) preferimos commits directos atómicos

### 16.5 Changelog

`CHANGELOG.md` en la raíz, formato Keep-a-Changelog:

```
## [0.4.0] — 2026-MM-DD
### Added
- Deck runtime DL1 completo
- Conformance DL1 al 100%
### Changed
- …
### Fixed
- …
```

Cada release tag incluye su sección.

---

## 17. Estrategia de paralelización

### 17.1 Qué puede ir en paralelo

En una sesión dada, dos tracks pueden avanzar si:

- No modifican los mismos archivos
- Uno no depende de código aún-no-mergeado del otro

Ejemplos:

- Track A (SDI driver wrapping) y Track D (DVC renderer) son casi disjuntos: uno toca `components/deck_sdi/`, el otro `ui_engine/`.
- Track C (conformance tests) puede escribir tests *para features que ya existen* mientras Track B trabaja en features nuevas.
- Track E (apps en Deck) puede escribir apps para features DL1 ya estables mientras Track B trabaja en DL2.

### 17.2 Qué NO puede ir en paralelo

- Dos cambios que toquen el mismo archivo central (ej: `deck_runtime/eval.c` o `ui_engine/ui_activity.c`)
- Un track que requiera un nuevo API del runtime antes de que ese API exista

### 17.3 Comunicación entre tracks

- Specs de `deck-lang/*.md` son la fuente de contrato; ningún track cambia una spec sin coordinación
- Headers públicos (SDI, runtime API) se versionan; cambios breaking requieren bump y update de consumidores

### 17.4 Secuenciación obligatoria (dependencias duras)

- Track B (runtime skeleton) **antes que** Track B (eval)
- Track B (eval + dispatcher) **antes que** Track C (conformance tests de efectos)
- Track D (DVC decoder) **antes que** Track E (apps en Deck con UI)
- Track A (SDI drivers DL2) **antes que** Track B (capabilities DL2 en dispatcher)

### 17.5 Granularidad paralela típica

En una sesión de trabajo:

- 1–3 commits en Track B (core)
- 1–2 commits en Track A o D (soporte)
- 0–1 commit en Track C (tests nuevos)
- 0–1 commit en Track E (apps; sólo en fases tardías)

Si un commit no está listo (no flashea, no funciona), no se mezcla con otros; se resuelve primero.

---

## 18. Riesgos y mitigaciones

| Riesgo | Probabilidad | Impacto | Mitigación |
|---|---|---|---|
| Footprint DL1 excede 120 KB flash | Media | Alto | Budget desde día 1. Medir en cada commit a runtime. Si excede, simplificar features antes de llegar a conformance |
| Ordenar snapshot/restore mal rompe apps Deck en suspend | Media | Alto | Desarrollar snapshot detrás de flag. Extensive testing en Fase 6 con apps simples antes de Bluesky |
| LVGL DVC renderer es más lento que widgets directos | Media | Medio | Benchmarks desde DL2. Si es lento, cache de árboles renderizados + diffing |
| SQLite en DL3 excede presupuesto de flash | Baja | Medio | SQLite amalgation ya conocido (~500KB). Si excede, reducir features compiladas via flags |
| Una feature DL2 resulta requerir rearquitectura del runtime | Alta | Alto | Reviewar cada feature contra arquitectura actual en fase 0; si detectamos mismatch, revisar spec y/o runtime antes de implementar |
| Refactor de app framework al migrar de C a Deck rompe flow existente | Alta | Alto | Feature flags durante migración (Fase 7); C native fallback hasta que Deck esté 100% paridad |
| Component registry no acepta bundles con submódulos git | Media | Medio | Plan alternativo: una sola monorepo publicada como mega-componente |
| Hot reload en DL3 corrompe state de apps | Baja | Alto | Marcar como experimental; apps opcionan con `@experimental.hot_reload: true` |
| Bluesky API cambia during desarrollo | Media | Medio | Aislar integration layer tras `@use api_client`; update es local a app |
| Test suite crece más allá de lo que cabe en memoria de runtime | Baja | Medio | Tests externos — cada `.deck` carga, ejecuta, reporta, se descarga antes del siguiente |

---

## 19. Checklist maestro resumido

### Antes de v0.4.0 (DL1)

- [ ] SDI formal con drivers DL1
- [ ] Runtime parsea + evalúa + dispatcha DL1
- [ ] 50+ tests DL1 pasan
- [ ] Footprint bajo presupuesto DL1
- [ ] Hardware verificado

### Antes de v0.6.0 (DL2)

- [ ] Lenguaje DL2 completo
- [ ] App model DL2 completo
- [ ] SDI DL2 drivers
- [ ] Bridge DVC decoder funcional
- [ ] Task Manager + Lockscreen en Deck
- [ ] 100+ tests DL1+DL2 pasan
- [ ] Footprint bajo presupuesto DL2
- [ ] Hardware verificado con WiFi + HTTP reales

### Antes de v1.0.0 (DL3)

- [ ] Lenguaje DL3 completo
- [ ] App model DL3 completo
- [ ] SDI DL3 drivers completo
- [ ] Bridge UI DL3 completo
- [ ] Las 4 apps bundled en Deck
- [ ] Bluesky kitchen sink
- [ ] 200+ tests DL1+DL2+DL3 pasan
- [ ] Footprint bajo presupuesto DL3
- [ ] 24h estabilidad en hardware
- [ ] Conformance report firmado
- [ ] Componentes IDF publicados
- [ ] Docs completos publicados

---

## 20. Meta-checklist por sesión

Antes de cerrar una sesión, verifica:

- [ ] ¿Mi último commit compila limpio (`idf.py build`)?
- [ ] ¿Flasheé en la placa (`idf.py -p /dev/cu.usbmodem1101 flash monitor`)?
- [ ] ¿Verifiqué visualmente que la feature funciona (si toca UI)?
- [ ] ¿El UART no muestra errores ni warnings nuevos?
- [ ] ¿El commit sigue el estilo (docs:/sdi:/lang:/app:/runtime:/bridge:/conformance:/chore:/perf:/release:)?
- [ ] ¿Actualicé version.txt si corresponde?
- [ ] ¿Actualicé CHANGELOG.md si corresponde?
- [ ] ¿Hay un tag pendiente de crear para este release?
- [ ] ¿Hay follow-ups pendientes que deberían ser tareas nuevas?

---

**Fin.** Este plan es vivo — se revisa y actualiza al cerrar cada fase.
