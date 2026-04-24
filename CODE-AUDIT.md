# CODE-AUDIT.md — Deck 3.0 firmware gap report

Read-only audit of `components/` against the five pillar specs
(`deck-lang/{LANG,SERVICES,CAPABILITIES,BUILTINS,BRIDGE}.md`, edition 2027)
and `apps/demo.deck`. Authority order: LANG > SERVICES > CAPABILITIES =
BUILTINS = BRIDGE.

Severities: **Block** (spec-mandated behaviour absent/wrong) ·
**Misalign** (exists but contract diverges) · **Dead** (spec removed /
obsoleted) · **Underspec** (behaviour exists but not to spec shape).

---

## components/deck_runtime

**What it currently implements.** A DL1/DL2-flavoured tree-walking
interpreter: arena/allocator, intern table, lexer, parser, AST, loader
(6 staged passes), evaluator, DVC tree builder, and module-local
`bridge.ui.*` DVC node constructors exposed as Deck builtins.

**Spec map.** LANG §1 (lex), §2 (types), §3–§5 (bindings/fns/exprs), §6
(annotations), §7–§10 (`@app`/`@needs`/`@use`/`@grants`), §11 (errors),
§13 (`@machine`), §14 (`@on`); BUILTINS §7–§21 (the 14 v1 modules);
CAPABILITIES §1–§4 (import/alias/config/calls); BRIDGE §6–§8 (content
pipeline, DVC wire envelope, intent round-trip).

**Gaps.**

1. **Block — `@needs` keyword not recognised.** Parser accepts only
   `@requires` and dispatches `parse_requires_decl`:
   `components/deck_runtime/src/deck_parser.c:2855` — `else if
   (dec_is(&p->cur, "requires"))`. LANG §8 renamed the annotation to
   `@needs`. `apps/demo.deck:70` uses `@needs`; it will fail parse with
   "unknown top-level decorator" (parser.c:2905).
2. **Block — `@grants` not parsed.** No case in `parse_top_item`
   (parser.c:2853–2907). `apps/demo.deck:111` opens a 65-line `@grants`
   block that the parser will reject. LANG §10 mandates it.
3. **Block — `@handles`, `@config`, `@service`, `@migrate` all route
   through `parse_opaque_block`** (parser.c:2881–2887): body is skipped
   to the matching DEDENT and replaced with a stub `AST_USE
   "__metadata"`. Spec-required semantics (deep link patterns §20,
   typed persistent config §12, IPC-callable fns §15, schema evolution
   §17) do not exist.
4. **Block — `@errors` parsed-and-discarded**
   (parser.c:2759–2798 `parse_metadata_block`). LANG §2.4 / §11.1 make
   `@errors` load-verified: the evaluator must treat the declared atoms
   as a closed variant for exhaustive match. `apps/demo.deck` declares
   three error domains (`:261 demo`, `:261 echo`, `:1675/1682 layerA/B`)
   that are silently swallowed.
5. **Block — serves field not extracted.** Loader's
   `extract_app_metadata` (loader.c:87) pulls `id`/`name`/`version`/
   `edition` only — `serves:` (LANG §7, apps/demo.deck:67) is ignored,
   so `@service` provider registration is impossible.
6. **Block — effect annotation `!` absent.** LANG §2.6 makes `!` the
   purity bit; BUILTINS §2–§3 pivots the whole catalog on it. Lexer
   and parser do not lex bang effects in fn signatures (no TOK_BANG
   consumer; fn params are untyped idents in `ast.h:200–210`). The
   `effects` array field exists but is always zero-length in practice;
   LANG §9 / BUILTINS §3 `_io` variants won't type-check.
7. **Block — `T?` sugar + the postfix `?` operator (LANG §1.10,
   §11.1) are not in the lexer.** No `TOK_QUESTION` emitted; there is
   no early-return lowering. `demo.deck` uses `?` only implicitly via
   `:ok`/`:err` match, so it parses, but spec-legal propagation syntax
   won't.
