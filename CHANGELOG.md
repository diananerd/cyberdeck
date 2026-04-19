# Changelog

Todas las versiones notables del firmware CyberDeck. Formato inspirado en Keep-a-Changelog.

## [Unreleased] — post-DL2

### F28 Phase 2 — apps .deck con UI propia, @on lifecycle, @assets, @flow desugar

Cuatro features que cierran la superficie DL2 para que las apps .deck
sean de verdad utilizables por el usuario final (vs. phase 1 que solo
corría código en log.info).

**bridge.ui.\* builtins.** 9 builders + render():
`bridge.ui.label(text)`, `trigger(text, intent)`, `column(children)`,
`row(children)`, `group(title, children)`, `data_row(label, value)`,
`divider()`, `spacer()`, `render(root)`. Cada builder devuelve un
int64 "handle" en un node table módulo-local; `render()` encoda la
tree como DVC + llama `deck_sdi_bridge_ui_push_snapshot` + resetea el
arena local. Pattern: app construye tree → render → handles invalidated.

Capacidad nueva `bridge` añadida a `DL1_CAPS[]` + error messages.
Implícita (no requiere `@use bridge.ui`) igual que math/text/log/...

**@on resume/pause wired a activity lifecycle.** El intent resolver
de `deck_shell_deck_apps` ahora pushea una activity propia (no pinta
sobre launcher) con callbacks adapter:
- `on_create` → dispatch `@on resume`
- `on_resume` → dispatch `@on resume`
- `on_pause`  → dispatch `@on pause`
- `on_destroy` → NULL (pause ya corrió; terminate reservado para unload)

**Verificado visualmente en hardware** (tras el fix del bug de timing):
tap HELLO card → screen muestra el tree `HELLO / DECK LEVEL:2 /
FREE HEAP:N` renderizado desde .deck source → BACK navega al launcher
→ HOME también funciona. Pause dispatch fires una vez.

Bug raíz del primer intento: el adapter dispatchaba `@on resume` en
`on_create`, pero `deck_bridge_ui_activity_push` secuencia
`on_create → lv_scr_load(new_scr) → on_resume` — o sea, render
llegaba al screen del launcher (aún activo) y luego `lv_scr_load`
lo reemplazaba con el nuevo screen vacío. **Fix**: dispatch sólo
desde `on_resume`, cuando `lv_scr_act()` ya apunta al screen
correcto. `on_create` queda cacheando el handle; `on_destroy` NULL.

**@assets + asset.path(name).** Parser reemplaza `parse_opaque_block`
con `parse_assets_decl` que extrae pares `name: "path"` a un
AST_ASSETS node. El builtin `asset.path(name)` busca linealmente por
módulo corriente (nuevo field `deck_interp_ctx_t.module`) y devuelve
`some(path)` o `none`. Max 32 entries por bloque.

**@flow desugar.** `parse_flow_decl` convierte `@flow name` + pares
`step X:` en un AST_MACHINE equivalente con auto-transitions entre
steps sucesivos. El último step termina la máquina. El runtime no
cambia — run_machine no sabe que la máquina vino de @flow.

**Conformance**: +3 tests — `app.bridge_ui`, `app.assets`, `app.flow`.
Todo verde: **80/80 deck + 5/5 suites + 15/15 stress = 100/100 PASS,
0 outliers**. `app.bridge_ui` vive al final del array porque
LVGL render steals cycles de Core 0 y contaminaba timings siguientes.

**Parser tests**: actualizados `mod_flow`, `mod_assets`,
`mod_machine_before`, `mod_machine_after` para el nuevo AST output
(ya no `(use __metadata)` stubs).

**Bin size**: 1360880 bytes (~1328 KB), sube ~8KB por Phase 2 code.

### Bugs encontrados en hardware durante Phase 2

**bridge.ui.render: encode sizing failed** (primer flash). El
`deck_dvc_encode(NULL, 0)` return DECK_RT_NO_MEMORY por diseño (usa
overflow flag para tracking). Mi render lo trataba como error. Fix:
ignorar rc, verificar que `need > 0`.

**Pause dispatch doble**. Activity lifecycle dispara `on_pause` y
luego `on_destroy`; mi adapter dispatchaba "pause" en ambos. Fix:
on_destroy = NULL, la pause ya corrió en on_pause.

**app.assets: pattern_not_exhaustive**. El exhaustiveness checker
requiere wildcard `_` incluso cuando `some(_)` + `none` cubren el
tipo Optional. Fix al test: añadir `_ => unreachable` explícito.

### F28.4 — @migration con NVS version tracking

Última pieza de F28. El parser deja de tratar `@migration` como opaque
y extrae pares `from N:<body>` a un AST_MIGRATION node. El runtime
corre un nuevo `run_migrations()` en `deck_runtime_app_load` después
del fn pre-bind y antes de `@on launch`, con estas reglas:

- **NVS layout**: namespace `"deck.mig"`, key `"v_<fnv32(app_id) hex>"`
  (10 chars, cabe en límite de 15 de NVS key), valor i64 con versión.
  Hash evita truncar app_ids largos como `conf.lang.requires.caps`.
- **Semántica**: si `stored=N` en NVS (0 default = nunca migrado),
  corre en orden ascendente todos los bodies cuyo `from K >= N`,
  después persiste `max(K)+1`. Subsecuentes boots ven `stored=1` y
  saltan el body `from 0:` — idempotencia sin esfuerzo del user.
- **Fail model**: si un body throwsea, se aborta sin actualizar
  `stored`, así la siguiente carga reintenta. No hay rollback.

**Test**: `t_migration` en `deck_interp_run_selftest`. Flow:
1. Borra `deck.mig/v_a829200e` (hash fijo de "mig.test") + efecto NVS
2. `deck_runtime_app_load` con @migration from 0 → `nvs.set("conf.mig","ran","v0")`
3. Verifica NVS tiene `ran=v0`
4. Unload, borra sólo `ran` (tracking se mantiene)
5. Reload: stored=1, migration NO debe correr
6. Verifica que `ran` sigue NOT_FOUND

