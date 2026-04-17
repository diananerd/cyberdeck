# DEVELOPMENT-PLAN-DL1.md — Ruta a conformance DL1 Core

**Alcance:** llevar el firmware desde `v0.1.0` (C app framework legacy) hasta `v0.4.0` — un runtime Deck DL1 Core al 100% de conformancia, ejecutando apps `.deck` sobre el hardware CyberDeck, con toda la arquitectura legacy en C **removida**.

**Estrategia:** clean-slate. Se borra `app_framework/`, `apps/`, `sys_services/`, `ui_engine/`, `main/main.c`. Se conserva solamente `board/` (HAL) como foundation. El nuevo firmware se construye detrás del contrato SDI + runtime Deck, sin shims de compatibilidad con código existente.

**"Done" sigue siendo hardware-verificado.** Cada commit debe flashear y correr en la placa (flash limpio + arranque limpio + no regresión visible). Compilar no es done.

---

## Índice

1. [Deliverables de v0.4.0 DL1](#1-deliverables-de-v040-dl1)
2. [Scope mínimo de DL1 Core (recap)](#2-scope-mínimo-de-dl1-core-recap)
3. [Arquitectura objetivo](#3-arquitectura-objetivo)
4. [Plan de fases atómicas](#4-plan-de-fases-atómicas)
   - [F0 — Wipe + bootstrap](#f0--wipe--bootstrap)
   - [F1 — SDI DL1](#f1--sdi-dl1)
   - [F2 — Runtime: tipos + allocator + lexer](#f2--runtime-tipos--allocator--lexer)
   - [F3 — Runtime: parser](#f3--runtime-parser)
   - [F4 — Runtime: loader + checker](#f4--runtime-loader--checker)
   - [F5 — Runtime: evaluator](#f5--runtime-evaluator)
   - [F6 — Runtime: dispatcher + effects](#f6--runtime-dispatcher--effects)
   - [F7 — @machine + lifecycle](#f7--machine--lifecycle)
   - [F8 — Shell DL1 single-app](#f8--shell-dl1-single-app)
   - [F9 — Conformance suite DL1](#f9--conformance-suite-dl1)
   - [F10 — Release v0.4.0](#f10--release-v040)
5. [Criterios de release v0.4.0](#5-criterios-de-release-v040)
6. [Protocolo de verificación por commit](#6-protocolo-de-verificación-por-commit)
7. [Estructura de archivos objetivo](#7-estructura-de-archivos-objetivo)
8. [Riesgos y mitigaciones DL1](#8-riesgos-y-mitigaciones-dl1)

---

## 1. Deliverables de v0.4.0 DL1

1. **Runtime Deck DL1** funcional que ejecuta programas `.deck` desde SD o flash embebida.
2. **SDI 1.0 con 5 drivers DL1 obligatorios:** `storage.nvs`, `storage.fs` (read-only), `system.info`, `system.time` (monotonic), `system.shell` (minimal single-app).
3. **Conformance suite DL1** con ≥ 50 tests verdes en hardware.
4. **Footprint dentro de presupuesto:** ≤ 120 KB flash runtime, ≤ 64 KB heap idle, ≤ 256 KB total platform image (según `16-deck-levels.md` §4.9).
5. **Reporte JSON de conformancia** generado en SD al correr el harness.
6. **Firmware arranca con el shell DL1** y carga automáticamente `apps/hello/main.deck` de flash embedida.
7. **Todo el código C legacy (app_framework, apps, sys_services, ui_engine, main/main.c) eliminado del repo.**

**Fuera de scope v0.4.0:** UI (bridge DVC es opcional en DL1; lo diferimos a DL2), WiFi, HTTP, NVS write + read-only FS es suficiente para DL1, lockscreen, Task Manager, OTA, content bodies, @flow, @task, @stream, closures, records, effect annotations.

---

## 2. Scope mínimo de DL1 Core (recap)

Tomado de `16-deck-levels.md` §4.

**Lenguaje (`01-deck-lang.md`):**
- Léxico: UTF-8, comentarios, indent-sensitive, identifiers, literales
- Primitivos: `int`, `float`, `bool`, `str`, `byte`, `unit`
- Composite: `list T`, `map K V`, tuples
- `Optional T` (`T?`)
- `let` bindings
- Funciones puras
- Pattern matching básico (atoms + literales + wildcards) + exhaustividad
- Operadores aritméticos/comparación/lógicos
- String concat + interpolación `${…}`
- Módulos (archivo = módulo, imports)
- Builtins conversión: `str()`, `int()`, `float()`, `bool()`

**App model (`02-deck-app.md`):**
- `@app` header (id, name, version, edition, requires.deck_level)
- `@app.entry` → state target
- `@use` solo módulos locales (NO capabilities OS vía `@use`)
- `@machine` con ≥ 2 states + ≥ 1 transition
- `on enter:` / `on leave:` hooks
- `@on launch`

**NO en DL1:** `@use` de OS capabilities, `@requires` más allá de `edition` + `deck_level`, `@permissions`, `@config`, `@errors`, `@flow`, `@stream`, `@task`, `@migration`, `@handles`, `@assets`, `content =`.

**OS surface (`03-deck-os.md`, `05-deck-os-api.md`):**
- Puros: `math.*`, `text.*`, `bytes.*`, `log.*`
- `time.now`, `time.duration`, `time.to_iso`
- `system.info.*` (device_id, free_heap, app_id, deck_level, deck_os, runtime, edition)
- `nvs.get`, `nvs.set`, `nvs.delete`
- `fs.read`, `fs.exists`, `fs.list`
- `os.resume`, `os.suspend`, `os.terminate`
- Event bus: `HOME`, `BACK`, `os.app_launched`, `os.app_terminated`

**Runtime (`04`, `11`):**
- Lexer + LL(1) parser
- AST tree-walking evaluator
- Loader stages 0–9 (sin snapshot)
- Pattern-match switch compilation
- Effect dispatcher síncrono (sin continuations)
- Immutable + refcount
- String interning
- Un solo evaluator thread
- Crash handler (panic → safe unwind → reboot/restart)
- Stack cap 512 frames
- Heap hard-limit enforcement

**SDI obligatorio (`12`):**
- `storage.nvs`
- `storage.fs` (read-only aceptable)
- `system.info`
- `system.time` (monotonic, wall opcional)
- `system.shell` (minimal: push/pop de una app)

**Shell (`09`):**
- Launcher stub auto-lanza la app instalada
- Enrutado HOME / BACK
- Ciclo launch / suspend / resume / terminate
- Sin statusbar, navbar, lockscreen, notifications

**Footprint:**
| Segmento | Budget |
|---|---|
| Runtime code (flash) | ≤ 120 KB |
| Runtime data (heap) | ≤ 64 KB |
| Una app cargada | ≤ 48 KB |
| Total platform image | ≤ 256 KB flash, ≤ 128 KB SRAM |

---

## 3. Arquitectura objetivo

```
┌─────────────────────────────────────────────────────────┐
│  apps/ (.deck bundles)                                  │
│   · hello.deck (F2+)                                     │
│   · launcher.deck (single-app stub, F8)                  │
│   · conformance/*.deck (F9)                              │
├─────────────────────────────────────────────────────────┤
│  deck_shell/            DL1 single-app shell            │
│   · boot sequence, app loader, HOME/BACK routing        │
├─────────────────────────────────────────────────────────┤
│  deck_runtime/          Interpreter                     │
│   · lexer, parser, loader, evaluator, dispatcher        │
│   · value types, refcount allocator, string intern      │
├─────────────────────────────────────────────────────────┤
│  deck_sdi/              Service Driver Interface         │
│   · vtables + registry                                   │
│   · DL1 drivers: nvs, fs-ro, info, time, shell           │
├─────────────────────────────────────────────────────────┤
│  board/                 Hardware abstraction (preserved) │
│   · LCD, touch (not used at DL1), CH422G, RTC, SD, ...  │
└─────────────────────────────────────────────────────────┘
```

**No existen en el nuevo firmware:**
- `components/app_framework/` → borrado
- `components/apps/` → borrado
- `components/sys_services/` → borrado (fragmentos útiles migran directamente a drivers SDI)
- `components/ui_engine/` → borrado (UI es DL2+)
- `main/main.c` original → reemplazado

**`components/board/` se mantiene intacto.** LCD/touch quedan inactivos en DL1; la panel stays dark o muestra log mínimo del boot — a decidir en F8.

---

## 4. Plan de fases atómicas

Cada bullet = 1 commit. Commit atómico = compila + flashea + boot limpio. Los commits se agrupan por *milestone*; el milestone se tag-ea como `v0.X.Y` cuando cumple sus criterios.

---

### F0 — Wipe + bootstrap

**Milestone → v0.2.0-dev.0**

Objetivo: dejar el árbol en estado "boot limpio mínimo" sin legacy.

- `chore: bump version.txt a 0.2.0-dev`
- `build: eliminar components/app_framework/`
- `build: eliminar components/apps/`
- `build: eliminar components/sys_services/`
- `build: eliminar components/ui_engine/`
- `build: reemplazar main/main.c por bootstrap mínimo (init NVS + heap report por UART)`
- `build: actualizar main/CMakeLists.txt — solo REQUIRES board`
- `build: actualizar main/idf_component.yml quitando dependencias de componentes borrados`
- `build: crear componente vacío components/deck_sdi/ (CMakeLists + include/ vacío)`
- `build: crear componente vacío components/deck_runtime/`
- `build: crear componente vacío components/deck_shell/`
- `build: limpiar sdkconfig.defaults de flags de componentes borrados`
- `docs: añadir ARCHITECTURE.md con diagrama de capas DL1`

**Criterio de salida F0:** flash limpio, boot imprime `CyberDeck DL1 bootstrap — free heap: NNNN bytes` por UART y entra en loop de idle. `idf.py size` reporta baseline nuevo (típicamente < 200 KB — el código custom desaparece).

---

### F1 — SDI DL1

**Milestone → v0.2.0**

Objetivo: los 5 drivers obligatorios DL1 existen, están registrados al boot, y un comando de debug por UART los enumera.

**F1.1 — Contrato SDI**

- `sdi: include/deck_sdi.h — enum deck_sdi_driver_id, result type deck_sdi_err_t`
- `sdi: include/deck_sdi_registry.h — tabla estática de drivers, lookup por nombre/id`
- `sdi: src/deck_sdi_registry.c — deck_sdi_register, deck_sdi_lookup, deck_sdi_list`

**F1.2 — Driver storage.nvs**

- `sdi: include/drivers/deck_sdi_nvs.h — vtable (get, set, del, iter)`
- `sdi: src/drivers/deck_sdi_nvs_esp32.c — impl sobre nvs_flash_t`
- `sdi: self-test — round-trip set/get/del de una clave int y string`

**F1.3 — Driver storage.fs (read-only)**

- `sdi: include/drivers/deck_sdi_fs.h — vtable (read, exists, list)`
- `sdi: src/drivers/deck_sdi_fs_spiffs.c — impl sobre SPIFFS de app partition`
- `sdi: self-test — leer un archivo embebido y listar directorio`

*Nota:* DL1 acepta read-only, así que evitamos montar SD aquí. Los `.deck` bundled irán en una partición SPIFFS de flash. SD llega en DL2.

**F1.4 — Driver system.info**

- `sdi: include/drivers/deck_sdi_info.h — vtable (device_id, free_heap, deck_level, deck_os, runtime, edition)`
- `sdi: src/drivers/deck_sdi_info.c — device_id desde MAC, heap_caps_get_free_size`
- `sdi: self-test — imprimir device_id + heap por UART`

**F1.5 — Driver system.time**

- `sdi: include/drivers/deck_sdi_time.h — vtable (monotonic_ms, wall_epoch_s opcional)`
- `sdi: src/drivers/deck_sdi_time.c — esp_timer_get_time + RTC read vía board`
- `sdi: self-test — monotonic diff entre dos lecturas separadas por vTaskDelay`

**F1.6 — Driver system.shell (stub)**

- `sdi: include/drivers/deck_sdi_shell.h — vtable (launch, terminate, current_app)`
- `sdi: src/drivers/deck_sdi_shell_stub.c — stub que registra "no app loaded"`

**F1.7 — Bootstrap main.c enumera drivers**

- `app: main.c llama deck_sdi_registry_init() + imprime por UART la lista: "deck_sdi: 5 drivers registered: storage.nvs, storage.fs, system.info, system.time, system.shell"`
- `app: main.c ejecuta los self-tests al boot bajo flag CONFIG_DECK_SDI_SELFTEST`

**Criterio de salida F1:** boot imprime los 5 drivers + self-tests pasan. Flash size documentado. Tag `v0.2.0`.

---

### F2 — Runtime: tipos + allocator + lexer

**Milestone → v0.2.1 (primer programa lexeado end-to-end)**

**F2.1 — Tipos base**

- `runtime: include/deck_types.h — deck_value_t (tagged union: int, float, bool, str, atom, unit, list, map, tuple, optional)`
- `runtime: include/deck_error.h — deck_err_t codes completos del spec §10 + tabla mensajes`

**F2.2 — Allocator**

- `runtime: src/deck_alloc.c — refcount retain/release sobre heap_caps_malloc(MALLOC_CAP_INTERNAL)`
- `runtime: límite hard-limit enforcement — deck_alloc_set_limit(bytes), deck_alloc_used(), panic si excede`
- `runtime: test — 1000 retain/release ciclos, sin leak`

**F2.3 — String interning**

- `runtime: src/deck_intern.c — hash table de strings inmutables, dedupe por hash`
- `runtime: test — interning idempotente + comparación por puntero`

**F2.4 — Lexer**

- `runtime: include/deck_lexer.h — deck_token_t, deck_lexer_init/next/free`
- `runtime: src/deck_lexer.c — literales int/float`
- `runtime: src/deck_lexer.c — bool + unit + atoms (`:foo`)`
- `runtime: src/deck_lexer.c — strings + escapes + interpolación `${expr}``
- `runtime: src/deck_lexer.c — identificadores + keywords`
- `runtime: src/deck_lexer.c — operadores (+ - * / < <= > >= == != && || !)`
- `runtime: src/deck_lexer.c — indent/dedent (WS-sensitive) siguiendo regla de `01-deck-lang.md` §2`
- `runtime: src/deck_lexer.c — comentarios `#` hasta fin de línea`

**F2.5 — Harness de tests lexer**

- `runtime: tests/lexer_cases.h — tabla de (input, expected_tokens) con ≥30 casos`
- `runtime: tests/run_lexer_tests.c — corre la tabla al boot bajo flag y reporta pass/fail por UART`

**Criterio de salida F2:** los 30 tests de lexer pasan en hardware. Tag `v0.2.1`.

---

### F3 — Runtime: parser

**Milestone → v0.2.2**

- `runtime: include/deck_ast.h — nodos AST para DL1 (Literal, Ident, BinOp, UnaryOp, Let, Lambda-no, Match, App, MachineDecl, StateDecl, TransitionDecl, OnEnter, OnLeave, ImportDecl)`
- `runtime: src/deck_parser.c — primary expressions (literales, idents, paréntesis)`
- `runtime: src/deck_parser.c — precedencia de operadores (Pratt o shunting)`
- `runtime: src/deck_parser.c — let bindings`
- `runtime: src/deck_parser.c — funciones puras (declaración + llamada)`
- `runtime: src/deck_parser.c — match con patrones DL1 (atom, literal, wildcard)`
- `runtime: src/deck_parser.c — `@app` header completo con fields`
- `runtime: src/deck_parser.c — `@machine` (states + on enter/leave + transitions)`
- `runtime: src/deck_parser.c — imports `@use module.name``
- `runtime: src/deck_parser.c — errores con línea/columna + mensaje estructurado`
- `runtime: tests/parser_cases.h — 40 programas Deck válidos + ≥10 inválidos con error esperado`
- `runtime: tests/run_parser_tests.c — imprime AST en s-expr format para comparación golden`
- `runtime: tests/golden/*.ast — AST goldens generados y committeados`

**Criterio de salida F3:** 40 tests parser verdes. Golden diffs reproducibles. Tag `v0.2.2`.

---

### F4 — Runtime: loader + checker

**Milestone → v0.2.3**

Loader de 10 stages según `04-deck-runtime.md` + `11-deck-implementation.md`. DL1 no tiene snapshot, así que stage 7 queda reservado/no-op.

- `loader: stage 0 — lexical pass wrapper`
- `loader: stage 1 — parse + AST construction wrapper`
- `loader: stage 2 — resolve symbols (module-local solo)`
- `loader: stage 3 — type check DL1 subset (primitivos + composites + Optional)`
- `loader: stage 4 — capability bind (solo permitidas DL1: math/text/bytes/log/time/nvs/fs/system.info/os)`
- `loader: stage 5 — pattern-match exhaustiveness check (atoms + literales + wildcard catch-all)`
- `loader: stage 6 — compat check (edition=2026, deck_level ≥ 1, deck_os ≥ 1, runtime semver)`
- `loader: stage 7 — reserved (no-op en DL1)`
- `loader: stage 8 — freeze module (refcount +1 de la imagen, readonly)`
- `loader: stage 9 — linkage a runtime + activa `@app.entry``
- `loader: errores estructurados (E_LEVEL_BELOW_REQUIRED, E_LEVEL_UNKNOWN, E_LEVEL_INCONSISTENT, E_CAPABILITY_MISSING, E_TYPE_MISMATCH, E_PATTERN_NOT_EXHAUSTIVE, …)`
- `loader: tests — programas que deben fallar en cada stage, con error esperado`

**Criterio de salida F4:** loader rechaza apps mal formadas con el error correcto y acepta apps DL1 válidas. Tag `v0.2.3`.

---

### F5 — Runtime: evaluator

**Milestone → v0.3.0 — primer `log.info` desde un `.deck`**

- `eval: value stack (512 deep) + frame stack`
- `eval: evaluar literales y atoms`
- `eval: evaluar let bindings con scope anidado`
- `eval: evaluar llamadas a funciones puras`
- `eval: evaluar operadores aritméticos`
- `eval: evaluar comparaciones + lógicos short-circuit`
- `eval: evaluar string concat + interpolación`
- `eval: evaluar match (patrones DL1)`
- `eval: evaluar módulo imports (lazy eval de módulos)`
- `eval: invocar effect dispatcher (stub por ahora)`
- `eval: crash handler longjmp al safe point + reporte por UART`
- `eval: tests — ejecutar 20 programas Deck que hacen solo cómputo puro + match, verificar output`

**Hito v0.3.0:** el programa `hello.deck`:
```
@app
  name: "Hello"
  id: "sys.hello"
  version: "1.0.0"
  edition: 2026
  requires:
    deck_level: 1

@on launch:
  log.info("Hello from Deck DL1")
```
imprime por UART al boot. **Ese es el momento fundacional del proyecto Deck en esta placa.**

---

### F6 — Runtime: dispatcher + effects

**Milestone → v0.3.1**

- `dispatcher: routing por capability.name → SDI driver`
- `dispatcher: marshaling deck_value_t ↔ tipos C por capability`
- `dispatcher: integrar builtins puros (math.add/sub/..., text.upper/lower/split, bytes, log.info/warn/error)`
- `dispatcher: integrar time.now (monotonic ms + wall epoch si disponible)`
- `dispatcher: integrar time.duration (resta de dos tiempos), time.to_iso`
- `dispatcher: integrar system.info.* (device_id, free_heap, deck_level=1, runtime, edition)`
- `dispatcher: integrar nvs.get/set/delete`
- `dispatcher: integrar fs.read/exists/list (read-only)`
- `dispatcher: integrar os.resume/suspend/terminate (sobre shell SDI)`
- `dispatcher: tests — programa que usa cada capability, verifica resultado por UART`

**Criterio de salida F6:** todas las capabilities DL1 reachable desde código Deck. Tag `v0.3.1`.

---

### F7 — @machine + lifecycle

**Milestone → v0.3.2**

- `machine: instanciar máquina desde AST (states + initial state)`
- `machine: evaluar `on enter` / `on leave` hooks`
- `machine: evaluar transiciones — `send(:event)` + `transition :state``
- `machine: lifecycle launch → idle → terminate según `02-deck-app.md`"
- `machine: event bus core — emit HOME/BACK/os.app_launched/os.app_terminated`
- `machine: tests — máquina de 2 estados con 2 transiciones, verificar traza de hooks`

**Criterio de salida F7:** máquina arbitraria DL1 ejecuta su ciclo de vida completo. Tag `v0.3.2`.

---

### F8 — Shell DL1 single-app

**Milestone → v0.3.3**

DL1 es single-app. El shell carga **una** app de la partición SPIFFS al boot.

- `shell: include/deck_shell.h — deck_shell_init, deck_shell_launch(app_id), deck_shell_terminate()`
- `shell: src/deck_shell_boot.c — discover `/spiffs/apps/<id>/manifest.deck` + cargar main.deck`
- `shell: enrutar eventos HOME / BACK al runtime (source: GPIO boot button si está, o UART char)`
- `shell: integrar con SDI system.shell (current_app, launch, terminate)`
- `shell: main.c queda como thin wrapper: init board → init SDI → init runtime → deck_shell_init → launch default`
- `shell: embeber `/spiffs/apps/hello/manifest.deck` + `main.deck` en la imagen vía SPIFFS partition`
- `shell: test manual — reboot lanza hello.deck automáticamente + BACK termina app + relanza`

**Criterio de salida F8:** al boot, hello Deck arranca sin intervención, corre su ciclo, puede terminar y relanzar. Tag `v0.3.3`.

---

### F9 — Conformance suite DL1

**Milestone → v0.3.9 → v0.4.0 RC**

Harness + batería de ~50 tests cubriendo todo el surface DL1.

**F9.1 — Harness**

- `conformance: tools/conformance_harness/ — utilidad host-side opcional (Python) que orquesta`
- `conformance: components/deck_conformance/ — componente que expone deck_conformance_run()`
- `conformance: deck_conformance_run lee `/spiffs/conformance/index.json`, corre cada `.deck`, registra pass/fail + mensaje`
- `conformance: reporte JSON en `/spiffs/reports/dl1-<timestamp>.json``

**F9.2 — Tests por área**

Cada bullet = 1 commit con 3–8 `.deck` tests + expected outputs.

- `conformance: lang — literales y tipos primitivos`
- `conformance: lang — arithmetic + comparison + logical`
- `conformance: lang — let + scoping + shadowing`
- `conformance: lang — funciones puras + recursión (DL1 no requiere tail call, pero sí soporte básico)`
- `conformance: lang — strings + concat + interpolación`
- `conformance: lang — pattern matching (atoms, literales, wildcards, exhaustividad)`
- `conformance: lang — list + map + tuple builtins mínimos`
- `conformance: lang — Optional + `?` sintax`
- `conformance: lang — módulos + imports locales`
- `conformance: app — @app header válido`
- `conformance: app — @machine 2 estados, 1 transición, hooks`
- `conformance: app — @on launch emisión de log`
- `conformance: os — math.* builtins`
- `conformance: os — text.* builtins`
- `conformance: os — bytes.* builtins`
- `conformance: os — log levels`
- `conformance: os — time.now + time.duration`
- `conformance: os — system.info.* completo`
- `conformance: os — nvs.set/get/delete round-trip`
- `conformance: os — fs.read + fs.exists + fs.list`
- `conformance: os — os.suspend/resume/terminate`
- `conformance: errors — E_LEVEL_BELOW_REQUIRED`
- `conformance: errors — E_LEVEL_UNKNOWN`
- `conformance: errors — E_LEVEL_INCONSISTENT`
- `conformance: errors — E_CAPABILITY_MISSING`
- `conformance: errors — E_TYPE_MISMATCH`
- `conformance: errors — E_PATTERN_NOT_EXHAUSTIVE`
- `conformance: stress — loop de 10 000 iteraciones sin leak`
- `conformance: stress — 1 000 allocs + releases (no leak via refcount)`
- `conformance: stress — recursión a 400 frames (bajo el cap de 512)`
- `conformance: stress — string interning de 500 strings únicos`
- `conformance: memory — heap idle ≤ 64 KB después de cargar hello`

**F9.3 — Iteración de bugfixes**

- `fix: bugs descubiertos por conformance (1 commit por fix, referenciando el test)`

**Criterio de salida F9:** 100% de los tests pasan en hardware. Reporte JSON committeado en repo como evidence snapshot. Tag `v0.3.9` RC.

---

### F10 — Release v0.4.0

**Milestone → v0.4.0 — DL1 conformance 100%**

- `perf: `idf.py size-components` — verificar ≤ 120 KB flash runtime`
- `perf: boot-time heap report — verificar ≤ 64 KB idle`
- `perf: si flash o heap excede budget — optimizaciones: LV_ flags off, strip unused, inline críticos`
- `docs: CHANGELOG.md — entry v0.4.0 con lista de features DL1 completos`
- `docs: README.md — actualizar "Estado actual: DL1 conformance certified"`
- `docs: ARCHITECTURE.md — actualizar con diagrama final DL1`
- `chore: version.txt → 0.4.0`
- `release: git tag v0.4.0`

---

## 5. Criterios de release v0.4.0

Checklist obligatorio antes de tag:

- ✅ `idf.py build` limpio en la placa CyberDeck, sin warnings (-Werror activo)
- ✅ `idf.py -p /dev/cu.usbmodem1101 flash monitor` arranca limpio — sin panic, brownout, wdt
- ✅ Runtime flash ≤ 120 KB (medido con `idf.py size-components`)
- ✅ Runtime heap idle ≤ 64 KB (medido por `heap_caps_get_free_size` post-boot - baseline)
- ✅ `system.info.deck_level` responde `1` correctamente
- ✅ Una app con `requires.deck_level: 2` obtiene `E_LEVEL_BELOW_REQUIRED` sin crash
- ✅ Conformance suite DL1 — 50+ tests verdes en hardware
- ✅ Reporte JSON dl1 committed en `tests/conformance/reports/`
- ✅ hello.deck arranca automáticamente al boot y corre su lifecycle
- ✅ Todo el código C legacy ausente del repo (grep de `app_framework`, `ui_engine`, `sys_services` → 0 matches en componentes activos)
- ✅ README + CHANGELOG actualizados

Si cualquier criterio falla → se arregla antes del tag o se marca como `v0.4.0-rcN`.

---

## 6. Protocolo de verificación por commit

Cada commit atómico pasa por:

```
1. idf.py build                                  # build limpio, sin warnings
2. idf.py -p /dev/cu.usbmodem1101 flash monitor  # flash + captura boot
3. Verificar output esperado por UART             # según feature del commit
4. (si cambia lifecycle) reboot + re-verificar    # catch de state estancado
5. git add -p + commit con mensaje en español     # estilo del repo
```

**Skip manual de flash permitido solo para:** commits que son 100% docs (`.md`), reorganización de headers sin cambio de símbolos exportados, o cambios que apagan código con `#if 0` (no ejecutado).

**NO permitido saltarse hardware verification para:** cualquier commit que toque runtime, SDI, shell, drivers, main.c, sdkconfig, particiones.

---

## 7. Estructura de archivos objetivo

Al finalizar v0.4.0, el árbol se ve así:

```
cyberdeck/
├── main/
│   ├── CMakeLists.txt               (requires: board deck_sdi deck_runtime deck_shell)
│   ├── Kconfig.projbuild             (flags: CONFIG_DECK_SDI_SELFTEST, CONFIG_DECK_CONFORMANCE_BUILD)
│   ├── idf_component.yml
│   └── main.c                        (thin bootstrap, <150 líneas)
├── components/
│   ├── board/                        (HAL intacto, unchanged)
│   ├── deck_sdi/
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   ├── deck_sdi.h
│   │   │   ├── deck_sdi_registry.h
│   │   │   └── drivers/
│   │   │       ├── deck_sdi_nvs.h
│   │   │       ├── deck_sdi_fs.h
│   │   │       ├── deck_sdi_info.h
│   │   │       ├── deck_sdi_time.h
│   │   │       └── deck_sdi_shell.h
│   │   └── src/
│   │       ├── deck_sdi_registry.c
│   │       └── drivers/
│   │           ├── deck_sdi_nvs_esp32.c
│   │           ├── deck_sdi_fs_spiffs.c
│   │           ├── deck_sdi_info.c
│   │           ├── deck_sdi_time.c
│   │           └── deck_sdi_shell_stub.c
│   ├── deck_runtime/
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   ├── deck_types.h
│   │   │   ├── deck_error.h
│   │   │   ├── deck_lexer.h
│   │   │   ├── deck_ast.h
│   │   │   ├── deck_parser.h
│   │   │   ├── deck_loader.h
│   │   │   ├── deck_eval.h
│   │   │   ├── deck_dispatcher.h
│   │   │   └── deck_machine.h
│   │   ├── src/
│   │   │   ├── deck_alloc.c
│   │   │   ├── deck_intern.c
│   │   │   ├── deck_lexer.c
│   │   │   ├── deck_parser.c
│   │   │   ├── deck_loader.c
│   │   │   ├── deck_eval.c
│   │   │   ├── deck_dispatcher.c
│   │   │   └── deck_machine.c
│   │   └── tests/
│   │       ├── lexer_cases.h
│   │       ├── parser_cases.h
│   │       ├── run_lexer_tests.c
│   │       ├── run_parser_tests.c
│   │       └── golden/*.ast
│   ├── deck_shell/
│   │   ├── CMakeLists.txt
│   │   ├── include/deck_shell.h
│   │   └── src/deck_shell_boot.c
│   └── deck_conformance/
│       ├── CMakeLists.txt
│       ├── include/deck_conformance.h
│       └── src/deck_conformance_run.c
├── apps/                              (bundled .deck, empaquetados en SPIFFS)
│   ├── hello/
│   │   ├── manifest.deck
│   │   └── main.deck
│   └── conformance/
│       ├── index.json
│       └── tests/*.deck
├── partitions.csv                     (app + spiffs + nvs)
├── sdkconfig.defaults
├── CLAUDE.md
├── DEVELOPMENT-PLAN.md                (general v0.1 → v1.0)
├── DEVELOPMENT-PLAN-DL1.md            (este documento)
├── ARCHITECTURE.md                    (nuevo)
├── CHANGELOG.md
├── GROUND-STATE.md                    (conservar como referencia histórica)
└── deck-lang/                         (specs, intactos)
```

---

## 8. Riesgos y mitigaciones DL1

| Riesgo | Mitigación |
|---|---|
| **Parser indent-sensitive es sutil** — Python-like WS puede tener edge cases | F3 aparta ≥ 40 tests parser antes de pasar a loader. Si surge un caso raro en F4+, volver a F3 antes de avanzar. |
| **Footprint excede ≤ 120 KB flash** | Medir temprano — al cierre de F5 ya hay número. Si excede, F6 toca optimizaciones antes de añadir más features. No diferir al final. |
| **Refcount cycles** si hay referencias cíclicas | DL1 values son puramente inmutables + sin closures con captura → cycles imposibles por construcción. Documentar la invariante en `deck_alloc.c`. |
| **Crash handler vs FreeRTOS** — longjmp desde task interrupts | Usar safe point solamente en el evaluator task, nunca desde ISR. Documentar ABI. |
| **Borrar legacy causa regresión de features que creíamos DL2+** | Antes de borrar en F0, crear `GROUND-STATE.md` snapshot para referencia. El snapshot ya existe → validar que nada DL1 dependía del legacy. |
| **Apps `.deck` embebidas en SPIFFS ocupan flash base** | Hello app objetivo ~2–5 KB. Dejar headroom en partition table (partitions.csv). |
| **Tests conformance drift** — un fix rompe otro test | Correr la batería completa después de cada bugfix F9.3, no solo el test que falla. |
| **Tiempo total hasta v0.4.0** | Estimado ~60–100 sesiones atómicas. Cada `vX.Y.Z` intermedio es release interna. El tag `v0.4.0` se hace solo cuando todo § 5 está verde. |

---

**Próximo paso concreto:** ejecutar F0 commit por commit, empezando por `chore: bump version.txt a 0.2.0-dev`, seguido del wipe ordenado de componentes legacy.