8. **Misalign — `@app` fields permitted differ from spec.** Parser's
   `parse_requires_fields` (parser.c:1515) accepts arbitrary key:value
   pairs, but `@app.serves`, `@app.license`, `@app.orientation`,
   `@app.log_level`, `@app.tags`, `@app.author`, `@app.icon` are never
   validated; the loader only uses four. LANG §7 specifies the closed
   set and its type rules.
9. **Misalign — BUILTINS catalog uses legacy `nvs.*`/`fs.*` names
   rather than `storage.nvs`/`storage.fs` service calls.** Registry at
   `deck_interp.c:5232–5260`. SERVICES §14/§15 put filesystem + NVS
   under the `storage.*` tier accessed through `@use storage.nvs as
   nvs`. The interpreter exposes these as builtins regardless of any
   `@use` declaration. CAPABILITIES §1 requires alias-scoped resolution;
   the current code short-circuits it.
10. **Misalign — `bridge.ui.*` is exposed as a set of Deck builtins.**
    `deck_interp.c:5270–5279` registers `bridge.ui.label`,
    `bridge.ui.trigger`, `bridge.ui.column`, `bridge.ui.row`,
    `bridge.ui.group`, `bridge.ui.data_row`, `bridge.ui.divider`,
    `bridge.ui.spacer`, `bridge.ui.render`. BRIDGE §0 invariant 3 +
    §2 say categorically that `bridge.ui.*` is **not** a capability;
    no Deck app may import or call the bridge. Every one of these
    builtins violates the presentation-policy invariant — apps can
    call them to construct `column/row/divider/spacer/data_row`, which
    are layout primitives LANG §0 invariant 1 + the `CLAUDE.md` design
    invariant forbid. **Dead** per the new spec.
11. **Block — BUILTINS v1 modules missing.** Per BUILTINS §7 the
    catalog is 14 modules. Implemented: `math`, `text`, `list`, `map`,
    `bytes` (partial), `time`, `log`, `type_of`. Missing: `stream`
    (BUILTINS §12 — core for `@on source` and `apps/demo.deck:1257`
    onward), `json` (§17 — used by any API integration), `rand`
    (§20), `option` (§14 — only `some`/`none` ctors exist, no
    `option.map/and_then/unwrap_or_else` module), `result` (§15 —
    partial: `ok`/`err`/`is_ok`/`is_err`/`unwrap`/`unwrap_or`/`map_ok`
    but no `and_then`/`or_else`/`map_err`), `record` (§16 entire
    module).
12. **Dead — `bridge.ui.column`/`row`/`spacer`/`divider`/`data_row`
    builtins.** BRIDGE §13 removes positional containers in favour of
    semantic `list`/`group`/`form` plus data wrappers §15. The bridge
    is expected to infer column vs row from substrate + form factor;
    explicit constructors for them are exactly the knobs BRIDGE §0
    invariants 3–5 forbid. Whole block at
    `deck_interp.c:4061–5279` slots `b_bui_*`.
13. **Underspec — content evaluator builds a fresh DVC tree on every
    state entry.** `content_render` (interp.c:4418) rebuilds from
    scratch; `interp.c:4422–4433` resets intent table and re-allocs.
    BRIDGE §9 mandates diffing with `(app_id, machine_id, state_id,
    frame_id)` identity — the wire envelope does not carry those at
    all (`deck_dvc.h:147–183`: magic+version+flags+root_offset only).
    The bridge cannot implement the mandated patch-vs-rebuild decision.
14. **Underspec — `LoadError` kinds do not match spec.** `deck_error.h`
    defines 21 numeric codes (`DECK_LOAD_*`); LANG §11.3 fixes **9
    kinds** (`:lex`, `:parse`, `:type`, `:unresolved`, `:incompatible`,
    `:exhaustive`, `:permission`, `:resource`, `:internal`) plus a
    structured `where: SourceSpan` + `context: {str:str}`. The C
    surface carries line/col in the loader struct but never produces
    the spec's `context` map or emits the error as a Deck value for
    `system.apps.load_error` (SERVICES §22).