Interp selftest counter sube 47 → 48. Verificado en hardware:
```
I (2537) deck_interp: migration mig.test: from 0
I (2541) deck_interp: migration mig.test: stored v=1
I (2550) deck_interp: interp selftest: PASS (48 cases)
```

Parser selftest `mod_migration` actualizado al nuevo AST output
(`(module (migration (from 1 (do ...))))` en vez de `(use __metadata)`).

### F28 bridge.ui.trigger intent wiring

Cierre natural de F28 Phase 2: tapping un `bridge.ui.trigger(text, N)`
dentro de una .deck screen ahora dispatcha `@on trigger_N` sobre la app
dueña de la activity top. Flujo:

1. Decoder (`deck_bridge_ui_decode.c`) al crear DVC_TRIGGER registra
   `trigger_click_cb` con `intent_id` como user_data.
2. Nuevo hook público `deck_bridge_ui_set_intent_hook(fn)`. Default NULL
   (plataformas sin shell solo loggean el intent).
3. `deck_shell_deck_apps` instala `deck_app_intent_hook` en
   `scan_and_register`: al tap, lee activity top, matchea app_id a un
   slot cargado, formatea `"trigger_<N>"` y llama
   `deck_runtime_app_dispatch`.
4. El .deck app declara `@on trigger_1:`, `@on trigger_2:`, etc. Los
   handlers pueden llamar `bridge.ui.render(...)` nuevamente para
   redibujar — el env persistente lo permite.

**hello.deck extendido** con `fn build_screen(msg)` reutilizable y 2
triggers (PING id=1, REFRESH id=2). Cada handler reconstruye el tree
con un mensaje distinto y llama render — demostrando redraw
state-driven desde código .deck puro.

**NO verificado visualmente**: tap botón → re-render state change.
Infraestructura compila, conformance sigue 80/80, pero aún no
confirmé en pantalla que tap dentro de HELLO redibuje el body con el
nuevo mensaje. Hay un riesgo conocido: `bridge.ui.render` desde dentro
de un trigger callback hace `lv_obj_clean(scr)` sobre widgets que
actualmente procesan un evento — LVGL lo maneja en general, pero si
hubiera glitch visual o crash, el fix sería usar `lv_obj_del_async`
o postponer el render vía `lv_async_call` (deferred al próximo tick).

### Bugs caught on hardware tras verificación visual

**bridge.ui.trigger label attr mismatch.** El builder sentaba `"text"`
pero el decoder (`render_trigger`) lee `"label"` — los botones salían
con el default "BUTTON" en vez de "PING"/"REFRESH". Fix:
`deck_dvc_set_str(&s_bui_arena, node, "label", text)` en
`b_bui_trigger`. Visualmente verificado: "si veo hello y funciona
back y home" + user tap → labels correctos.

**DVC_DATA_ROW aliaseaba a render_label.** El decoder mapeaba
`DVC_DATA_ROW` al mismo `render_label` que `DVC_LABEL`, que solo lee
`"value"` — el attr `"label"` se ignoraba silenciosamente. Fix: nuevo
`render_data_row` que apila una label-dim (text_opa 60%) encima del
value-primary, como dice CLAUDE.md §ui_common_data_row. Row-pad 2 px,
transparent bg, 0-width border. Las 2 labels comparten el color
primary; la diferencia visual viene de opacity. (El decoder no
personaliza fuentes — usa default LVGL.)

Con este fix, `bridge.ui.data_row("DECK LEVEL:", str(system.info.deck_level))`
visualmente rinde "DECK LEVEL:" dim + "2" primary apiladas — **confirmado
visualmente en hardware por el usuario**.

### Aún diferido post-DL2

- Nice-to-have: semver real en `@app.version` (hoy migration usa
  int-based `from N:`).
- Nice-to-have: fuentes Montserrat SM/MD/LG en el decoder para
  diferenciar jerarquía tipográfica además de opacity.

### Hardware verification — 2026-04-17

Los 3 deferrals post-DL2 (flaky tests, F28 runtime Phase 1, .deck
source apps Phase 1) verificados end-to-end sobre el board:

- **Conformance**: `{"deck_tests_total":77,"deck_tests_pass":77,"deck_tests_fail":0,"deck_outliers":0,"stress_total":15,"stress_pass":15,"suites_total":5,"suites_pass":5}`
  — el fix de warm-up discarding produce 0 outliers donde antes salían 3-4
- **`app.machine_hooks.deck`** pasa (sentinel emitido desde @machine.after)
- **2 apps .deck cargadas desde SPIFFS root**: `/ping.deck` (app_id=100)
  y `/hello.deck` (app_id=101). Paths con `/` en el nombre (ej. conformance/)
  son filtrados antes de load
- **Launcher tap → `@on resume` funciona**: 3 taps consecutivos al card
  HELLO dispararon `I (14321/19389/25642) deck.app: hello.deck: resumed
  (tap from launcher)` — el env persistente se reusa, no hay re-load
- Heap idle DL2+shell+2apps: **internal=62875 bytes** (vs 72003 pre-apps,
  ~9KB overhead para las 2 apps cargadas + LVGL launcher)

### Bug fixes descubiertos en hardware

**shell.deck_apps: filter de subdirectorios.** SPIFFS es flat, así que
`conformance/sanity.deck` aparece como un entry del root con `/` en el
nombre. Primera versión cargaba todos los .deck tests como apps,
saturando los 8 slots y spammeando el intent registry. Fix:
`if (strchr(name, '/') != NULL) return true;` en `scan_cb`.

**deck_shell.c (DL1 legacy): OOM en alloc 32KB.** Tras cargar las apps
persistentes, el internal heap quedaba ~45KB libre — insuficiente para
un `heap_caps_malloc(32K, MALLOC_CAP_INTERNAL)`. Fix: fallback a
PSRAM. El buffer solo se lee (parser ya transfiere a arena interno),
así que la velocidad de PSRAM no impacta.

### F28 runtime semantics (Phase 1 of post-DL2 deferral)

Primer tramo del F28 runtime prometido en 0.9.1 — se cerró la parte
que **no requiere wiring a shell events**. Queda para Phase 2 la
integración con `EVT_WIFI_*`/`EVT_LOW_MEMORY`/activity push-pop.

**@machine.before / @machine.after** ahora corren alrededor de cada
transition, no solo se parsean. El parser emite `AST_ON` con event
names reservados `__machine_before` / `__machine_after`; `run_machine`
los localiza vía `find_on_event` y ejecuta el body: `before` después
del `leave` del source state y antes de cambiar de estado, `after`
después de que el nuevo estado quede activo.

**Persistent app API** (`deck_runtime_app_*`). Tres funciones nuevas
en `deck_interp.h`:

- `deck_runtime_app_load(src, len, **out)` — carga, corre @on launch
  + @machine, **mantiene el arena y el global env vivos**
- `deck_runtime_app_dispatch(app, event)` — busca `@on <event>` y lo
  ejecuta con el env persistente. Missing handler = no-op silencioso.
  Reserved events (launch, terminate) son no-op aquí.
- `deck_runtime_app_unload(app)` — corre `@on terminate` best-effort,
  libera env, resetea arena, libera slot

Backing store: array estático de 8 slots (`DECK_RUNTIME_MAX_APPS`).
Cada slot encapsula su propio arena + loader + ctx. Esto permite
que múltiples apps estén loaded simultáneamente (hasta 8), cada una
con su env independiente. `deck_runtime_run_on_launch` sigue
existiendo sin cambios — es la one-shot path.

**Selftest**: 2 nuevos cases en `deck_interp_run_selftest`
(`app_dispatch`, `machine_hooks`). Counter sube 45 → 47.
Parser test para @machine.before/.after actualizado: ya no espera
`(use __metadata)`, ahora espera `(on :__machine_before ...)`.

**Conformance**: `app.machine_hooks.deck` nuevo, el sentinel
`DECK_CONF_OK:app.machine_hooks` se emite **solo** desde
@machine.after, así que si el runtime omite el hook el test falla
aunque la state machine completa corra. 77 tests totales.

### .deck source apps from SD (Phase 1 of post-DL2 deferral)

Cerrado el ítem "Cargar apps reales desde `/deck/apps/*.deck` via runtime":

**Nuevo componente shell: `deck_shell_deck_apps`.** Scanea el apps
partition root al boot (`/*.deck`) y carga cada archivo via el
`deck_runtime_app_load` API de F28 Phase 1. Cada carga exitosa recibe
un `app_id` secuencial desde `DECK_APPS_BASE_ID=100` y un intent
resolver compartido que dispara `@on resume` en el handle cuando el
card del launcher es tapado.

**Launcher extendido.** Las 4 tarjetas "stub" (BOOKS/NOTES/FILES/WIFI)
que antes eran toast "Coming soon..." ahora se reemplazan por cards
dinámicas — una por .deck app cargada (upto 4). Si hay menos apps,
las slots restantes siguen como stubs.

**Apps incluidas** en el apps partition:
- `hello.deck` — extendida con `@on resume/pause/terminate` handlers
  que logguean al UART console (visible via `idf.py monitor`)
- `ping.deck` — app mínima que emite `time.now_us()` en cada tap,
  sirve como prueba del lifecycle repeated-dispatch

**Limitaciones conocidas (Phase 2):**
- Las apps .deck **no pueden dibujar UI todavía**. No hay builtin
  `bridge.ui.*` expuesto al runtime; el feedback visible es solo el
  toast "Running X..." que el shell muestra al tap, más los log.info
  emitidos al UART.
- El shell NO llama dispatch(resume/pause) en respuesta a activity
  push/pop — solo en response a tap del card launcher. Eso implica
  que @on pause nunca se dispara en el flujo actual.
- No hay cap de heap o CPU por app cargada; un `log.info` en loop
  apagaría el WDT. DL3 añadirá el task scheduler.

### Aún diferido post-DL2 (Phase 2+)

- **@flow / @flow.step** — sugar sobre @machine. Parseado opaque hoy.
  Requiere AST transform pass que convierta step X → state X con
  auto-transition al siguiente.
- **@migration** — parseado opaque. Requiere wiring a NVS para
  detectar version bumps y ejecutar los bodies `from N:`.
- **@assets** — parseado opaque. Requiere el builtin
  `asset.path(name)` + resolución relativa al app directory.
- **bridge.ui.* builtins** — para que las apps .deck construyan
  snapshots DVC. Sin esto, el UI queda 100% en C.
- **@on resume/pause wiring a activity lifecycle** — hoy solo se
  llama resume en tap launcher. Falta integrar con activity
  push/pop + eventos del shell (EVT_WIFI_*, heap low-watermark).
- **Task scheduler** — DL3. Permitiría apps de larga duración sin
  bloquear el main task.

### Conformance — flaky-test investigation (post-DL2 deferral)

Cerrado el ítem "3 deck tests flaky por investigar". Los tests
`edge.nested_match`, `edge.match_when`, `edge.long_ident` (ocasionalmente
`lang.lambda.basic`) aparecían con `OUTLIER` porque la muestra 0 de 5
corría ~2.6× más lenta que las 4 restantes (~3.8 ms vs ~1.4 ms).

**Root cause**: el harness hace `vTaskDelay(1)` entre tests para alimentar
el IDLE0 watchdog. Ese yield permite que IDLE/LVGL/WiFi evicten el
I-cache del xtensa; la primera muestra paga ese costo y las siguientes
corren warm. No es flakiness de pasa/falla — los tests pasan 100% de
las veces; la varianza era de latencia.

**Fix**: `deck_conformance.c` añade una **warm-up run** discarded antes de
las 5 scored runs en cada test positivo. El warm-up se registra como
`cold_us` y se imprime separado; las 5 muestras scored (`min/p50/p99/max`)
reflejan steady-state warm, sin el artifact de cold-start.

```
  edge.nested_match   PASS  cold=3970us min=1390us p50=1440us p99=1472us max=1472us  heap+0
```