15. **Underspec — `@on` variant coverage.** Parser handles `@on
    <dotted.event>` and a few ad-hoc atoms (`launch`/`resume`/
    `suspend`/`terminate`/`back`/`every`/`after`/`watch`/`source`/
    `open_url`/`overrun`) but LANG §14 + SERVICES §51–§54 layer a
    specific semantics per source (scheduler vs events vs apps vs
    stream) that the dispatcher does not distinguish; every handler
    goes through the same `AST_ON` body eval with no payload-binding
    validation beyond a cursory `ast_on_param_t` list.
16. **Underspec — `@machine` payload on transitions.** AST keeps
    `event`/`from_state`/`to_state`/`when_expr` (ast.h:259) but no
    payload destructuring; LANG §13 + §13.4 specify typed payloads.
17. **Misalign — `AST_REQUIRES` node + naming.** Whole AST kind is
    named `AST_REQUIRES` (ast.h:82). Per LANG §8 it should be
    `AST_NEEDS`. Purely cosmetic for C callers but a tripwire for
    anyone diffing against the spec.

Files inside this component that most directly need to change to meet
spec: `deck_parser.c` (keyword, `@grants`, `@handles`, `@config`,
`@service`, `@migrate`, `@errors`), `deck_interp.c` (remove
`bridge.ui.*` builtins, split `nvs.*`/`fs.*` into capability-bound
shims, add missing BUILTINS modules), `deck_dvc.h` (envelope identity
triple), `deck_error.h` (collapse to 9 LoadErrorKinds + context map),
`deck_loader.c` (needs/grants/serves extraction).

---

## components/deck_bridge_ui

**What it currently implements.** LVGL-backed reference bridge: LVGL
mutex, 4-slot activity stack, statusbar + navbar, DVC decode →
`lv_obj` widget tree, overlays (toast, loading, confirm, keyboard),
rotation.

**Spec map.** BRIDGE §4 (SDI vtable), §6–§10 (content pipeline,
wire format, intent round-trip, diffing, identity), §13–§17
(primitive inference), §21–§34 (UI services: statusbar, navbar, toast,
confirm, loading, keyboard, choice, date, share, permission,
lockscreen, badge).

**Gaps.**

1. **Block — SDI vtable is non-stratified and minimal.** `deck_sdi_
   bridge_ui.h:21–31` defines a flat 3-method vtable (`init`,
   `push_snapshot`, `clear`). BRIDGE §4 mandates a layered vtable:
   Core (+ `set_intent_hook`, `toast`, `confirm`, `loading_show/hide`,
   `progress_show/set/hide`, `choice_show`, `multiselect_show`,
   `date_show`, `share_show`, `permission_show`, `set_locked`,
   `set_theme`), Visual (`keyboard_show/hide`, `set_statusbar`,
   `set_navbar`, `set_badge`), Physical-display (`set_rotation`,
   `set_brightness`). None of these are SDI-exposed; the shell calls
   into `deck_bridge_ui_*` C helpers directly.
2. **Block — no `(app_id, machine_id, state_id, frame_id)` snapshot
   identity.** `deck_bridge_ui_render` (`deck_bridge_ui_decode.c:706`)
   takes only a root node pointer. BRIDGE §10 makes identity-tuple-
   indexed state essential for activity-stack routing, scroll
   restoration, focus preservation. Consequence: the bridge can't
   decide push-vs-replace-vs-patch; it always clears the screen
   (`lv_obj_clean(scr)` at `decode.c:722`) and rebuilds.