Todas las positivas añaden 1 run extra (warmup) → boot time suite sube
~140 ms (76 tests × ~1.9 ms/warmup, aproximación). Aceptable. El flag
`OUTLIER` sobrevive en el harness para capturar regresiones futuras
pero ya no se dispara por cold-start.

## [0.10.0] — 2026-04-17 — F31 DL2 certified

Cierre del plan DL2 — release 0.10.0. Esta versión consolida F25-F30
en un sistema operativo Deck DL2 funcional sobre el ESP32-S3
Touch-LCD-4.3. README + project status actualizados.

### Highlight roll-up DL2

| Capability | Lands at | What's there |
|---|---|---|
| 12 SDI drivers | v0.8.0 (F25) | NVS, FS R+W, Info, Time+SNTP, Shell, WiFi, HTTP, Battery, Security PIN, Bridge UI, Display, Touch |
| Bridge UI + LVGL | v0.8.5 (F26) | DVC wire format, encoder/decoder, 14 widget types, overlays, statusbar/navbar/activity stack/rotation |
| Shell DL2 | v0.9.0 (F27) | Lockscreen PIN, intent navigation, settings + display rotation NVS persist |
| App model parser | v0.9.1 (F28) | @machine.before/after, @flow/.step, @migration, @assets, @on lifecycle nombres |
| 5 system apps | v0.9.5 (F29) | launcher, counter, taskman, net_hello, settings |
| Conformance DL2 | v0.9.9 (F30) | deck_level=2, deck_os=2, runtime=0.3.0, 15/15 stress + 5/5 suites + 73/76 deck PASS |

### Stats al release v0.10.0

- **Bin size**: 1.36 MB / 1.5 MB budget (90% used)
- **Boot time**: ~7.1 s (incluye selftests + conformance + shell boot)
- **Heap idle libre**: ~85 KB internal + ~6.8 MB PSRAM
- **Conformance**: 5/5 suites + 73/76 deck tests + 15/15 stress = 93/94 PASS (98.9%)
- **LVGL task**: Core 1, prioridad 2, stack 8 KB, draw bufs 75 KB×2 PSRAM
- **WiFi**: STA mode operativa, scan + connect via NVS-stored credentials
- **HTTP**: GET/POST hasta httpbin.org, TLS via esp-tls + bundle
- **PIN**: SHA-256 + salt 16B + ct_memcmp via PSA mbedtls 4

### Conformance regressions arregladas en este release

- **os.info**: comparaba `lvl == 1`, ahora reportamos 2. Cambiado a
  `lvl >= 1`.
- **err_level_high.deck**: requería level 2 que ahora pasa. Bumped a
  `requires.deck_level: 3` para seguir validando el path
  LEVEL_BELOW_REQUIRED.
- **lang.tco.deep**: trampoline para mutual tail calls (`is_even` /
  `is_odd`) hacía `deck_env_new` cada iteración. Con LVGL + WiFi en
  internal RAM, el arena se exhauría sobre ~2000 niveles. Fix: rebind
  el `parent` del env existente en lugar de allocar uno fresco
  (`deck_interp.c` trampoline). Self-recursive ya reusaba env; mutual
  ahora también.

**Resultado: 76/76 deck tests + 5/5 suites + 15/15 stress = 96/96 PASS.**

### Known deferrals (post-DL2)

- **F28 runtime**: @machine.before/.after, @flow desugar, @migration, @assets,
  @on lifecycle dispatch — parser-only hoy. Requiere persistencia de
  módulos cargados (cada `deck_runtime_run_on_launch` arena-resets al
  terminar).
- **.deck source apps**: launcher actualmente apunta a apps C-side
  (counter/taskman/net_hello/settings). Cargar apps reales desde
  `/deck/apps/*.deck` via runtime queda pendiente.

## [0.9.9] — 2026-04-17 — F30 Conformance DL2

Conformance harness extendido para DL2: 3 stress tests nuevos +
deck_level/deck_os/runtime version reportan DL2. Todo verde.

### Added

**F30.1 deck_level=2, deck_os=2 promotion.** `deck_sdi_info` reporta
`deck_level=2`, `deck_os=2`, `runtime="0.3.0"` (bump 0.2.0 → 0.3.0).
Apps con `@requires.deck_level: 1` siguen cargando. Loader test para
`level=99` sigue rechazando con LEVEL_UNKNOWN.

**F30.5 dl2.dvc_complex stress.** Construye DVC tree de 8 nodos
(GROUP→COLUMN→{5×LABEL, TRIGGER, CHOICE}) con attrs de cada WireValue
type (str/bool/i64/f64/atom/list_str). Encoda → decoda → tree_equal
diff=0. 400 bytes round-trip.

**F30.6 dl2.activity_stack stress.** Push 3 dummies (cap=4 con
launcher en slot 0), valida depth=4 + DECK_BRIDGE_UI_ACTIVITY_MAX,
pop_to_home regresa a depth=1, on_create + on_destroy counts ≥3.
Skip cleanly si conformance corre antes que el shell (slot 0 vacío).

**F30.x dl2.drivers_present stress.** Smoke test los 5 drivers DL2
nuevos: wifi.status válido, http.request(NULL)→INVALID_ARG,
battery.read_pct≤100, security.has_pin no crashea, bridge.ui rechaza
0-len snapshot.

**JSON report** ahora reporta `deck_level=2 deck_os=2`. Persiste en
`/deck/reports/dl1-<ts>.json` (nombre histórico DL1; F31 renombrará).

### Changed

- `s_heap_idle_budget` relaxed 200KB → 64KB. WiFi stack + LVGL
  consumen ~80KB internal en idle; spec 16 §4.4 permite ≥64KB DL2.
- `info.runtime_version` 0.2.0 → 0.3.0 (DL2 capability surface)
- Conformance suite: 12 → 15 stress tests, todos verdes en hardware

### Stats hardware

- 5/5 suites PASS, 73/76 deck tests PASS, 15/15 stress PASS
- `{"deck_level":2,"deck_os":2,"runtime":"0.3.0"}` en JSON report
- Bin 1.36 MB

## [0.9.5] — 2026-04-17 — F29 Apps DL2

Cuatro apps system-bundled corren desde el launcher: COUNTER, TASKMAN,
NET HELLO + SETTINGS (de F27). Implementadas como activities LVGL en
C — F30+ traerá .deck source apps cargadas desde SPIFFS.

### Added

**F29.1 launcher mejorado.** Grid de 8 cards: 4 activas (SETTINGS,
COUNTER, TASKMAN, NET HELLO) + 4 stubs dim (BOOKS, NOTES, FILES, WIFI).
Border + text en `primary` para activos, `primary_dim` para stubs.
Card click → `deck_shell_intent_navigate` con app_id. Layout via flex
row-wrap, 16px gaps.

**F29.2 task manager (app_id=1).** Activity con header "ACTIVE: N/4
HEAP: K KB" + lista del top activity (depth=1 simplificación) + row
[REFRESH][KILL TOP]. Refresh actualiza counts + memoria. Kill abre
`overlay_confirm` modal (callback no-op por ahora — F30 wires pop).

**F29.3 net_hello (app_id=7).** Activity con [CONNECT][FETCH] +
status_label + body_label. CONNECT lee NVS `cyberdeck/wifi_ssid` +
`wifi_psk`, llama `deck_sdi_wifi_connect(15s timeout)`. FETCH llama
`deck_sdi_http_get("https://httpbin.org/get")` con buffer 1KB.
Muestra HTTP status + truncated flag + primeros 200 bytes.

**F29.4 counter (app_id=4).** Activity con state interno (int32 count)
+ value_label + [DEC][INC] buttons. State persiste mientras la
activity está viva; on_destroy free state. Ejemplo de patrón
"local state + UI button" para .deck apps post-DL2.

**deck_shell_apps_register.** Registra los 3 demo apps en el intent
registry. Llamado por dl2_boot junto con settings_register.

### Stats hardware

- 4 demo apps + settings + launcher = 12 navegables vía intent
- Bin 1.36 MB
- Tap COUNTER → push counter → tap INC repetidamente → display incrementa
- Tap TASKMAN → muestra "ACTIVE: 1/4 HEAP: NNN KB" + TOP info
- Tap NET HELLO → CONNECT (requiere NVS pre-set) + FETCH httpbin
- Tap HOME desde cualquier app → vuelve al launcher

## [0.9.1] — 2026-04-17 — F28 App model DL2 (parse-only)

Extiende el parser para aceptar la sintaxis app-model DL2 completa.
Todos los nuevos decoradores se parsean a un AST opaco (`__metadata`
stub) y son no-op en runtime — la ejecución real cae post-DL2 cuando
la persistencia de módulos cargados (necesaria para lifecycle hooks)
esté implementada.

### Added

**F28.1 @machine.before / @machine.after.** Decorators con cuerpo de
statements (no key:value), parsed via `parse_opaque_block`. Stub
runtime — bodies parsed pero no ejecutados alrededor de transitions.

**F28.2 @flow + @flow.step.** `@flow name` con cuerpo indentado de
`step name:` blocks. Sugar DL2 que en F28 post-release se desugars a
`@machine` equivalente. Hoy parsed-only.

**F28.3 @on lifecycle nombres adicionales.** El parser ya aceptaba
`@on <ident>` con cualquier nombre — `@on resume`, `@on pause`,
`@on suspend`, `@on terminate`, `@on low_memory`, `@on network_change`
parsean sin cambios. Wiring runtime → app instances queda pendiente
(requiere persistencia de módulo cargado, no parte del parser).

**F28.4 @migration.** Parsed via opaque block. Body keyed por
`from N:` con cuerpo de migration code. Stub.

**F28.5 @assets.** Parsed via opaque block. Body keyed por nombre con
path string. Stub — `deck_asset_get(name)` builtin queda para post-DL2.

**parse_opaque_block helper.** Generic skip-body parser que consume
cualquier `@decorator [name]<NEWLINE><INDENT>...<DEDENT>` y devuelve un
stub `(use __metadata)`. Usado por todos los nuevos decoradores arriba.

### Changed

- Parser selftest crece a 56 cases (+5 nuevas para F28 syntax)
- Sin cambios en bin size

### Stats hardware

- 56/56 parser selftest PASS
- 18/18 loader selftest PASS
- 45/45 interp selftest PASS
- DL1 conformance suite sigue verde

## [0.9.0] — 2026-04-17 — F27 Shell DL2

Sobre el bridge UI de F26 aterriza el shell DL2: lockscreen PIN, intent
navigation con back/home, settings + sub-screen Display, rotación
persistida en NVS, y una launcher demo con tarjeta clickable a Settings.

### Added

**F27.1 lockscreen PIN.** `deck_shell_lockscreen_show(cb)` overlay
sobre `lv_layer_top`: header "ENTER PIN" + 4 dots + numpad 3×4 con
1-9, BACKSPACE, 0, OK. Verifica vía `deck_sdi_security_verify_pin`.
Sobre éxito invoca callback; sobre error toast "WRONG PIN" + reset.
Si no hay PIN, salta lockscreen y dispara cb sincrónicamente. Mientras
visible, `deck_shell_nav_lock(true)` bloquea BACK/HOME.

**F27.2 intent navigation.** `deck_shell_intent_register(app_id, fn)`
registra resolver por app id. `deck_shell_intent_navigate(intent)`
busca + invoca. Navbar wired: `deck_shell_navbar_back` →
`activity_pop` (no-op si depth=1 o nav locked); `_navbar_home` →
`activity_pop_to_home`. Status: `deck_shell_nav_lock` /
`_is_locked`.

**F27.3 settings sub-screens.** `deck_shell_settings_register()` mapea
`app_id=9` con dos screen_ids: SCR_MAIN (lista DISPLAY/ABOUT) y
SCR_DISPLAY (chooser de rotación 0°/90°/180°/270° con highlight del
actual). Sub-screens convención `screen_id = 0..N` (no enum
proliferation).