3. **Block — diffing/patch path absent.** Implementation is full
   rebuild per snapshot (`decode.c:7–10` comment: "Each render wipes
   the active screen and rebuilds the tree top-down. Per-render
   diffing is a future optimization"). BRIDGE §9 explicitly admits
   full rebuild as conformant but then BRIDGE §10 requires
   preserving widget ownership + focus across patches — impossible
   without at least the identity triple.
4. **Block — intent ID frame scoping.** BRIDGE §7.3 mandates intent
   IDs be snapshot-scoped and invalidated on every new snapshot.
   `deck_bridge_ui_decode.c` keeps the widget→intent map alive
   across renders as long as `lv_obj` objects live; if the runtime
   reused an ID across snapshots the hook would misfire.
5. **Block — DVC coverage gaps vs spec §13–§17.**
   `decode.c:669–703` switch: handles
   `GROUP/COLUMN/FORM/LIST/LIST_ITEM/ROW/LABEL/DATA_ROW/TRIGGER/
   NAVIGATE/TEXT/PASSWORD/TOGGLE/SWITCH/SLIDER/CHOICE/PROGRESS/
   SPACER/DIVIDER`; renders `[dvc_type=N]` placeholder for all the
   rest. Missing: **MULTISELECT** (no DVC type exists — BRIDGE §16),
   **SEARCH** (ditto), **PIN** (type 18 but no render), **DATE_PICKER**
   (type 17 but no render), **CONFIRM** as intent-level (type 25 only
   as overlay), **MARKDOWN**/**MARKDOWN_EDITOR** (types 31/32),
   **RICH_TEXT** (20), **MEDIA** (21), **CHART** (27), **SHARE** (26),
   **CREATE** (no DVC type), **LOADING/TOAST** as content-level.
6. **Misalign — SDI driver is registered under the flat 3-method
   vtable.** `deck_bridge_ui_lvgl.c` and `deck_sdi_bridge_ui_skel.c`
   register only `init`/`push_snapshot`/`clear` — all richer bridge
   features (statusbar, navbar, rotation, overlays) are reachable
   only via direct `deck_bridge_ui_*` C functions, not the SDI. This
   means a voice or e-ink bridge cannot be substituted by swapping
   SDI drivers (BRIDGE §1 invariant: a platform ships exactly one
   bridge behind the SDI vtable).
7. **Block — `@on back :confirm` not routed.** BRIDGE §18 Inline-vs-
   modal table explicitly lists `back-confirm` (`@on back :confirm`)
   as a Confirm Dialog trigger. Navbar BACK path is
   `deck_bridge_ui_navbar.c:32 s_back_cb()` → shell handler → raw pop;
   no lookup into the running app's `@on back` to decide between
   `:handled`/`:unhandled`/`:confirm`. `apps/demo.deck:1376` returns
   `:handled`/`:unhandled`; there is no dispatch path at all.
8. **Underspec — destructive colour inference.** BRIDGE §17.6 says a
   `confirm` whose label matches `DELETE|REMOVE|KILL|FORMAT|RESET`
   renders OK in a fixed destructive colour (`#FF3333`). The overlay
   at `deck_bridge_ui_overlays.c:1` renders every confirm identically.
9. **Underspec — action composition (BRIDGE §17).** `render_trigger`
   treats every `TRIGGER`/`NAVIGATE` identically; there is no detection
   of tail-single-action (full-width primary §17.1), action pair
   (`Cancel`/`Back` naming → right-most-non-secondary promotion
   §17.2), or three+-action collapse (§17.3). Missing inference means
   the visual grammar described in `CLAUDE.md` is implemented only as
   ad-hoc C screens in `deck_shell_apps.c`, not as emergent bridge
   behaviour.
10. **Underspec — content-tree bounds (BRIDGE §5.4).** No truncation
    logic anywhere — a runtime-emitted snapshot with 1000-item list
    would be rendered as-is, or crash LVGL when it runs out of objs.
11. **Dead — `render_column`/`render_row`/`render_group` as separate
    renderers.** BRIDGE §13 exposes only `list`/`group`/`form`. The
    existence of a `DVC_COLUMN`/`DVC_ROW`/`DVC_GRID`/`DVC_FLOW`
    renderer and decoder is a vestige of a pre-3.0 layout DSL. Under
    the new spec, any snapshot the runtime emits should contain only
    `LIST/GROUP/FORM/LOADING/ERROR/MEDIA/RICH_TEXT/STATUS/CHART/
    PROGRESS/MARKDOWN/MARKDOWN_EDITOR` + intents. Whole column/row/
    flow/grid decoder family is dead code per spec.
12. **Underspec — universal invariants (BRIDGE §20).** No assertion
    that intents originate only from user actions; `s_toast_timer`
    in `overlays.c:32` could synthesize state-mutating behaviour, and
    `render` wipes screens without tear-down ordering guarantees.

---

## components/deck_sdi

**What it currently implements.** A driver-id enum (`deck_sdi.h:43–59`),
a fixed-slot registry (`deck_sdi_registry.c`), and per-driver C
wrappers for `storage.nvs` / `storage.fs` / `system.info` /
`system.time` / `system.shell` (DL1), plus `network.wifi` /
`network.http` / `system.battery` / `system.security` / `bridge.ui` /
`display.panel` / `display.touch` (DL2). Drivers are ESP32-specific
`*_esp32.c` implementations.

**Spec map.** SERVICES §4.1 (`:native` services), §10 (threading),
§13 (catalog overview), §14–§45 (individual services). BRIDGE §4
(bridge.ui vtable).

**Gaps.**

1. **Block — SERVICES catalog undersupply.** SERVICES §13 lists **31
   first-class system services**; the SDI registers **7**. Missing:
   `storage.cache`, `network.ws`, `network.bluetooth`, `system.apps`,
   `system.power`, `system.display` (as a service — only a driver exists
   for the panel), `system.audio`, `system.locale`, `system.ota`,
   `system.url`, `system.notify`, `system.theme`, `system.logs`,
   `system.scheduler`, `system.events`, `system.intents`,
   `system.services`, `system.tasks`, `system.platform` (not
   `system.info`). Tier 4 domain services (`api.client`, `media.image`,
   `media.audio`, `auth.oauth`, `data.cache`, `share.target`) are
   entirely absent. `apps/demo.deck` requires every one of these (see
   `@needs.services` at `apps/demo.deck:78–109`).
2. **Block — no error model alignment.** SERVICES §6 specifies shared
   error atoms `:unavailable`/`:permission_denied`/`:timeout`/
   `:invalid_arg`/`:not_found`/`:io`/`:busy`/`:unsupported`. The SDI
   enum (`deck_sdi.h:24–35`) carries a subset with different names
   (`DECK_SDI_ERR_NOT_FOUND`/`_INVALID_ARG`/`_NOT_SUPPORTED`/
   `_NO_MEMORY`/`_TIMEOUT`/`_IO`/`_ALREADY_EXISTS`/`_BUSY`/`_FAIL`).
   Mapping to Deck atoms is ad-hoc per driver.
3. **Block — service versioning (SERVICES §8) not enforced.** The
   driver struct has a `version` string, but registry's `register`
   (`deck_sdi_registry.c:19`) does not consult any `@needs.services`
   version range. Loader stage 6 (`deck_loader.c:583 stage6_compat`)
   checks runtime/edition but not per-service compatibility.
4. **Block — service discovery (SERVICES §11) missing.** No
   `system.services.list()` / `describe()` surface.
5. **Block — service authoring model (SERVICES §4.2
   `:deck-app`, §4.3 `:language-integrated`) absent.** There is no
   way for a Deck app to `@service "id"` and have the OS register it
   in the service registry; `@service` is parsed-and-discarded in
   `deck_runtime`.
6. **Misalign — `system.info` vs `system.platform`.** SERVICES §21
   renamed the service to `system.platform` and gave it a specific
   shape (`versions()`, `device_id()`, `free_heap()`, `reset_reason()`,
   `boot_count()`, …). `deck_sdi_info.c:58` still registers as
   `system.info`; `apps/demo.deck:86` uses `system.platform`.
7. **Misalign — `system.battery` is not in the SERVICES catalog.**
   Per SERVICES §23 the concept moved into `system.power`.
   `deck_sdi_battery.h` exposes a standalone battery driver that no
   longer matches any spec service name.
8. **Misalign — `bridge.ui` SDI is treated as a general service
   driver.** Per BRIDGE §4 the vtable is vastly richer than the
   3-method contract in `deck_sdi_bridge_ui.h`; and per BRIDGE §1 it
   is **not** a Deck-facing capability, so it shouldn't appear in
   any app-visible service list that the catalog generates.

---

## components/deck_shell

**What it currently implements.** Legacy C-app shell (not Deck
apps): lockscreen, navbar wiring, rotation restore, intent registry,
four bundled C demos (Settings, Counter, TaskMan, Net Hello), and a
scanner (`deck_shell_deck_apps.c`) that reads `/deck/apps/*.deck` and
launches up to 4 slots. A DL1-only fallback shell
(`deck_shell.c`) simply reads the first `.deck` at `/` via SPIFFS.

**Spec map.** SERVICES §22 `system.apps` (lifecycle, registry, launch,
badges); SERVICES §34 `system.scheduler`; BRIDGE §10 activity stack +
§24 navbar + §34 lockscreen; CAPABILITIES §1 app→service discovery.

**Gaps.**

1. **Block — launcher is a hard-coded C grid**
   (`deck_shell_dl2.c:95–148`), not driven by a registry.
   `apps/demo.deck` with `@app.id = "deck.conformance.demo"` gets no
   intent-router entry; the launcher only wires `APP_ID_TASKMAN=1`,
   `APP_ID_COUNTER=4`, `APP_ID_NET_HELLO=7`, `APP_ID_SETTINGS=9`,
   plus four dynamic .deck slots. SERVICES §22 mandates the launcher
   be a consumer of `system.apps.list()` + `system.apps.launch(id)`.
2. **Block — `system.apps` service not implemented.** No
   `launch/list/current/kill/install/uninstall/load_error` surface.
   The shell invokes `deck_runtime_run_on_launch` directly
   (`deck_shell.c:72`).
3. **Block — `system.apps.load_error(app_id) -> LoadError?` absent.**
   LANG §11.3 + SERVICES §22 require the shell to surface structured
   load errors; the shell only logs with `ESP_LOGE`
   (`deck_shell.c:65`).
4. **Block — activity-stack identity.** `deck_bridge_ui_activity_t`
   (bridge_ui.h:106–112) carries `app_id`+`screen_id`+`state`+
   `lvgl_screen` but no `machine_id` or `state_id`. BRIDGE §10
   requires the triple; screen_id isn't the same concept.
5. **Block — back navigation doesn't consult the app.** Navbar BACK
   just calls `deck_bridge_ui_activity_pop()` via the shell's hook
   (`deck_shell_dl2.c`, `deck_bridge_ui_navbar.c:32`). LANG §14 +
   BRIDGE §18 require the OS to give the app's `@on back` handler
   the first chance to return `:handled`/`:unhandled`/`:confirm`; if
   `:confirm`, the bridge raises a Confirm Dialog before the actual
   pop. None of that routing exists.
6. **Block — intent pipeline is shell-local.** `deck_shell_intent`
   wires a generic `intent_t {app_id, screen_id, data}` (intent.h);
   this is pre-3.0 terminology. SERVICES §36 `system.intents` is a
   first-class service for launching deep links + cross-app
   navigation; absent.
7. **Misalign — DL1 fallback shell (`deck_shell.c`) reads first
   `.deck` from `/` and runs it as the only app.** Hard-coded
   "launch the first file" semantics clash with the spec's multi-app
   model and pin the launcher identity to slot 0.
8. **Dead — bundled C demo apps.** `deck_shell_apps.c` (Counter, Task
   Manager, Net Hello) are written in C using `lv_obj_*` directly.
   `LANG §0` invariant 1 + `BRIDGE §0` invariant 3 make that
   architecturally illegal for an "app" in the spec sense. They're
   pre-3.0 demo scaffolding that should go.

---

## components/deck_conformance

**What it currently implements.** A boot-time harness that runs
C-side self-tests (`deck_runtime_selftest`, lexer/parser/loader/
interp selftests) and a ~60-entry `.deck` regression suite from
`/deck/conformance/*.deck`, capturing ESP_LOG output and matching
sentinel strings.

**Spec map.** BUILTINS §24 (conformance tests), LANG §11 (error
codes the harness asserts on), BRIDGE §20 invariants, SERVICES §6
shared error atoms.

**Gaps.**

1. **Block — the harness does not exercise the spec's pillar
   documents.** It tests DL1/DL2 features (`lang.literals`, `lang.fn.*`,
   `lang.lambda.*`, `lang.tco.deep`, `os.math`, `os.text`, etc.) against
   the pre-3.0 `01-deck-lang.md`/`02-deck-app.md`/`16-deck-levels.md`
   axes (see comments at `deck_conformance.c:146–202`). There is no
   test row for:
   - `@needs` / `@grants` / `@handles` / `@service` / `@migrate`
     parsing,
   - the 9 spec `LoadErrorKind` values (harness has ~8 load-error
     tests but they map to the 21-code C enum, not the 9 atoms),
   - BUILTINS modules missing from the interpreter (`stream`, `json`,
     `rand`, `option`, `result` full surface, `record`),
   - BRIDGE inference rules (content-tree-bounds, `:confirm` routing,
     destructive-colour, action composition, modal-vs-inline
     dispatch),
   - CAPABILITIES aliasing semantics (evaluated once at bind time
     §9.1).
2. **Block — `apps/demo.deck` (the 1988-line "hard-final" conformance
   app) is not part of the harness.** It lives in `/apps/` not
   `/conformance/`, and `DECK_TESTS[]` does not reference it; the
   "if this file parses, type-checks, and runs to its `ALL_PASS`
   sentinel without panic, the implementation conforms to Deck 3.0"
   contract has no executor.
3. **Misalign — sentinel contract.** Tests use `DECK_CONF_OK:<name>`
   (deck_conformance.c:55), demo.deck uses `ALL_PASS:demo.conformance`
   (demo.deck:49). Even if demo.deck were added, the harness's
   substring search would have to be extended.
4. **Underspec — no timing / bridge envelope checks.** BRIDGE §5.3
   says content decode ≤ 50 ms, intent ack ≤ 5 ms. Harness captures
   `duration_us` + samples but only uses them for OUTLIER detection,
   not for gating against bridge envelope targets.

---

## components/board

**What it currently implements.** The ESP32-S3 Waveshare HAL —
`hal_lcd`, `hal_touch` (declared in header only), `hal_backlight`,
`hal_ch422g`, `hal_battery`, `hal_rtc`, `hal_sdcard`, and pin
definitions in `include/hal_pins.h`.

**Spec map.** SERVICES §0 ("platform primitives live under the SDI"),
BRIDGE §4 physical-display stratum, CyberDeck reference-platform
appendix.

**Gaps.**

1. **Good — HAL containment holds.** `rg hal_` across `deck_runtime`,
   `deck_bridge_ui`, `deck_shell` returns nothing. HAL symbols only
   reach `components/deck_sdi/src/drivers/*.c` (battery, display,
   plus ch422g/backlight pulled in by the display init). This
   satisfies SERVICES §0's layering invariant.
2. **Misalign — display/touch init ordering bleed.** `hal_lcd`
   implicitly re-writes the CH422G OUT register (backlight clearing
   gotcha per `CLAUDE.md`), requiring `hal_backlight_on()` to be
   called *after* `hal_lcd_init()`. That sequencing leaks into
   `deck_sdi_display_esp32.c:34–45`. Not a spec violation — the SDI
   absorbs it — but it is the kind of hardware-specific idiom that
   must not leak further up.
3. **Block (indirect) — no SDI `system.power` backend.** HAL exposes
   `hal_battery_read_{mv,pct}` but SERVICES §23 `system.power` asks
   for `battery_level`, `is_charging`, `sleep(duration)`,
   `reboot(:reason)`, `thermal_state()`. Only the first two are
   wired, via the pre-3.0 `system.battery` driver.

---

## Appendix — Files scanned

Specs (read-only):
`deck-lang/LANG.md`, `deck-lang/SERVICES.md`,
`deck-lang/CAPABILITIES.md`, `deck-lang/BUILTINS.md`,
`deck-lang/BRIDGE.md`, `apps/demo.deck`.

`components/deck_runtime/`:
- `include/deck_alloc.h`, `deck_arena.h`, `deck_ast.h`, `deck_dvc.h`,
  `deck_error.h`, `deck_intern.h`, `deck_interp.h`, `deck_lexer.h`,
  `deck_loader.h`, `deck_parser.h`, `deck_runtime.h`, `deck_types.h`
- `src/deck_alloc.c`, `deck_arena.c`, `deck_ast.c`, `deck_dvc.c`,
  `deck_error.c`, `deck_intern.c`, `deck_interp.c`,
  `deck_interp_test.c`, `deck_lexer.c`, `deck_lexer_test.c`,
  `deck_loader.c`, `deck_loader_test.c`, `deck_parser.c`,
  `deck_parser_test.c`, `deck_runtime.c`, `deck_types.c`

`components/deck_bridge_ui/`:
- `include/deck_bridge_ui.h`, `deck_bridge_ui_internal.h`
- `src/deck_bridge_ui.c`, `deck_bridge_ui_activity.c`,
  `deck_bridge_ui_decode.c`, `deck_bridge_ui_lvgl.c`,
  `deck_bridge_ui_navbar.c`, `deck_bridge_ui_overlays.c`,
  `deck_bridge_ui_statusbar.c`

`components/deck_sdi/`:
- `include/deck_sdi.h`, `deck_sdi_registry.h`
- `include/drivers/deck_sdi_battery.h`, `deck_sdi_bridge_ui.h`,
  `deck_sdi_display.h`, `deck_sdi_fs.h`, `deck_sdi_http.h`,
  `deck_sdi_info.h`, `deck_sdi_nvs.h`, `deck_sdi_security.h`,
  `deck_sdi_shell.h`, `deck_sdi_time.h`, `deck_sdi_touch.h`,
  `deck_sdi_wifi.h`
- `src/deck_sdi_registry.c`, `deck_sdi_strerror.c`
- `src/drivers/deck_sdi_battery_esp32.c`,
  `deck_sdi_bridge_ui_skel.c`, `deck_sdi_display_esp32.c`,
  `deck_sdi_fs_spiffs.c`, `deck_sdi_http_esp32.c`,
  `deck_sdi_info.c`, `deck_sdi_nvs_esp32.c`,
  `deck_sdi_security_esp32.c`, `deck_sdi_shell_stub.c`,
  `deck_sdi_time.c`, `deck_sdi_wifi_esp32.c`

`components/deck_shell/`:
- `include/deck_shell.h`, `deck_shell_apps.h`,
  `deck_shell_deck_apps.h`, `deck_shell_dl2.h`,
  `deck_shell_intent.h`, `deck_shell_lockscreen.h`,
  `deck_shell_rotation.h`, `deck_shell_settings.h`
- `src/deck_shell.c`, `deck_shell_apps.c`,
  `deck_shell_deck_apps.c`, `deck_shell_dl2.c`,
  `deck_shell_intent.c`, `deck_shell_lockscreen.c`,
  `deck_shell_rotation.c`, `deck_shell_settings.c`

`components/deck_conformance/`:
- `include/deck_conformance.h`
- `src/deck_conformance.c`

`components/board/`:
- `include/hal_backlight.h`, `hal_battery.h`, `hal_ch422g.h`,
  `hal_lcd.h`, `hal_pins.h`, `hal_rtc.h`, `hal_sdcard.h`,
  `hal_touch.h`
- `hal_backlight.c`, `hal_battery.c`, `hal_ch422g.c`, `hal_lcd.c`,
  `hal_rtc.c`, `hal_sdcard.c`