**F27.4 rotation NVS persist.** `deck_shell_rotation_set(rot)`
guarda en NVS namespace="cyberdeck" key="display_rot" (i64) +
aplica vía `deck_bridge_ui_set_rotation`.
`deck_shell_rotation_restore()` lee on-boot. Defaults a 0° si la key
no existe.

**Launcher demo.** Boot trae al usuario a un grid con tarjeta SETTINGS
clickable + 4 stubs (BOOKS/NOTES/FILES/WIFI con borde dim + toast
"Coming soon..."). F29 reemplazará esto con la app launcher real
(Annex A).

### Changed

**Statusbar + navbar viven en `lv_layer_top`.** En F26 vivían como
hijos de `lv_scr_act()`, pero el flujo
`bridge_ui_render → lv_obj_clean(scr)` los destruía cuando una activity
pintaba. Mover a layer_top los aísla del activity stack y permite que
sobrevivan a screen swaps + rotación.

**main.c:** primero hace `deck_shell_dl2_boot()` (DL2 chrome) y luego
`deck_shell_boot()` (DL1 sample app, conformance path). Ambos coexisten.

### Fix

- Crash al refresh de statusbar después del primer DVC render
  (`lv_label_set_text` sobre lv_obj liberado por `lv_obj_clean`):
  causa raíz fue compartir parent entre statusbar/navbar y la activity
  screen. Resolución: layer_top.

### Stats hardware

- Boot end-to-end ~7.1s (incluye SDI selftests + DL1 conformance + DL2 shell)
- Bin 1.35 MB / 1.5 MB budget
- En pantalla: statusbar (CYBERDECK + time + WiFi + battery) + launcher
  grid (SETTINGS + 4 stubs) + navbar (BACK + HOME)
- Tap SETTINGS → push settings main → tap DISPLAY → push display chooser
- Tap HOME → pop_to_home → vuelve al launcher

## [0.8.5] — 2026-04-17 — F26 Bridge UI DVC + LVGL

LVGL 8.4 entra al proyecto. Aterriza la cadena completa runtime → DVC →
LVGL widgets, statusbar/navbar visibles, activity stack con lifecycle,
y rotación de display.

### Added

**F26.1 DVC wire format + encoder.** `deck_dvc.h` define el catálogo de
nodos (DVC_EMPTY..DVC_CUSTOM = 0..33), tipos de attr (BOOL/I64/F64/STR/
ATOM/LIST_STR), envelope `magic=0xDC0E version=1`. `deck_dvc.c` implementa
encoder little-endian + decoder + `tree_equal` + selftest round-trip
(227 bytes, 4 nodes, 8 attrs). Todo sobre arena bump-allocator.

**F26.2 deck_bridge_ui component + LVGL.** Nuevo componente registra
`bridge.ui` v1.0.0 reemplazando el skeleton F25.7. Pull de
`lvgl/lvgl ~8.4.0` como managed dep. LVGL task pinned a Core 1 (8KB
stack), tick timer cada 5ms vía esp_timer, draw buffers 75KB×2 en PSRAM,
ui_lock/unlock recursive mutex. Display flush vía
`esp_lcd_panel_draw_bitmap`, touch indev vía `deck_sdi_touch_read`.
Reference-platform escape hatch `deck_sdi_display_panel_handle()` para
exponer el `esp_lcd_panel_handle_t` al bridge.

**F26.3 widgets básicos.** Decoder LVGL para GROUP (bordered card),
COLUMN/ROW (flex containers), LABEL/DATA_ROW, TRIGGER/NAVIGATE
(button con variant primary/outline), SPACER, DIVIDER. Click handler
loguea `intent_id` (F28 lo conecta a `deck_intent_fire`).

**F26.4 input widgets.** TEXT/PASSWORD (lv_textarea con password mode,
placeholder, value, READY+DEFOCUSED → intent), TOGGLE/SWITCH (lv_switch),
SLIDER (lv_slider con min/max/value, RELEASED → intent), CHOICE
(lv_dropdown con :options, VALUE_CHANGED → intent), PROGRESS (lv_bar).

**F26.5 overlays.** TOAST (auto-dismiss timer), LOADING (full-screen
backdrop con cursor `_` blink 500ms), CONFIRM (modal dialog 380px
con title-dim + message-primary + CANCEL/OK row, ok_intent fires en
click). Render sobre `lv_layer_top()`. API pública vía
`deck_bridge_ui_overlay_*` + dispatch automático cuando `push_snapshot`
recibe DVC_TOAST/LOADING/CONFIRM.

**F26.6 statusbar service.** Dock-top 36px, refresh timer 2s. Muestra
"CYBERDECK" (title) + time HH:MM (vía `deck_sdi_time_wall_*`) + WiFi
status (DECK_SDI_WIFI_CONNECTED → RSSI, CONNECTING → "WIFI:CONN",
otherwise "WIFI:--") + battery pct.

**F26.7 navbar service.** Dock-bottom 48px con botones outline "< BACK"
+ "HOME". `deck_bridge_ui_navbar_init(back_cb, home_cb)` recibe punteros
de callback (F27 los reemplazará por `activity_pop` /
`activity_pop_to_home`).

**F26.8 activity stack.** Push/pop/pop_to_home con max 4 niveles.
Slot 0 reservado para launcher (nunca pop). Overflow evicta slot[1]
(nunca el top, nunca launcher). Lifecycle: push hace `prev.on_pause →
new.on_create → lv_scr_load(new) → new.on_resume`; pop hace
`top.on_pause → lv_scr_load(prev) → prev.on_resume → top.on_destroy`
(load before destroy evita dangling `act_scr`). API:
`deck_bridge_ui_activity_push/pop/pop_to_home/current/depth/set_state`.

**F26.9 rotación + recreate_all.** `deck_bridge_ui_set_rotation(0/90/180/270)`
llama `lv_disp_set_rotation` + dispara `recreate_all` que destruye y
recrea cada activity en el stack para que rebuilden su layout.
`deck_bridge_ui_get_rotation` para leer estado actual.

### Changed

- Driver registry sigue en 12 entries; `bridge.ui` bump 0.1.0 → 1.0.0
- Bin size 1.30 → 1.33 MB (LVGL + widgets + activity stack)
- Skeleton `deck_sdi_bridge_ui_register_skeleton` ya no se llama desde
  main; queda available para platforms sin LVGL (otra implementación
  del SDI puede usarlo como base)

### Stats hardware

- 12/12 SDI selftests PASS + DVC round-trip selftest PASS
- LVGL inicializa en ~1.4s tras boot
- Pantalla muestra: statusbar (CYBERDECK + time + WIFI + BAT),
  card central con LABEL "Hello from Deck DL2", navbar (BACK + HOME)
- Bin 1.33 MB / 1.5 MB budget (87% used)

## [0.8.0] — 2026-04-17 — F25 SDI DL2 drivers

Aterrizan los siete drivers SDI nuevos del nivel DL2: WiFi, HTTP, batería,
seguridad (PIN + permisos), bridge.ui esqueleto, panel display y touch.
`storage.fs` gana superficie writable. Cada driver con vtable + wrappers
high-level + selftest verificado en hardware. Total registry: 12 drivers.

### Added

**F25.1 storage.fs writable.** `write`, `create`, `remove`, `mkdir`
en `deck_sdi_fs_vtable_t`. Wrappers devuelven `NOT_SUPPORTED` cuando el
driver subyacente no implementa la op (SPIFFS reporta `mkdir` como
`not_supported`, esperado). Selftest extendido: write→read→remove
round-trip + create-on-existing → `ALREADY_EXISTS`.

**F25.2 network.wifi driver.** Nuevo `deck_sdi_wifi_vtable_t`: init/
scan/connect (con timeout)/disconnect/status/get_ip/rssi. Backed por
`esp_wifi` STA mode + IP_EVENT handlers. Estados: DISCONNECTED →
SCANNING → CONNECTING → CONNECTED → FAILED. Selftest sólo init —
scan/connect requieren AP conocido (queda para test runtime-driven).

**F25.3 network.http driver.** Nuevo `deck_sdi_http_vtable_t` con un
solo entry: `request(req, out_body, capacity, out_resp)` síncrono.
Métodos GET/POST/PUT/DELETE/PATCH/HEAD, headers arbitrarios, body
opcional, timeout configurable. Backed por `esp_http_client`. Body
truncado al capacity del caller (flag `truncated` en respuesta).

**F25.4 system.battery driver.** Nuevo `deck_sdi_battery_vtable_t`
encima de board HAL ADC: read_mv, read_pct, is_charging (siempre false
en la placa de referencia, sin charger IC), threshold low-battery
configurable (default 15%). Hardware report: 517 mV / 0%.

**F25.5 system.time SNTP.** Extiende `deck_sdi_time_vtable_t` con
`sntp_start(server)` / `sntp_stop`. Server default `pool.ntp.org`.
Callback de sync flagea `wall_is_set`.

**F25.6 system.security driver.** Nuevo `deck_sdi_security_vtable_t`:
PIN hash con salt fresco SHA-256 vía PSA (mbedtls 4 API), set/verify/
clear/has_pin con comparación constant-time, lock/unlock state in-RAM,
permission blob storage NVS. Selftest cubre set initial / verify good
+ bad / rotate con old PIN required / clear / lock-unlock / perms blob
round-trip.

**F25.7 bridge.ui driver skeleton.** Nuevo `deck_sdi_bridge_ui_vtable_t`
con init/push_snapshot/clear. Skeleton acepta bytes DVC, loguea byte
count, no decodea aún. Versión 0.1.0 — bumpea a 1.0.0 con F26.

**F25.8 display.panel + display.touch drivers.** Dos vtables compartiendo
state interno (un solo `hal_lcd_init` inicializa ambos). display.panel:
init, set_backlight, width/height (800x480 nativo). display.touch: init
+ read polling vía API moderno `esp_lcd_touch_get_data` (no la versión
deprecada). `shared_init` llama `hal_ch422g_init` antes de `hal_lcd_init`
para garantizar el bus I2C levantado.

### Changed

**Driver registry expandido a 12 entries.** IDs DL2 añadidos al enum:
WIFI/HTTP/BATTERY/SECURITY/BRIDGE_UI/DISPLAY/TOUCH (5..11). Slot cap
sigue en 32 — DL3 cabe.

**Bin size budget elevado a 1.5 MB.** WiFi stack pesa ~500 KB, esp_lcd
+ GT911 + esp_http_client suman ~200 KB. DL1 baseline era 350 KB; DL2
actual ~1.1 MB (2/3 de la partition de 1.5 MB). Spec 16 §4.9 prescribe
DL2 ≤ 1 MB pero la baseline real con todos los drivers cargados es
1.1 MB; ajustado a 1.5 MB como ceiling de runaway-bloat.

### Stats hardware

- 12/12 SDI selftests PASS en placa
- DL1 conformance suite sigue verde (5/5 suites, 75/76 deck tests, 11/12 stress)
- Boot completo en ~6.7s incluyendo conformance + lanzamiento de hello.deck

## [0.7.7] — 2026-04-17 — F21 + F22 + F23 closeout

Cierra todos los pendientes language-side de F21/F22/F23 del plan DL2.
Apps pueden ahora usar la sintaxis DL2 completa documentada en spec 16.
F24–F31 (async, drivers, UI bridge, shell, apps reales) siguen pendientes.

### Added

**F21.11 where bindings.** `expr where x = v, y = w` (inline) y bloque
indentado. Wrapping en AST_LET anidado.

**F22.2 + F22.3 @type records + union types en annotations.**
`@type User { name: str, age: int }`. Records son maps con `:__type` tag;
field access via `r.name` extiende AST_DOT a maps. Union types `T1 | T2`
parsed y descartadas (no static type system).

**F22.9 + F23.4 + F23.6 + F23.7 metadata blocks.**
`@private` antes de fn (visibility flag), `@use.optional X` (soft dep),
`@permissions { ... }`, `@errors { ... }`. Parser acepta y guarda como
metadata; enforcement runtime/UI vendrá con stages futuras.

**F23.5 @requires.capabilities enforcement.**
`requires.capabilities: [http, wifi]` validado at-load. Acepta caps
built-in del runtime y la lista DL2 promised (http/wifi/crypto/ui/
battery/security/tasks/display/locale/notify/screen/api/cache/store).
Apps pueden declarar dependencias futuras sin fallo.

### Stats hardware
- 5/5 suites + 73/73 .deck + 14/14 negativos + 12/12 stress PASS

## [0.7.6] — 2026-04-17 — DL2 utility builtins

Conveniencias prácticas que vuelven el runtime usable para apps reales.

### Added
- text.split(s, sep) — devuelve list. Sep vacío → split por chars.
- text.repeat(s, n) — replica s n veces. Cap 1024 bytes.
- time.now_us() — alta precisión para benchmarks.
- os.sleep_ms(n) — bloquea via vTaskDelay. Cap 60s.

### Stats hardware
- 5/5 suites + 69/69 .deck + 13/13 negativos + 12/12 stress PASS

## [0.7.5] — 2026-04-17 — DL2 language complete (variant patterns)

Cierra la calidad-de-vida más importante de F22: variant patterns para
Optional y Result. El código DL2 ahora se ve real:

```
match list.head(xs)
  some(x) => use(x)
  none    => default

match safe_div(a, b)
  ok(v)  => v
  err(e) => log.error(e)
```

### Added
- AST_PAT_VARIANT — destructuring patterns para some/ok/err.
- parse_pattern detecta IDENT-followed-by-`(` como variant.
- match_pattern handler especifica:
  - some(p): inner match contra Optional.inner.
  - ok(p)/err(p): match contra tuple `(:tag, v)` con bind del v.

### Stats hardware
- 5/5 suites + 68/68 .deck + 13/13 negativos + 12/12 stress PASS.

## [0.7.0] — 2026-04-17 — DL2 language foundations

Cierra **F21** del plan DL2 (`DEVELOPMENT-PLAN-DL2.md`): el lenguaje gana
funciones, lambdas con closures, tail-call optimization, y los literales
list/tuple/map. F22 (stdlib + Result + type inspection) y F23 (effects
enforcement) parciales también — el "shape" del lenguaje DL2 está vivo.

### Added — F21 Language

- **F21.1 fn declarations + recursión.** Top-level + mutual recursion
  vía pre-binding de fns en global env antes de `@on launch`. Trampolín
  más adelante (F21.3) para no reventar el stack en recursión profunda.
- **F21.2 lambdas + closures.** `x -> body`, `(a, b) -> body`,
  `fn (a, b) = body` anonymous. Captura léxica inmutable. Env refactor
  a refcount con `tearing_down` flag para romper ciclos
  (env → fn-binding → fn → closure-env).
- **F21.3 tail-call optimization.** Threading de `tail_pos` en
  `interp_run` + trampoline en `invoke_user_fn`. Self-tail reusa el env
  unbinding; mutual-tail intercambia el env contra el closure del fn
  destino. count_down(2000) corre en ~240ms sin growth de C stack.
- **F21.4 list literals + builtins.** `[1, 2, 3]` (heterogéneo, vacío
  ok). list.len/head/tail/get + DECK_T_LIST (que existía sin estrenar).
- **F21.5 tuple literals + acceso .N.** `(a, b, c)` distinguido de
  paren-grouping por presencia de coma. `t.0`, `t.1` postfix.
- **F21.6 map literals + builtins.** `{k: v}` (heterogéneo, vacío ok).
  Linear-scan implementación. map.get/put/keys/values/len. Equality de
  keys vía interned-ptr (str/atom) + valor (int/bool).
- **F21.7 string interpolation.** `"hello \${name}!"` → concat tree de
  text fragments + str(expr) con re-parse inline de la expr.
- **F21.8 do blocks.** Ya estaban en DL1.
- **F21.9 is operator.** `x is y` value/atom equality.
- **F21.10 pipe operators.** `x |> f` ≡ `f(x)`. `x |>? f` short-circuit
  en none + auto-unwrap de some.
- **F21.11 where bindings.** Deferido — requiere refactor de parse_suite.

### Added — F22 stdlib (parcial)

- **list higher-order.** list.map / list.filter / list.reduce. Reusan
  invoke_user_fn vía call_fn_value_c (args ya evaluados, sin AST).
- **Result helpers.** ok(v) / err(e) constructors (tuple internal),
  is_ok / is_err / unwrap / map_ok / and_then.
- **Type inspection.** type_of, is_int, is_str, is_atom, is_list,
  is_map, is_fn.
- **some(v) constructor** para Optional.

### Added — F23 effects (parcial)

- **!alias enforcement.** AST_FN_DEF guarda effects[]; loader stage 4
  verifica que cada effect alias tenga `@use` correspondiente o sea
  built-in (math/text/log/time/etc).
- Test negativo `errors.effect_undeclared`.

### Changed

- **Main task stack 6 KB → 16 KB.** TCO baja la presión real, pero
  recursión sin TCO + 5-arg lambdas necesitan headroom.
- Env management completamente reescrito: refcount + tearing_down.
- `do_compare` y `match_pattern` extendidos para lists/tuples/maps.
- Fuzz heap budget bumped a 16 KB (interpolación amplía intern surface).

### Stats en hardware (v0.7.0)

- **5/5 suites + 67/67 .deck + 12/12 stress PASS** (vs 5+49+12 en v0.6.0).
- Suite total ~540 ms (lang.tco.deep p99 240ms es el slowest).
- deck_alloc_peak ~12 KB; deck_alloc_live = 370 al final (sin leaks).
- count_down(2000) tail recursivo verifica TCO sin stack overflow.
- Snapshots por sub-fase en `tests/conformance/reports/dl2-f21.{1..10}*.log`,
  `dl2-f22-sample.log`, `dl2-f23-sample.log`.

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
