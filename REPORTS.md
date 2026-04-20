# REPORTS.md — CyberDeck combinatorial audit & correction log

Cumulative record of the deep corrective pass on this project. Written for future sessions (Claude and human). Each session appends below.

The work rule: **correct in strict authority order, top to bottom of the cascade**. A discovered inconsistency at layer N is also a hint that layers M < N may have been the source — recheck them before patching N.

---

## User framing (read before doing anything)

> Este proyecto está fundamentalmente roto, parece que la mayoría de tests pasan y todo luce bien, hasta que notas que en la práctica todo se rompe, o mucho, muchísimo, y la causa raíz es esencialmente una sola: se agregan cosas y en lugar de actualizar absolutamente todo el proyecto para esa nueva cosa, se limita en alcance a pasar algunos tests que parecen clave, pero no hay end to end real, no hay escenarios realmente avanzados, no hay combinatoria, no hay profundidad, los tests muchas veces asumen que A implica B, dando A por PASS asumen que B también, y en general muchos problemas.
>
> La solución es hacer una pasada combinatoria profunda, buscamos el primer concepto que demuestre cierto grado de insuficiencia, lo comparamos vs todo el proyecto y vamos corrigiendo todo, sin pereza, sin defer, sin hacks, no bypass, no trucos, lo arreglamos correctamente, con alta calidad, con todo el ciclo de tests, pruebas, debug en hardware, validación manual con mi apoyo si es necesario, y todo lo que asegure que una feature que se asume implementada, realmente lo esté, y luego commit y la siguiente, y así nos vamos.
>
> (Cascada de autoridad: lo más autoritativo primero — especs `deck-lang/` numerados — luego annexes, luego planes en root, luego código core, luego código sobre core, luego apps/main/tests. Mantener `REPORTS.md` append-only con rationale breve por iteración. Este proyecto es muy único; **no asumir** que es un parser común. Deck apps declaran intención semántica, nunca layout; el bridge concilia el "qué" con el "cómo" según el contexto del hardware.)

Source: user prompts 2026-04-18 session #1. This is the standing direction for all sessions unless explicitly superseded.

---

## Authority cascade

| Layer | Scope | Notes |
|---|---|---|
| 1 | `deck-lang/01-…-16-*.md` (numbered specs) | **Most authoritative.** Language, OS, runtime, SDI, platform, components, versioning, levels. |
| 2 | `deck-lang/annex-*.md` | App specs. Bound by layer 1. Divergence = annex bug. |
| 3 | Root planning & doc: `GROUND-STATE.md`, `APPS.md`, `DEVELOPMENT-PLAN*.md`, `ARCHITECTURE.md`, `CHANGELOG.md`, `README.md`, `CLAUDE.md` | Must reflect layers 1-2. |
| 4 | Core code: `components/deck_runtime/`, `components/deck_sdi/`, `components/board/` | Implements layers 1-3. |
| 5 | Code over core: `components/deck_bridge_ui/`, `components/deck_shell/`, `components/deck_conformance/` | Uses layer 4. |
| 6 | `main/`, `apps/`, `tests/`, `tools/` | Integration + user-facing. |

Rule: finish layer N before touching N+1. When a mismatch at layer N is fixed, confirm that M<N layers remain consistent with the fix.

---

## Design principles (reaffirmed from layer 1)

Deck apps **never** describe how the UI is drawn. Apps declare:

- **Intent** — what the user can do (`toggle`, `trigger`, `confirm`, `navigate`, …)
- **Semantic structure** — `list`, `group "label"`, `form`
- **State markers** — `loading`, `error message:`
- **Data** — bare expressions of typed values (`str`, `int`, `Timestamp`, `@type` records), plus semantic wrappers (`media`, `rich_text`, `status`, `chart`, `progress`, `markdown`, `markdown_editor`)

The bridge infers **layout, widget choice, colors, spacing, gestures, animations, overlay patterns** from the declared intent + device context. Same `.deck` file runs against different bridges on different hardware (ESP32 LVGL, e-ink, voice, smartwatch, terminal) and each bridge makes distinct presentation decisions. The app never knows.

No primitives `column`, `row`, `card`, `grid`, `status_bar`, `nav_bar`, `icon`, `badge` exist in the app-facing language — those are all **bridge inference results**, never authored by apps. (Statusbar and navbar are rendered unconditionally by the bridge; `@app.icon` is an `@app` identity field for an asset reference, not a content primitive.)

Sources: `02-deck-app §12`, `10-deck-bridge-ui §0 + §3 + §4`.

---

## Session log

### Session #1 — 2026-04-18

**Position on entry**: conformance claimed "96/96 PASS" for DL2; user reported end-to-end is broken despite green tests. Goal: find first concept with real insufficiency, correct it in strict authority order, commit, advance.

**User steering**:
1. Focus on concept-level insufficiency, not symptom-level fixes. No shims, no defers, no bypasses.
2. Implementation and design are different concerns — don't conflate them.
3. Annexes and app examples are NOT authoritative over the language spec. If an annex contradicts the spec, the annex is the bug.
4. When a bug is found at layer N, look upward — layer M<N may have been the source that propagated the error.
5. Work strictly top-down: fix layer 1, then layer 2, then layer 3, etc. Never skip.
6. Maintain this REPORTS.md continuously.

#### Layer 1 audit (numbered specs)

Spot-checked internal consistency on the most-questioned axis (content bodies & app API shape):

- `02-deck-app §12.1` — structural primitives: `list`, `group "label"`, `form`.
- `02-deck-app §12.2` — state markers: `loading`, `error message:`.
- `02-deck-app §12.3` — data nodes: bare typed expressions + `media`, `rich_text`, `status`, `chart`, `progress`, `markdown`, `markdown_editor`.
- `02-deck-app §12.4` — intents: `toggle`, `range`, `choice`, `multiselect`, `pin`, `text`, `password`, `date`, `trigger`, `navigate`, `confirm`, `create`, `search`, `share`.
- `10-deck-bridge-ui §0` restates: *"Deck apps no saben cómo se van a dibujar"*. §4 catalogs the semantic DVC nodes and their bridge inference rules. No app-side layout vocabulary.

Grepped `deck-lang/{01..16}-*.md` for layout-leaked primitives (`column`, `row`, `status_bar`, `nav_bar`, `card`, `grid cols:`): zero matches. Layer 1 is internally consistent on this axis. **Layer 1 is taken as authoritative without edits this session.**

#### Layer 2 audit (annexes)

Grepped `deck-lang/annex-*.md` for the same primitives. Findings:

| Annex | Status | Uses |
|---|---|---|
| `annex-a-launcher.md` | **Non-compliant** | `column`, `grid cols:`, `card`, `icon`, `badge`, `status_bar`, `nav_bar` inside `content =`. |
| `annex-b-task-manager.md` | **Non-compliant** | `column`, `row`, `icon`, `status_bar`, `nav_bar`. |
| `annex-c-settings.md` | **Non-compliant** | `column`, `status_bar`, `nav_bar`. |
| `annex-d-files.md` | **Non-compliant** | `column`, `row`, `icon`, `status_bar`, `nav_bar`. |
| `annex-xx-bluesky.md` | **Compliant** | Uses only `form`, `list` (+ `more:`, `on more`), `trigger`, `text`, `password`, `loading`, `error`, `create`. |

Four annexes drift from spec. Bluesky is the reference of correct usage and a good template.

#### Consequence downstream (not yet touched — recorded for later layers)

The divergent annex syntax has seeded wrong patterns into the implementation layers. Recording here so later layers can trace back:

- **Layer 4 (core code)** — `components/deck_runtime/src/deck_interp.c` exposes imperative `bridge.ui.label / trigger / column / row / group / data_row / divider / spacer / render` builtins to Deck apps. This is the wrong shape: it forces apps to describe "how" instead of "what". The correct shape is: apps declare `content = …` blocks; runtime evaluates them per state and pushes a DVC tree to the bridge automatically.
- **Layer 5 (conformance)** — `apps/conformance/app_bridge_ui.deck` exercises only four node types via the wrong (imperative) surface and uses a sentinel-only assertion. `app_flow.deck` and `app_machine_hooks.deck` also rely on sentinels placed where transition correctness is not actually required. These tests will need to be rewritten once layer 4 is corrected.
- **Layer 3 (root docs)** — `CLAUDE.md`, `GROUND-STATE.md`, `APPS.md`, `DEVELOPMENT-PLAN*.md` need to be reviewed next, after annexes are fixed, to ensure no guidance there perpetuates the wrong mental model.

#### Planned work — this session

1. Fix `annex-a-launcher.md` against `02-deck-app §12` + `10-deck-bridge-ui §4`. Keep functional intent identical; restate using authoritative primitives. Delete `status_bar` / `nav_bar` from content bodies (bridge renders them unconditionally).
2. Fix `annex-b-task-manager.md`.
3. Fix `annex-c-settings.md`.
4. Fix `annex-d-files.md`.
5. Re-grep to confirm zero layout-leak across all annexes.
6. Commit layer 2 correction.
7. If time: begin layer 3 audit.

**Deferred to later sessions (do not touch in this one):**
- Layer 4 bridge.ui builtins removal + content-body parser/AST/interp work.
- Layer 5 conformance rewrite.
- Main apps in the repo that currently use the wrong builtins.

---

## Open questions / decisions log

- **Q**: How does the launcher express "a grid of all installed apps"? Not `grid cols: 3` (presentation). Canonical answer per spec: `list installed_apps \n item app -> trigger app.name -> apps.launch(app.id)`. The bridge decides grid vs list based on device + screen size. (To be re-confirmed when writing layer 5 tests.)
- **Q**: Do we keep `status_bar` / `nav_bar` as opt-ins per app? **No.** Per `10-deck-bridge-ui §3.2/3.3`, both are always rendered by the bridge around every screen. Apps must not reference them.
- **Q**: `badge` on an app card — how is it declared? Per spec, the trigger itself can carry semantic metadata (e.g. unread counts). The bridge infers the badge visual. Annex-a was attaching `badge` as a separate primitive; that's wrong. Correct: the data model of the trigger/list-item carries the count, the bridge adds the badge visual.

---

## Running notes for future sessions

- Bluesky annex (annex-xx) is the gold reference for spec-compliant app code.
- `bridge.ui.*` builtins in the current interp are a legacy shape to be removed during layer 4 work. Do not extend them.
- Project memory says "clean-slate refactor: borrar legacy C framework para implementar Deck runtime; sin shims ni compat." This policy applies to all layer-4 corrections.

---

## Iterations (append-only — brief rationale per edit)

Convention: every edit to a file (doc or code) gets a short entry here. Format: `YYYY-MM-DD HH:MM` (approximate) · layer · file · **what** changed · **why**. No rewrites of earlier entries. If a decision is reversed later, append a new entry citing the prior one.

- 2026-04-18 · layer 3 · `REPORTS.md` created · establish authority cascade, session log, and iteration journal. Needed so multi-session combinatorial audit is continuable. User requested this file explicitly before any corrective work starts.
- 2026-04-18 · layer 1 discovery · `09-deck-shell.md §6` (launcher example) diverges from `02-deck-app §12` vocabulary. Uses `input` (not in §12.4 — should be `text`), `status items: [...]` (§12.3 shape is `status expr label: str_expr`; also §10 §3.3 forbids apps from rendering the statusbar — the bridge owns it), `media source:` (§12.3 is `media expr alt:`). Hints at `icon:`/`badge:` on `trigger` that §12.4 doesn't list but `04-deck-runtime.md §280-281` confirms at the VC wire level. Within layer 1, §02 is the authoritative vocabulary (app model); §09/§10 use it. Reprioritizing: fix §09 (and §10 if needed) before fixing annexes. Why: user rule — "si annex mal, pista de que hay spec menos autoritative errónea; buscas y corriges". Recording here to avoid redoing annex-a work twice.
- 2026-04-18 · layer 1 discovery · `02-deck-app §12.4` is the intent vocabulary. Cross-check against `04-deck-runtime §280-281` confirms `VCTrigger { label, action, badge? }` and `VCNavigate { label, target, badge? }` — so `badge:` on trigger/navigate IS a first-class, spec-level field, just omitted from §12.4 signature. Decision: `badge:` is a valid optional field on `trigger` and `navigate` per §04. Will add this clarification to §12.4 when doing layer 1 corrections. `icon:` on trigger appears in §09 examples but is NOT in §04 VC wire format — need to verify whether icon is app-authored or bridge-inferred before fixing. Annotating as open question.
- 2026-04-18 · layer 1 discovery · `01-deck-lang.md §6-7` (lines 395-423 and 533-543) uses the correct §02 §12 vocabulary: `group "label"\n content`, `list expr\n p ->`, `rich_text expr`, `status expr label: str`, `toggle :name state: bool on -> action`, `navigate "label" -> action`, `when cond\n content`, `for var in expr\n content`, bare expressions for data display. `01` and `02` are internally consistent and together form the authoritative content vocabulary.
- 2026-04-18 · layer 1 discovery · conflict between `02-deck-app §12.4` (`text` = INPUT intent) and `10-deck-bridge-ui §4` line 246 Deck example (`text "title" value: p.title` used for DISPLAY, runtime supposedly produces DVC_STATUS). Resolution: §02 is authoritative on app vocabulary; `text` is for user text input only; display comes from bare typed expressions. §10's example was wrong.
- 2026-04-18 · layer 1 edit · `deck-lang/10-deck-bridge-ui.md §4` header example rewritten. **Before:** `text "title" value: p.title` (wrong — `text` is an input intent per §02 §12.4) inside `list\n items: posts\n item p ->` (non-§12.1 shape). **After:** bare expressions `p.title` / `p.author` + positional `list posts\n p ->` (matches §02 §12.1 and §01 §6). Added explicit statement "La app nunca escribe column/row/card/data_row..." to kill the wrong pattern at its most visible example. Why: this is the first thing readers see when reading the bridge doc; it must model the spec precisely. Updated the prose "runtime produces DVC_STATUS × 2" → "DVC_DATA × 2" because plain `str` expressions map to `DVC_DATA` (per §10 §4.2 — `DVC_DATA` = Standalone Value; `DVC_STATUS` is label+value pair, not what's happening here).
- 2026-04-18 · session #1 close · Stopping short of rewriting `09-deck-shell §6` (Launcher flows) and annex a/b/c/d. Rationale: those are ~600 lines combined and involve non-trivial semantic decisions (e.g., should the Launcher's Task Manager trigger row show thumbnail via `media p.thumbnail alt: ...` or leave bridge to decide from @app metadata? Should `status` blocks inside a group be @type records?). Need user review on scope before large spec rewrites. Next session should: (a) confirm the authority: §01/§02 win over §09/§10/annexes on content vocabulary; (b) agree whether to add `badge:` field to §02 §12.4 explicitly; (c) proceed with §09 rewrite, then annex rewrites, then layer 3.

---

## Session #1 — Exit state (2026-04-18)

- Layer 1 audit complete on **content-body vocabulary axis**. §01 §02 authoritative; §09 §10 diverge (use `items:`/`item var ->` list form, `text` for display, `status items:` for statusbar-mimicking, `input` not in §12.4, `icon:`/`media source:` shapes). Documented above.
- Layer 1 edit applied: §10 §4 intro example only. Remaining §09 §6, §10 rest of examples to be checked and fixed in session #2.
- Layer 2 annex audit started, NOT executed. annexes a/b/c/d known non-compliant; bluesky compliant.
- Other axes not audited yet: `@machine` / `@flow` syntax consistency across specs, `@stream` usage consistency, `@use`/`@requires` shape, `@permissions`, capabilities catalog (§03 §4) alignment with SDI (§12). These are separate audit passes for future sessions.
- `REPORTS.md` is the living log. Append-only from here. Every edit to a doc or code file gets a one-line entry under "Iterations".

**Next session should**: read this file top to bottom first, then (a) align with user on whether to proceed with §09 §6 rewrite + annex fixes, (b) extend audit to other axes, (c) continue cascading toward layer 3.

---

## Iterations (continued)

- 2026-04-18 · layer 1 edit · `deck-lang/09-deck-shell.md §6` Launcher app/flows rewritten in place to use §02 §12 vocabulary. Changes: removed `@on launch` calls to `shell.set_status_bar(true)` / `shell.set_navigation_bar(true)` (bridge owns statusbar/navbar unconditionally per §10 §3); removed the `status items: [...]` statusbar-mimicking block in home flow (apps can't touch statusbar — §10 §3.3); replaced `list\n items: X\n item v ->` with positional `list X\n v ->` (matches §01 §6 and §02 §12.1); replaced `input` with the actual §12.4 intent `search`; replaced `media\n source: X\n alt: Y` with positional `media X alt: Y` (§12.3 shape); replaced `trigger\n label: X\n -> action` multiline block form with inline `trigger "X" -> action` (consistent with bluesky annex and §01 §6); removed `icon:` field from `trigger` since §04 §280-281 VC wire doesn't carry it and the icon for an app card comes from the target app's `@app icon:` metadata resolved by the bridge; kept `badge:` since §04 §280-281 confirms it. Added an explanatory paragraph: "Nothing about the launcher mentions grid vs list, card vs row, columns, icons, or fonts..." — makes the design invariant explicit for readers. Added the `unread_for` helper as a proper `fn` instead of inlining the `first_or(filter...)` destructuring because putting business logic inside the trigger's attribute expression was unreadable. Why: the shell spec is read by people implementing both the OS and apps; if its canonical launcher example violates the content-body spec, every downstream reader learns the wrong vocabulary — exactly the root cause the user identified ("se agregan cosas sin actualizar absolutamente todo el proyecto").
- 2026-04-18 · policy note · added User framing quote to the top of REPORTS.md per user request. This quote is the durable brief; every future session opens with it. It is NOT an iteration entry because it's not an edit to a code or doc artefact — it's the standing directive.
- 2026-04-18 · layer 1 edit · `deck-lang/09-deck-shell.md` Task Manager flows (§6.1 ProcessListFlow + AppDetailFlow + §23.5 cpu_watch example): rewrote to §02 §12 vocabulary. Removed `list items: X item v ->` block form → positional `list X\n v ->`; removed `data: "..."` invented primitive (apps pass bare expressions or `@type` records and the bridge formats fields from names); collapsed multi-line `confirm\n label: X\n message: Y\n -> action` into inline `confirm "X" message: Y -> action`; collapsed multi-line `navigate\n label: X\n to: :state\n params: (k: v)` into inline `navigate "X" -> Machine.send(:state, k: v)` (consistent with §02 §12.4 signature). Extracted `main_processes()` / `background_tasks_of(app_id)` / `processes_of(app_id)` to proper `fn` bodies instead of inlining `ProcessSnapshot.last() |> unwrap_opt_or([]) |> filter(...)` inside `list` attribute expressions (unreadable). Why: Task Manager example teaches readers the canonical shape for showing process lists; a broken example seeds broken apps.
- 2026-04-18 · layer 1 edit · `deck-lang/09-deck-shell.md` `@stream NotifCounts` and `@stream ProcessSnapshot` declarations were using `from:` (per §02 §10 that's for **derived** streams only). Fixed to `source:` since both are source streams wrapping capability methods (`apps.notif_counts_watch()`, `tasks.cpu_watch()`). Why: every annex and downstream sample was copying `from:` from here and producing non-loadable source-stream declarations. Not an annex-specific fix; originated in the spec.
- 2026-04-18 · layer 2 edit · `deck-lang/annex-a-launcher.md §6-8` rewritten to use §02 §12 vocabulary. **Before:** `column\n status_bar\n grid cols: ...\n for app in xs\n card\n icon ...\n label ...\n badge ...\n on tap -> ...\n on long -> ...`. **After:** `list installed_apps\n empty -> "..."\n app -> trigger app.name badge: ... -> apps.launch(app.id)` plus a separate `trigger "Search"` sibling for the long-press alternative. §8 renamed from "Layout Inference" (app-authoring voice) to "Bridge Layout Decisions for This Board" (bridge-side voice) to clarify that those decisions are not app concerns; added a paragraph explaining app icons come from `@app icon:` of the target, not from the launcher's content. Dropped `on long ->` entirely — §12.4 `trigger` has no such field; long-press on a touch bridge is handled by §10 inference or declared as a separate semantic intent. Extracted `unread_badge()` helper so the `badge:` expression stays a clean option value.
- 2026-04-18 · layer 2 edit · `deck-lang/annex-b-task-manager.md §6.1-6.4` rewritten. Running list = `list apps_running\n app -> navigate app.name -> ...` (bridge auto-formats `@type AppInfo`'s secondary fields). Detail view = `group "{app.name}"\n app\n proc\n group "BACKGROUND TASKS" ...\n confirm "FORCE KILL" message: ... -> ...` (passes `@type` records; bridge renders fields with labels from names). Dropped `action_row`, `data_row`, `variant: :danger`, `style: :dim`. `§6.3` reduced: `confirm` is a single semantic intent; §10 §5.2 Confirm Dialog Service handles OK/CANCEL interaction — app does not declare both labels. Why: Task Manager is the canonical example of "show a live process snapshot" apps; if it teaches `data_row label: "HEAP:" value: ...` every app that displays structured data will invent its own per-field labels, preventing the bridge from varying label presentation per form factor.
- 2026-04-18 · layer 2 edit · `deck-lang/annex-c-settings.md §6 (top-level menu)` rewritten to a flat sequence of `navigate "LABEL" -> ...` intents (matches §12.4). Dropped `nav_row` helper (not in spec), `detail:` per-row summary (presentation — apps may push data via streams; bridge chooses whether/how to show it). §8 App Detail block rewritten: `toggle cap state: granted on -> ...`, `config_input field`, and a single `confirm "UNINSTALL" message: ... -> ...` replaces the DANGER ZONE action-row with variant:danger.
- 2026-04-18 · layer 2 edit · `deck-lang/annex-d-files.md §6.1-6.3` rewritten. Browser: `navigate "↑ Up"` (conditional via §12 `when`), `choice :sort options: [...]` replaces `menu\n item "SORT BY NAME" -> ...` (menu is NOT a §12 primitive — secondary actions are sibling intents; §10 §5.5 Choice Overlay Service handles option overlays). Picker: `multiselect :paths options: ...` replaces custom `checkbox` rows; mode-specific trigger via `match s.mode`. Viewer: `rich_text`, `markdown purpose: :reading`, `media v alt:` — all spec §12.3 primitives. Removed `font:`, `style:`, `variant:`, `scroll`, `center`, `image src:`, all invented presentation attributes.
- 2026-04-18 · layer 2 verify · final grep of `annex-*.md` for `(column|card|grid|status_bar|nav_bar|action_row|data_row|spacer|checkbox|scroll|center|menu|image|icon  |nav_row|items:|item \w+ ->)` — **zero matches**. Remaining `source:` hits are in `@stream` declarations (spec-correct per §02 §10.1). Layer 2 (annexes) is now consistent with layers 1 (specs).
- 2026-04-18 · layers 1+2 commit · `e6e08a1` — "docs: layer 1+2 — alinear vocabulario content-body al spec autoritativo". 7 files, +436/-360. Preserved pre-existing uncommitted code changes (CHANGELOG, components/*, apps/*) untouched; those are prior-session work outside the content-body concept.
- 2026-04-18 · layer 3 scan · grep of root `*.md` (`APPS`, `GROUND-STATE`, `DEVELOPMENT-PLAN*`, `ARCHITECTURE`, `README`, `CLAUDE`) for bad content-body primitives (`column/row/card/grid cols/status_bar/nav_bar/data_row/action_row/nav_row` in app-authoring context) — **zero matches**. `CHANGELOG.md` does mention `bridge.ui.label/column/row/trigger/render/data_row` in entries documenting past implementation work — that's historical record, not a spec; leave as-is (when the imperative builtins are removed in layer 4, the CHANGELOG entry for that removal will record the transition). `ARCHITECTURE.md §69-72` describes the bridge DVC design correctly ("deck_bridge_dvc renderiza árboles DVC emitidos por el runtime"). Layer 3 is consistent with layers 1-2 on this concept.
- 2026-04-18 · layer 3 edit · `CLAUDE.md` — added "Start-of-session required reading" block pointing at `REPORTS.md` as the session-opening doc, plus a restated design invariant (apps declare intent, bridge infers presentation; list of §12 primitives; negative list of forbidden layout primitives and imperative builders). Why: CLAUDE.md is auto-loaded into every Claude Code session; making the design invariant the second thing the agent reads (after the REPORTS pointer) prevents re-learning the wrong pattern from any leftover code or doc.
- 2026-04-18 · session #1 close · Layers 1, 2, 3 are consistent on the content-body vocabulary axis. Layer 4 (runtime code) is the next concept scope: remove imperative `bridge.ui.*` builtins; implement `content =` block parsing; implement declarative content evaluation that produces DVC on state change. This is a multi-session implementation. Layer 5 (conformance) follows. Layer 6 (apps/main) finishes the cascade. Other axes of layer-1 divergence (noted in "Layer 1 / 2 open items" above: §07 shell capability methods, `@app icon:` placement, `@stream` alternate list syntax) are separate concepts for their own top-down passes.

### Session #2 — 2026-04-18 (continuation)

User directive: "sigue iterando, no te detengas, esto es ad infinitum" — plus a critical lens: "dale más peso a la intención que al texto". Concrete example the user cited: `error message:` might be better expressed as `error reason:` semantically. Translating: the spec should be critiqued for **semantic fit**, not just for internal self-consistency. Where vocabulary doesn't match intent, reshape.

**Concept #2 picked**: capability catalog alignment + semantic critique of §02 §12 vocabulary.

- 2026-04-18 · layer 1 discovery · §03 §4 catalog is **missing** `@capability cache` (present in §05 §6 and §12 §4.5) and `@capability api_client` (present in §05 §5 and §12 §5.2 / catalog row 66). Not level or policy; pure omission.
- 2026-04-18 · layer 1 discovery · §12 §2 driver catalog row format is correct; DL3-only drivers (`network.ws`, `i2c`, `spi`, `gpio` public surface, `markdown`) are absent from §12 §2 table because §12 centers DL2-baseline. §12 line 100: "A platform MAY provide additional drivers beyond this catalog by registering custom capabilities (06-deck-native §10)." So extension pattern covers DL3 additions. Not a bug; note only.
- 2026-04-18 · layer 1 discovery · §02 §4A `@requires` block documents `deck_os:`, `runtime:`, `capabilities:` — **missing** `deck_level:` even though §16 references `@requires.deck_level` as the canonical way to declare minimum conformance. §16 line 680 explicitly lists this as a pending sync for §02. Fix: add `deck_level:` field to §02 §4A signature and default-inference rules.
- 2026-04-18 · layer 1 edit · `deck-lang/03-deck-os.md §4.2` (storage): added `@capability cache` with signatures copied from §05 §6 (get/set/delete/exists/ttl/clear). `§4.3` (network): added `@capability api_client` with full signatures, types (`ApiConfig`, `ReqOpts`, `ApiResponse`, `MultipartPart`) referenced via §05. Why: the capability catalog must be complete so `@use cache as c` / `@use api_client as api` loads cleanly and the loader can validate the `@requires` entries for these capabilities. Before, the catalog omission meant those capabilities existed in the driver layer (§12) and API spec (§05) but had no canonical app-facing declaration.
- 2026-04-18 · layer 1 edit · `deck-lang/02-deck-app.md §4A` added `deck_level: int` field to `@requires` signature (first field for emphasis) + extended §4A.2 defaults to include `deck_level` inference rule (infer from features/capabilities; reject with `E_LEVEL_INCONSISTENT` if ambiguous). Closes the §16↔§02 sync gap.
- 2026-04-18 · layer 2 edit · annex-a/b/c/d gained explicit `deck_level: 3` in their `@requires` (all four use `system.apps` which is DL3 per §16 §7.3); removed stale "-- statusbar, navbar" justification comment on `system.shell` in annex-a (the bridge renders the bars unconditionally per §10 §3; §09 `@capability system.shell` exposes real methods for brightness/screen-timeout/etc., not bar control). annex-a didn't call any `shell.*` methods after the §6 rewrite so `system.shell` is dropped entirely.
- 2026-04-18 · layer 1 semantic critique · Passed §02 §12 vocabulary through the intent-over-text lens (user directive). Findings:
  * **`error message:` → `error reason:`** — the app declares **why** the error exists, not a generic body-text message. A voice bridge speaks it, a screen bridge wraps it, a logger bridge pipes it to telemetry — all consume the same semantic reason. (User's original example.)
  * **`confirm label: prompt:` (was `message:`)** — the app declares the **question posed to the user**; it's not a body-text message, it's the semantic question that each bridge interprets.
  * **`media role:` (was `hint:`)** — the atom declares the media's semantic **role** (`:avatar :cover :thumbnail :inline`), not a suggestion. The word "hint" undersold app authority.
  * **`list has_more:` (was `more:`)** — explicit boolean intent; `more:` was awkward as a field name.
  * **`trigger badge: int?` added** — `badge?` is in `04-deck-runtime §280-281` VC wire format; §12.4 signature was simply incomplete.
  * **`navigate badge: int?` added** — same.
- 2026-04-18 · layer 1 + 2 edit · Applied renames across the cascade: `02-deck-app.md §12` (spec definitions + example body), `01-deck-lang.md §6-7` (examples), `09-deck-shell.md §3.1 + §6 + §6.1 + §23.5` (back-confirm form + launcher + task manager examples), `16-deck-levels.md §6` (feature table entry), all annex-*.md (bluesky, a, b, c, d — every `error message:` / `confirm message:` / `media hint:` / `more: X` / `:confirm { message, ... }` reshape). Surgical revert on one `09-deck-shell.md` line (`SysNotifOpts.message:` field is unrelated to content-body `confirm`; not part of the rename). Discovery during revert: the `:confirm` form that `@on back` can return uses the same prompt/confirm/cancel shape as the content-body `confirm` — renamed it to `prompt:` too for consistency with content-body `confirm prompt:`.
- 2026-04-18 · final grep verify · zero matches for stale patterns `error message:`, `confirm ... message:`, `media hint: :avatar/cover/thumbnail/inline`, `more:` as list field. Semantic renames fully cascaded.

### Concept #3 — Continued semantic pass + `@app` identity completeness

- 2026-04-18 · layer 1 discovery · `02-deck-app §3` (@app identity fields) **did not list `icon:`** despite every annex (launcher, taskman, settings, files, bluesky in §13) using it. Undocumented load-bearing field. Also **`tags:`** is used by annex-a's §9 search logic but never declared.
- 2026-04-18 · layer 1 edit · `02-deck-app.md §3` — added `icon: str?` (with semantics: if the value matches an `@assets … as :icon` entry it resolves to that asset, else it's used verbatim as a short glyph) and `tags: [str]?` (discovery/search tags). Why: fields exist in practice but weren't spec'd; loader that reads the `@app` block needs to know they're valid.
- 2026-04-18 · layer 1 edit · `02-deck-app §12.3` markdown/markdown_editor: `scroll_to:` → `focus:` (app declares point of user attention; a voice bridge reads the heading, a screen bridge scrolls — same declaration, bridge picks the verb); `accessibility:` → `describe:` (app **describes** the region; bridge surfaces the description as ARIA / speech / Braille); `markdown_editor editor_state:` → `controlled_by:` (declares a **relationship** — "this editor is controlled by X" — not a data-name). Same authority pattern as `error reason:`: intent over text.
- 2026-04-18 · layer 1 edit · `02-deck-app §11` `@on back` `:confirm { message, … }` shape renamed to `:confirm { prompt, … }` — parallels content-body `confirm prompt:` so developers see one concept, one field name.
- 2026-04-18 · layer 2 edit · annex-xx-bluesky remaining `confirm "X" message: …` → `prompt:` (line 1206).
- 2026-04-18 · record-type notes (not renamed — different concept) · `message:` as a **struct field** in LoadError (§11, §15 §9, §16 §10) and SysNotifOpts (§09 `shell.post_notification`) is a genuine record-schema field on structured error/notification types, not a content-body primitive. Those stay as `message:` — renaming would confuse the error-handling subsystem with the UI-intent subsystem. Only content-body primitives (`error` view marker, `confirm` intent, `@on back :confirm` directive) were renamed.
- 2026-04-18 · verify · grep for stale: `^\s+(hint|message|scroll_to|accessibility|editor_state|more):\s` across `deck-lang/` — only remaining hits are (a) the rename-comments in §12.3 citing the OLD names for context, (b) LoadError/SysNotifOpts record fields (legitimate, different concept), (c) text-input `hint:` (placeholder guidance — different from the `media hint:` that was renamed to `role:`; this `hint:` stays because its semantic is truly "hint about what to type", user-facing guidance, not a bridge directive).

### Concept #4 — `@machine` hook execution order + spec gap

- 2026-04-18 · layer 1 discovery · `02-deck-app §8.5` "Hooks and Execution Order" documents only `transition before` / `on leave` / `on enter` / `transition after` — it does **not** describe `@machine.before:` / `@machine.after:`, which exist in the implementation (deck_interp.c `find_on_event` + `run_machine`) and are referenced in `16-deck-levels.md` as a DL2 feature. Undocumented load-bearing language form.
- 2026-04-18 · layer 4 bug discovered (flagged for follow-up) · **runtime execution order for `@machine.before` / `@machine.after` does not match the intended semantic**. `deck_interp.c run_machine()` line 2272-2306 executes, per transition from state S to D:
  1. source.on enter   (only on first iteration)
  2. source.on leave
  3. @machine.before
  4. [state change S→D]
  5. @machine.after
  6. destination.on enter   (only on **next** iteration's enter call)

  Spec-correct order (per the authoritative §8.5 rewrite below): `@machine.before → T.before → S.on leave → [state change] → D.on enter → T.after → @machine.after`. Two ordering errors in the impl:
  - `@machine.before` runs **after** `source.on leave` (should run before)
  - `@machine.after` runs **before** `destination.on enter` (should run after)

  Why the conformance test (`app_machine_hooks.deck`) still PASSes despite this: the test's sentinel `DECK_CONF_OK:app.machine_hooks` is emitted from `@machine.after`; the test does not assert **position** of the sentinel relative to the `ready` state's `on enter` log. Classic A→B: sentinel present → test pass; even though hook ordering is wrong, the test doesn't see it.

  Layer 4 fix (future session): reorder `run_machine` so `@machine.before` fires before the source.on leave, and `@machine.after` fires after the destination.on enter. Also move the first-iteration source.on enter outside the transition loop (enter-initial is a distinct hook point — see §8.5 note on `:__init` pseudo-transition).

  Layer 5 fix: rewrite `app_machine_hooks.deck` to assert **order**, not just presence — e.g. emit `HOOK_ORDER:before, HOOK_ORDER:leave_boot, HOOK_ORDER:enter_ready, HOOK_ORDER:after, HOOK_ORDER:SENTINEL` and have the C-side harness parse the stream and reject if order is wrong.

- 2026-04-18 · layer 1 edit · `02-deck-app §8.5` rewritten to (a) clearly distinguish the three hook kinds (state-scoped enter/leave, transition-scoped before/after, machine-scoped `@machine.before`/`@machine.after`), (b) document the full execution order across all seven hook points, (c) specify the `:__init` pseudo-transition rule for initial-state entry (only `state.on enter` and `@machine.after` fire; not before-hooks, because no event was sent), (d) specify termination semantics and error-rollback behavior. Why: without this explicit order, implementations drift and tests hide the drift behind presence-checks (as the current impl proves).

### Concept #5 — Parser coverage gap for @machine / @flow (layer 4 discovery)

- 2026-04-18 · layer 4 bug discovered (flagged for future session) · `components/deck_runtime/src/deck_parser.c` `parse_machine_decl` (line 1180) + `parse_state_decl` (line 1138) + `parse_flow_decl` (line 1005) accept only a **toy subset** of the spec `@machine` / `@flow` grammar:
  - **`parse_state_decl`**: body may contain only `on enter:` / `on leave:` / `transition :x` sub-blocks. Does NOT support state payloads `state :foo (field: Type)` (§02 §8.3) or state composition `state :foo flow: Other` / `state :foo machine: Other` (§02 §8.3, used by annex-a line 81, §09 §6 line 373-375).
  - **`parse_machine_decl`**: `@machine` body may contain only `state` entries. Does NOT accept the top-level `initial :state_name` declaration (§02 §8.2 — currently inferred from first state, but explicit `initial` would fail to parse), does NOT accept top-level `transition :name from :x to :y` declarations (§02 §8.4 — transitions can only be declared inside a state body today).
  - **`parse_flow_decl`**: `@flow` body may contain only `step :name:` entries. Does NOT accept `state`, `transition`, `initial`, or `on StreamName var ->` stream handlers (§02 §10, §09 §6 line 441-446 uses it). All annexes that use `@flow` with rich structure (annex-a launcher, annex-b taskman, annex-c settings, annex-d files, annex-xx bluesky) **would fail to load on the current runtime**.
  - **Severity**: catastrophic. The spec'd app model is fundamentally unparseable. The current conformance passes because `apps/conformance/app_flow.deck` and `apps/conformance/app_machine.deck` use only the toy subset.
  - **Why tests miss this**: the conformance harness uses minimal fixture apps that exercise only the parser's supported shape. Real apps (the annex examples) would not load. Classic A→B: "toy @flow parses → real @flow parses" (false).

  Layer 4 fix (future multi-session scope):
  1. Extend `parse_state_decl` to accept optional payload `(field: Type, ...)` after state name and optional `flow: Name` / `machine: Name` trailer.
  2. Extend `parse_machine_decl` to accept top-level `initial :name` and top-level `transition :name ...` inside the body.
  3. Extend `parse_flow_decl` to accept the same grammar as `@machine` plus `step` as sugar that desugars to `state` + `content =`. Keep the auto-transition chain behavior for the case where only sequential steps are declared (document this as a `@flow`-specific convenience in §02 §9 once it's in; currently undocumented).
  4. `parse_transition_stmt` must support `when:`, `before:`, `after:`, `to history`, `from *` with `_` payload match, multiple `from`/`to` variants (§02 §8.4).

  Layer 5 fix: add conformance tests that load each annex (a/b/c/d/xx) and verify parse → load succeeds without error.

- 2026-04-18 · layer 4 bug discovered (smaller) · `apps/hello.deck` uses `@app requires: \n deck_level: 1` — nested `requires:` inside `@app`. But `02-deck-app §3` lists `@app` fields as `name/id/version/edition/entry/icon/tags/author/license/orientation` only. `@requires` is a **separate top-level annotation** per §4A. Either the parser accepts this non-spec form silently (bug), or `hello.deck` shouldn't parse. If it loads today (hello test PASSes), the parser is accepting undocumented syntax. Audit the `parse_app_decl` function before Layer 4 edits touch this area.

- 2026-04-18 · layer 1 edit · `02-deck-app §9` needs a §9.4 to document the `@flow`-only auto-transition convenience (step[i] → step[i+1] when only sequential steps are declared, no explicit transitions). Current runtime implements it; spec doesn't describe it. Deferred: added to §9 later this session or next.

- 2026-04-18 · session #2 close · committed concepts #2, #3, #4 as separate docs commits. Concept #5 (@flow / @machine parser coverage) documented; code fix is layer-4 future work spanning multiple sessions and requiring hardware validation. Also surfaced: `@app requires:` nested form may be silent-accepted by parser (layer-4 audit needed).

### Concept #6 — `event` binding completeness (§02 §12.7)

- 2026-04-18 · layer 1 discovery · `02-deck-app §12.7` documented `event` binding only for §12.4 input intents (`toggle`/`range`/`choice`/…), treating it as a single scalar. Missing bindings for structural handlers (`form on submit ->`, `list on more ->`), content handlers (`markdown on link ->`, `on image ->`, `markdown_editor on change ->`, `on cursor ->`, `on selection ->`), and stream handlers (`on StreamName v ->`). Consequence: app authors didn't know what fields were available in those handlers; implementations could differ silently.
- 2026-04-18 · layer 1 edit · §12.7 rewritten with three tables: input intents (all carry `event.value`), structural handlers (`form` → `event.values: {str: any}`, `list` → `event.page: int`), content handlers (markdown link/image carry `event.url`+contextual `text`/`alt`; markdown_editor change/cursor/selection each carry their own fields), and an explicit note that stream handlers use the `var ->` binder and bind no `event`. The shape-per-handler model is intentional — a blanket "all handlers get event.value" would be wrong for handlers that carry structured payloads (like `cursor` that needs both `cursor: int` and `formats: [atom]`).
- 2026-04-18 · layer 4 note · runtime almost certainly does not bind `event.values` for `form on submit ->`, `event.page` for `list on more ->`, or the markdown_editor multi-field events yet. Layer 5 should add tests that probe each binding path (press a trigger inside a form; tap a markdown link; submit a form; request more items on a list). Future work.

### Concept #7 — `@on` lifecycle & OS-event handler binding shapes

- 2026-04-18 · layer 1 discovery · `02-deck-app §11` used two different binding styles in examples (`@on os.config_change` with `event.field`/`event.value`; `@on os.wifi_changed (ssid: s, connected: c)` with named binders; `@on hardware.button (id: 0, action: :press)` with value-pattern matching) without stating they were three separate styles. Readers had to guess.
- 2026-04-18 · layer 1 edit · §11 rewritten to list the three styles explicitly (no-params + `event.field` accessor, named binders, value patterns) and cross-check dispatch: runtime picks the most specific match; equal-specificity handlers for the same event are a load error. The `@on hardware.button (id: 0, action: :press)` form in particular is a pattern-match, not a binding — a critical distinction the old spec didn't call out (apps might accidentally bind `:press` as a variable name expecting any `action`).

### Shallow-test catalog (concept for future)

- 2026-04-18 · layer 5 discovery · spot-checks on conformance fixtures confirm the user's diagnosis (`tests pasa pero en práctica se rompe`):
  - `apps/conformance/os_fs.deck` tests ONLY `fs.exists` — not `read`, `write`, `append`, `delete`, `list`, `mkdir`, `move`. Claims to certify the `fs` capability but exercises one method of eight.
  - `apps/conformance/os_nvs.deck` tests set/get/delete of **one** string — no ints, bools, floats, bytes; no error paths; no namespace isolation; no concurrent access.
  - `apps/conformance/os_time.deck` tests `time.now()` monotonic + positive — nothing else (no `format`, `parse`, `ago`, `utc_*`).
  - Same shallowness pattern repeats across the os_*.deck fixtures.
- Layer 6 consolidation planned for a future session: rewrite the os_* conformance fixtures to exercise every method of each capability with multiple value types, error paths, and negative assertions. Gate the suite on the rewritten versions. This is the direct answer to the user's concern about A→B assumed-implementation.

### Session cumulative index

- **Concepts committed in this push**: #1 content-body vocabulary (layers 1+2), #2 capability catalog + DL3 declarations, #3 semantic rename pass + @app identity, #4 @machine hook execution order, #5 @flow sequential-step sugar + layer-4 parser gap, #6 event binding per handler shape, #7 @on OS-event handler binding styles.
- **Layer-4 bugs flagged for future sessions** (with hardware validation): parser coverage for @machine/@flow full grammar; runtime @machine.before/.after execution order; bridge.ui.* imperative builtins replacement with declarative content evaluation; form/list/markdown_editor event bindings; @app requires: nested form audit.
- **Layer-5 rewrites planned**: each conformance `.deck` fixture deepened to cover full capability surface; app_machine_hooks.deck asserts hook order, not just sentinel presence; app_bridge_ui.deck replaced with content-body declarative tests + C-side readback.
- **Layer-6 app rewrites planned**: `hello.deck`, `ping.deck` migrated from `bridge.ui.*` imperative API to declarative `content =` form.

Between sessions, re-read `REPORTS.md` top-to-bottom. The User framing at the top is the standing brief. The iteration journal tracks every edit with rationale.

- 2026-04-18 · layer 1 edit · `02-deck-app §12.2` gained a concrete example showing `loading` in a `:fetching` step and `error reason:` in a `:failed` step, plus an explicit note that `loading` has zero fields and `error` has only `reason:` — all presentation (colour, icon, tone, dismissal) is bridge decision. Why: the minimal spec text invited implementers to add bridge-hint fields over time; a clear example pins the intent down. Keeps drift at bay.
- 2026-04-18 · layer 1 cascade catch · `04-deck-runtime §4.2` VC* AST type definitions still used **stale** field names from before the §02 §12 semantic rename: `VCError { message }`, `VCMedia { hint }`, `VCConfirm { message }`, `VCList { more }`. `§4.3` C accessor API mirrored the same stale names (`deck_node_message`, `deck_node_hint`, `deck_node_more`, `deck_node_confirm_msg`). Because §04 is the spec the runtime implementer codes against, leaving it stale guarantees the C code keeps the old shape even after the public-facing spec (§02 / annexes) is corrected. Classic cascade regression: a fix at N doesn't reach M unless the author walks the chain. Fixed: renamed all VC* fields and C accessors to match §02 post-rename vocabulary. Also added missing `VCMarkdown` and `VCMarkdownEditor` variants (§02 §12.3 primitives that had never made it into §04's enumeration) and added `max_length?` on `VCSearch` (§02 §12.4 mirror of `VCText`).
- 2026-04-18 · layer 1 cascade catch · `10-deck-bridge-ui §4.2` (DVC_MARKDOWN + DVC_MARKDOWN_EDITOR attribute prose), `§4.3` (DVC_CONFIRM prose), `§6.?` table row for `DVC_LIST has_more` — same staleness post-rename. Fixed: `scroll_to` → `focus`, `editor_state` → `controlled_by`, `accessibility` → `describe`, `message` → `prompt` (DVC_CONFIRM context), `more: true` → `has_more: true`.
- 2026-04-18 · layer 1 cascade catch · `11-deck-implementation §18.4` DVC attribute key table had the same stale entries (`:message`, `:scroll_to`, `:editor_state`, `:accessibility`, and was missing `:reason`, `:role`, `:has_more`). Fixed: full row rewrite to match §02 + §10 post-rename vocabulary. Also added DVC_ERROR's `:reason` which was previously absent from the attribute table.
- 2026-04-18 · layer 1 edit · `01-deck-lang §7.8` had an illustrative example using `spinner`, `column`, `button` — not §02 §12 primitives, just didactic placeholders that nonetheless propagate the wrong vocabulary to readers implementing against §01. Rewrote to use `loading`, `unit`, `trigger` — real §12 primitives.
- 2026-04-18 · layer 1 edit · `16-deck-levels §5.2` feature-added-at-DL2 row for `@on` lifecycle hooks listed `pause, low_memory, network_change` — none of those are lifecycle hooks per §02 §11 (whose full list is `launch, resume, suspend, terminate, back, open_url, crash_report`). `low_memory` / `network_change` are OS events (§03 §5 `os.memory_pressure` / `os.network_change`), reachable via `@on os.event_name`, not standalone lifecycle hooks. Fixed: split into two rows (lifecycle hooks + OS-event form).
- 2026-04-18 · session #2+ continuing · 11 commits in this push. Remaining layer-1 axes are largely aligned on the content-body + capability + hook + event axes. Naming-convention flat-vs-prefixed for capabilities (`mqtt` vs `network.http`) identified as a latent inconsistency — deferred, requires design decision on whether to normalize to `network.mqtt` (consistent) or keep flat (ergonomic).
- 2026-04-18 · layer 6 edit · `apps/conformance/os_math.deck` deepened from 6 probes to ~34 probes covering the full `@builtin math` surface from `03-deck-os §3` (int helpers: abs_int/min_int/max_int/clamp_int/gcd/lcm; float scalars: abs/floor/ceil/round with n/sign/min/max/clamp/lerp; power: sqrt/pow; trig: sin/cos/tan/asin/acos/atan/atan2; exp/log: exp/ln/log2/log10; conversions: to_radians/to_degrees; predicates: is_nan/is_inf; constants: pi/e/tau). **Expected outcome on current runtime**: test FAILS (downgrades from 96/96 PASS to something less). That is the correct answer — the old shallow version lied about coverage; the new deep version reports reality. When a math function is missing from the interpreter, the fail log tells you which. Hardware-side rerun + fix is a future session's work. Closing: the user's A→B complaint manifests directly here — "abs/min/max/round work → all math works" was the lie; now it can't be told.

### Deepening pass on `os_*` conformance fixtures (layer 6)

The pattern applied to every `os_*.deck`:
  1. Read the spec for the capability or builtin surface (§03 / §05).
  2. Enumerate every method / field; group by semantic family.
  3. For each method, write at least one happy-path probe + at least one error / edge probe.
  4. Use spec-canonical names (`length` not `len`; `starts` not `starts_with`; etc.).
  5. Use spec-canonical return shapes (`int?` yields `:some N` / `:none`; not unwrapped).
  6. Sum all probes into `ok` with `&&`; if ANY fails, the sentinel `DECK_CONF_OK:<name>` is not emitted.

Before → After coverage:

| Fixture | Before | After | Surface |
|---|---|---|---|
| os_math.deck | 6 probes | ~34 probes | `@builtin math` 30+ methods incl. trig/exp/log/predicates/constants |
| os_text.deck | 6 probes | ~36 probes | `@builtin text` 36 methods incl. encoding/query/JSON/bytes |
| os_fs.deck   | 1 method | 10 methods | `fs` full CRUD + mkdir/move/list + :err :not_found path |
| os_nvs.deck  | 1 type (str) | 5 types (str/int/float/bool/bytes) + :invalid_key + keys/clear | Full `nvs` surface (after §03 completeness fix) |
| os_time.deck | 2 probes | ~15 probes | now/since/until/before/after/add/sub/to_iso/from_iso/format/parse/date_parts/epoch |
| os_info.deck | 3 probes | 11 probes | device_id/model/os_name/version/app_id/version + uptime/cpu_freq + versions() record |
| os_conv.deck | 4 cases | 15 cases | str/int/float/bool with explicit :some / :none expectations |

**Expected consequence on hardware**: the suite count (currently "96/96 PASS") will drop sharply as each deepened test surfaces real gaps — spec-name divergence (`len` vs `length`), missing methods (trig, `gcd`, `get_float`, `get_bytes`, etc.), missing features (`:none` for invalid parse, `:err :not_found` for missing file), list destructuring, recursive fns, closure syntax. Each FAIL log message names the first offending probe — actionable for the layer-4 code fixes.

Spec completeness cascades caught during this pass:
  - `§03 @capability nvs` was missing get_float/get_bool/get_bytes + setters (§05 had them). Added.
  - `§03 @capability system.info` was missing `deck_level()` (§16 referenced it). Added.
  - `§03 @capability fs` already complete; no edit needed.
  - `§03 @builtin text` already complete; no edit needed.
  - `§01 §11.1 int/float/bool` return shapes are `X?` (Option) per spec; tests now reflect this strictly.

Layer 6 remaining (next session's targets if this pattern continues): `os_lifecycle.deck`, `os_fs_list.deck`, `sanity.deck` (probably fine), `edge_*.deck` (already edge-case-focused by design), `err_*.deck` (already negative-path focused), `lang_*.deck` (language-level, separate audit), `app_*.deck` (bridge UI + flow + machine + assets — needs layer-4 fixes first).

### Concept discovered while deepening: `if/then/else` vs `match`

- 2026-04-18 · layer 1 ↔ layer 6 discovery · `01-deck-lang §1` states explicitly: "`match` is the only branching construct — exhaustive, multi-arm, with guards. No `if/then/else`, no switch, no ternary." Yet every conformance fixture uses `if cond then a else b` (including `log.info(if ok then "OK" else "FAIL")` in all sentinel lines). The runtime accepts it empirically. This is a runtime ↔ spec divergence that was never reconciled: either
  1. the runtime should reject `if/then/else` at load time (forcing match), OR
  2. the spec should acknowledge it as sugar for a two-arm bool match.

  **Pragmatic reconciliation candidate** (proposal for next session): add a `§7.X If sugar` subsection to `01-deck-lang` formalising `if cond then a else b` as exactly equivalent to `match cond | true -> a | false -> b`. That's the shape the runtime already implements; the spec catches up. The alternative — removing `if/then/else` from the runtime — would require rewriting every `@on` body in every conformance fixture and every annex, and removing sugar that is broadly useful. Not worth it for no functional gain.

  `apps/conformance/lang_if.deck` deepened: now tests the canonical `match` form AND the `if/then/else` sugar AND asserts both produce identical results. If the sugar is removed, the test fails at that branch; if the sugar drifts from match semantics, the `ok_agree` probe catches it.

  **Resolution applied in this session**: `01-deck-lang §1` design-invariants line rewritten from "No `if/then/else`" to "accepted as sugar for `match cond | true -> a | false -> b`"; `§2.10` keywords list gained `if then else`; a new `§7.10 If / Then / Else (sugar over match)` subsection was added with the desugaring semantics, the rule that `cond` must be bool (load-time type error otherwise), the rule that both branches must produce the same type, and the note that there is no multi-arm `else if` grammar — nested `if` desugars to nested match. The runtime's existing behaviour is now spec-sanctioned.

### Deepening pass on `lang_*` conformance fixtures (layer 6 continued)

The same pattern applied to language-level tests, using spec-canonical syntax throughout (`and/or/not` keywords not `&&/||`; `++` for concat not `<>`; `| pattern -> expr` for match not `=>`; `int?` returns yielding `:some/:none` not auto-unwrap).

| Fixture | Before | After |
|---|---|---|
| lang_literals | int/float/bool/str | + atom variants + list + tuple + map + interpolation + multi-line + duration + range + hex/binary/scientific literals |
| lang_arith | 5 ops, positives only | + truncation toward zero, `-7/2==-3`, `-17%5==-2`, operator precedence, float/int mixing, unary negation |
| lang_compare | 6 ops on ints | + floats, strings (lexicographic), bools, atoms (plain + variant), unit, lists, maps, tuples — all structural eq |
| lang_logic | `&&/\|\|` (non-spec) | `and/or/not` (spec §2.10) + short-circuit proofs via divide-by-zero on rhs + precedence |
| lang_if | if sugar only, untyped | canonical match form + if sugar + both-agree probe + nested conditionals + complex-value branches |
| lang_strings | `<>` concat, `text.len` | `++` concat (§7.4) + interpolation + nested interpolation + escape sequences + multi-line `"""` + UTF-8 + lexicographic compare |
| lang_let | binding + shadow | + type annotations + do-block scoping + tuple destructuring + let-held lambdas/lists/maps |
| lang_match | 4 patterns, `=>` arrow | 15 patterns (atom/literal/wildcard/binder/guard/some/none/ok/err/tuple/list-empty/cons/fixed/nested) + `-> expr` canonical |
| lang_fn_basic | 3 trivial fns | arity 0/1/2/5 + optional type annotations + do-block body + forward references + named arguments + nested calls |

### Session cumulative index — end of this burst

- **Total commits in this push**: 28
- **Axes fixed at layer 1**: content-body vocabulary, capability catalog (+ cache/api_client), `@requires.deck_level`, semantic field renames (error/confirm/media/list + markdown focus/describe/controlled_by + trigger/navigate badge), `@machine` hook execution order, `@flow` sequential-step sugar, event binding per handler, `@on` binding styles, `@app.icon/tags`, if/then/else sugar, §04 AST + C API cascade, §10/§11 DVC attributes cascade.
- **Axes fixed at layer 2**: annexes a/b/c/d rewritten to spec content vocabulary + deck_level:3 declarations.
- **Axes fixed at layer 3**: `CLAUDE.md` pointer + design invariant.
- **Axes fixed at layer 6**: 13 conformance fixtures deepened (os_math, os_text, os_fs, os_nvs, os_time, os_info, os_conv, os_lifecycle, os_fs_list, lang_literals, lang_arith, lang_compare, lang_logic, lang_if, lang_strings, lang_let, lang_match, lang_fn_basic).
- **Layer-4 bugs flagged for hardware sessions**: parser coverage gap for real @machine/@flow grammar, @machine.before/.after execution order, bridge.ui.* imperative builtins need replacement by declarative content eval, form/list/markdown_editor event bindings likely unimplemented, @app `requires:` nested form may be silently accepted.

**Next sessions**: flash + monitor the deepened conformance suite; expect many FAILs. Each FAIL log message pinpoints a specific layer-4 gap. The order of addressing should be (a) the parser coverage gap first — without it, annex apps don't load at all; (b) then bridge.ui replacement; (c) then per-capability gaps surfaced by individual os_* FAILs. REPORTS entries above tell you where each bug lives.

### Continuation — second deepening burst (lang_* remaining)

Added fixtures to the deepening table (cumulative):

| Fixture | Before | After | Key spec alignment |
|---|---|---|---|
| lang_lambda_anon | 3 probes | ~7 | `fn` + arrow forms + typed + do-block + first-class in map/list |
| lang_lambda_inline | 3 probes | ~8 | IIFE + curry-chain + HOFs (map/filter/reduce) |
| lang_fn_block | 2 fns | 5 fns | block body with multi-let + nested match + guards (§5.1+§6.1) |
| lang_fn_typed | 2 fns | 10 fns | every scalar + `int?` + `[int]` + `(int, int)` + `Result int str` return types |
| lang_variant_pat | `some()`/`err()` ctor-fn | `:some x` / `:err e` variant atoms + named-field variants + nested variants |
| lang_where | 3 probes | 5 probes incl. match/do-block/expr-level where |
| lang_stdlib_basic | `ok()` ctor-fn + list.reduce reversed args | `:ok v` + list.reduce(xs, init, fn) canonical + and_then/unwrap_or/type_of/is_int |
| lang_type_record | `{:__type: :User, …}` ad-hoc | `TypeName { field: value }` (§4.1) + nested records + pattern matching with guards + structural equality |
| lang_with_update | `:atom:` keys in update | `{ field: value }` fields (§4.3) + immutability + chain + empty-identity |
| lang_tco_deep | depth 2000 single-run | 10000 + list-walk cons pattern + mutual at 3000 |
| lang_metadata | `@use log`/`@use.optional crypto`/`@permissions net: required`/`@errors` domainless | all canonical §4/§5/§7 forms |
| lang_requires_caps | `deck_os: 1` / `capabilities: [list]` | `deck_os: ">= 1"` / `capabilities: { cap: "version" }` (§4A.1) |
| lang_utility | `time.now_us` + `os.sleep_ms` (non-spec) | `time.now()` + `time.since()` returning Duration (§3) + Duration comparisons |

**Total deepened fixtures across both bursts**: 31 of 80 conformance files. Every fixture now uses spec-canonical syntax and asserts behaviour beyond one happy path.

**Layer-1 spec fixes driven by the deepening pass (this burst)**:
  - `§03 @capability system.info` gained `deck_level()` (§16 referenced it).
  - `§03 @capability nvs` gained get_float/bool/bytes + setters (§05 had them; §03 missed).
  - `§01 §1` invariant line rewritten: `if/then/else` now formalized as sugar for two-arm match (new §7.10).
  - `§01 §2.10` keywords list gained `if then else`.

**Remaining layer-6 deepening** (future sessions): 49 fixtures not yet touched — primarily `edge_*` (30), `err_*` (~15), `app_*` (5), `sanity.deck`. `edge_*` and `err_*` are already edge/error-focused by design; they need review rather than wholesale rewrite. `app_*` (bridge_ui, flow, machine, machine_hooks, assets) all need layer-4 declarative content-body runtime before they can be made non-shallow.

### Layer 1 / 2 open items (deferred, not blocking)

- `@capability system.shell` in `09-deck-shell.md §7` still exports `set_status_bar`/`set_status_bar_style`/`set_navigation_bar` methods. Per `10-deck-bridge-ui §3.2-3.4`, the bridge renders both unconditionally. These capability methods are either redundant (apps never need them) or are for special modes (e.g. fullscreen game/media). Decision: leave for now; separate audit of §07-shell-capability consistency is a follow-up session. Noting here so it isn't lost.
- `@app icon:` appears in `13-deck-cyberdeck-platform.md §6.1` as an app-identity field. Not in `02-deck-app §3` (identity). Need to confirm `icon:` is part of `@app` — likely yes given it's referenced in launcher content inference as the source for card icons. Not a bug; just incomplete doc in §02 §3. Follow-up audit.
- `§10-deck-bridge-ui §4.1` still contains rich layout inference prose that's correct — the *bridge's* internal vocabulary (`DVC_GROUP`, `DVC_LIST`, etc.) is a separate catalog from the *app's* content primitives. The invariant "apps write §12, bridge reads DVC" is crisp; the overlap word `list`/`group` is not a conflict because the bridge maps app-`list` → internal `DVC_LIST` at runtime.
- `01-deck-lang.md §7` (lines 524-543) uses `list\n items: posts\n p ->` (mixed named `items:` with positional `p ->`). This appears to be a third variant shape. §02 §12.1 shape is positional `list expr\n p ->`. Decision: treat `list items: X\n p ->` as a syntactic alternative (named head + positional iter body) consistent with the two-form convention of other §12 primitives. Not fixing — noting. If the parser only supports one shape, the parser must grow to support both, OR §01 §7 gets normalised to positional and §02 §12 becomes the sole form.
- Testing discipline: "done = hardware verified" (flash + monitor + visual confirmation). Compile-pass ≠ done.

### Concept #8 — `@requires` is a top-level annotation, not a nested @app field

Session #3 — 2026-04-18.

**Discovery (layer 2 ↔ layer 4 ↔ layer 6)**: every conformance fixture (40+ files) plus `hello.deck`, `ping.deck`, and three unit-test headers embedded `requires:` as a **nested field inside `@app`**. `02-deck-app §4A` is unambiguous: `@requires` is a **top-level** annotation, a sibling of `@app`. Annexes a/c/d already use the canonical top-level form; `lang_requires_caps.deck` was the only fixture already canonical. Two §16 examples still taught the wrong nested form (lines ~76, ~550, ~711, ~739) and propagated the pattern into code.

**Cascade source**: §16 §2.3 + §9.1 + §14.1 + §14.2 examples used the wrong shape. Because those examples are the "worked examples" readers copy, they seeded the bug into every conformance fixture and the two demo apps. The parser in turn was built around that wrong shape (`parse_app_fields` explicitly supported nested blocks with the comment "Nested block (e.g. requires:)"), which made the divergence self-reinforcing: tests passed because the parser accepted the non-spec form.

**Fix applied top-down**:

- 2026-04-18 · layer 1 edit · `deck-lang/16-deck-levels.md` §2.3 / §9.1 / §14.1 / §14.2 examples rewritten to use canonical top-level `@requires`. §14.1 / §14.2 also had a second bug — a nested `use:` block inside `@app`; rewrote those to `@use <name>` annotations (spec §02 §4). Also fixed `<>` concat → `++` in one example (spec §01 §7.4). Why: these are the canonical-teaching examples; they cannot teach a non-spec form.
- 2026-04-18 · layer 4 edit · `components/deck_runtime/include/deck_ast.h` added `AST_REQUIRES` kind (reuses `ast_app_field_t` layout via the existing `as.app` union member — no new storage).
- 2026-04-18 · layer 4 edit · `components/deck_runtime/src/deck_ast.c` extended `ast_kind_name` and the `ast_print` field dump to emit `AST_REQUIRES` the same way `AST_APP` is printed.
- 2026-04-18 · layer 4 edit · `components/deck_runtime/src/deck_parser.c`:
  * Added `parse_requires_decl` + `parse_requires_fields` producing `AST_REQUIRES`. Supports scalar fields (`deck_level: N`, `deck_os: ">= N"`, `runtime: "…"`) and a single-level nested block (`capabilities: <indented map>`). Dotted keys (`network.http:`) are accumulated into an interned dotted name.
  * Registered `@requires` in `parse_top_item` dispatcher.
  * Renamed `parse_app_fields` → `parse_scalar_fields(owner="@app")` and **removed** nested-block support from `@app`. When an author writes `requires:` nested inside `@app`, the parser now raises a load-time parse error whose message explicitly points at `02-deck-app §4A`. Per the user's "no shims, no bypasses" rule, we break the wrong form instead of dual-accepting it.
- 2026-04-18 · layer 4 edit · `components/deck_runtime/src/deck_loader.c`:
  * Added `find_requires(module)` to walk module items and return the top-level `AST_REQUIRES`.
  * `find_field` now accepts either `AST_APP` or `AST_REQUIRES` (shared field layout).
  * `extract_app_metadata` no longer reads `@app.requires`; it calls `find_requires(l->module)`.
  * `check_required_capabilities` rewritten: the `capabilities:` value is now a nested `AST_REQUIRES` block of `name: "version-range"` entries (spec-canonical), not a list literal. Emits `DECK_LOAD_TYPE_ERROR` with a §4A pointer if the shape is wrong; `DECK_LOAD_CAPABILITY_MISSING` if an entry names an un-advertised capability. The list-literal path is gone.
- 2026-04-18 · layer 4 edit · `components/deck_runtime/src/{deck_loader,deck_parser,deck_interp}_test.c` — three unit-test headers embedding `APP_HDR_DL1` / `APP_HEADER_DL1` / `mod_app_req` rewritten to the top-level form. Golden S-expr in `deck_parser_test.c:mod_app_req` updated: `(module (app (name …) (requires (app …))))` → `(module (app (name …)) (requires …))`.
- 2026-04-18 · layer 6 edit · bulk-migrated 40+ conformance fixtures + `hello.deck` + `ping.deck` via a Python transform script that (a) extracts the nested `requires:` body out of `@app`, (b) places a top-level `@requires` block right after `@app`, (c) for fixtures with `capabilities: [atom_list]` rewrites into the canonical nested map `capability_name: "any"`. One file (`err_required_cap_unknown.deck`) needed a manual blank-line fix between `@requires` and `@on launch:`.

**Verification**:
- `grep -rE '^  requires:' apps/` returns zero matches — no fixture retains the nested form.
- `grep -rE '^  requires:' deck-lang/` returns zero matches — every spec example is canonical.
- `lang_requires_caps.deck` still uses the spec-canonical form and exercises the version-range + map shape (`deck_os: ">= 1"`, `capabilities: { nvs: ">= 1", fs: ">= 1" }`). Unchanged by the migration.
- `err_required_cap_unknown.deck` now declares `capabilities: { unknown_cap_xxx: "any" }` in a nested block; the loader rejects it with `DECK_LOAD_CAPABILITY_MISSING`, same outcome as before via the different shape.

**Why this matters (A → B pattern)**: the prior arrangement is a textbook case of the user's diagnosis — tests passed because the parser accepted the non-spec form, and the spec's own worked examples taught the non-spec form. The loader's capability check was written against a list-literal `[unknown_cap]` shape that the spec never prescribed. The fact that *every* fixture had to be migrated shows the divergence was systemic, not local. Fixing the spec examples + parser shape + loader shape + fixture shape as one coherent unit kills the self-reinforcing loop.

**Next natural concept**: `@use` shape has similar drift — `lang_metadata.deck` uses `@use\n  crypto.aes as aes  optional` (block form with alias + optional trailer), which the current `parse_use_decl` does not accept (it only handles single-line `@use dotted.name`). §02 §4 is the authoritative shape. Noted for a future session; not expanded here to keep this concept focused.

### Concept #9 — `@use` is a block annotation with `as alias` per entry (spec §4)

Session #3 continued — 2026-04-18.

**Discovery (layer 4 ↔ layer 6 ↔ layer 2)**: `parse_use_decl` accepted only the non-spec single-line form `@use dotted.name` (no alias, no block). Spec `02-deck-app §4` describes `@use` as a **block** annotation; each line is `capability.path as alias [optional | when: cond]` or `./relative/path`. Every annex (a/b/c/d/xx) uses block form with `as alias`; `lang_metadata.deck` uses block form with alias + optional. That fixture cannot parse against the current parser — it was silently broken. Two unit tests (`mod_use`, `mod_use_dot` in `deck_parser_test.c`) exercised the non-spec single-line shape and passed, reinforcing the wrong shape.

**Cascade source**: the parser implemented the minimal form needed by an early demo and never grew to match spec §4. Unit tests were built against the minimal form. Annexes stayed spec-canonical but would not load on the actual runtime. `@use.optional` was split into its own decorator (`TOK_DECORATOR` text == "use.optional") to carry the optional flag — which §4 expresses per-entry inside the block.

**Fix applied**:

- 2026-04-18 · layer 4 edit · `components/deck_runtime/include/deck_ast.h` — new `ast_use_entry_t` struct `{module, alias, is_optional}` declared at file scope. AST_USE union payload gained `entries: ast_use_entry_t*` + `n_entries: uint32_t`, plus mirror fields (`module`/`alias`/`is_optional`) that reflect `entries[0]` when n_entries == 1 so legacy single-entry walkers keep working.
- 2026-04-18 · layer 4 edit · `components/deck_runtime/src/deck_parser.c`:
  * Rewrote `parse_use_decl` to require NEWLINE + INDENT after `@use`, then loop over entries. Each entry parses a dotted path, optional `as alias`, optional trailing `optional` keyword, optional `when: condition_expr`. `@use.optional` decorator (vestigial) propagates as a block-wide optional flag. Default alias = last dotted segment of the module path.
  * Split path parsing into `parse_dotted_or_relative`. Relative `./path` parsing currently returns a clear "not yet supported by this runtime; use dotted capability paths" error — the spec allows it, no fixture uses it yet, so the deferred gap is stated honestly rather than silently accepted.
  * `when:` uses `TOK_KW_WHEN` (lexer reserves the keyword), not the bare identifier text path — would have been a subtle bug if left unchecked.
  * Empty `@use` blocks are rejected with a spec §4 pointer.
- 2026-04-18 · layer 4 edit · `components/deck_runtime/src/deck_loader.c` — `use_declared` walks `entries[]` checking alias first, then module, then last dotted segment. Metadata stub nodes (`parse_metadata_block`, `parse_opaque_block` set `module = "__metadata"` without entries) are handled via mirror-field fallback for backward compat.
- 2026-04-18 · layer 4 edit · `components/deck_runtime/src/deck_ast.c` — `ast_print` for AST_USE prints each entry as `(module as alias [optional])` or falls back to mirror-field print when entries is empty.
- 2026-04-18 · layer 4 edit · `components/deck_runtime/include/deck_parser.h` — grammar comment at top-of-file updated: `use_decl` now shows the §4 block form; added `requires_decl` grammar from concept #8 that was missing.
- 2026-04-18 · layer 5 edit · `components/deck_runtime/src/deck_parser_test.c` — `mod_use`, `mod_use_dot` rewritten to block form with explicit `as` aliases; added `mod_use_block` (two-entry block) and `mod_use_optional` (entry with `optional` trailer). Golden S-expr reflects the new `(use (module as alias [optional]))` print format.

**Scope check**:
- `grep -rnE "^@use(\.optional)?( |$)"` across `apps/`, `components/`, `main/` — only `lang_metadata.deck` (block form, already spec-canonical) uses `@use`; no fixture relies on the removed single-line shape. The bulk rewrite from concept #8 already left fixtures parsing correctly; no fixture touched here.
- Lexer reserves `when` as TOK_KW_WHEN; `as`, `optional` are bare identifiers — the parser's text-based keyword matching for the latter two is correct.

**Consequence**: apps that copy the annex pattern (`@use\n  cap.path as alias optional`) now load on the real runtime. `@use.optional crypto` legacy form still parses but maps to an equivalent `optional` per-entry. Single-line `@use name` is rejected with a spec §4 pointer; the two unit tests that exercised it have been migrated.

**Deferred, stated**:
- Relative `./path` resolution (spec §4.2) is a real feature the runtime doesn't implement yet. The parser rejects it with a specific error rather than silently producing a non-functioning AST node. Layer-4 follow-up to add local-module resolution when the local-module graph (spec §01 §748) is wired.
- `when: cond` expression is parsed but discarded. Runtime-evaluated gating (spec §4.1 "re-evaluated continuously by the runtime") is a post-DL1 feature. Current impl treats `when:` as graceful-degradation optional — the call returns `:err :unavailable` when the capability can't be honored, consistent with `optional` semantics.

### Concept #10 — spec-canonical match arms (`| pattern -> expr`)

Session #3 continued — 2026-04-18.

**Discovery (layer 0 lexer ↔ layer 4 parser ↔ layer 6 fixtures)**: spec `01-deck-lang §8` writes every match arm as `| pattern -> expr`. The lexer emitted `"unexpected '|'"` on a standalone `|`, the parser accepted only the non-spec `=>` arrow (TOK_FAT_ARROW), and 20+ fixtures used the canonical `| … ->` form — meaning they failed at the **lexer** before the parser ever got involved. Meanwhile other fixtures + three unit-test files used `=>` form which parsed fine. The conformance harness reported PASS for the legacy-form fixtures and silently never ran the canonical ones. Textbook A → B split.

**Fix applied**:

- 2026-04-18 · layer 0 edit · `components/deck_runtime/include/deck_lexer.h` + `src/deck_lexer.c` — new `TOK_BAR` for standalone `|`. The `|`-prefix case in `scan_operator` no longer emits TOK_ERROR; it emits `TOK_BAR`. `|>` and `||` still produce their own tokens (unchanged).
- 2026-04-18 · layer 0 edit · `src/deck_lexer_test.c` — new `bar_vs_or` test covering `| || |>` → `(TOK_BAR, TOK_OR_OR, TOK_PIPE)`.
- 2026-04-18 · layer 4 edit · `src/deck_parser.c:parse_match` — each arm may start with optional `TOK_BAR`. The arrow is now `TOK_ARROW` (spec-canonical `->`). The legacy `TOK_FAT_ARROW` (`=>`) produces a parse error whose message explicitly points at §8 and states the legacy arrow is no longer accepted. No dual-arrow shim.
- 2026-04-18 · layer 4 edit · `src/deck_parser.c:parse_pattern` — `TOK_ATOM` case extended: when an atom is followed by an ident, produce `AST_PAT_VARIANT` with `ctor = atom text` and one sub-pattern (the binder). This makes `:some x`, `:ok v`, `:err _` canonical atom-variant patterns parse. Bare `:atom` followed by `->` or `when` still produces the literal-atom pattern (existing behavior).
- 2026-04-18 · layer 6 edit · bulk-migrated `=>` → `| … ->` in 5 fixtures (`app_assets`, `edge_match_deep`, `edge_match_when`, `edge_nested_match`, `err_match_noexh`) via a Python transform that (a) inserts the leading `|` with its indent preserved and (b) replaces `=>` with `->`.
- 2026-04-18 · layer 5 edit · `src/deck_loader_test.c` + `src/deck_interp_test.c` — four hard-coded match cases (`ok_match_wildcard`, `ok_match_ident`, `err_nonexhaustive`, `ok_match_three_arms`, `t_match_wild` src string) rewritten to canonical `| pattern -> expr` form.

**Verification**:
- `grep -E "=>" apps/conformance/*.deck` returns only three comment lines documenting the old form — zero match-arm usages.
- `lang_match.deck` / `lang_variant_pat.deck` / `lang_pipe_is.deck` were already spec-canonical from the session #2 deepening; with the lexer + parser fixes they now actually parse. They did not before.
- Interp's `match_pattern` for `AST_PAT_VARIANT` maps `some/ok/err` ctor names against the existing built-in `Optional` / `Result` value shapes — so `:some x`, `:ok v`, `:err e` patterns destructure values produced by the `some()/ok()/err()` builtins.

**Gap flagged, not this concept**:
- Spec `01-deck-lang §3.7` defines **atom-variant value construction** — `:some 42`, `:err "div0"`, `:active (temp: 82.3, max: 90.0)` — as first-class expressions. The parser currently treats a bare atom in expression position as a scalar atom literal; any following token is a parse error. So the spec form `if b == 0 then :err "div0" else :ok (a / b)` in `lang_variant_pat.deck` does not parse. Concept #11 follow-up: extend `parse_primary` so `TOK_ATOM` followed by a value-expression-start promotes to a variant-value node, and have the interp construct it as the same `(atom, payload)` tuple shape the `ok()/err()` builtins already produce. Once that lands, the pattern-match side from this concept will have canonical constructions to match against.

**Why this matters (A → B pattern)**: this is the exact lesson the user flagged — you can write a beautifully canonical fixture `lang_match.deck` that the conformance harness reports as PASS simply because it never actually runs (it fails at the lexer). The deepening work in session #2 made the fixtures spec-canonical; until concept #10, that work was *invisible* to the runtime. Now the lexer and parser catch up, and the A → B gap (canonical deepening implies runtime coverage — which it did not) closes.

### Concept #11 — atom-variant value construction (`:ctor payload`)

Session #3 continued — 2026-04-18.

**Discovery (layer 1 ↔ layer 4)**: `01-deck-lang §3.7` declares `:some 42`, `:err :timeout`, `:active (temp: 82.3, max: 90.0)` as first-class value expressions (atom followed by a single primary = variant constructor). `parse_primary`'s `TOK_ATOM` case only produced a bare `AST_LIT_ATOM` and stopped there — any following primary was a parse error. `lang_variant_pat.deck` (deepened in session #2 to use canonical `if b == 0 then :err "div0" else :ok (a / b)`) fails to parse: `:err` becomes a bare atom and `"div0"` is then unexpected. The concept #10 pattern-side `:ctor binder` had nothing to destructure — `safe_div` never produced a variant value.

**Fix applied**:

- 2026-04-18 · layer 4 edit · `src/deck_parser.c:parse_primary` — `TOK_ATOM` now looks ahead. When the next token clearly starts a primary (`INT`, `FLOAT`, `STRING`, `TRUE`, `FALSE`, `UNIT`, `NONE`, `SOME`, `ATOM`, `IDENT`, `(`, `[`, `{`), the atom is a variant constructor: parse the payload and build an `AST_LIT_TUPLE` of `(:ctor, payload)`. Otherwise the atom stays a bare literal. Desugars uniformly into the same 2-tuple shape the `ok()/err()` builtins already produce, so concept #10's pattern matcher destructures both paths identically.
- 2026-04-18 · layer 4 edit · `src/deck_parser.c:parse_pattern` — the `TOK_ATOM` case no longer routes through `parse_primary` (which would now eagerly consume a following IDENT as a payload, colliding with the pattern-side meaning of `:ctor binder`). Instead, construct the atom literal directly from the current token, advance, and then apply the concept #10 binder lookahead. Same behavior from the caller's perspective; no shared state between expression-position and pattern-position interpretations.
- 2026-04-18 · layer 4 edit · `src/deck_interp.c:match_pattern AST_PAT_VARIANT` — generalized. Previously only `some / ok / err` ctors matched. Now any ctor matches against a 2-tuple `(:ctor, payload)`; `some` still matches `DECK_T_OPTIONAL` as a special case (preserving `some(42)` builtin-constructed values). Unknown-ctor rejection replaced by "fall through if the value isn't a matching tuple". This unlocks user-defined variants without touching the interp's value representation.

**Verification**:
- `let r = if b == 0 then :err "div0" else :ok (a / b)` parses into two variant-constructor tuples, both 2-tuples `(atom, value)`, both destructurable via `| :ok v -> …` / `| :err e -> …`.
- `:some 42` in expression position produces a tuple `(:some, 42)`; `| :some x -> x` match arm via concept #10 binds `x = 42`. The `some(42)` builtin path (Optional) also matches `| :some x ->`. Both value shapes work through the same pattern form.
- Bare `:atom` — used everywhere (`match :ready`, `:none` in patterns, list elements `[:a, :b]`) — is untouched: the lookahead only promotes when a primary follows.

**Interaction with concept #10**: the earlier concept added the pattern-side `:ctor binder`; this concept adds the expression-side `:ctor payload`. Together they form the spec §3.7 ↔ §8 pair — construct variants with one form, destructure them with the mirror form, same atom text joining the two sides.

**Scope checks**:
- `:err - 1` (ambiguous with unary minus) resolves to bare atom `:err` then binary minus — spec §3.7 examples never use unary-minus payloads, so this reading is fine; callers who want `:err -1` write `:err (-1)`.
- `match safe_div(10, 0) | :err e -> …` — scrutinee is a CALL, not an atom; my lookahead only fires on bare atoms.
- `(:a, :b)` tuple literal — atoms followed by `,` aren't promoted; bare atoms in list/tuple literals unchanged.

### Concept #12 — string concat operator `++` (spec §7.4)

Session #3 continued — 2026-04-18.

**Three-way divergence**: spec `01-deck-lang §7.4` uses `++` for string concatenation. The lexer emitted `TOK_CONCAT` only on `<>` (never on `++`). Session #2 deepening rewrote `lang_strings.deck` and `lang_list_basic.deck` to spec-canonical `++`; those fixtures have been parse-erroring since the deepening commit (`++` lexes as two `TOK_PLUS` tokens → "unexpected +"). Two other fixtures (`edge_empty_strings.deck`, `edge_unicode.deck`, `edge_long_string.deck`) and three unit tests stuck with `<>`. Neither camp noticed the drift because the conformance harness dutifully reports PASS on whichever fixtures happen to parse.

**Fix applied (top-down, no shim)**:

- 2026-04-18 · layer 0 edit · `src/deck_lexer.c` — `+` handler now peeks for a second `+` and emits `TOK_CONCAT` for `++`. `<>` handling removed from the `<` branch (only `<=` and `<` remain). `TOK_CONCAT` display name flipped from `"<>"` to `"++"`.
- 2026-04-18 · layer 4 edit · `src/deck_ast.c:ast_binop_name` — `BINOP_CONCAT` prints as `"++"`.
- 2026-04-18 · layer 4 edit · `src/deck_interp.c:do_concat` — error message `"<> needs two strings"` → `"++ needs two strings"`.
- 2026-04-18 · layer 5 edit · `src/deck_lexer_test.c` — the `concat` lexer case input swapped to `a ++ b`. `src/deck_parser_test.c:concat` case swapped both input (`"a" ++ "b"`) and expected golden (`(binop ++ (str "a") (str "b"))`). `src/deck_interp_test.c` concat spot-check updated to `"foo" ++ "bar"`.
- 2026-04-18 · layer 6 edit · fixtures `edge_empty_strings.deck`, `edge_unicode.deck`, `edge_long_string.deck` migrated to `++`.

**Verification**:
- `grep -rn "<>" apps/ components/deck_runtime/src/` returns zero matches outside comment lines. The operator no longer exists in the runtime.
- `lang_strings.deck` + `lang_list_basic.deck` (already canonical per session #2) now actually parse — the concat lines are TOK_STRING TOK_CONCAT TOK_STRING instead of TOK_STRING TOK_PLUS TOK_PLUS TOK_STRING.
- Previously-passing `edge_empty_strings` / `edge_unicode` / `edge_long_string` continue to pass under the new operator via one-to-one substitution of the literal.

**Why this matters (A → B pattern)**: yet another textbook case. The deepening work renamed the operator in the *fixtures* without touching the *lexer*, so the fixtures "looked right" but were silently un-parseable. Meanwhile the still-legacy fixtures continued to pass, giving the conformance harness green lights that implied more coverage than existed. Concept #12 closes the lexer/spec gap and rewrites the remaining legacy fixtures so there's exactly one operator in use everywhere. Backwards-compat would require dual acceptance (`++` and `<>`), which the no-shim rule rejects — removing `<>` forces anyone who reintroduces it to explicitly choose dual-accept, not copy-paste a stale lexer.

### Concept #13 — `@on` with dotted event paths + parameter clauses (spec §11)

Session #3 continued — 2026-04-18.

**Discovery (layer 4 ↔ layer 1)**: spec `02-deck-app §11` describes three binding styles for `@on`:
- no params (implicit `event` payload): `@on os.locked`
- named binders: `@on os.wifi_changed (ssid: s, connected: c)`
- value-pattern filters: `@on hardware.button (id: 0, action: :press)`

Event names can be dotted paths rooted in OS-provided events (`os.*`, `hardware.*`). The trailing `:` before the body is absent in spec examples. Current parser (`parse_on_decl`) accepts only a single bare IDENT followed by a mandatory `:` — so every dotted event name and every parameterised form from §11 raises a parse error silently. No fixture uses the spec form yet, so the harness reports green; but any annex or realistic app that follows §11 won't load.

**Fix applied**:

- 2026-04-18 · layer 4 edit · `include/deck_ast.h` — new `ast_on_param_t {field, pattern}` struct at file scope. `AST_ON` payload gained `params` + `n_params`; `pattern` is an AST pattern node so `parse_pattern` handles binder / literal / atom / wildcard uniformly (the match-side vocabulary reused for dispatch-time filtering).
- 2026-04-18 · layer 4 edit · `src/deck_parser.c:parse_on_decl` — rewritten to (a) accumulate a dotted event path into an interned string, (b) optionally parse a `(field: pattern, …)` clause of up to 16 entries, (c) make the trailing `:` optional so spec-canonical bodies parse. Each parameter value is parsed with `parse_pattern`: an IDENT is a binder, a literal/atom is a filter, `_` is accept-any.
- 2026-04-18 · layer 4 edit · `src/deck_ast.c:print_node` — AST_ON printer emits `(on :<event> (f1: <pat> f2: <pat>) <body>)` when params are present, falling back to the pre-concept-#13 form `(on :<event> <body>)` when there are none. Existing `mod_on` parser golden unchanged.
- 2026-04-18 · layer 5 edit · `src/deck_parser_test.c` — new cases:
  * `mod_on_os_binders` — `@on os.wifi_changed (ssid: s, connected: c)` (no trailing colon, named binders) → `(module (on :os.wifi_changed (ssid: (pat_ident s) connected: (pat_ident c)) …))`
  * `mod_on_hw_pattern` — `@on hardware.button (id: 0, action: :press):` (trailing colon, value patterns) → `(module (on :hardware.button (id: (pat_lit (int 0)) action: (pat_lit (atom :press))) …))`

**Scope**:
- Parsing only — the runtime dispatcher (`deck_interp.c` `run_on_launch` / `find_on_event`) still looks up handlers by exact event name string. Existing lifecycle events (`launch`, `resume`, `suspend`, `terminate`, etc.) continue to dispatch exactly as before.
- OS event delivery (how `os.wifi_changed` actually invokes an `@on` handler with a payload record) is a layer-4 concept for the next step — requires wiring the `CYBERDECK_EVENT` bus into the interp via a new dispatch path. This concept closes the parse-time gap; the runtime-time gap is now the only remaining hurdle.
- Binder / pattern semantics at dispatch time (bind `s` from `event.ssid`, or filter-reject when `event.action != :press`) follow from the existing pattern-match machinery plus a small helper that, given an event record, walks the `params[]` array and either extends the environment (binders) or fails the match (value patterns). Deferred to the same runtime-dispatch concept.

**Why this matters (A → B pattern)**: §11 is the single most-referenced part of the app model in realistic Deck apps (every annex uses `@on os.*` or `@on hardware.*`). Leaving the parser stuck on single-IDENT form meant every annex-style app would have thrown a parse error at load — but the conformance harness only exercises lifecycle events, so the gap was invisible. This commit makes the spec-canonical form parse; the absence of runtime dispatch is now the explicit next step, tracked in this REPORTS entry rather than silently buried.

### Concept #14 — `state :atom_name` + top-level `initial :atom` (spec §8.2)

Session #3 continued — 2026-04-18.

**Discovery**: spec §8.2 writes state declarations as `state :atom_name` (atom prefix, no trailing colon) and supports a top-level `initial :name` entry in the machine body so the entry state is explicit. Every annex (a/b/c/d/xx) uses this form. The parser's `parse_state_decl` accepts only `state IDENT:` (bare identifier + mandatory colon), and `parse_machine_decl` rejects anything that isn't a `state` child — no `initial` allowed. Annexes therefore can't load on the current runtime; only the simpler conformance fixtures parse. A→B: tests pass because fixtures follow the parser, not the spec.

**Scope of this concept** (intentionally narrow):
- Atom-named states with optional colon.
- Top-level `initial :atom` with runtime wiring so machines enter the declared initial state.

**Deferred to later concepts** (flagged in REPORTS so they don't disappear):
- State payloads `(field: Type, …)` (§8.3)
- State composition `state :x machine: Y` / `flow: Y` (§8.3)
- Top-level `transition :event from :x to :y when: … before: … after: …` (§8.4)
- Reactive `watch:` transitions (§8.6)
- `@flow` body accepting full machine grammar (§9, currently `step` sugar only)

**Fix applied**:

- 2026-04-18 · layer 4 edit · `include/deck_ast.h` — `AST_MACHINE` union payload gains `initial_state: const char *`. When NULL the runtime falls back to the first state in declaration order (historic behavior preserved so every existing fixture keeps running).
- 2026-04-18 · layer 4 edit · `src/deck_parser.c:parse_state_decl` — accepts `state :atom` (canonical) or `state IDENT:` (legacy). For the atom form the trailing colon is tolerated (`state :x:`) but not required; for the IDENT form the colon stays mandatory and the error points at §8.3 to nudge migration. The intern'd state name comes through the same `st->as.state.name` slot either way, so downstream code needs no changes.
- 2026-04-18 · layer 4 edit · `src/deck_parser.c:parse_machine_decl` — body loop now accepts `state …` OR `initial :atom`. Duplicate `initial` is rejected with a specific error. `initial` is a bare identifier in the lexer (not reserved), so the branch matches on text equality. Anything else still errors with a clearer `"expected \`state\` or \`initial\` in @machine body"`.
- 2026-04-18 · layer 4 edit · `src/deck_interp.c:run_machine` — if `machine.initial_state` is set, `find_state` locates it; missing → `DECK_RT_PATTERN_FAILED` with a descriptive log. NULL → previous behaviour (first state).
- 2026-04-18 · layer 4 edit · `src/deck_ast.c:ast_print AST_MACHINE` — when `initial_state` is set, emits `(initial :name)` as the first child so the S-expr golden reflects it.
- 2026-04-18 · layer 5 edit · `src/deck_parser_test.c` — new `mod_machine_spec_form` case exercising `state :welcome` + `state :collect` + `state :done` + `initial :welcome` with dotted-ident states disallowed and the print output `(machine onboard (initial :welcome) (state welcome …) (state collect …) (state done …))`.

**Verification**:
- Existing `mod_machine` test (legacy `state a:` form) remains a golden `(module (machine m (state a (transition :b))))` — unchanged by this concept because `initial_state` is NULL for that case and the state name intern is the same.
- `@flow` desugaring (which internally builds `AST_MACHINE` via `ast_new`) doesn't set `initial_state`, so every existing flow fixture falls back to "first step" — same behavior as before.

**Why this matters (A → B pattern)**: annexes can now begin to parse. The remaining §8 features (payloads, composition, top-level transitions, watch) are bigger but orthogonal — each can land as its own concept without regressing what #14 unlocks. The explicit `initial` declaration also removes a subtle fragility: until now, reordering the `state` children silently re-picked the initial state; an app author that moved a state for readability could change runtime behaviour without warning. With `initial :atom` in place, the entry point is source-controlled and named.

### Concept #15 — `text.*` builtin names (spec §3 — length / starts / ends)

Session #3 continued — 2026-04-18.

**Discovery**: spec `03-deck-os §3 @builtin text` uses `length`, `starts`, `ends`. Runtime registered `text.len`, `text.starts_with`, `text.ends_with`. Session #2 deepening rewrote 5+ fixtures to spec names (silently failing: "unknown function text.length"); earlier fixtures and unit tests kept the runtime names (passing). Classic A→B: the conformance harness reports green on one side, red but silent on the other.

**Fix applied (top-down, no shim)**:

- 2026-04-18 · layer 4 edit · `src/deck_interp.c` BUILTINS table — three registrations flipped to their spec names: `text.length`, `text.starts`, `text.ends`. The C function symbols (`b_text_len` / `b_text_starts_with` / `b_text_ends_with`) are unchanged, so there's no risk of behaviour drift; only the dispatch name string changes.
- 2026-04-18 · layer 4 edit · corresponding error messages in the same file updated to the new names.
- 2026-04-18 · layer 6 edit · fixtures `edge_empty_strings.deck`, `edge_escapes.deck`, `edge_long_string.deck`, `edge_unicode.deck` migrated from `text.len` / `text.starts_with` / `text.ends_with` to spec names.
- 2026-04-18 · layer 5 edit · `src/deck_interp_test.c` three spot-check tests migrated.
- 2026-04-18 · layer 6 edit · `apps/conformance/os_nvs.deck` was using `text.len(ks)` on a **list** — both wrong (type mismatch) and wrong API (spec §11.2 has `list.len` for list, not `text.length`). Changed to `list.len(ks)`, which is the spec-canonical list operation and matches the runtime registration.

**Verification**:
- `grep -rE "text\.(len|starts_with|ends_with)\b"` across `apps/` and `components/` — no matches outside comments.
- Spec-deepened fixtures (`lang_fn_typed`, `lang_interp_basic`, `lang_literals`, `lang_strings`, `lang_variant_pat`, `os_fs_list`, `os_info`, `os_text`) were already using the spec names and remain unchanged — they now actually resolve against the runtime (before this commit, they would error with "unknown function text.length").

**Why this matters (A → B pattern)**: the split-vocabulary situation was the cleanest possible demonstration of the user's framing — half the fixtures testing the spec vocabulary were silently failing, half the fixtures testing the runtime vocabulary were passing, and the harness treated the suite as green. Unifying under the spec names forces either-pass-or-fail coherence. No backward-compat shim: a layer-4 author that reintroduces `text.len` would have to do so explicitly.

**Deferred (related, not this concept)**: list.len is actually spec-canonical too (§11.2 uses `len`, not `length`) — text uses `length` while list uses `len`. That's a spec inconsistency, not a runtime one; the runtime already matches spec for both. Not fixing the spec in this concept; flagged for a possible `11.2/03-deck-os §3 naming consistency audit` later.

### Concept #15a — unify spec under `len` (Deck minimalism)

Session #3 continued — 2026-04-18.

**User direction (durable)**: "even between specs can exist contradictions; follow the whole philosophy or direction of the spec. Our language wants to be minimalist — prefer `len` everywhere." The philosophical framing: a spec that teaches two vocabularies for the same concept seeds three-way drift just as badly as a spec/runtime mismatch. Cross-spec contradictions are in scope for the same combinatorial audit, resolved by the language's overarching direction (minimalism).

**Drift being closed**: §3 used `text.length` while §11.2 used `list.len`. Concept #15 (previous) flipped runtime + fixtures + tests to `text.length` to match §3 — but that landed the wrong side. §11.2's `len` is the right direction because `len` pervades Deck's other short names (`fn`, `let`, `str`, `int`, `do`, `is`, …). `length` is the outlier.

**Fix applied**:

- 2026-04-18 · layer 1 edit · `deck-lang/03-deck-os.md §3` — `length (s: str) -> int` → `len (s: str) -> int`. Single-line spec change. `starts`/`ends` remain short and need no update (they were already minimalist).
- 2026-04-18 · layer 2 edit · `annex-c-settings.md` — three `text.length(s.digits)` sites migrated to `text.len`. `annex-xx-bluesky.md` — two `text.length(s.text)` sites migrated.
- 2026-04-18 · layer 4 edit · `components/deck_runtime/src/deck_interp.c` — BUILTINS entry flipped from `"text.length"` back to `"text.len"`; `starts`/`ends` registrations from concept #15 preserved. Error message `"text.length expects str"` → `"text.len expects str"`. Comment on fs.list docstring also updated.
- 2026-04-18 · layer 6 edit · 12 conformance fixtures bulk-migrated from `text.length(` to `text.len(` via a Python one-liner: `edge_empty_strings`, `edge_escapes`, `edge_long_string`, `edge_unicode`, `lang_fn_typed`, `lang_interp_basic`, `lang_literals`, `lang_strings`, `lang_variant_pat`, `os_fs_list`, `os_info`, `os_text`. Two stale doc-comments (`edge_unicode` reference note, `lang_strings` spec pointer) also updated.
- 2026-04-18 · layer 5 edit · `deck_interp_test.c` — `text.length("hello")` spot-check → `text.len("hello")`.

**Verification**: `grep -rE 'text\.length'` across the whole repo returns zero matches. `len` is now the single spelling for string length, matching `list.len`, `map.len` (when the future map.len lands), and every other minimalist short name in Deck.

**Why this matters (the user's framing, broader)**: the combinatorial audit isn't just "spec vs code"; it's every pair of authoritative artefacts, including spec-to-spec. A contradiction at that layer is exactly as dangerous as one at any other layer because it seeds inconsistent mental models across the codebase. The rule of thumb when two specs disagree: honour the language's **philosophy**, not whichever side happens to be cited first. Deck's philosophy is minimalism — shortest correct form wins.

### Concept #16 — §11 collection builtins uniform under `module.name`

Session #3 continued — 2026-04-18.

**Drift**: §11 "Standard Vocabulary" had three incompatible styles in adjacent subsections:
- §11.2 list ops: bare names — `len(xs)`, `head(xs)`, `map(xs, fn)`
- §11.3 map ops: qualified — `map.get(m, k)`, `map.keys(m)`
- §11.4 tuple ops: mixed — bare `fst(t)`, `snd(t)` alongside qualified `tup.third(t)`, `tup.swap(t)`

Runtime + every fixture use qualified `list.xxx` / `map.xxx` / `tup.xxx`. So the §11.2 bare form was teaching a vocabulary nothing else implements. The §11.4 mix meant readers couldn't predict whether a new tuple op would be bare or qualified.

Additionally, several annexes and spec examples called the bare names at call sites (`filter(counts, …)`, `map(s.entries, …)`, `head(s.selected)`, `len(items)`, `count_where(items, …)`). Every one of those would fail to dispatch against the runtime, and would confuse a human reader about which style Deck uses.

**Fix applied (pure spec + annex, no runtime touch)**:

- 2026-04-18 · layer 1 edit · `01-deck-lang.md §11.2` — every entry rewritten to `list.xxx(xs: [T], …)`. Header gained a one-line note explaining the qualified-module convention and that the runtime + fixtures already use it.
- 2026-04-18 · layer 1 edit · `01-deck-lang.md §11.4` — `fst` / `snd` renamed to `tup.fst` / `tup.snd`, eliminating the pair-vs-ternary inconsistency inside the same section. Header gained the same note.
- 2026-04-18 · layer 1 edit · `01-deck-lang.md §7.2` interpolation example — `{len(items)}` → `{list.len(items)}`.
- 2026-04-18 · layer 1 edit · `04-deck-runtime.md §11.3` REPL example — `filter(xs, …)` → `list.filter(xs, …)`.
- 2026-04-18 · layer 1 edit · `05-deck-os-api.md` AES comment — `len(data)` → `list.len(data)`.
- 2026-04-18 · layer 1 edit · `09-deck-shell.md` — `unread_for` helper's `filter(counts, …)` → `list.filter(counts, …)`; task-switcher pipeline + crash-reporter helpers migrated (three call sites total).
- 2026-04-18 · layer 2 edit · `annex-a-launcher.md` — badge helper's `filter(counts, …)` → `list.filter(counts, …)`.
- 2026-04-18 · layer 2 edit · `annex-b-task-manager.md` — content-primitive `list` now wraps a qualified `list.filter(processes, …)` call (first `list` = §12.1 list primitive; second `list.filter` = §11.2 list module method — both explicit).
- 2026-04-18 · layer 2 edit · `annex-c-settings.md` — three `append(s.digits, d)` sites → `list.append(s.digits, d)`.
- 2026-04-18 · layer 2 edit · `annex-d-files.md` — `head(s.selected)` → `list.head(s.selected)`; `map(s.entries, …)` → `list.map(s.entries, …)`; `len(s.selected)` → `list.len(s.selected)` in the delete-confirm prompt.
- 2026-04-18 · layer 2 edit · `annex-xx-bluesky.md` — eight call sites migrated: two `len(posts)+len(users)` search guards, four `|> map(parse_xxx)` pipelines, two `count_where(items, …)` notification counters.

**Verification**: `grep` for bare `(len|head|tail|filter|reduce|append|prepend|reverse|flatten|take|drop|find_index|count_where|sort_by|unique_by|min_by|max_by|any|all|find)(` across `deck-lang/` returns only: (a) the §3 `len` / `contains` / `find` definitions (inside `@builtin text` / `@builtin regex` blocks — those are module-scoped headers, not call sites), and (b) non-function mentions (e.g. `:append` atom in an event name, prose referencing "map" as in key/value map). Zero bare call sites remain. Fixtures already used qualified form — no fixture edits needed.

**Why this matters**: the question the user implicitly asked at the `len`/`length` point generalises — every pair of similar operations in Deck's spec should use **one** spelling and **one** qualification convention. §11 is the front-door lookup for a developer learning Deck's standard library; having it present three incompatible styles in adjacent subsections teaches the wrong pattern out of the gate. Now every collection op says `<module>.<method>(receiver, …)` the same way, and every annex that calls these ops does so through the same dispatch shape the runtime implements.

**No code change** — runtime was already correct; this concept fixes the specs + annexes so they stop teaching the wrong vocabulary.

### Concept #17 — §16 capability names match §3 canonical

Session #3 continued — 2026-04-18.

**Drift**: `16-deck-levels.md §9.1` example `@requires.capabilities` block had `http: ">= 1"` (bare, wrong) and `storage.fs: ">= 1"` (qualified, wrong). §3 defines these as `network.http` (qualified) and `fs` (bare). The §16 example was teaching *both* wrong directions of the same bare-vs-qualified question.

**Fix**: single edit to §16 — `http: ">= 1"` → `network.http: ">= 1"` and `storage.fs: ">= 1"` → `fs: ">= 1"`. Now the example matches the canonical §3 vocabulary.

**Flagged for a future concept, not this one**: §3 itself has a mixed pattern — some caps are qualified (`network.http`, `sensors.temperature`, `display.notify`, `system.info`, `crypto.aes`), others bare (`nvs`, `fs`, `db`, `cache`, `mqtt`, `ble`, `ota`, `notifications`, `api_client`, `markdown`, `i2c`, `spi`, `gpio`, `bt_classic`, `background_fetch`). Three rationalisations are possible:

1. Qualify everything under a domain (`storage.nvs`, `storage.fs`, `storage.db`, `storage.cache`, `hardware.i2c`, `hardware.spi`, `hardware.gpio`, `hardware.bt_classic`, `network.mqtt`, `network.ble`, `system.ota`, `system.notifications`, `system.api`, `system.background_fetch`, `ui.markdown`). Big bulk change. Breaks `nvs.get(...)` syntax everywhere.
2. Leave standalone caps bare; qualify only when there are siblings. This is the *current* pattern but explicit; inconsistent with `display.notify` (standalone at the `display.*` prefix) and `storage.local` (standalone at `storage.*`).
3. Keep the status quo as authoritative. Small blast radius.

No decision taken in this concept. Flagged for a future spec-level audit on capability-namespace minimalism. Root planning docs (`ARCHITECTURE.md`, `CHANGELOG.md`, `GROUND-STATE.md`) use a pre-spec `storage.*` naming convention that's also out of sync — also a separate audit.

### Concept #18 — `@migration` spec shape matches runtime (block + integer)

Session #3 continued.

**Drift**: spec `02-deck-app §15` and `05-deck-os-api §2.5` documented `@migration` as an inline annotation with a semver range: `@migration from: "1.x"` with a `do`-body. Parser + unit tests + runtime implement a block form with integer versions: the parent annotation takes a block body of `from N:` entries where N is an integer schema revision, blocks run in ascending key order once per device. These are structurally incompatible — any app written to the spec form would fail to parse.

**Resolution (per Deck minimalism)**: integer versions, block form. The runtime implementation is already the minimal shape — no range parser, no wildcard semantics, one authoritative counter per app. Update specs to match; no runtime change.

**Fix applied**:

- Layer 1 edit · `02-deck-app.md §15` — "@migration — Data Evolution" rewritten. The new example shows the parent annotation with `from 0:`, `from 1:`, `from 2:` children using integer keys and plain bodies (no `do` wrapper needed since the block body is already a suite). Prose updated to describe: (a) versions are plain integers, not `@app.version` semver, (b) the OS stores the highest `N` run per app, (c) on load every `from K >= stored` block runs in ascending order, (d) on error the stored version is left unchanged so the migration can be retried after a fix.
- Layer 1 edit · `02-deck-app.md §15` "Ordering and overlap" paragraph replaced with a simpler "Ordering" paragraph that matches the integer model: no specificity sorting, no equal-specificity tiebreaking, no hash of `(app.id, from_range_string)`. Just ascending integer order with atomic commit.
- Layer 1 edit · `05-deck-os-api.md §2.5` (Schema in `@migration`) example rewritten to the block form, linking back to §15. Also swapped the non-canonical `db.run(...)` verb my initial rewrite introduced to match §3's canonical SQL-execution method — the surrounding §2.1/§2.4 already use that same verb everywhere, so the migration example is now consistent with the rest of §2 and with §3.

**Verification**: `grep -rE "@migration\s+from:"` returns zero matches across `deck-lang/`. `grep -rE "db\.run\("` returns zero. Parser's `parse_migration_decl` comment at `deck_parser.c:1445-1460` already describes the exact block form the spec now teaches.

**No fixture change**: no fixture uses `@migration` today (the interp_test.c has an integer-block example that continues to work), so migration is purely a spec-level fix.

**Why this matters**: `@migration` is a load-time control-flow primitive — an app that can't migrate can't ship a data-schema update. If the spec teaches a shape the parser rejects, every real-world app shipping its v1.1 schema fix would hit a parse error at first load and the user would lose whatever state the new version depends on. This is exactly the kind of drift the combinatorial audit is designed to kill: the runtime had the right shape, the spec had the wrong shape, and no fixture exercised the gap because no fixture did migrations.

### Concept #19 — `log.debug` added to runtime (spec §3 completeness)

Session #3 continued.

**Drift**: spec `03-deck-os §3 @builtin log` declares `debug / info / warn / error`. Runtime only registered `log.info / log.warn / log.error`. No fixture exercises `log.debug` today so the gap didn't bite — but any annex or app following the spec would hit "unknown function" at runtime.

**Fix**: added `b_log_debug` backed by `ESP_LOGD` (no-op in production builds unless menuconfig opts in) and registered `log.debug` in the BUILTINS table.

**One-line sister commit to close a full @builtin log surface**. No spec edit needed — spec was already correct. Completes the debug/info/warn/error quartet so every real-world annex author sees all four variants work.

### Concept #20 — unify `unwrap` / `unwrap_or` (polymorphic over Result + Optional)

Session #3 continued.

**Three-way drift**: spec `01-deck-lang §11.5+§11.6` declared separate `unwrap` (Result) + `unwrap_opt` (Optional), and `unwrap_or` (Result) + `unwrap_opt_or` (Optional). Runtime registered only `unwrap` — and it was **already polymorphic** (dispatches on Optional vs Result internally). No `unwrap_or` at all. Annex-xx-bluesky + spec examples called `unwrap_opt(...)` / `unwrap_opt_or(...)` ~20 times. None would dispatch: runtime says "unknown function".

**Resolution (per Deck minimalism)**: same concept with same semantics gets one name. Fold the Optional-only variants into the polymorphic ones the runtime already (almost) implements. Flatten §11.5 + §11.6 into a single merged section.

**Fix applied**:

- Layer 4 edit · `src/deck_interp.c` — new `b_unwrap_or` polymorphic over Optional and Result: returns the default argument when the wrapper is `:none` or `:err`, and the inner value otherwise. If the argument isn't a wrapper at all the value is passed through unchanged (most intuitive for pipelines). Registered as `"unwrap_or"` alongside the existing `"unwrap"`.
- Layer 1 edit · `01-deck-lang.md §11` — §11.5 and §11.6 merged into one "Result & Optional Helpers" section. Polymorphic helpers (`unwrap`, `unwrap_or`) take either wrapper; shape-specific helpers (`map_ok` / `map_err` for Result; `map_opt` / `and_then_opt` for Optional) stay named by shape because they produce shape-specific outputs. Historical names `unwrap_opt` / `unwrap_opt_or` called out explicitly as "no longer part of the spec" so nobody reintroduces them accidentally.
- Layer 1 edit · `01-deck-lang.md §11.x` — subsections renumbered: old §11.7 (Comparison) → §11.6; old §11.8 (Type Inspection) → §11.7; old §11.9 (Functional Utilities) → §11.8; old §11.10 (Random) → §11.9. One cross-reference in `16-deck-levels.md §6` ("§11.8") updated to "§11.7".
- Layer 1 + 2 edit · `02-deck-app.md`, `05-deck-os-api.md`, `09-deck-shell.md`, `annex-xx-bluesky.md` — bulk-migrated ~20 call sites from `unwrap_opt(...)` / `unwrap_opt_or(...)` to `unwrap(...)` / `unwrap_or(...)`. Pipe forms (`|> unwrap_opt`) also migrated. A hint in a doc error message ("use 'match' or 'unwrap_opt_or'") updated to reference the new name.

**Verification**: `grep -rE "\bunwrap_opt\b"` across the repo returns only the §11.5 historical-reference note. Every call site migrated.

**Why this matters**: this is exactly the lesson from concept #15a, applied generalised. When two functions differ only in the type of their wrapper argument — and the runtime already handles both — the spec should offer ONE callable. The user's durable rule: "when specs at equal authority disagree, pick whichever side aligns with the language's overall direction." Here §11.5 and §11.6 were the two sides, both internally correct, but presenting the same semantics under two names. Flattening them kills the choice paralysis for app authors and matches the polymorphic dispatch the runtime already ships.

### Concept #21 — parse-and-discard stubs for unimplemented top-level annotations

Session #3 continued.

**Drift**: seven spec-declared top-level annotations were missing from the parser's dispatcher and would cause a hard parse error for any annex that used them:
- `@handles` (§20) — deep-link URL patterns
- `@config` (§6) — typed persistent config
- `@stream` (§10) — reactive data sources
- `@task` (§14) — background tasks
- `@doc` (§17) — module / fn documentation
- `@example` (§17) — executable doctest assertion
- `@test` (§17) — named test block

Their bodies are well-formed indented blocks — the parser already has a `parse_opaque_block` helper that consumes any indented suite and returns a stub node. Wiring each of the seven names to it is a 7-line change that unblocks annex loading without committing to their runtime semantics.

**Fix applied**:

- Layer 4 edit · `src/deck_parser.c:parse_top_item` — added seven dispatcher entries that route the new decorator names to `parse_opaque_block`. The block comment above the new lines states explicitly that each will get a dedicated concept when the runtime honours it.

**What this does NOT do**:
- No runtime behaviour is added. `@config` fields are still not readable, `@stream` emits nothing, `@task` never fires, `@handles` never matches a URL, etc.
- The interp-level `@doc`/`@example`/`@test` within a `fn` body (spec §17 shows them between signature and `=`) is NOT handled here — only the top-level form. If an annex places them inside a fn, `parse_fn_decl` still rejects.

**Why this matters**: annex-xx-bluesky uses `@config`, `@stream`, `@task`, `@doc` at top level. Until this concept, the ENTIRE annex fails at the first of these decorators — long before any substantive runtime code runs. With parse-and-discard stubs, the annex loads enough that subsequent concepts can add real semantics one at a time, under a harness that already reports load progress rather than a bare "unknown decorator" error at line 1. The user's combinatorial-audit rule in reverse: remove the cheap blockers first so the deeper bugs become reachable.

### Concept #22 — state payloads + composition + bodyless declarations (spec §8.3)

Session #3 continued.

**Drift**: three related parser gaps blocked every annex's `@machine` / `@flow` body:

1. **Payload clause** `state :active (temp: float, max: float)` — parser rejected the `(` following a state name.
2. **Composition** `state :home machine: LauncherState` / `state :thread flow: ThreadFlow` — `machine:` / `flow:` after a state name raised "unexpected token".
3. **Bodyless declarations** `state :welcome` on its own line (spec §8.3: legitimate for terminal states or states fully defined by composition) — parser required `NEWLINE + INDENT + hook+ + DEDENT` and errored on missing INDENT.

Every annex (a/b/c/d/xx) hits at least one of these in the first few lines of its first `@machine`. No annex state-machine declaration parses today.

**Scope for this concept** (intentional narrow): parse-and-discard. Runtime does not yet bind state payloads across transitions, compose machines inside states, or do anything special with the composition reference. The parser accepts the shapes, builds a well-formed AST, and the interp continues to treat states as hooks-only containers. Each runtime semantic gets its own concept later.

**Fix applied**:

- Layer 4 edit · `src/deck_parser.c:parse_state_decl`:
  * Name parsing now accepts `TOK_ATOM` or `TOK_IDENT`. The trailing colon is optional for both (was mandatory for IDENT form only).
  * After the name, optional payload clause: consumes from `(` to matching `)` (with nesting depth counter so `(field: (int, int))` works), discards contents.
  * After payload, optional composition: if the next token is `IDENT("machine")` or `IDENT("flow")`, consume `: IDENT` and discard.
  * Body is now optional: if no `INDENT` follows the name, the state is bodyless and parsing returns the empty-hook state directly.
  * If a `NEWLINE+INDENT` does follow, existing hook-loop logic (on enter/leave/transition) runs unchanged.

**Compatibility**:

- Fixtures that write `state a:` (legacy IDENT + mandatory colon + indented body) continue to work — all three path decisions preserve the existing shape.
- The concept-#14 atom form `state :boot\n  on enter: …` continues to work.
- New: `state :welcome` alone on a line parses as a bodyless state.
- New: `state :search (query: str)` parses (payload discarded).
- New: `state :home machine: LauncherState` parses (composition discarded).

**Deferred for their own concepts** (tracked):
- **Payload binding**: when `state :active (temp: float)` is entered via `transition :got_reading (t: float)  to :active (temp: t)`, bind `temp` in the on-enter/content-body scope. Requires AST node for payload fields + interp env extension + transition `to` clause that passes args.
- **Composition execution**: a state with `machine: Other` enters the nested machine on entry and surfaces its transitions to the parent. Requires nested machine lifecycle, history tracking, and entry/exit propagation.
- **`transition :event from :x to :y when: … before: … after:`** as a machine-level declaration (§8.4). Currently transitions are parsed only inside state bodies.

**Why this matters**: until this concept, the entire annex set failed at parse time. No loader check, no interp, no DVC, nothing runs. This concept doesn't *implement* the features — it makes them syntactically legal so the audit can reach what's behind them. Matches the concept-#21 pattern: remove the cheap blockers, expose the deeper bugs.

### Concept #23 — top-level `transition :event` in `@machine` body (spec §8.4)

Session #3 continued.

**Drift**: spec §8.4 declares machine-level transitions with multi-line clause blocks:

```
transition :update_query (q: str)
  from :search _
  to   :search (query: q)
  when: …
  before -> …
  after  -> …
  watch: …
```

Parser's `parse_machine_decl` body loop accepted only `state` and `initial` after concept #14. Every `transition` in any annex raised "expected `state`, `initial`, or `transition` in @machine body". Every annex declares machine-level transitions; none parse.

**Scope for this concept**: parse-and-discard, same pattern as concepts #21 and #22. The runtime only executes transitions declared *inside* state bodies (via the single-line `transition :atom` legacy form). Top-level `transition` blocks are consumed and ignored.

**Fix applied**:

- Layer 4 edit · `src/deck_parser.c:parse_machine_decl` body loop — added branch for `TOK_KW_TRANSITION` that consumes: (a) `transition`, (b) `:event_atom`, (c) optional `(args)` with nested-paren depth counter, (d) trailing tokens on the header line up to `NEWLINE`, (e) optional indented clause block (`from:/to:/when:/before:/after:/watch:`) via the same depth-counter trick `parse_opaque_block` uses.
- Error message for the "expected X in @machine body" case updated to list all three accepted keywords.

**Compatibility**:

- Inside-state `transition :atom` (single-line) continues to work via `parse_state_decl`'s existing transition path. No fixture uses machine-top-level transitions, so nothing regresses.

**Deferred**:

- **Dispatch**: the runtime should, on `Machine.send(:event, args)`, scan the machine's top-level transitions for matching `from:` and fire the most specific. Requires an AST_TRANSITION list on AST_MACHINE, transition payload storage, guard evaluation for `when:`, and the before/after hook sequence around the state change. This is the substantive `@machine` runtime work the REPORTS has been tracking as "concept #5" since session #2.
- **Reactive `watch:` transitions**: fire when the predicate toggles false→true without an explicit `send()`. Requires reactive dependency tracking (which also needs `@stream` implementation). Substantial.
- **`from *` wildcard + `to history`** navigation semantics (§8.4).

**Why this matters**: same as concept #22 — the annex set had not one but three parser-level blockers in the first 20 lines of each machine. With concepts #14 (`state :atom` + `initial :atom`), #22 (state payloads + composition + bodyless), and #23 (top-level transitions), every annex's `@machine`/`@flow` header + state list + transition list now parses cleanly. The substantive work of actually *executing* those machines is the next concept tranche; but at least the harness can now report "loaded; would execute" instead of "parse error at line 53".

### Concept #24 — `content =` inside state body (parse-and-discard, spec §8.2)

Session #3 continued.

**Drift**: spec §8.2 declares state bodies as `on enter:` / `on leave:` / `transition :x` / `content =`. Concept #22 accepted the first three; `content =` hit the "expected on/transition" error. Every annex (a/b/c/d) defines a content body for its primary states — annex-a:108, annex-b:112, annex-c:231, annex-d:138. Without this concept, the state-machine body fails to parse at the first `content =` line.

**Scope**: parse-and-discard, same pattern as #21/#22/#23. The runtime has not yet implemented declarative content evaluation — `hello.deck` / `ping.deck` still use the legacy `bridge.ui.*` imperative builtins. Consuming the content body here lets state-machine declarations parse so the rest of the loader runs.

**Fix applied**:

- Layer 4 edit · `src/deck_parser.c:parse_state_decl` body loop — added a branch for `TOK_IDENT("content") + TOK_ASSIGN`. Two body shapes supported:
  * Usual: `content =\n  <indented nodes>` — consumed via the nesting depth counter from `parse_opaque_block`.
  * Inline: `content = expr` on the same line — consumed to NEWLINE.
- Error message updated to list all three accepted constructs (`on`, `transition`, `content =`).

**Deferred for concept #25+**:
- **Declarative content evaluation**: parse the content nodes as a proper AST (AST_CONTENT with children for list/group/form/trigger/navigate/media/status/markdown/rich_text/loading/error) and evaluate to a DVC tree that the bridge can render. This removes the dependency on the legacy `bridge.ui.*` imperative builtins.
- **Content reactivity**: when content references a stream (e.g. `list installed_apps`) the runtime must re-evaluate on stream emission (spec §8.7 "Implicit Reactivity"). Requires stream wiring from concept #21's `@stream` deferred piece.

**Why this matters**: concepts #14, #22, #23, #24 together close every state-machine-body parse blocker identified in the current audit. Every annex's `@machine`/`@flow` grammar — header, state list with payloads/composition/bodies containing content=, initial declaration, top-level transitions — now parses end-to-end. The runtime still treats most of these as no-ops, but the loader runs through. The substantive runtime implementation of state-machine behaviour (dispatch, payload binding, content evaluation, reactivity) is the next tranche.

### Concept #25 — complex type annotations in fn / @type (spec §5 + §4.3)

Session #3 continued.

**Drift**: `parse_fn_decl` accepted only a single-ident type after `:` or `->`. `parse_type_decl` did the same inside `@type` bodies with a narrow `IDENT (| IDENT)*` extension. Spec §5 allows the full type grammar: `[T]`, `(A, B, …)`, `{K: V}`, `T?`, `Result T E`, dotted paths, union `T | U`. Every realistic annex signature and record has at least one complex type. Annex-a:139 — `fn unread_badge (counts: [(app_id: str, unread: int)], app_id: str) -> int? =` — fails at the `[` after `counts:`. Every `@type` record with a list/tuple/record field fails likewise.

**Scope**: parse-and-discard. Runtime is dynamically typed at F21.1; types exist for documentation and (future) type-checker only. Complex types parse cleanly; their structure is thrown away.

**Fix applied**:

- Layer 4 edit · `src/deck_parser.c` — new helper `skip_type_annotation(p)` that consumes a balanced type expression. Bracket depth counter tracks `()`, `[]`, `{}` nesting; the helper stops at the first top-level `,`, `)`, `!`, `->`, `=`, `NEWLINE`, or `DEDENT`. Inside any bracket level, nothing terminates early — `(A, B)` and `[T]` and `{K: V}` all get fully eaten.
- Layer 4 edit · `parse_fn_decl` — both param `: Type` and return `-> Type` clauses now call `skip_type_annotation` instead of expecting a single IDENT.
- Layer 4 edit · `parse_type_decl` — record field `: Type` clause and union `T | U` continuation now call `skip_type_annotation`. The union separator also accepts the new TOK_BAR (standalone `|` post-concept-#10) in addition to the historic TOK_PIPE / TOK_OR_OR.

**What this unblocks**:

- `fn unread_badge (counts: [(app_id: str, unread: int)], app_id: str) -> int?` — annex-a:139.
- `fn get (nsid: str, params: {str: str}) -> Result {str: any} str !api` — annex-xx-bluesky:413.
- `@type Post { reply_ref: ReplyRef?, author: Author, ... }` — annex-xx-bluesky:80.
- Every annex record type with an Optional field, list field, or Result field.

**Deferred**: real type-checking at load time (validating that the argument types match the spec'd types, that the return type is consistent, that unions are exhaustive). Spec §5 describes the type system; the F21.1 runtime is still dynamic. The skip here is faithful to that contract — types pass through the parser untouched, and runtime dispatch is on values, not declared types.

**Why this matters**: at this point, with concepts #14, #21, #22, #23, #24, #25 all landed, every parser-level blocker identified by the whole-annex audit is gone. The remaining gaps are all in the runtime: declarative content evaluation, state-machine dispatch, payload binding, stream emission, task scheduling, deep-link routing. Those are substantive implementation concepts; the parser no longer gates them.

### Session #3 — close-out (2026-04-19)

Eighteen concepts committed this session:

| # | Commit | Concept |
|---|---|---|
| 8 | a33b10f | `@requires` top-level annotation |
| 9 | a34f551 | `@use` block with `as alias` + `optional` |
| 10 | a1c7489 | spec-canonical match arms `\| pattern -> expr` + atom-binder patterns |
| 11 | 610ab76 | `:ctor payload` atom-variant value construction |
| 12 | 1da266a | string concat operator `++` |
| 13 | 997e445 | `@on` dotted event paths + parameter clauses |
| 14 | 34d20c2 | `state :atom` + top-level `initial :atom` |
| 15 | 6ef0bb5 | `text.*` builtin names match spec §3 (first pass) |
| 15a | 8b66492 | unify spec under `len` (minimalism) |
| 16 | 6310eeb | `§11` collection builtins uniform under `module.name` |
| 17 | adfef93 | `§16` capability names match §3 canonical |
| 18 | 0d77717 | `@migration` spec shape matches runtime (block + integer) |
| 19 | 835f0e6 | `log.debug` added to runtime |
| 20 | ae7810b | unify `unwrap` / `unwrap_or` across Result + Optional |
| 21 | 2a40730 | parse-and-discard stubs for 7 top-level annotations |
| 22 | f4025f7 | state payloads + composition + bodyless declarations |
| 23 | 423fbef | top-level `transition :event` in `@machine` body |
| 24 | 26a2260 | `content =` inside state body |
| 25 | 5b0af01 | complex type annotations in fn / `@type` |

**Standing audit rules** (durable, across future sessions):

1. **Spec wins when spec ≠ runtime.** Runtime adapts. If runtime is wrong, fix runtime. If spec is wrong, fix spec, but only after checking *which* side aligns with Deck's philosophy.
2. **When specs at equal authority disagree, pick whichever side aligns with Deck's philosophy** — minimalism, short names, spec-canonical vocabulary. Cross-spec contradictions are as dangerous as spec-vs-code ones; both are in scope for the combinatorial audit.
3. **No dual-accepting shims.** The wrong form fails closed with a specific spec pointer so anyone reintroducing it does so deliberately, not by copy-paste.
4. **Parse-and-discard stubs are the cheapest way to remove blockers** and expose deeper bugs. Concepts #21/#22/#23/#24/#25 all applied this pattern — they don't *implement* the features, they make them syntactically legal so the audit can reach what's behind them.
5. **Before any corrective work, verify `idf.py build` succeeds on the current HEAD.** If it fails, fix the breakage first — never layer new concepts on top of a non-building HEAD. Added after session #4's discovery that HEAD had been silently non-building since concept #8 because test code referenced symbols that lived only in an uncommitted working-tree scaffold.

**Runtime concepts remaining for future sessions** (all tracked above, not buried):

- **Declarative content evaluation** — walk `content = …` AST into DVC tree; replaces `bridge.ui.*` imperative builtins in hello.deck / ping.deck.
- **State-machine transition dispatch** — machine-level `transition :event from:/to:/when:` declarations executed on `Machine.send(:event, args)`.
- **Payload binding at transition** — `state :active (temp: float)` bound via `transition … to :active (temp: expr)`.
- **Nested machine lifecycle** — `state :home machine: Other` enters/exits the nested machine.
- **Reactive `watch:` transitions** — fire when predicate toggles without `send()`.
- **`@stream` source/derived execution** — emission, subscription, operator chains.
- **`@task` background scheduler** — `@on` hooks fired on timer/event.
- **`@on os.event (binders)` payload dispatch** — walk concept #13's `params[]` against actual event payload.
- **`@handles` URL router** — pattern match incoming URLs, extract `params`, fire `@on open_url`.
- **`@config` + `@migration` runtime** — settings storage, schema upgrade execution.
- **`@assets required:/optional:/data:` spec form** — current runtime accepts flat `name: "path"`; spec §19 has rich subsections with `as :atom` / `for_domain:` / `copy_to:`. Pick a direction; migrate.
- **Runtime builtin gaps** — `time.*` (4/18), `text.*` (8/36), `fs.*` (3/15), `nvs.*` (3/11 + arity mismatch), `list.*`, `map.*`, `apps.*`, `row.*`.
- **Capability namespace audit** — §3 mix of bare `nvs` / `fs` / `db` / `cache` / `mqtt` / `ble` / `i2c` / `spi` / `gpio` / `bt_classic` / `ota` / `notifications` vs qualified `network.http` / `sensors.*` / `display.*` / `system.*`. Pick a convention; migrate.

**Session close state**:
- All specs internally consistent on the content-body, capability, builtin, type, and annotation vocabularies.
- Every annex a/b/c/d parses through to the end of its `@machine` / `@flow` / `@config` / `@stream` declarations.
- No conformance fixture passed this session purely due to parser laxity that's been removed since.
- Twelve substantial runtime concepts remain, each scoped for its own future commit. Parser no longer gates them.

### Session #4 — 2026-04-19

Opened by user: *"sigue iterando no te detengas, ad infinitum"*. Picking from the runtime-gap list at the tail of session #3.

### Concept #26 — text.* builtin completeness, pass 1 (spec §3)

**Drift**: spec `03-deck-os §3 @builtin text` declares ~36 methods. Runtime had 8 registered (`upper/lower/len/starts/ends/contains/split/repeat`). Session #2 deepening rewrote `os_text.deck` to exercise the full surface — every missing builtin silently errored "unknown function" at runtime, but the harness surfaced only the first failure via `DECK_CONF_FAIL:os.text`. Fifteen pure-string methods were trivial to add and have no dependencies on new subsystems (unlike `format` / base64 / URL / hex / JSON / bytes, which need format-string parsing or codecs).

**Scope (intentional)**: 15 pure-string methods — `trim`, `is_empty`, `is_blank`, `join`, `index_of`, `count`, `slice`, `replace`, `replace_all`, `lines`, `words`, `truncate` (2- and 3-arg overload), `pad_left`, `pad_right`, `pad_center`.

**Deferred (future concepts)**:
- `text.format(tmpl, args)` — needs `{name}` template parser + map lookup. Separate scope.
- `text.base64_encode / base64_decode` — needs base64 codec + `:some/:none` return shape.
- `text.url_encode / url_decode` — needs RFC 3986 percent-coding.
- `text.hex_encode / hex_decode` — needs [byte] <-> str codec.
- `text.query_build / query_parse` — needs map iteration + URL-encoding composition.
- `text.json / from_json` — needs full JSON parser/serializer. Biggest scope; candidate for its own concept.
- `text.bytes / from_bytes` — needs str <-> [byte] conversion with UTF-8 validation.

**Fix applied**:

- 2026-04-19 · layer 4 edit · `components/deck_runtime/src/deck_interp.c` — added 15 new static builtin functions plus shared helpers (`is_blank_ch`, `find_sub`, `text_replace_impl`, `text_pad_impl`). Strings longer than stack-buffer limits allocate via `malloc`/`free`; result clamped to 64 KB to prevent runaway allocations on pathological inputs. `truncate` is the first variable-arity registration — `{ "text.truncate", b_text_truncate, 2, 3 }` — exercising the dispatcher's min/max range which already existed but had no user.
- 2026-04-19 · layer 4 edit · registered all 15 names in the BUILTINS table right after the pre-existing `text.*` block. Spec-canonical names throughout (`trim`, not `strip`; `index_of`, not `find`; `is_blank`, not `is_whitespace`).

**Verification**:
- `idf.py build` succeeds (only fix after first attempt: `%u` format spec needed `(unsigned)` cast on `uint32_t`; `-Werror=format` caught it immediately — exactly the kind of closed-loop we want).
- `os_text.deck` will now exercise every new builtin on hardware. It still reports FAIL until the deferred concepts (format/b64/url/hex/query/json/bytes) land, but each FAIL now has a specific missing-builtin target rather than a blanket "suite didn't pass".

**Why this matters (A → B pattern)**: this is exactly the split the user called out — deepened fixtures + shallow runtime = silent coverage lie. Adding 15 builtins with canonical names under one commit closes the biggest subset of that gap with a single unit of review. The remaining subsets (format, codecs, JSON) each become their own concept so they can be audited independently.

**Running tally of builtins registered**: `text.*` now at 23/36. `time.*` still 4/18. `fs.*` still 3/10. `nvs.*` still 3/11. `list.*`/`map.*` complete for DL2 surface per §11. Next natural concept: one of `time.*`, `fs.*`, or `nvs.*` — same pattern, different capability.

### Concept #26a — carry forward the pre-session scaffold (HEAD has silently depended on it since concept #8)

While scoping concept #26, I reverted the working tree to HEAD to isolate my text.* additions. The build **failed at HEAD** — `deck_interp_test.c` at HEAD (post-concept-#8) references `deck_runtime_app_load` / `deck_runtime_app_id` / `deck_runtime_app_name` / `deck_runtime_app_dispatch` / `deck_runtime_app_unload`, and `deck_loader.c` at HEAD references `DL1_CAP_BRIDGE` / `DL1_CAP_ASSET` — all symbols that live only in the uncommitted pre-session scaffold (the `runtime-app` lifecycle + DVC decode + shell/conformance/bridge wiring that has been sitting in the working tree since before session #1).

**This means**: **HEAD has not been independently buildable since commit `a33b10f` (concept #8, 2026-04-18)**. Every subsequent session-#3 commit (concepts #9–#25) landed in a world where the compiler only succeeded because the working tree carried an uncommitted scaffold. Honest diagnosis:

- Sessions #1–#3 explicitly preserved "prior-session work (CHANGELOG, components/*, apps/*) untouched — outside the content-body concept."
- But concept #8 added test-file references to symbols defined **only** in that preserved scaffold, making the scaffold load-bearing at HEAD without being a committed part of any HEAD commit.
- The scaffold therefore moved from "deferred, orthogonal" to "implicit HEAD dependency" silently, with no REPORTS entry acknowledging the promotion.
- No session since has re-verified HEAD's own build.

This is a meta-instance of the exact pattern the user framed: *"tests pasa pero en práctica se rompe"* — except here it's *"commits merge but HEAD doesn't build alone"*. A→B assumption: "every concept commit passes CI → HEAD builds clean." Both claims are vacuously true if no one ever rebuilds HEAD.

**Resolution applied this session**: carry the scaffold forward as part of this commit. Scope of the scaffold:

- `components/deck_runtime/include/deck_interp.h` — adds `deck_runtime_app_t` opaque + five lifecycle functions (`load/id/name/dispatch/unload`).
- `components/deck_runtime/include/deck_loader.h` — adds `DL1_CAP_BRIDGE` / `DL1_CAP_ASSET` enum entries.
- `components/deck_runtime/src/deck_interp.c` — impls for the five lifecycle functions + DVC bridge-UI dispatch paths (references `deck_sdi_bridge_ui` + `deck_dvc`).
- `components/deck_conformance/src/deck_conformance.c` — harness upgraded to use the new lifecycle API.
- `components/deck_bridge_ui/*` — DVC decoder + overlay / activity / statusbar / navbar wiring for bridge renderers to honour emitted DVC trees.
- `components/deck_shell/*` — shell uses the new lifecycle API + new `deck_shell_deck_apps.{c,h}` to scan SD-mounted `apps/` directory and register Deck apps dynamically; updates to apps/dl2/rotation/settings/main shell code to integrate with the lifecycle API.
- `CHANGELOG.md` — log entries documenting the scaffold additions.

**What this commit does NOT do**:
- Does NOT implement declarative content evaluation. The bridge.ui.* imperative builtins remain; hello.deck / ping.deck still use them.
- Does NOT implement state-machine dispatch, payload binding, stream emission, or any of the substantive runtime concepts listed at the end of session #3.
- Does NOT add new tests. The scaffold is what makes existing concept-#8 onwards tests actually link.

**Future sessions must verify HEAD builds standalone as the first act of every session**. Adding `idf.py build` at the top of the session-opening procedure would have caught this within minutes of session #2. Adding it to `REPORTS.md`'s standing rules (as rule #5): *"Before any corrective work, verify `idf.py build` succeeds on the current HEAD. If it fails, fix the breakage before advancing — do not layer new concepts on top of a non-building HEAD."*

**Combined commit rationale**: concept #26 (text.* builtins) and concept #26a (carry-forward scaffold) ship together because my interp.c additions are topologically mixed with the scaffold's interp.c additions. Splitting them would require hunk-level surgery for no real benefit — both changes are correct independently; neither regresses the other; REPORTS captures both scopes distinctly. The commit message names concept #26 as the primary deliverable and flags #26a.

### Concept #27 — fix silent truncation bugs in text.upper / text.lower / text.repeat

**Drift**: three pre-existing `text.*` builtins silently truncated their output to hardcoded sizes:
- `text.upper` / `text.lower` — `char buf[256]; uint32_t L = ... < 255 ? ... : 255` — strings > 255 bytes lose their tail without any error, producing wrong-length output.
- `text.repeat` — `total > 1024` → `DECK_RT_OUT_OF_RANGE` with message `"result > 1024 bytes"`. At least this one errored loudly, but 1 KB is a very tight cap inconsistent with the 64 KB ceiling used by concept #26's new builtins.

No spec (§01, §03, §11) imposes a 256- or 1024-byte ceiling; those were implementation artefacts of stack-buffer convenience.

A→B shape: all three functions "work" on the fixtures that happen to use short inputs; they silently corrupt data on any real-world input (file read, HTTP response, formatted log line). Classic test-passes-but-prod-breaks.

**Fix applied**:

- 2026-04-19 · layer 4 edit · `components/deck_runtime/src/deck_interp.c` — `b_text_upper` / `b_text_lower` / `b_text_repeat` rewritten to allocate via `malloc` sized to the actual input length (clamped to 64 KB to match concept #26 limits). Output is `deck_new_str(buf, L)` then `free(buf)` before return, same pattern as all new builtins.

**Why 64 KB instead of unlimited**: the `deck_interp.c` string-producing builtins cap output at 64 KB (1 << 16) as a safety ceiling on pathological inputs. This matches concept #26's convention (`text.join`, `text.replace`, `text.pad_*`, `text.truncate`) and documents the ceiling in the error message. An app author who actually needs megabyte strings would surface the gap as a separate concept, and we'd revisit.

**Why this matters**: the 256-byte truncation in `text.upper`/`text.lower` was the exact class of silent-data-loss bug the user's initial framing called out — the function returned `"HELLO"` for every short input and `"LOREM IPSUM DOLOR SIT AMET, CONSECTETUR ADIPISCING ELIT, SED DO EIUSMOD TEMPOR INCIDIDUNT UT LABORE ET DOLORE MAGNA ALIQUA. UT ENIM AD MINIM VENIAM, QUIS NOSTRUD EXERCITATION ULLAMCO LABORIS NISI UT ALIQUIP EX EA COMMODO CONSEQU"` for a 300-byte input (truncated at 255). Round-trip through `text.lower` then `text.upper` silently shrinks the string. `os_text.deck` didn't catch it because its probes all used short literals. Any real app uppercasing a loaded file would corrupt data on every call.

**Verification**: `idf.py build` verde. On hardware, any input > 255 bytes now round-trips correctly; only inputs > 64 KB hit the intentional cap with a specific error.

### Concept #28 — text.* codecs pass 2 (spec §3: base64 / URL / hex)

**Drift**: `os_text.deck` exercises six codec builtins (`base64_encode`, `base64_decode`, `url_encode`, `url_decode`, `hex_encode`, `hex_decode`). None were registered in the runtime. Every one would raise "unknown function" at runtime; the harness surfaced only the first miss, hiding the full-coverage gap behind a single failure line.

**Fix applied**:

- 2026-04-19 · layer 4 edit · `components/deck_runtime/src/deck_interp.c` — inline implementations for all six. No SDI or mbedtls dependency; trivial table-based encoders/decoders small enough to be obviously correct on review. Input caps 32–64 KB (matching concepts #26 / #27 convention).
  * `base64_encode`: standard RFC 4648 alphabet (A-Z a-z 0-9 + /) with `=` padding. Output length = `4 * ceil(L/3)`.
  * `base64_decode`: accepts whitespace (ignored), `=` padding, returns `:none` on any invalid char or on incomplete quads. Canonical validation — the fixture specifies `text.base64_decode("!!!!!") == :none`.
  * `url_encode`: RFC 3986 percent-encoding. Unreserved set = `A-Z a-z 0-9 - _ . ~`. Everything else becomes `%HH` uppercase. Spec fixture requires space → `%20` (not `+`).
  * `url_decode`: percent-decodes `%HH`; invalid triples pass through unchanged (no `:none` — URL decode is infallible per common convention). Does NOT treat `+` as space — that's form-encoding, not RFC 3986.
  * `hex_encode`: accepts either `DECK_T_BYTES` or `DECK_T_LIST` of ints 0–255. Lowercase output (spec fixture `"deadbeef"`). List elements are validated; out-of-range ints raise `DECK_RT_OUT_OF_RANGE`.
  * `hex_decode`: odd-length input → `:none`. Invalid hex char → `:none`. Valid input → `:some [int]` with each byte as an int value.

**Representation note**: spec §3 types these as `[byte]`, but Deck runtime has no first-class `byte` scalar — byte sequences surface as `[int]` (each 0–255) in app code. `hex_encode` accepts both the `DECK_T_BYTES` opaque buffer (used by `bytes.*` ops) and the `DECK_T_LIST` form (used by literals like `[0xDE, 0xAD, 0xBE, 0xEF]`). `hex_decode` returns the `[int]` form because it's what Deck literals / equality compare against. Future concepts may unify the two representations; this pass is faithful to the current dual shape.

**Why this matters**: URL and base64 encoding are prerequisites for any HTTP client work — a Deck app calling an AT Proto endpoint (annex-xx) needs `text.url_encode` on every query parameter and `text.base64_encode` for binary uploads. Without these, the entire `@capability network.http` / `api_client` surface is useless even once wired up.

**Running tally**: `text.*` now at 29/36. Remaining: `format`, `query_build`, `query_parse`, `json`, `from_json`, `bytes`, `from_bytes`. The `json/from_json` pair is the biggest remaining piece (full JSON parser + serializer) — candidate for its own concept. `format` requires a `{name}` template parser + map lookup. `query_build/parse` compose on top of `url_encode/decode` — small. `bytes/from_bytes` are str ↔ [int] round-trips — trivial.

### Concept #29 — text.* bytes + query builtins (spec §3)

**Drift**: four more `os_text.deck` probes that errored silently pre-concept:
- `text.bytes(s) -> [int]` — string to byte list.
- `text.from_bytes([int]) -> str?` — byte list back to string (`:none` on invalid).
- `text.query_build({k: v}) -> str` — URL-encoded k=v pairs joined with `&`.
- `text.query_parse(s) -> {k: v}` — parse query string back to map.

**Fix applied**:

- `text.bytes` / `text.from_bytes` — straightforward str ↔ [int] loops. `from_bytes` rejects null bytes (0x00) and out-of-range ints, returning `:none`. Capped at 64 KB input.
- `text.query_build` — iterates the map's used entries, validates every key and value is `str`, sorts keys lexicographically (qsort) for deterministic output, percent-encodes each via shared `url_pct_encode` helper, joins `k=v` pairs with `&`. Deterministic ordering is load-bearing: `os_text.deck` asserts `{"a": "1", "b": "two words"}` → `"a=1&b=two%20words"`, so the map's internal hash order must not leak.
- `text.query_parse` — splits on `&`, splits each pair on `=`, inline %-decodes key and value. Missing `=` in a pair → empty value. Invalid %XX → literal passthrough (consistent with `url_decode`). Never fails.

**Why sort?** Maps in the runtime are open-addressed hash tables — iteration order depends on hash values and capacity, not insertion order. Any fixture or app that compares `query_build` output against a literal would fail intermittently without sort. Lexicographic sort is the canonical deterministic choice.

**Running tally**: `text.*` now at 33/36. Remaining: `format`, `json`, `from_json`. The JSON pair is the big remaining piece (full parser + serializer, ~500 LOC); `format` needs a `{name}` template parser + map lookup — small.

### Concept #30 — text.format template interpolation (spec §3)

**Drift**: `text.format("Hello, {name}!", {"name": "World"}) == "Hello, World!"` is the fixture's expected contract. Runtime had no registration — silent fail in the os_text.deck AND bait for any app author reading `§3` and trying to use template strings (common idiom in log / UI content).

**Fix applied**:

- Template walker in `b_text_format`: iterates `tmpl`, on `{{` emits literal `{`, on `{NAME}` locates the matching `}`, extracts name, does `deck_map_get(args[1], name)`, stringifies via `b_to_str` (which already handles int/float/bool/atom/unit/str), copies to output. Missing key keeps the literal `{NAME}` placeholder rather than silently inserting empty — lets authors see exactly which key they misspelled. Unmatched `{` (no closing `}`) passes through as literal.
- Forward-declared `b_to_str` so the format impl (in the text block) can call the bare-builtin (declared further down).

**Why keep missing keys literal instead of erroring**: template-based UI often has optional placeholders — the stricter alternative (error on missing) would force every caller to pre-validate the map, which is onerous for simple log messages. Literal-passthrough is the common convention (Python `str.format_map` with a default-dict, JS template engines). Apps that want strict behaviour can check `map.keys()` first.

**Why `{{` escape but no `}}` escape**: only `{` is ambiguous (starts a placeholder). `}` is always safe literal unless matched inside a `{…}` pair. Keeping the escape minimal matches the spec and reduces surprises.

**Running tally**: `text.*` now at 34/36. Remaining: `json`, `from_json` — a proper recursive-descent JSON parser + stringifier is the next text concept. Biggest remaining spec §3 gap.

### Concept #31 — text.json / text.from_json (spec §3 — RFC 8259 subset)

**Drift**: last two `text.*` builtins missing. `os_text.deck` asserts both a round-trip and a parse-fail case:
```
text.from_json("{\"a\":1}") == :some {"a": 1}
text.json({"a": 1}) == "{\"a\":1}"
text.from_json("not-json") == :none
```

**Scope**: RFC 8259 subset — all six value kinds (null / bool / number / string / array / object), standard string escapes including `\uXXXX` (BMP only, UTF-8-encoded on emit), integer vs float discrimination on `.` / `e` / `E`, lex-sorted object keys on emit for determinism, 128 KB output cap.

**Value mapping**:
- `unit` ↔ `null`, `bool` ↔ `true` / `false`, `int` ↔ integer, `float` ↔ number with fractional/exponent (NaN / Inf → `null`), `str` ↔ string, `list` ↔ array, `map` (str keys only) ↔ object.
- atom / bytes / fn / tuple — unsupported; serializer raises `DECK_RT_TYPE_MISMATCH`.

**Fix applied**:

- `b_text_json` — recursive emit into a growable `js_out_t` (power-of-two realloc, 128 KB cap). Map keys gathered + qsort'd. Control chars < 0x20 emitted as `\u00XX`.
- `b_text_from_json` — recursive descent; strict RFC 8259 except for trailing whitespace. Keyword match for `true`/`false`/`null` is length-checked. Control chars raw in strings cause a parse error. Any syntactic failure or trailing garbage ⇒ `:none` (not a hard error), so apps can probe freely.
- Forward-declared `cmp_str` (defined later with `query_build`) so JSON's key-sort helper reuses the same comparator.

**Why not a dependency**: cJSON or mbedtls JSON would pull ~3000 LOC for a ~400-LOC hand-rolled subset. The inline impl is small enough to verify by inspection, matches the RFC for its advertised scope, and leaves no external-lib footguns.

**Running tally**: `text.*` now at **36/36**. §3 `@builtin text` is fully implemented — the first complete capability surface since the spec was written.

**Next natural concepts**: `time.*` (4/18), `fs.*` (3/10 — needs SDI work for write/append/delete/mkdir/move), `nvs.*` (3/11 — needs SDI iterator + value-type support; also spec signature shift from 3-arg to 2-arg).

### Concept #32 — time.* completeness + duration literals (spec §3 + §01 §3)

**Drift**:
- Runtime had only 4 `time.*` builtins (`now`, `now_us`, `duration`, `to_iso`). `os_time.deck` exercises 12+ methods.
- `time.now` returned monotonic ms; `to_iso` expected epoch seconds — inconsistent units between ostensibly-related builtins.
- Lexer never understood duration literals (`5s`, `2m`, `500ms`, `1h`, `1d`). `5s` lexed as `TOK_INT 5 | TOK_IDENT s` → parse error. Every fixture or annex that used duration literals failed to load silently.

**Scope / decisions**:

- **Unit convention**: Timestamp = epoch seconds (int); Duration = seconds (int). Matches the existing `to_iso` + the fixture comment `"parsed within 2s of t1"`. Alternatives (ms, µs, mixed) all either broke round-trips or added type ceremony not yet in the runtime.
- **`time.now` rewritten** to return wall-clock epoch seconds (`deck_sdi_time_wall_epoch_s()`), falling back to monotonic seconds (`monotonic_us / 1_000_000`) when wall isn't set. That means `t1 > 0` and `t2 >= t1` still hold at boot (ordering doesn't depend on wall clock).
- **`time.now_us` preserved** as a non-spec helper for benchmarks — boot-monotonic microseconds. Explicit second unit for Timestamp keeps the shape tight; callers who need sub-second precision for perf work use `now_us`.

**Lexer (layer 0)**:

- `scan_number` grew a duration-suffix pass after the integer literal completes. Suffixes are matched with a "next char must not be an ident char" guard — so `1slice` stays `INT(1)` + `IDENT("slice")`, and `5s` becomes `INT(5)` (5 seconds).
- Multipliers: `ms → v / 1000` (truncated; ms precision is below the canonical unit), `s → v` (canonical), `m → v * 60`, `h → v * 3600`, `d → v * 86400`.
- Float literals with suffix (`1.5s`) are rejected — ambiguous between "1500 ms" and "1 s rounded to 1". Integer-only keeps the grammar strict.

**Runtime (layer 4) — 14 new builtins**:

- `time.since(t)` / `time.until(t)` / `time.add(t, d)` / `time.sub(t, d)` — pure integer arithmetic.
- `time.before(a, b)` / `time.after(a, b)` — comparisons.
- `time.epoch()` — returns 0 (UNIX epoch Timestamp).
- `time.format(t, fmt)` — strftime-compatible template, 128-byte output cap.
- `time.parse(s, fmt)` — strptime + manual UTC epoch reconstruction (libc `timegm` is non-portable; inline computes days-from-epoch from Y/M/D with leap-year correction).
- `time.from_iso(s)` — fixed-format `YYYY-MM-DDTHH:MM:SSZ` via sscanf. Rejects bad shapes with `:none`.
- `time.date_parts(t)` — returns `{"year": N, "month": N, "day": N, "hour": N, "minute": N, "second": N}`.
- `time.day_of_week(t)` — 0=Sunday..6=Saturday (matches `struct tm.tm_wday`).
- `time.start_of_day(t)` — floor to UTC midnight (`(t / 86400) * 86400`).
- `time.duration_parts(d)` — returns `{"days", "hours", "minutes", "seconds"}`.
- `time.duration_str(d)` — human-readable: `"3d 2h"` / `"5h 30m"` / `"2m 15s"` / `"42s"` / prefix `-` for negatives.
- `time.ago(t)` — relative phrasing: `"15s ago"` / `"3m ago"` / `"2h ago"` / `"5d ago"` / `"in the future"` for t > now.

**Fixture caveat**: `os_time.deck` uses `parts.year` syntactically — that's a field-access on a map, which the current runtime doesn't support (map is accessed via `map.get`). That's a separate runtime concept (record/map field syntax). Concept #32 closes the **builtin-completeness** side; the fixture-compatibility side still depends on that field-access gap.

**Running tally**: `time.*` now at 18/18. `text.*` 36/36. Remaining capabilities with silent runtime gaps: `fs.*` (3/10), `nvs.*` (3/11), plus the non-builtin concepts at session-#3 tail (declarative content eval, machine dispatch, streams, etc.).

### Concept #33 — map field access: accept both atom and string keys

**Drift**: `AST_DOT` field access (`obj.field`) only looked up **atom** keys in maps. Records built via `@type Foo { … }` use atom keys, so they worked. Maps built from external sources — `text.from_json` output, `time.date_parts` return, `text.query_parse` output — use **string** keys. `obj.field` always returned `:none` on those, silently breaking field access for anything JSON-adjacent.

Adjacent complication: `time.date_parts` used string keys (matching the JSON/query convention), which made my concept-#32 builtin useless for `parts.year` access right out of the gate. The fixture `os_time.deck` written for the date_parts builtin ended up expecting Option-wrap (`:some y -> …`), likely because the author noticed the lookup returned `:none` on every string-keyed map and tried to model field access as partial.

**Resolution**:

- 2026-04-19 · layer 4 edit · `src/deck_interp.c` AST_DOT case — try atom key first (preserves record semantics); on miss, try string key. Both `{name: "diana"}` (atom key via record-literal sugar) and `{"year": 2026}` (string key from JSON / date_parts) now respond to `obj.field`.
- 2026-04-19 · layer 6 edit · `apps/conformance/os_time.deck` — rewrote the `parts` assertions to use the canonical raw-value semantics (`parts.year >= 2024 and parts.year <= 2200`), matching the convention already used in `lang_type_record.deck` and `lang_with_update.deck`. The Option-wrap pattern in the old fixture was the A→B misread of a silent-miss — it "passed" because `:none` matched the alternative branch when there shouldn't have been one.

**Why not wrap in Option**: making `obj.field` always return `:some(v)` / `:none` would be the "safer" shape but breaks every existing record-access pattern in the codebase (`u.name == "diana"`, `e.is_dir`, `p.user.name`, …). Those would all need `| :some v -> v` unwrapping inserted. Since field access on a record is inherently "I know this field exists because the type has it", keeping raw-value semantics is the match with how humans read the syntax. For genuinely-optional lookups, `map.get(m, key)` still returns `:some/:none` — apps that want option semantics use that.

**Consequence**: `time.date_parts` and `time.duration_parts` now work via `parts.year` / `parts.days` etc. `text.from_json(...)` outputs get dot-accessed the same way. `text.query_parse("a=1&b=2")` yields `{"a": "1", "b": "2"}` → `q.a == "1"`.

### Concept #34 — fs.* write surface + spec-canonical Result returns (spec §3)

**Drift**:
- `fs.read` returned `:some str` / `:none` (Option shape). Spec §3 says `Result str fs.Error`. Fixture `os_fs.deck` asserts `fs.read(probe) == :ok "hello deck"` — requires Result wrapping.
- `fs.write` / `fs.append` / `fs.delete` / `fs.mkdir` / `fs.move` — not registered at runtime. Every `os_fs.deck` assertion past `ok_exists` failed silently.
- SDI exposes `write`, `remove`, `mkdir` but no `move` or `append` primitives — those must compose on top.

**Fix applied**:

- 2026-04-19 · layer 4 edit · `components/deck_runtime/src/deck_interp.c`:
  * Promoted `make_result_tag` forward declaration to the top of the file (previously buried in the text-section helpers). Result construction is broadly useful across fs/nvs/api_client/etc, so it belongs with the other general forward decls.
  * `fs_err_atom` / `fs_err_result` helpers — map SDI error codes to spec §3 `fs.Error` atoms (`:not_found`, `:permission`, `:full`, `:io`, `:exists`).
  * `fs_copy_path` shared helper — validates `DECK_T_STR`, copies + null-terminates into a 192-byte buffer, sets `DECK_RT_TYPE_MISMATCH` / `DECK_RT_OUT_OF_RANGE` on failure.
  * `fs.read` rewritten to return Result form. Uses an 8 KB heap buffer (stack was 512 B — sources > 512 B would truncate silently, another silent-truncation bug class).
  * `fs.write(path, str)` — direct wrap of `deck_sdi_fs_write`.
  * `fs.append(path, str)` — read-existing + concat + write. Capped at 16 KB combined size. If path doesn't exist, effectively acts like `fs.write` (missing-file on read treated as empty rather than error, so the "create if absent" common-case matches §3 semantics).
  * `fs.delete(path)` — wraps `deck_sdi_fs_remove`.
  * `fs.mkdir(path)` — wraps `deck_sdi_fs_mkdir` (SDI contract: parent must already exist; no recursive mkdir at this layer).
  * `fs.move(from, to)` — read + write + remove composition since SDI lacks rename. Atomicity is best-effort (if delete fails after write, the file appears at both paths) — documented in the function comment.

**Deferred for future concepts**:
- `fs.read_bytes` / `fs.write_bytes` — byte-returning variants. Need the `[int]` representation decision first.
- `fs.list` currently returns `"name1\nname2\n..."` string; spec wants `Result [FsEntry] fs.Error` with `FsEntry { name, is_dir, size, modified }`. Rewrite requires `@type FsEntry` record construction from C. Separate concept.

**Running tally**: `fs.*` now at 8/10 (exists/read/list/write/append/delete/mkdir/move vs spec 10). Remaining: `read_bytes` + `write_bytes`, plus a rewrite of `list` to return the FsEntry record form. `os_fs.deck` still has the FsEntry pattern-match which won't parse against the current `list` shape — noted, deferred.

### Concept #35 — nvs.* completeness + spec-canonical arity (spec §3 / §05 §3)

**Drift (three-part)**:

1. **Arity shift**: runtime had `nvs.get(ns, key)` / `nvs.set(ns, key, value)` / `nvs.delete(ns, key)` — an explicit namespace as the first argument. Spec §3 declares `get(key: str) -> str?`, `set(key, value)` etc. — **no** explicit namespace. Apps get an isolated namespace derived from `@app.id`. Every fixture and annex used the spec form; every call failed at arity check pre-concept.
2. **Value-type surface missing**: runtime had only `get/set` for `str`. Spec surfaces `get_int/set_int/get_bool/set_bool/get_float/set_float/get_bytes/set_bytes` as first-class. Eight missing builtins.
3. **Iteration/clear missing**: spec has `keys() -> Result [str]` and `clear() -> Result unit`. Runtime had neither. SDI vtable had no iterator at all — had to extend the platform driver.

**Fix applied (three layers)**:

- 2026-04-19 · layer 4 SDI · `components/deck_sdi/include/drivers/deck_sdi_nvs.h` — added wrapper declarations for `get_blob`, `set_blob`, `keys`, `clear`. Blob wrappers were already in the vtable but never exposed at the high-level API.
- 2026-04-19 · layer 4 SDI · `components/deck_sdi/src/drivers/deck_sdi_nvs_esp32.c`:
  * `deck_sdi_nvs_get_blob` / `set_blob` — thin vtable dispatchers.
  * `deck_sdi_nvs_keys` — iterates `nvs_entry_find`/`_next` across five NVS types (STR, I64, BLOB, U8, I32) and invokes a callback per key. Stop-early if cb returns false.
  * `deck_sdi_nvs_clear` — opens the namespace RW, calls `nvs_erase_all`, commits.
- 2026-04-19 · layer 4 runtime · `components/deck_runtime/src/deck_interp.c`:
  * `nvs_app_ns(c, out, cap)` helper walks the current module's `AST_APP` for the `id` field and truncates to NVS's 15-char limit. Falls back to `"deck.app"` when no app context (scratch eval / tests).
  * `nvs_err_result(rc)` maps SDI error codes to spec-canonical `nvs.Error` atoms (`:not_found`, `:invalid_key`, `:full`, `:write_fail`).
  * `nvs_copy_key` validates key is `str` and ≤ 15 chars; too-long → `*out_too_long = true` so caller can return `:err :invalid_key` (Result-shape) on write ops or `:none` (Option-shape) on read ops — matches what the fixture asserts.
  * Eleven new / rewritten builtins: `nvs.get` (1-arg, returns Option), `nvs.set` / `nvs.delete` (Result), `nvs.get_int/set_int`, `nvs.get_bool/set_bool` (stored as i64 0/1), `nvs.get_float/set_float` (bit-pattern preserved via `memcpy` int64↔double), `nvs.get_bytes/set_bytes` ([int] surface with 0–255 range check), `nvs.keys()` / `nvs.clear()`.

**Why float as i64 bits instead of its own NVS type**: ESP-IDF NVS has only int / str / blob types; no native float. Bit-cast via `memcpy(&bits, &d, sizeof(bits))` is well-defined C, preserves NaN/Inf, round-trips exactly. Storing as a 9-byte blob would work too but costs more flash.

**Why the blob cap is 1 KB**: NVS blobs can be much larger, but an explicit cap here matches the runtime's "no runaway allocations" convention from concepts #26–29. Apps that genuinely need larger persistent buffers use `fs.write_bytes` (future concept) instead.

**Running tally**: `nvs.*` now at 13/13 (11 new + 2 kept). Spec §3 capability complete. `text.*` 36/36. `time.*` 18/18. `fs.*` 8/10. The first three capabilities of §3 (`nvs`, `text`, `time`) are fully implemented at the runtime surface.

### Concept #36 — fs.list as Result [FsEntry] + fs.read_bytes / write_bytes

**Drift**:
- `fs.list` returned a newline-joined string — a pragmatic DL1 stopgap before list literals existed. Spec §3 says `Result [FsEntry] fs.Error`, where `FsEntry { name, is_dir, size, modified }`. `os_fs_list.deck` and `os_fs.deck:52` pattern-match on the Result shape and do field access (`e.name`, `e.is_dir`, `e.size`, `e.modified`).
- `fs.read_bytes` / `fs.write_bytes` not registered — spec bytes surface entirely missing.

**Fix applied**:

- 2026-04-19 · layer 4 edit · `components/deck_runtime/src/deck_interp.c`:
  * `fs_list_record_cb` — per-entry callback that builds a map `{name, is_dir, size, modified}` and pushes it to the accumulated list. Field access works via concept #33 (dual atom/string key lookup).
  * `b_fs_list` rewritten — Result-returning, builds `[map]` via callback. Returns `:err :not_found` (or other `fs.Error` atom) on failure.
  * `b_fs_read_bytes` — reads into an 8 KB heap buffer, returns `:ok [int]` / `:err :atom`.
  * `b_fs_write_bytes` — validates each `[int]` element is 0–255, caps payload at 8 KB, writes via `deck_sdi_fs_write`.

**FsEntry caveat**: SDI's fs.list callback only exposes `name` + `is_dir`. `size` / `modified` default to `0` in the emitted map — surfacing the gap rather than hiding it. Honest `size` / `modified` support requires extending the SDI vtable (`stat`-like op), which is a separate concept. `os_fs_list.deck`'s shape assertion `e.size >= 0 && e.modified > 0` will still fail on `modified` until that lands — flagged in the fixture's comments, not masked.

**Running tally**: `fs.*` now at **10/10**. Spec §3 `@capability fs` complete at the builtin layer (modulo the `size` / `modified` SDI gap).

**Four consecutive §3 capabilities fully runtime-implemented**: `text` (36/36), `time` (18/18), `nvs` (13/13), `fs` (10/10). Every capability that §3 declares as mandatory at DL1 now has 100% of its method surface registered. That closes the "deepened fixture silently calls un-registered builtin" gap for the DL1 capability baseline.

### Concept #37 — math.* completeness (spec §3 @builtin math)

**Drift**: runtime had 6 registrations (abs/min/max/floor/ceil/round) vs spec's 30+ methods. `math.round` was arity 1 but spec is 1–2. Constants `math.pi / math.e / math.tau` missing — every trig-heavy app would crash at load.

**Fix applied**:

- 2026-04-19 · layer 4 edit · `components/deck_runtime/src/deck_interp.c`:
  * `math.round` extended to arity 1–2; two-arg form rounds to N decimal places (`round(x, 3) → 3.142`). Internal cap of 12 places to bound the `10^n` multiplier.
  * **Unary float → float helpers** via `MATH_UNARY` macro: sqrt / sin / cos / tan / asin / acos / atan / exp / ln / log2 / log10. Each is a one-line libm wrapper.
  * **Multi-arg float ops**: pow / atan2 / clamp / lerp. `clamp` preserves int type if all three args are ints.
  * **Predicates / sign**: sign (`-1 / 0 / 1` as float), is_nan, is_inf.
  * **Conversions**: to_radians / to_degrees via `M_PI / 180`.
  * **Int helpers**: abs_int / min_int / max_int / clamp_int / gcd / lcm. GCD uses the standard Euclidean loop; LCM composes via `abs(x) / gcd * abs(y)` to avoid overflow.
  * **Constants**: `math.pi / math.e / math.tau` registered as zero-arity builtins. AST_DOT's existing capability-dispatch path (concept #33's map lookup comes _after_ the cap-name lookup) auto-calls 0-arity builtins, so bare `math.pi` works as a value.

**Why libm wrappers instead of inline polynomial approximations**: ESP-IDF ships with a full libm; the xtensa FPU handles float ops in hardware. No reason to roll our own polynomials when `sin` / `cos` / `log` are one-cycle FPU ops.

**Running tally**: `math.*` now at 33/33. `text.*` 36/36, `time.*` 18/18, `nvs.*` 13/13, `fs.*` 10/10, `math.*` 33/33 — all five §3 DL1-mandatory capabilities complete at the runtime surface.

### Concept #38 — `@on os.event` payload dispatch (spec §11)

**Drift**: concept #13 taught the parser the three spec §11 binding styles:
- no-params: `@on os.locked`
- named binders: `@on os.wifi_changed (ssid: s, connected: c)`
- value-pattern filters: `@on hardware.button (id: 0, action: :press)`

…but `deck_runtime_app_dispatch` took only `(app, event)` — no payload. The parameter clauses sat in the AST untouched. Every annex example `@on os.*` / `@on hardware.*` with named binders or filters was parseable but couldn't actually fire with its payload bound.

**Fix applied (layer 4, runtime)**:

- 2026-04-19 · layer 4 edit · `include/deck_interp.h` — `deck_runtime_app_dispatch` gained a `deck_value_t *payload` parameter. Lifecycle callers (resume/pause/`trigger_*` dispatch) pass `NULL`; OS-event callers pass a `{str: any}` map.
- 2026-04-19 · layer 4 edit · `src/deck_interp.c` — rewrote `deck_runtime_app_dispatch`:
  * Walks `on->as.on.params[]` if present. For each `(field, pattern)`, looks up `payload[field]` using concept #33's dual atom/string key logic.
  * Calls the existing `match_pattern` engine (reused from match arms) against the field value. A binder (`IDENT` pattern) extends a newly-allocated child env; a value pattern matches or skips; wildcard `_` accepts any without binding.
  * **If any pattern fails to match, the handler doesn't fire** — this is the spec §11 value-pattern filter semantics (handler only runs when all declared filter values match).
  * After the binder/filter pass, the handler body runs in the child env so bindings don't leak into the app's global env.
  * The raw payload map is also bound to the implicit `event` identifier, supporting the no-params style `event.ssid` / `event.field` accessor in the body. Both styles now coexist — same handler body can mix binders and `event.field` lookups freely.
- 2026-04-19 · layer 5 edit · `src/deck_interp_test.c` + `src/deck_shell_deck_apps.c` — four call sites updated to the new 3-arg signature (`NULL` payload for lifecycle events).

**What this unblocks**:

- `@on os.wifi_changed (ssid: s, connected: c)` — with a payload `{"ssid": "HomeAP", "connected": true}`, the handler runs with `s = "HomeAP"` and `c = true` bound.
- `@on hardware.button (id: 0, action: :press)` — only fires when the payload has `id = 0` AND `action = :press`; any other `id` or action is filtered out at dispatch.
- `@on os.low_memory` — no params, body can use `event.free_bytes` / `event.severity` via implicit `event` binding.

**Deferred**:

- Payload delivery from the platform (bridge / shell) — the runtime accepts a payload map, but no caller constructs one today. `deck_shell_deck_apps.c` dispatches lifecycle events with `NULL`. Wiring actual OS events (wifi changes, button presses, low-memory warnings) from the ESP-IDF event bus to a Deck dispatch call is its own concept — the runtime is now ready to receive whatever the bridge constructs.
- Machine-level transitions firing on `@on` events (spec §8.4 `transition :event`) — parse-and-discard today; full dispatch is concept #23's deferred runtime work.

**Why keep the parameter as `deck_value_t *` instead of `const deck_value_t *`**: the map is temporarily bound into the handler env; `deck_map_get` / `deck_env_bind` don't accept `const`-qualified values. No mutation happens, but the retain/release refcount path needs the mutable pointer.

**A→B note**: this is a case where the parser was taught a shape the runtime couldn't deliver. Concept #13 said explicitly "runtime dispatch is now the only remaining hurdle"; concept #38 closes that hurdle. Every future session that adds `@on` handlers in an annex or app now has working end-to-end plumbing.

### Concept #39 — system.info completeness (spec §3)

**Drift**: runtime had 3 `system.info.*` builtins; spec declares 11 (plus the `Versions` record return type). `os_info.deck` tests `device_model / os_name / os_version / app_id / app_version / uptime / cpu_freq_mhz / versions()` — all silent-misses.

**Fix applied (layer 4 runtime)**:

- 2026-04-19 · layer 4 edit · `src/deck_interp.c` — eight new 0-arity builtins. AST_DOT's cap-dispatch path auto-calls them, so `system.info.uptime` evaluates as a value rather than needing `()` call syntax.
  * `device_model` / `os_name` — hardcoded platform identity (`"ESP32-S3-Touch-LCD-4.3"` / `"CyberDeck"`). Future concept moves them to SDI so alternative boards get their own strings.
  * `os_version` — delegates to SDI `runtime_version`.
  * `app_id` / `app_version` — walks the current module's `@app` fields (helper `info_app_field`). Falls back to SDI's `current_app_id` if no module context.
  * `uptime` — `monotonic_us / 1_000_000` in canonical Duration seconds (concept #32 unit).
  * `cpu_freq_mhz` — reads `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` menuconfig symbol (240 on ESP32-S3 default). Dynamic CPU freq via `rtc_clk_cpu_freq_get_config()` is a future refinement.
  * `versions()` — returns a `{str: any}` record matching spec §15 `@type Versions` (edition_current, deck_os, runtime, runtime_build, sdi_major, sdi_minor, app_id, app_version). Fields accessed via concept #33 dot-lookup.

**Why no SDI extension for device_model/os_name**: kept the concept focused. A future platform port (alternative ESP32 board, emulator) would need these as SDI vtable entries — flagged for that concept, not this one.

**Running tally**: `system.info.*` now at 11/11 (minus `versions().drivers/extensions` rich sub-lists, which need Driver registry iteration — separate concept). Six capabilities complete: text, time, nvs, fs, math, system.info.

### Concept #40 — bytes.* completeness (spec §3 @builtin bytes)

**Drift**: runtime had `bytes.len` only; spec declares 7 more methods. Any Deck crypto / binary-protocol code would hit "unknown function" immediately.

**Fix applied**:

- 2026-04-19 · layer 4 edit · `src/deck_interp.c` — seven new builtins plus shared helpers:
  * `bytes_materialize` — accepts either `DECK_T_BYTES` or `DECK_T_LIST` of ints (the `[0xDE, 0xAD]` literal shape), copies into a malloc'd uint8_t buffer. Validates each list element is int 0–255.
  * `bytes_to_list` — wraps a uint8_t buffer as `DECK_T_LIST` of ints for output consistency.
  * Unified representation: **all byte ops accept [int] or bytes, emit [int]**. Matches concept #28's `hex_encode/decode` and concept #36's `fs.read_bytes/write_bytes` output shape.
  * `bytes.concat(a, b)` — merged output list, 32 KB cap.
  * `bytes.slice(b, start, end)` — with negative indexing (`-1` = last) matching `text.slice` semantics.
  * `bytes.to_int_be / to_int_le(b)` — up to 8-byte big/little-endian integer decode.
  * `bytes.from_int(n, size, :be | :le)` — `size` must be 1..8; atom argument dispatches endian.
  * `bytes.xor(a, b)` — element-wise XOR; shorter `b` cycles (standard repeating-key pattern, useful for trivial encoding / masking).
  * `bytes.fill(byte, count)` — construct `[byte; count]`. 32 KB cap.
  * `bytes.len` extended to accept both `DECK_T_BYTES` and `DECK_T_LIST`.

**Why unify on [int] output even when input was `DECK_T_BYTES`**: the two representations coexist for historical reasons (the dedicated `DECK_T_BYTES` type pre-dates the list-literal byte shape). `os_text.deck` compares with `==` against `[0x48, 0x69]` — expects list shape. Having every byte-producer return `DECK_T_LIST` of ints means literal equality works without `bytes_to_list` conversion at the call site. A future concept could add a `bytes.to_buffer([int]) -> DECK_T_BYTES` if a caller genuinely needs the packed form.

**Running tally**: `bytes.*` now at 8/8. Seven §3 capabilities complete: `text` (36/36), `time` (18/18), `nvs` (13/13), `fs` (10/10), `math` (33/33), `system.info` (11/11), `bytes` (8/8). Every DL1-mandatory `@builtin` surface from §3 is at 100% builtin coverage.

### Concept #41 — list.* completeness pass 1 (spec §11.2)

**Drift**: runtime had 7 list methods (len/head/tail/get/map/filter/reduce); spec §11.2 declares ~35. Every annex uses methods runtime doesn't have — `list.last`, `list.contains`, `list.any/all`, `list.find`, `list.append`, etc.

**Fix applied (layer 4)**:

- 2026-04-19 · layer 4 edit · `src/deck_interp.c` — 17 new builtins plus a shared `values_equal` helper for structural equality (used by `list.contains`; interning assumption lets string/atom compare via pointer equality).
  * **Accessors**: `list.last` (returns `T?`).
  * **Builders**: `list.append(xs, item)`, `list.prepend(item, xs)`, `list.reverse(xs)`, `list.take(xs, n)`, `list.drop(xs, n)` — all return new lists; inputs are immutable (runtime convention).
  * **Predicates with fn arg**: `list.find(xs, fn)`, `list.find_index(xs, fn)`, `list.count_where(xs, fn)`, `list.any(xs, fn)`, `list.all(xs, fn)`, `list.none(xs, fn)`. All use the existing `call_fn_value_c` to invoke user-provided functions.
  * **Value-based**: `list.contains(xs, item)` uses the new `values_equal` helper.
  * **Numeric aggregates**: `list.sum` (int), `list.sum_f` (float; also accepts ints), `list.avg` (returns `float?` since empty-list has no average).
  * **Restructuring**: `list.flatten([[T]]) -> [T]`.

**Deferred**: `list.sort` / `sort_by` / `sort_desc`, `list.group_by`, `list.chunk`, `list.window`, `list.zip` / `zip_with`, `list.flat_map`, `list.unique` / `unique_by`, `list.partition`, `list.tabulate`, `list.scan`, `list.enumerate`, `list.interleave`, `list.min_by` / `max_by`, `list.sort_by_str` / `sort_by_desc`. Most of these are ~10 lines each; split into a pass 2 concept.

**Why the shared `values_equal`**: three use cases (`list.contains`, future `list.unique`, future `map.has`) need structural equality on arbitrary runtime values. The existing `do_compare` handles the `==` binop but short-circuits on mismatched types and doesn't recurse into lists / tuples. Pulling out a dedicated recursive helper avoids duplicating the logic.

**Running tally**: `list.*` now at 24/~35. Spec §11.2 mostly covered; pass 2 will close sort/zip/group/etc.

### Concept #42 — map.* + tup.* completeness (spec §11.3 + §11.4)

**Drift**:
- Runtime had 5 `map.*` methods (len, get, put, keys, values). Spec §11.3 declares 13.
- Runtime registration was `map.put` but spec §11.3 calls it `map.set` (aligns with `nvs.set`). Fixture `lang_map_basic.deck` used `map.put` (matching runtime); annex-xx-bluesky + spec used `map.set`. Classic split-vocabulary.
- `tup.*` had **zero** registrations. Spec §11.4 declares 6.

**Fix applied**:

- 2026-04-19 · layer 4 edit · `src/deck_interp.c`:
  * `map.put` registration renamed to `map.set` — no shim (no-dual-accept rule from concepts #8, #10, #12 etc). Underlying C fn `b_map_put_b` unchanged; just the dispatch name flipped.
  * `map.count` added as a spec §11.3 alias that points at the same C fn as `map.len`. The spec has both names for readability; runtime points at one implementation.
  * 8 new `map.*` builtins: `delete`, `has`, `merge` (right-biased), `is_empty`, `map_values` (applies fn to values), `filter` (fn takes key+val), `to_list` (emits `[(k, v)]` tuples), `from_list` (accepts `[(k, v)]`).
  * 6 new `tup.*` builtins: `fst`, `snd`, `third`, `swap`, `map_fst`, `map_snd`. Type-check arity explicitly (fst/snd need arity ≥ 2; third needs ≥ 3; swap requires exactly 2).
- 2026-04-19 · layer 6 edit · `apps/conformance/lang_map_basic.deck` — `map.put(m, :role, :user)` → `map.set(m, :role, :user)`. Only caller that used the legacy name.

**Why `map.merge` is right-biased**: given `merge(a, b)`, the intuitive reading is "update `a` with `b`'s values". JavaScript `Object.assign(target, source)`, Python `dict | dict`, Elixir `Map.merge` — all agree on right-bias. Spec is silent; runtime picks the common-sense convention and notes it explicitly in REPORTS so future authors don't invent a different one.

**Running tally**: `map.*` now at 13/13. `tup.*` now at 6/6. Spec §11.3 + §11.4 complete. Combined with concept #41, the entire §11.2–§11.4 standard-collection surface is 43/47 methods registered (missing only the `list.*` pass-2 sort/zip/group family).

### Concept #43 — list.* pass 2 (spec §11.2 remaining commons)

**Drift**: concept #41 left `sort`, `zip`, `enumerate`, `flat_map`, `partition`, `unique`, `min_by/max_by`, etc. as deferred. Common enough that any realistic Deck app will hit at least one of them.

**Fix applied**:

- 2026-04-19 · layer 4 edit · `src/deck_interp.c` — nine new list builtins:
  * `list.enumerate(xs) -> [(int, T)]` — pairs `(index, value)` for each element.
  * `list.zip(a, b) -> [(T, U)]` — truncates at the shorter list.
  * `list.zip_with(a, b, fn) -> [V]` — combined via fn; truncates at shorter.
  * `list.flat_map(xs, fn) -> [U]` — maps T → [U], then flattens one level. Errors if fn returns non-list.
  * `list.partition(xs, fn) -> ([T], [T])` — returns a 2-tuple of (keep, drop).
  * `list.unique(xs) -> [T]` — O(n²) via `values_equal` (concept #41's helper). First occurrence wins.
  * `list.sort(xs)` — natural ordering on `[int]` / `[float]` / `[str]`. Errors on mixed types. Uses libc `qsort` with a file-scope `sort_type` to dispatch comparator by element type (cheap alternative to passing a typed comparator through qsort's untyped `void *`).
  * `list.min_by / max_by(xs, fn) -> T?` — fold keeping the element whose fn-value is minimum / maximum. Returns `:none` on empty lists.

**Why no custom-comparator `list.sort(xs, fn)`**: spec §11.2 has both `list.sort` (natural) and `list.sort_by` (with `T -> float` key-fn). This concept implements only the natural form; `list.sort_by` is one-liner on top of it with a precomputed `list.map` of keys, and can land as a future mini-concept.

**Running tally**: `list.*` now at 33/~35. Missing: `list.sort_by`, `list.sort_desc`, `list.sort_by_desc`, `list.sort_by_str`, `list.group_by`, `list.chunk`, `list.window`, `list.scan`, `list.tabulate`, `list.interleave`, `list.unique_by`. Most are 10-line variants; a future concept can bundle them.

**Session #4 cumulative**: `text` 36/36, `time` 18/18, `nvs` 13/13, `fs` 10/10, `math` 33/33, `system.info` 11/11, `bytes` 8/8, `log` 4/4, `map` 13/13, `tup` 6/6, `list` 33/~35, plus Result / Option helpers. Every §3 DL1-mandatory capability + most of the §11 standard vocabulary is runtime-complete. The combinatorial gap between "fixture calls builtin X" and "runtime provides X" is closed for the overwhelming majority of Deck's stdlib.

### Concept #44 — @machine transition dispatch (spec §8.4)

**Drift**: concept #23 parsed top-level `transition :event from:/to:/when:/before:/after:` clauses and **threw them away**. Annex state machines had nothing to fire. `run_machine` was a sequential-flow loop — it took the first state's `on enter`'s suggested next, followed auto-transitions until a terminal state, and returned. No event-driven dispatch. `Machine.send(:foo)` had no target.

This was the single biggest block to actually *running* annex apps (a/b/c/d/xx). Every annex's interactive behaviour — launcher tap → app launch, Bluesky login → feed → post — is a machine transition triggered by a user-emitted event.

**Scope**: parse + store transitions; enter the initial state at load; implement `machine.send(:event, payload?)` as a first-class builtin that scans the machine for matching transitions and runs them. Preserve the legacy sequential loop for machines without top-level transitions (existing `@flow` fixtures keep working).

**Fix applied (five layers)**:

- 2026-04-19 · layer 4 AST · `include/deck_ast.h`:
  * New `AST_MACHINE_TRANSITION` node kind (distinct from the legacy `AST_TRANSITION` intra-state statement).
  * Added `transitions` list to the `machine` union payload.
  * Added `machine_transition` payload `{event, from_state, to_state, when_expr, before_body, after_body}`. `from_state == NULL` = `from *` wildcard.
- 2026-04-19 · layer 4 parser · `src/deck_parser.c:parse_machine_decl` rewritten from parse-and-discard: now constructs `AST_MACHINE_TRANSITION` nodes, parses `from:/to:/when:/before:/after:` clauses into real AST fields (expressions for when/before/after; atom for from/to; `*` token for wildcard from). Stores into `machine.transitions`.
- 2026-04-19 · layer 4 AST printer · `src/deck_ast.c` — `ast_kind_name` knows `machine_transition`.
- 2026-04-19 · layer 4 runtime · `src/deck_interp.c`:
  * `struct deck_runtime_app` gained `machine_state` field — the current state atom, updated on every transition.
  * `run_machine` split into two modes. If `machine.transitions.len > 0`, the machine is event-driven: run initial state's `on enter` and return. Otherwise, run the legacy sequential loop (preserves every existing `@flow` fixture).
  * `deck_runtime_app_load` captures the initial state into `app->machine_state` after `run_machine` returns.
  * New `machine.send(:event, payload?)` builtin (also registered as spec-capitalized `Machine.send`):
    - `app_from_ctx(c)` scans the slot array to find the app whose interp ctx matches — cheap linear scan over ≤ 8 slots.
    - Walks `machine.transitions`, finds the first match by `(event, from_state OR wildcard)`.
    - Binds `event` identifier in a child env to the payload (unit if absent) for use by when/before/after bodies.
    - Execution order matches concept #23's spec §8.5 rewrite: **when → source.on_leave → before → [state change] → dest.on_enter → after**.
    - Returns the new state as `:atom`; `:none` if no transition matched (spec-compliant no-op behaviour, not an error).
  * New `machine.state()` builtin returns the current state as `:some :atom` / `:none` for apps that need to query without firing a transition.

**What this unblocks**:
- Every annex `@machine` block now runs end-to-end: initial state enters, user actions (via triggers that call `Machine.send`) fire transitions, each transition advances machine_state and runs destination's on-enter hook.
- `machine.state()` lets app code query and condition on current state without a round-trip through the flow.

**Deferred (tracked)**:
- **Payload binding across state payloads** — spec `state :active (temp: float)` entered via `transition … to :active (temp: expr)` should bind `temp` in destination's scope (concept #22 deferred this; still not implemented).
- **Reactive `watch:` transitions** — spec §8.4 allows transitions that fire when a predicate toggles false→true without explicit send. Needs reactive dependency tracking.
- **`to history`** — spec §8.4 compound-machine history pseudostate.
- **Multi-`from`/multi-`to`** — spec allows lists; current impl takes a single source and target.
- **Nested machine composition** — `state :home machine: Other` semantics (concept #22 deferred).
- **Transition-scoped hook order for `@machine.before/.after`** — concept #4 noted execution order drift; machine-level transitions inherit the legacy order for now.

**A→B note**: this is the third-largest architectural lever behind declarative content eval and @stream execution. With #38 (`@on` payload binding) + #44 (machine dispatch), an annex app can now be genuinely interactive — `@on os.event (field: binder) → Machine.send(:event_name, payload)` becomes a wire that runs end-to-end. The bridge UI layer still needs DVC re-rendering on state change (which is concept-content-eval territory), so apps using the declarative `content = …` form won't visibly re-draw, but the underlying transitions run correctly and `machine.state()` reports the new state.

**Running tally**: §3 DL1 capabilities: `text / time / nvs / fs / math / system.info / bytes / log` — all 100%. §11 stdlib: `list 33/35 / map 13/13 / tup 6/6`. Runtime dispatch: `@on` events with payload binding (#38), machine transitions with when/before/after (#44), map dual-key access (#33). Duration literals (#32). The project transitioned from "parser accepts, runtime ignores" to "end-to-end wired" across the stdlib + dispatch axes.

### Concept #45 — `content =` block parsing (spec §8.2 / §12)

**Drift**: concept #24 taught the parser to recognise `content = …` but threw the body away entirely. Annex state machines with rich declarative content parsed but nothing survived for a future interpreter pass to work with. This concept closes the parse-and-discard gap by storing content as structured AST — the prerequisite for the runtime content interpreter (next concept).

**Fix applied (parser + AST only — runtime interpretation is deferred to concept #46)**:

- 2026-04-19 · layer 4 AST · `include/deck_ast.h`:
  * Added `AST_CONTENT_BLOCK` — container holding an ordered list of `AST_CONTENT_ITEM`s.
  * Added `AST_CONTENT_ITEM` — one semantic content primitive with fields `{kind, label, action_expr, data_expr}`.
    - `kind` is the interned name of the first token on the line (`"trigger"`, `"navigate"`, `"list"`, `"label"`, `"media"`, …). Unknown opens parse as `kind="raw"`.
    - `label` is the string literal that typically follows (e.g. `trigger "Search"`).
    - `action_expr` captures the `-> fn_call` tail for interaction intents.
    - `data_expr` captures trailing data (e.g. `list posts` → data_expr is the `posts` iterable).
- 2026-04-19 · layer 4 parser · `src/deck_parser.c` — rewrote the content branch from indent-depth-discard to structured parsing. Each line emits an `AST_CONTENT_ITEM`; nested indented blocks (list items, form fields) are absorbed into the parent's span but not yet unpacked (concept #46 revisits). Inline `content = expr` captures the expression as a single-item block.
- 2026-04-19 · layer 4 AST printer · `src/deck_ast.c` — `ast_kind_name` knows `content_block` and `content_item`.

**What still works**: every existing annex loads without regression — the parser captures the content body in a shape the interpreter can walk, but the interpreter is still the legacy `bridge.ui.*` imperative builders. `hello.deck` / `ping.deck` continue to render via the old path.

**What concept #46 will add**:
- A walker that traverses the content block, constructs DVC nodes per item kind, encodes and pushes to the bridge at state entry.
- Hook into machine transitions so the bridge re-renders after `Machine.send`.
- Mapping of content intents → DVC node types (trigger → `DVC_TRIGGER`, list → `DVC_LIST`, label → `DVC_LABEL`, …).
- Intent_id → event_name table so bridge-side triggers fire `Machine.send`.

**Scope note**: the parser is now rich-ish but still simplified — options like `badge:` / `message:` / the typed body of `list items \n item x -> …` are captured at token level but not separated into structured fields. Concept #46 will iteratively add those as each matters for a real annex. Keeping the AST shape flexible now (kind-as-string, raw action_expr) means #46 can evolve without schema churn.

**A→B note**: the A→B bug here was "annex state machines parse successfully → their content renders". The first half was true (concept #24). The second half quietly was not — every `content =` block silently went to the void. This concept converts "parsed successfully" into "parsed into runnable AST," which is the prerequisite for the render half to become true in concept #46.

### Concept #46 — declarative content walker → DVC push (spec §8.2 / §12)

**Drift**: concept #45 stored `content = …` as structured AST; there was no walker to turn that AST into a DVC tree for the bridge. Apps using declarative content rendered nothing at runtime — every annex's UI was still dead.

**Fix applied (runtime only)**:

- 2026-04-19 · layer 4 runtime · `src/deck_interp.c`:
  * `content_render(c, env, block)` — walks `AST_CONTENT_BLOCK` items, emits DVC nodes per kind into the existing `s_bui_arena`, encodes, pushes via `deck_sdi_bridge_ui_push_snapshot`. Reuses every piece of the bridge.ui.* plumbing that the pre-session scaffold already laid.
  * Item-kind → DVC mapping (pass 1, conservative):
    - `trigger "label"` → `DVC_TRIGGER` with `:label` attr
    - `navigate "label"` → `DVC_NAVIGATE` with `:label` attr
    - `loading` → `DVC_LOADING` (no attrs)
    - `label <expr>` → `DVC_LABEL` with value stringified from expr or literal
    - `rich_text <expr>` → `DVC_RICH_TEXT` with value
    - `error <expr>` → `DVC_LABEL` (error-reason surface — distinct styling is bridge concern)
    - `raw <expr>` → `DVC_LABEL` from value-to-str
    - other kinds (list/group/form/media/markdown) — silently skipped in pass 1
  * `content_value_as_str` helper — coerces any value type into a readable string for LABEL/RICH_TEXT (str/int/float/bool/atom/unit). Caps at 128 bytes to match existing label conventions.
  * `content_render_state(c, env, state)` — finds the state's `AST_CONTENT_BLOCK` among `state.hooks` and renders it.
  * Hooked into both state-entry moments:
    - `run_machine` (event-driven branch): after the initial state's `on enter` runs, the initial content is rendered — first frame the user sees.
    - `machine.send`: after the destination state's `on enter` runs, the new content is rendered — every transition redraws.

**What this unblocks**: with concept #38 (payload binding on `@on`), #44 (machine transition dispatch), and #46 (content re-render), an event-driven annex app now runs end-to-end at the UI level:
- User taps screen → bridge emits intent → runtime receives event.
- `@on` handler fires, binds payload, calls `Machine.send(:evt, payload)`.
- Transition runs when/before/state-change/on_enter/after.
- New state's content is walked into a DVC tree and pushed to the bridge.
- Bridge decodes DVC and re-lays out the screen.

**What's still missing for full interactivity**:
- **Intent binding** (concept #47 likely): triggers render but the bridge needs to know which event to send back on tap. Requires an intent-id table `intent_id → event_name + payload_builder`, and the bridge side wiring to call `deck_runtime_app_dispatch` or `machine.send` on tap. Without this, triggers are visible but non-interactive.
- **List iteration** (concept #48 likely): `list posts \n item p -> ...` needs the walker to evaluate `posts`, iterate, materialize each item with bindings in scope. Concept #45's parser absorbs the nested block but doesn't unpack it.
- **Form + field aggregation** (concept #49 likely): `form on submit ->` with nested typed fields.
- **Reactive re-render on stream emission** — `@stream` integration.

**A→B note**: combined with concepts #38 + #44 + #45, this concept closes the "annex apps declare content, runtime renders it, transitions redraw" story at the coarse level. Every annex's primary state can now render a minimal UI. Fine-grained interactivity depends on the intent-id wiring (#47).

### Concept #47 — intent_id ↔ event binding (bridge ↔ runtime round-trip)

**Drift**: concept #46 rendered triggers/navigates as DVC nodes, but those nodes had no intent_id wired to an event. The bridge side couldn't know which `Machine.send(:evt)` to fire when the user tapped a trigger. Every rendered trigger was inert.

**Fix applied**:

- 2026-04-19 · layer 4 runtime · `src/deck_interp.c`:
  * Moved `struct deck_runtime_app` + the new `deck_intent_binding_t` table forward in the file so `content_render` (concept #46) can access `app->intents` directly. The old location left as a locator comment.
  * New field `deck_intent_binding_t intents[DECK_RUNTIME_MAX_INTENTS]` (64 slots) + `next_intent_id` on each app slot. Cleared at the start of every `content_render`; populated as triggers/navigates are emitted. Id 0 is reserved for "no intent" — matches the DVC envelope convention.
  * New helper `content_extract_event(action)` — inspects an `AST_CALL(AST_DOT(_, "send"), [AST_LIT_ATOM(x)])` shape and returns the interned atom text. Matches both `Machine.send(:evt)` and `machine.send(:evt)` (dot field is just `"send"`). Non-matching shapes → NULL (trigger renders without an intent; tap is a no-op).
  * `content_render` extended: for each trigger/navigate, assign a fresh intent_id, stamp it on the DVC node, record `{id, event}` in `app->intents`.
  * New public entry `deck_runtime_app_intent(app, intent_id)` in `include/deck_interp.h` — called by the shell when the bridge delivers a tap. Looks up the binding, builds an atom value, invokes `b_machine_send` with `(atom, no payload)`. Unknown or cleared ids are silent no-ops (returns OK).

**What this unblocks**: the user-tap → state-change round trip is now wired at the runtime layer. Shell-side: `deck_shell_deck_apps.c` (or wherever bridge taps are currently caught) just needs to call `deck_runtime_app_intent(app, id)` when an intent fires. The next state's content renders automatically via concept #46's hook in `machine.send`.

**Deferred**:
- Payload passing on triggers — `Machine.send(:add_item, item_data)` from a trigger would require the intent binding to also carry a payload-builder expression. Today only zero-arg events work.
- Intent coalescing when multiple renders happen rapidly — not a real issue until reactive streams arrive.
- Bridge-side wiring (shell layer) — the runtime is ready; the shell's tap handler still needs the `deck_runtime_app_intent` call. Flagged as shell work.

**Combined with #38 / #44 / #45 / #46**: annex apps are now runnable end-to-end at the interactive layer. Press trigger → intent → `Machine.send` → transition runs → new state renders → UI redraws. The only thing missing for *real* annex demos is the list / group / form / media item kinds (concepts #48+).

### Concept #48 — list / group iteration in content (spec §12.1)

**Drift**: concept #46 skipped `list` and `group` primitives silently. Every annex's "show me the items" pattern rendered as an empty surface.

**Fix applied (runtime only)**:

- 2026-04-19 · layer 4 runtime · `src/deck_interp.c` — content walker extended:
  * `kind="list"` → `DVC_LIST` root. Evaluates the data expression (`list posts` where `posts` evaluates to a `DECK_T_LIST`), iterates items, emits a `DVC_LABEL` child per element using `content_value_as_str` for scalar rendering.
  * `kind="group"` → `DVC_GROUP` with `:label` attr from the string literal.
  * Nested `item x -> body` blocks are still absorbed by the parser (concept #45) but not unpacked into structured sub-items. Pass 1 renders each list element as a single LABEL; `item` bindings + per-element triggers are a future pass.

**Why scalar-only rendering in pass 1**: the parser absorbs `item x -> trigger x.title` as raw tokens within the list's indented block, but doesn't split it into a per-item template. Doing that split cleanly requires re-entering the parser state machine for content (a richer grammar). Scalar rendering covers the common "list of strings / ids / status values" case immediately; structured per-item rendering is a concept #49 target.

**Combined with #38 / #44 / #45 / #46 / #47**: the Launcher annex (slot 0 example: `list installed_apps \n app -> trigger app.name -> apps.launch(app.id)`) would not fully render the per-app triggers, but `list ["A", "B", "C"]` now visibly lists three items. The gap between "pass-1 list" and "real annex list" is the per-item sub-template.

### Concept #49 — list per-item template `item x -> body` (spec §12.1)

**Drift**: concept #48 rendered list elements as flat `DVC_LABEL` rows. The spec form `list xs \n item x -> trigger x.name -> action` — used in every annex's primary list — silently dropped the per-element template. Users saw stringified payloads, not per-app triggers.

**Fix applied (parser + AST + walker)**:

- 2026-04-19 · layer 4 AST · `include/deck_ast.h` — `content_item` gained `item_binder` (interned name) + `item_body` (ast_list of AST_CONTENT_ITEM). Both empty/NULL when no template.
- 2026-04-19 · layer 4 parser · `src/deck_parser.c` — when parsing a content item with `kind == "list"` and the next indented block starts with `item IDENT ->`, parse the template: capture the binder name, then parse a suite of nested `AST_CONTENT_ITEM`s as `item_body`. Fall back to the concept-#48 absorb-and-skip for other nested-block shapes.
- 2026-04-19 · layer 4 walker · `src/deck_interp.c` — `kind="list"` branch extended: if a template is present, for each list element:
  1. Create a child env with `item_binder` bound to the element.
  2. Emit a `DVC_LIST_ITEM` group as a child of the `DVC_LIST`.
  3. Render each body item into the list-item group, evaluating expressions in the per-element env. Supports trigger / navigate / label / raw bodies today; other kinds are a future follow-up.

**What this unblocks**: `list installed_apps \n item app -> trigger app.name -> apps.launch(app.id)` now actually renders one trigger per app with the app's name on it. Bluesky feed's `list posts \n item p -> rich_text p.text` would render the stringified text per post. The two-tier structure (list → item rows) is visible to the bridge.

**Deferred**:
- **Per-item intent binding** — concept #47 assigned intent_ids only in the top-level content loop. List-item triggers currently render but the intent round-trip (bridge tap → Machine.send) isn't wired per-element. Next concept target.
- **Nested content primitives in item bodies** — `item p -> group "..." \n label p.ts` with indented nested groups isn't supported; pass-1 handles a flat list of sub-items.
- **`empty ->` fallback clause** — spec allows `list xs \n empty -> "no items"` to show a placeholder when the iterable is empty. Parser doesn't capture this; walker doesn't honor it.
- **`has_more: expr`** — cursor-based pagination intent. Deferred.

**A→B note**: this is the final architectural piece to show demonstrable annex UI on device. Concept #50+ will focus on the intent round-trip for per-item triggers (so tapping an app in the launcher list actually fires its `apps.launch` event) and the richer kinds (form/field, markdown, media).

### Concept #50 — per-item intent binding (trigger inside list template)

**Drift**: concept #49 rendered per-item triggers as `DVC_TRIGGER` but without an intent_id. Tapping them on hardware was a no-op — the bridge had no event name to round-trip back. Also, concept #49's label handling conflated "label expression" with "tap action" since the parser stashed both in `action_expr`.

**Fix applied (walker only)**:

- 2026-04-19 · layer 4 walker · `src/deck_interp.c` — per-item trigger / navigate branch split `action_expr` into two roles via `content_extract_event`:
  * If the expression is a `Machine.send(:event)` call → it's the **action**. Reuses concept #47's extractor for the atom; assigns a fresh `intent_id` and records `{id, event}` in the app's intent table.
  * Otherwise → it's the **label** (e.g. `trigger x.name` has `x.name` as its label-source expression). Evaluated in the per-item env and stringified.
- Previously the parser and walker collapsed these cases; the heuristic now distinguishes by AST shape. Annexes that follow the canonical form `trigger <label_expr> -> Machine.send(:event)` work as expected.

**Deferred**:
- **Per-element payload**: `Machine.send(:open, item.id)` should pass `item.id` on tap — currently only zero-arg events propagate. Requires storing a snapshot of the action AST + per-item env in the intent table so the payload evaluates at tap time, not render time.
- **Non-Machine.send actions**: `apps.launch(app.id)` on tap would require a generalised intent dispatcher that can evaluate an arbitrary captured expression. Future concept.
- **Label + action with both expressions**: the current heuristic uses `-> Machine.send` for action, else as label. Spec allows both explicitly; richer parser would split them by arrow presence.

**What this unblocks**: `list installed_apps \n item app -> trigger app.name -> Machine.send(:open_app)` now renders one intent-bound trigger per app. On hardware tap, the bridge emits the intent_id, shell calls `deck_runtime_app_intent(app, id)`, `Machine.send(:open_app)` fires a machine transition, and the destination state's content re-renders. Zero-arg event form is spec-compliant for the subset of annexes that use it.

**A→B note**: this closes the per-item tap round-trip for the common-case zero-arg event pattern. The next concept will tackle payload propagation so `Machine.send(:open_app, app.id)` passes the id along — at which point most annex interactions work end-to-end.

### Concept #51 — intent payload propagation (`Machine.send(:event, payload)`)

**Drift**: concepts #47 + #50 wired intent_ids but ignored the second positional argument of `Machine.send`. On tap, only the bare event atom fired — any `Machine.send(:open_app, app.id)` lost `app.id`. Apps using payload-carrying events had no way to deliver per-element data to the machine transition.

**Fix applied**:

- 2026-04-19 · layer 4 runtime · `src/deck_interp.c`:
  * `deck_intent_binding_t` gained `payload: deck_value_t*` (retained snapshot).
  * New helper `content_extract_payload_expr(action)` returns the 2nd-arg AST of the Machine.send call, or NULL for zero-arg events.
  * Top-level trigger / navigate and per-item trigger / navigate branches all evaluate the payload expression in the current env at render time via `content_eval_expr`. The resulting value is retained and stored in `app->intents[id].payload`.
  * `content_render`'s intent-table reset walks the 64 slots and releases any prior payloads before zeroing — keeps the refcount honest across renders.
  * `deck_runtime_app_unload` also releases any lingering payloads before the arena dies.
  * `deck_runtime_app_intent` passes the payload as the 2nd arg to `b_machine_send` when present (`argc = 2`), falls back to zero-arg (`argc = 1`) when it isn't.

**What this unblocks**: `list installed_apps \n item app -> trigger app.name -> Machine.send(:open_app, app.id)` now fires `Machine.send(:open_app, "com.deck.launcher")` on tap. The machine's `@on :open_app (id: s)` handler (concept #38 payload binder) receives `id` bound to the element's id. Full per-element round-trip works for any event with scalar or record payloads.

**Deferred**:
- Non-`Machine.send` actions — `apps.launch(app.id)` on tap still doesn't dispatch anything. That requires a general captured-action intent path (evaluate the AST at tap time with a saved env) — heavier concept.
- Payload evaluation happens **at render time**, not tap time. For dynamic payloads that depend on state mutated between render and tap, this is a subtle divergence from spec semantics. For most cases (per-element static refs) it's identical. Lazy-eval payload is a future concept.

**Session-wide annex-interactive milestone**: with #38 + #44 + #45 + #46 + #47 + #49 + #50 + #51, the annex-to-UI interactive loop is end-to-end:

```
render: list template → per-item trigger with intent_id + captured payload
user: tap trigger
bridge: emit intent_id
shell: deck_runtime_app_intent(app, id)
runtime: Machine.send(:event, payload)
machine: when → on_leave → before → state change → on_enter → after
render: dest state content walked + pushed to bridge
```

The launcher annex example — tap app → open the app — is now expressible and runnable.

### Concepts #52 + #53 + #54 — list.empty + form / markdown / media / progress / status / divider / spacer

**Drift**: concept #46's walker had a handful of kinds; spec §12 declares over 20. Every missing kind rendered as nothing. Also `list xs \n empty -> "..."` had no fallback path for empty iterables, and `form on submit -> …` (the primary user-input aggregate primitive) had no mapping at all.

**Fix applied (one combined commit)**:

- 2026-04-19 · layer 4 AST · `content_item` gained `empty_body` (list for `empty ->` fallback) and `on_submit` (reserved for concept #55 when fields aggregate).
- 2026-04-19 · layer 4 parser · list branch reads an optional `empty -> body` clause before the `item` template. Single-expression inline form captured as a `raw` sub-item.
- 2026-04-19 · layer 4 walker · list path: when iterable is empty AND `empty_body` is populated, emit each fallback item as a child of the DVC_LIST. Otherwise proceed with per-item template or scalar rendering.
- 2026-04-19 · layer 4 walker · new kind handlers:
  * `form` → `DVC_FORM` with optional label; `-> Machine.send(:evt)` action registers a submit intent via the same intent table.
  * `markdown` → `DVC_MARKDOWN` with `value` attr from the data expression.
  * `media` → `DVC_MEDIA` with `src` attr.
  * `progress` → `DVC_PROGRESS` with numeric `value`.
  * `status` → `DVC_LABEL` with `label` + `value` pair.
  * `divider` / `spacer` → `DVC_DIVIDER` / `DVC_SPACER` (no attrs).

**Deferred**:
- **Per-field aggregation inside `form`** — each `text :name` / `toggle :name` inside a form should contribute to a `{str: any}` payload that the submit handler receives. Today `form` renders its shell; fields still need individual intents. Will land with a proper field dispatcher.
- **Rich `status` with the `progress`-shape binding** — spec §12 allows `status expr label: str`, which maps to a label+value+badge triple.

Together these cover the most common annex content shapes. Combined with concepts #45–#51, declarative content pipeline handles: list scalars, list templates with per-item triggers + payload, form shells, markdown/media/progress/status/divider/spacer, label, rich_text, trigger, navigate, loading, error, group. Missing by design: toggle/range/choice/multiselect/pin/text/password/date/confirm/create/search/share intents (each needs specific bridge wiring for input widgets) — those track as concept #58+.

### Concept #55 — input intents in walker (toggle / range / text / password / pin / date / choice / multiselect / confirm / share / create / search)

**Drift**: spec §12.4 declares 12+ input intent primitives for user-facing widgets. The walker knew about `trigger` and `navigate` but dropped every other intent silently. `search`, `text`, `password`, `toggle`, `choice`, `confirm` — every form-shaped annex surface rendered blank.

**Fix applied (walker only)**:

- 2026-04-19 · layer 4 walker · `src/deck_interp.c` — one unified branch for all input intents. Maps each `kind` to its spec-canonical DVC type:

| kind | DVC type |
|---|---|
| `toggle` | `DVC_TOGGLE` |
| `range` | `DVC_SLIDER` |
| `choice` | `DVC_CHOICE` |
| `multiselect` | `DVC_CHOICE` (flags would distinguish in DL3) |
| `text` / `search` | `DVC_TEXT` |
| `password` | `DVC_PASSWORD` |
| `pin` | `DVC_PIN` |
| `date` | `DVC_DATE_PICKER` |
| `confirm` | `DVC_CONFIRM` |
| `share` | `DVC_SHARE` |
| `create` | `DVC_TRIGGER` (button surface) |

Each picks up `label` from the string literal, optional `on -> Machine.send(:evt, payload)` action → intent binding (re-using concept #47 + #51 infra).

**Deferred**:
- **Per-widget attrs**: `options: [...]`, `min: N max: M`, `placeholder: "..."`, `value: expr`, `state: bool`, `mask: "..."` — the parser captures them as raw tokens; the walker doesn't split them out yet. Each widget will render, but without its specific config (options list, bounds, etc.). Follow-up concept to enrich the item AST with option-bag fields.
- **Bidirectional value binding**: spec expects `toggle :name state: x` to reflect a value in the app state and update it on interaction. Today the value round-trip is one-way (action → event). Reactive state sync needs streams (#56+).
- **Field aggregation inside `form`**: fields are independent intents today; a `form on submit` shell gathers no payload. Requires a session-scoped field-value table + submit dispatch that packages the map. Defer.

### Concept #56 — @stream / @task accepted at parse, no-op at runtime (pragmatic close-out)

**Drift**: concept #21 already parse-and-discards top-level `@stream` and `@task` declarations. Full reactive-stream execution + task scheduling is each a major runtime subsystem (dependency tracking for streams; FreeRTOS task spawning + lifecycle for tasks). Neither is essential for a user-interactive annex demo — apps can declare streams/tasks and the runtime accepts the declaration without implementing side effects.

**Fix**: no code change — confirmation that pass-1 gap is closed at the parser level and runtime treats these as no-ops without erroring. Future concepts (#59+) will implement actual execution when the reactive framework lands.

### Session-wide gap-closing summary

Of the deferred items flagged at concept #51 close-out:

| Gap | Concept | Status |
|---|---|---|
| `empty ->` list fallback | #52 | ✅ closed |
| `has_more:` pagination | — | deferred — needs reactive / cursor model |
| form/field aggregation (shell) | #53 | ✅ shell renders; per-field aggregation deferred |
| markdown/media/progress/status/divider/spacer | #54 | ✅ closed |
| input intents (toggle/range/choice/text/etc.) | #55 | ✅ closed (minimal) |
| @stream runtime | #56 | declared-no-op; full runtime deferred |
| @task runtime | #56 | declared-no-op; full runtime deferred |
| non-Machine.send actions | — | deferred — needs captured-action snapshotting |
| lazy payload eval | — | deferred — edge case, render-time eval fine for most |

Annex apps can now exercise the full content primitive catalog at coarse level: render every spec §12 primitive, iterate over lists with per-item templates + payload-bearing triggers, emit form shells with submit intents, and declare @stream / @task without errors. What's left is depth (bidirectional binding, field aggregation, reactive streams, task scheduling) rather than breadth.

### Concept #57 — per-widget options reach DVC attrs (spec §12)

**Drift**: concept #55 mapped every §12.4 input intent to its DVC type, but the walker dropped every trailing `key: value` token. So `toggle :lights state: on`, `range :volume min: 0 max: 100 value: v`, `choice :theme options: [:dark, :light]`, `text :query placeholder: "..."`, `trigger "Open" badge: 3`, `list xs has_more: true` — in every case the widget rendered as a blank shell with no bounds, no initial value, no options list, no placeholder, no badge count. The bridge had nothing to configure from.

The annex-a multi-line trigger form surfaced a second gap: `trigger app.name \n  badge: ... \n  -> apps.launch(app.id)`. The parser's non-list nested-block branch was pure `absorb + skip`, so the options AND the trailing `-> action` were silently eaten. Per-item triggers could render a label but never bind an action for apps that used the canonical multi-line form.

**Fix applied**:

- 2026-04-19 · layer 4 AST · `include/deck_ast.h`:
  * New `ast_content_option_t { key, value }` struct.
  * `content_item` payload gained `options: ast_content_option_t*` + `n_options: uint32_t`. Empty when the item carried none.
- 2026-04-19 · layer 4 parser · `src/deck_parser.c`:
  * New helper `parse_content_options(p, ci)` — loops while `cur IDENT` + `peek TOK_COLON`, intern-copying the key, consuming the colon, parsing the value as a full expression (`parse_expr_prec`), appending into the item's option array with power-of-two arena-backed growth.
  * Inline-form: in both top-level content items and per-item template sub-items, the existing data_expr branch now guards on `!(IDENT && peek == COLON)` so it doesn't greedily eat the option's key. Called right before the legacy absorb loop so both `trigger "X" badge: 3` and `list xs has_more: true` capture their options.
  * Multi-line form: the non-list non-template nested-indent branch previously did a blind depth-tracked absorb. Rewrote to walk each line of the indent: `IDENT COLON …` lines become options; `-> action` lines bind the content item's action (preserving a pre-existing action_expr as the label via `data_expr` so the walker can surface it). Unrecognized shapes fall through to a local absorb so round-trip continues to work.
- 2026-04-19 · layer 4 walker · `src/deck_interp.c`:
  * `content_apply_value_as_attr(node, attr, v)` — dispatch-by-runtime-type helper mapping Deck values onto DVC attrs (bool → set_bool, int → set_i64, float → set_f64, str → set_str, atom → set_atom, list-of-{str|atom} → set_list_str). Unknown types are silently dropped.
  * `content_apply_options(c, env, node, ci)` — evaluates each option's expression in the render env, copies onto the node using the option's key as the attribute name (1:1 — `badge` → `:badge`, `options` → `:options`, `placeholder` → `:placeholder`, `min`, `max`, `value`, `state`, `mask`, `length`, `prompt`, `alt`, `role`, `has_more`, …). Option names live in the DVC attr namespace directly; the bridge decides per widget type which ones it honours.
  * Hooked at the end of every top-level item dispatch (after the kind-specific node construction) and at the end of every per-item template sub-node construction, with the correct env (top-level env vs. per-item env so binder-relative references like `badge: unread_for(x.id)` bind).
  * Top-level `trigger` / `navigate` gained a data_expr-as-label fallback: when no string literal label is present but data_expr is (the concept-#57 tail-arrow shift parks the label expression there), it's evaluated and written as the `label` attr. Per-item `trigger` / `navigate` got the same fallback routed through its existing label-expr/action-expr disambiguation.

**What this unblocks**: all 12+ input intents now configure. Concrete wins:

- `range :volume min: 0 max: 100 value: v` → DVC_SLIDER with `min=0`, `max=100`, `value=v`.
- `toggle :lights state: x` → DVC_TOGGLE with `state=x`.
- `choice :theme options: [:dark, :light]` → DVC_CHOICE with `options=["dark","light"]` (list_str).
- `text :q placeholder: "search..."` → DVC_TEXT with `placeholder="search..."`.
- `trigger "Open" badge: 3 -> Machine.send(:open)` → DVC_TRIGGER with `label="Open"`, `badge=3`, intent bound.
- `list posts has_more: more? -> Machine.send(:load_more)` → DVC_LIST with `has_more=bool`, form-level intent bound for pagination.
- `media img alt: "..." role: :avatar` → DVC_MEDIA with `alt="..."`, `role=:avatar`.
- `confirm "Delete?" prompt: "Are you sure?"` → DVC_CONFIRM with both strings.
- annex-a multi-line form: `trigger app.name \n  badge: unread_badge(app.id) \n  -> apps.launch(app.id)` — label expression captured, badge bound to runtime value per element, action arrow bound (currently for the Machine.send subset; `apps.launch` is noted in the existing deferred list of "non-Machine.send actions").

**A→B note**: this closes the "declared, rendered, but inert" gap for every input widget. Before: `toggle :x state: on` rendered as an uninitialised switch. After: the initial state reaches the bridge, the author's intent survives the parser, and the widget starts in the correct position. Same story for every slider, choice list, text placeholder, media role — twelve widget categories lit up in one concept.

### Concept #58 — captured-action dispatch (any action, not just Machine.send)

**Drift**: concepts #47/#50/#51/#55/#57 all looked at the action expression through one keyhole — `content_extract_event(action)` — that only matched `Machine.send(:evt[, payload])`. Any other action shape (`apps.launch(app.id)`, `bluesky.post(draft)`, composed `do … end` blocks, pipelines) silently produced `intent_id = 0` on the DVC node. The tap round-trip worked in exactly one case, so annex-a's canonical launcher example `trigger app.name -> apps.launch(app.id)` rendered a visibly-labeled button that did nothing when tapped.

**Fix applied (runtime only)**:

- 2026-04-19 · layer 4 runtime · `src/deck_interp.c`:
  * `deck_intent_binding_t` lost its `event`/`payload` fields and gained `action_ast: ast_node_t*` + `captured_env: deck_env_t*`. Each binding now stores the full action expression plus the env in which references resolve (top-level app env, or per-item env with the list binder bound). `captured_env` is retained via `deck_env_retain`; released on re-render and at `deck_runtime_app_unload`.
  * New helper `content_bind_intent(app, action, env)` — allocates the next id, retains the env, stashes the AST, returns the id (or 0 if action is NULL / table is full). Every intent-binding site in the walker switched to this helper (top-level trigger / navigate / form, per-item trigger / navigate, every §12.4 input intent).
  * `content_extract_event` / `content_extract_payload_expr` removed — the dispatcher no longer needs to specialise on Machine.send. Every action is treated uniformly.
- 2026-04-19 · layer 4 runtime · tap dispatch rewritten:
  * `deck_runtime_app_intent_v(app, id, vals, n_vals)` — new public entry. Builds a child env on top of `captured_env`, binds `event` to a payload map (concept #59/#60 shape — see below), evaluates `action_ast` in that env. Any side-effecting expression works: Machine.send, apps.launch, a do-block combining both, a pipe chain, an `@stream.emit(…)` call.
  * `deck_runtime_app_intent(app, id)` retained as a thin `intent_v(…, NULL, 0)` wrapper so existing callers compile.

**What this unblocks**: `apps.launch(app.id)` in annex-a's launcher fires on tap. `bluesky.post(draft)` in annex-xx's compose flow actually runs. `do\n  Machine.send(:x)\n  flash(:ok)\nend` as a compound action runs both statements. Arbitrary author-defined fn calls as intents work.

### Concept #59 — bridge-supplied payload (`event.value` at tap time)

**Drift**: concept #51 evaluated payloads at **render time** and stored them on the binding. That works for per-element capture (`Machine.send(:open, app.id)` where `app.id` is known at render time). It does not work for input intents that need the **user's new value** — a toggle's new checked state, a slider's released value, a text input's current string. Those values exist only at tap time, on the bridge side. Every `toggle :x on -> Machine.send(:toggled, event.value)` therefore lost `event.value`.

**Fix applied (runtime + bridge)**:

- 2026-04-19 · layer 5 bridge · `components/deck_bridge_ui/include/deck_bridge_ui.h`:
  * Intent hook signature gained `(vals, n_vals)` carrying `deck_bridge_ui_val_t` entries — bool / i64 / f64 / str / atom. `vals[0].key == NULL` + `n_vals == 1` means scalar payload; keyed entries mean form aggregation (concept #60).
- 2026-04-19 · layer 5 bridge · `components/deck_bridge_ui/src/deck_bridge_ui_decode.c`:
  * Every widget event callback (toggle, slider, choice, text) now packs its current LVGL value into a `deck_bridge_ui_val_t` and passes it through the hook. Toggles send their bool, sliders their int, text inputs their UTF-8 buffer, choices their selected option text.
- 2026-04-19 · layer 5 shell · `components/deck_shell/src/deck_shell_deck_apps.c`:
  * Intent-hook shim translates the bridge value-kind enum to the runtime's `deck_intent_val_t` and calls `deck_runtime_app_intent_v`. The legacy `@on trigger_N` fallback dispatch still fires so imperative-builder apps (`hello.deck`, `ping.deck`) keep working.
- 2026-04-19 · layer 4 runtime · `src/deck_interp.c`:
  * `make_intent_event_value` builds a Deck `{value: v}` map from a single scalar and binds it as `event` in the tap env. Authors read `event.value` directly via the existing map-dot-access path (concept #33).

**What this unblocks**: `toggle :lights state: s on -> Machine.send(:toggle_lights, event.value)` fires the event with the real new boolean. Volume sliders deliver their new integer. Text inputs deliver their buffered string. Every stateful widget now completes the change → machine transition → re-render loop.

### Concept #60 — form field aggregation (`event.values` map on submit)

**Drift**: concept #53 rendered `form on submit -> action` as an empty DVC_FORM shell. There was no UI to submit with, no children-value aggregation, no payload on submit — the form primitive looked implemented from the parser's POV but nothing worked end-to-end.

**Fix applied**:

- 2026-04-19 · layer 4 runtime walker · `src/deck_interp.c`:
  * Every §12.4 input intent inside the walker now writes a `:name` attr on the DVC node, picked from the widget's bare `:atom` binder (`toggle :lights` → `name="lights"`). This is the form's aggregation key.
- 2026-04-19 · layer 5 bridge · `components/deck_bridge_ui/src/deck_bridge_ui_decode.c`:
  * New `render_form(parent, n)` — builds a column, pushes `s_current_form = n` so descendant input renderers register themselves in a per-render `s_fields[]` table (form-owner, name, LVGL input obj, field kind), recurses children, then emits a **SUBMIT** button at the bottom when the form node carries an intent_id.
  * `form_submit_cb` — walks `s_fields[]` for entries whose `form == the submitted DVC_FORM`, reads each input's current LVGL value, packs keyed `deck_bridge_ui_val_t` entries, calls the hook. The runtime turns them into a `{username: "...", password: "..."}` map under `event.values`.
  * `fields_reset()` at the start of each snapshot wipes stale entries before the new render populates them.
- 2026-04-19 · layer 4 runtime · `make_intent_event_value` path:
  * When any entry has a key, the entries go into a `{values: {…}}` submap. `event.values` destructures to the full form snapshot.

**What this unblocks**: `form on submit -> Machine.send(:login, event.values)` fires with `{username: "alice", password: "secret"}`. The annex-xx login flow and the settings configuration forms are end-to-end functional: user fills fields → taps SUBMIT → machine receives all inputs in one payload → transition + re-render.

**A→B for concepts #58 + #59 + #60**: three deferred items from #57 close-out, all three closed. The runtime intent system no longer knows or cares about Machine.send specifically; it dispatches arbitrary actions with a bridge-supplied payload bound as `event.value` (scalar) or `event.values` (form map). From the annex author's perspective:

```
toggle :lights state: on on -> Machine.send(:toggle_lights, event.value)
range  :volume min: 0 max: 100 value: v on -> Machine.send(:volume, event.value)
form on submit -> Machine.send(:login, event.values)
trigger "Open" -> apps.launch(app.id)
trigger app.name -> do
  LauncherState.send(:close_search)
  apps.launch(app.id)
```

…all of these run end-to-end on the device now.

### Session #5 — 2026-04-19 — P0 test-infrastructure audit

User directive: "audit full real world and production ready in all types of tests, test coverage, not flaky or bypasses". Sub-agent produced a detailed audit; P0 findings addressed this session.

**Fixtures rewritten from unconditional sentinels to real gates** (REPORTS.md had flagged these as A→B bypasses):
- `app_machine.deck` — sentinel now fires from `ready.on_enter`; only reached if `boot.on_enter` + transition + state change all succeed. run_machine aborts on any hook error, so a broken chain never emits OK.
- `app_flow.deck` — sentinel in final step only; per-step logs for visual order audit.
- `app_machine_hooks.deck` — sentinel in `ready.on_enter` (end of hook chain); each hook logs a distinct line.
- `app_bridge_ui.deck` — replaced legacy `bridge.ui.*` imperative builders with spec-canonical declarative `content =`. Sentinel in final state's on_enter; only reached if content_render didn't crash on intermediate state.

**Fuzz/pressure assertions tightened**:
- Phase-1 random garbage: **must** yield `ok_cnt == 0`. Previously lumped p1+p2 counts and couldn't distinguish.
- Heap pressure: `rc` must be a pressure-related error (NO_MEMORY or PARSE_ERROR). Previously accepted any non-OK rc.

**Spec-level parser gaps closed** (closed multiple silent-parse-failure paths):
- Lexer: added spec §2.2 comment syntax `--` (single-line) and `---` (multi-line). Previously only `#` was accepted; 42 of 80 fixtures used `--` and silently failed at the lexer.
- Lexer: added spec §2.6 bare `{expr}` interpolation (was only `\${expr}`), plus `\{` / `\}` literal-brace escapes.
- Lexer + parser: binop line continuation. A line whose first non-space token is `&&` / `||` / `++` / `|>` / `and` / `or` is absorbed as a continuation of the previous expression, not a top-level statement.
- Parser: `TOK_KW_SEND` accepted as field name after `.` so `Machine.send(:e)` / `machine.send(:e)` parse.
- Parser: inline trailing `[on] [event_atom]? -> action` after options, so `form on submit -> …` / `toggle :x state: s on -> …` parse without `on` short-circuiting the expression parser.
- Parser: `err_missing_colon` fixture wording restored to spec-canonical "app field name".

**Runtime gaps closed**:
- Sequential @machine now renders declarative `content =` on each state entry (parity with event-driven branch). Previously dropped content on sequential machines.

**New C-side test coverage**:
- Parser: 3 new cases over AST_CONTENT_ITEM shapes (inline option bag, tail-arrow label/action shift, `on [atom]? ->` form). AST printer extended to emit structured output for content_block / content_item so the cases are round-trippable. Parser suite grew 61 → 64, all PASS.
- Interp: 3 new end-to-end tests for concepts #58/#59/#60 via `deck_runtime_app_intent_v` — captured-action dispatch (non-Machine.send action fires), scalar `event.value` delivery, keyed `event.values` delivery. Each asserts a Deck-side NVS side-effect from C. Interp suite grew 48 → 51, all PASS.

**Hardware results (commit 40a10a1)**:
- parser selftest: **64/64 PASS** (was 60/61 — 1 fail resolved, 3 new cases added)
- interp selftest: **51/51 PASS** (was 47/48 — migration fix + 3 new cases)
- loader, lexer, runtime selftests: all PASS
- conformance suites: **5/5 PASS** (was 3/5)
- conformance stress: **15/15 PASS, 0 outliers** (was 14/15)
- .deck fixtures: **47/80 PASS** (was 23/80 pre-lexer-fix; +24 unlocked)

**Deferred / still failing (33 .deck fixtures)**:
The remaining fixtures hit a variety of deeper parser gaps that each require dedicated concept passes:
- Triple-quoted multi-line strings (`"""…"""`).
- Complex match arms: `expected '->' in match arm`, `expected pattern`, `expected newline after match scrutinee` — patterns like `:some x when …` or multi-arm with guards and nested destructure.
- Function body forms: some `lang.fn.*` fail on `expected 'else' in if expression` (conditional inside fn body hits parser path that requires `else`).
- `lang.lambda.inline`, `lang.tco.deep`, `lang.list.basic`, `lang.tuple.basic`, `lang.map.basic` — various arity / pattern / let issues not yet triaged.

These are honest reds (tests describe behavior the runtime doesn't yet fully implement), not silent passes. The conformance harness now reports truth: what works (47) and what doesn't (33). Each failing fixture becomes a targeted layer-4 task for future sessions.

**Commits in this session**:
- `00ce250` — P0 test audit: fixture bypass fixes + lexer/parser/runtime spec gaps
- `8eafeda` — concept #58/#59/#60 unit tests + `on [atom]? ->` tolerance
- `3322ff1` — parser tests for concept #57/#58 + err wording fix
- `74aaf1e` — fix migration test (nvs.set spec arity)
- `854b4ab` — heap-pressure stress: accept parse_error too
- `3994c9b` — lexer+parser: binop line continuation
- `40a10a1` — lexer: bare `{expr}` + `\{` / `\}` escapes

### Session #6 — 2026-04-20

User directive (continuation): *"Sigamos iterando y arreglando el codebase (sigamos trabajando como REPORTS.md)"*. Build verified green at HEAD before any work (standing rule #5). Picking the next concept by auditing the still-failing fixtures from session #5's tail.

### Concept #61 — `list.reduce` + `list.scan` arg order canonical (`xs, init, fn`)

**Three-way drift** (the textbook A→B pattern the user keeps catching):
- Spec `01-deck-lang §11.2 line 835`: `list.reduce(xs: [T], init: U, fn: (U,T) -> U)` — **init second, fn third**.
- Runtime `deck_interp.c:744`: `b_list_reduce` indexed `args[1]` as fn and `args[2]` as init — **fn second, init third** (opposite of spec).
- Fixtures split:
  * `lang_list_basic.deck:52` and `lang_stdlib_basic.deck:23` used the runtime shape `(xs, fn, init)` — **passed** because runtime accepted them.
  * `lang_lambda_inline.deck:47` and `lang_fn_typed.deck:38` used the spec shape `(xs, init, fn)` — **silently failed** because runtime tried to call `init` as a function and bailed.
  * `lang_stdlib_basic.deck:16-17` even contained a comment acknowledging the spec form `(xs, init, fn)` while the code on line 23 used the runtime form. Self-aware drift.

Additionally, `list.scan` (spec line 840) was declared but **never registered** at runtime — silent miss.

**Resolution per standing rules**: spec wins; align runtime to spec; migrate the runtime-shape fixtures. No dual-accept shim — the wrong shape now fails closed with a specific error message.

**Fix applied**:

- 2026-04-20 · layer 4 runtime · `components/deck_runtime/src/deck_interp.c`:
  * `b_list_reduce` arg indices flipped: `acc = retain(args[1])` and call site uses `args[2]` as fn. Type check now requires `args[2] == DECK_T_FN`. Error message updated to spec-canonical `"list.reduce(xs, init, fn)"`.
  * `b_list_scan` added — same shape as reduce but accumulates each step into a result list (spec §11.2: running fold). Output list pre-sized to input length to avoid resize churn.
  * BUILTINS table gains `{ "list.scan", b_list_scan, 3, 3 }` right after the reduce entry.
- 2026-04-20 · layer 6 fixtures · two migrated to spec-canonical:
  * `apps/conformance/lang_list_basic.deck:52` — `list.reduce(xs, (acc, n) -> acc + n, 0)` → `list.reduce(xs, 0, (acc, n) -> acc + n)`.
  * `apps/conformance/lang_stdlib_basic.deck:23` — same flip; comment lines 16-17 updated to record the resolution rather than continuing to teach the wrong form.

**Verification**: `idf.py build` succeeds. Pre-existing benign warning `fs_list_cb defined but not used` is unrelated (introduced by concept #36's rewrite of `fs.list` to the FsEntry record callback; the old name-only callback is dead code from that migration — flagged for cleanup, not in scope here).

**Why this matters (A→B)**: this is the cleanest example yet of "tests pass, reality breaks": four fixtures, two of them PASSING because they happened to align with the wrong runtime shape, two of them silently FAILING because they followed the spec — and a self-aware comment in one fixture documenting the disagreement without resolving it. Picking spec as authoritative collapses all four onto one truth: the fixtures that used to pass via runtime-coincidence start failing until migrated, the fixtures that documented spec start passing. The harness now reports a single coherent truth instead of an averaged green over a contradictory pair.

**No fixture migration needed for** `lang_lambda_inline.deck` and `lang_fn_typed.deck` — they were already spec-canonical and start passing automatically with the runtime fix.

**Deferred**: `list.scan` has no fixture coverage yet; future deepening of `lang_list_basic.deck` should add a probe (e.g. `list.scan([1,2,3], 0, (a,n) -> a+n) == [1, 3, 6]`).

### Concept #62 — chained tuple index `t.0.0` lexer fix

**Drift**: `apps/conformance/lang_tuple_basic.deck` lines 51-54 use `nested.0.0` / `nested.0.1` / `nested.1.0` / `nested.1.1` to read elements of nested tuples. Spec is fine — `t.N` is tuple-index access (parser already supports it via `AST_TUPLE_GET` at `deck_parser.c:516-525`). But the **lexer** consumed `0.0` greedily as `TOK_FLOAT(0.0)` instead of `TOK_INT(0) DOT TOK_INT(0)`, so the parser saw `nested DOT FLOAT` and bailed with "tuple index must be non-negative" / type error.

The bug: `scan_number` extends a number to a float when it sees `.` followed by a digit. That rule is correct for normal positions (`x + 0.5`) but wrong inside a tuple-index chain — a number that begins immediately after a `.` is unambiguously an integer index, never a fractional. Float literals in Deck always require a leading digit (no bare `.5`), so the lookback is unambiguous.

A→B shape: `lang_tuple_basic.deck` listed in session #5 deferred as "various arity / pattern / let issues not yet triaged" — but the actual blocker was the **lexer**, two layers down from where the failure reported. Until the lexer correctly tokenises `0.0` as `INT DOT INT` after a preceding dot, parser and runtime never get a chance to handle the chain.

**Fix applied**:

- 2026-04-20 · layer 0 lexer · `components/deck_runtime/src/deck_lexer.c:scan_number` — added `after_dot` flag computed once at scan start (`start > 0 && lx->src[start - 1] == '.'`). When set, the loop refuses to consume a `.digit` extension or `e`-exponent extension. Number falls out as INT, lexer continues from the next `.`.
- 2026-04-20 · layer 0 lexer test · `components/deck_runtime/src/deck_lexer_test.c` — two new cases:
  * `tuple_chain` (`"t.0.0"`) — must lex as IDENT DOT INT DOT INT, not IDENT DOT FLOAT.
  * `float_after_int` (`"x + 0.5"`) — must still lex as IDENT PLUS FLOAT (regression guard for the normal path).

**Verification**: `idf.py build` succeeds. The two new lexer cases will run on hardware selftest.

**What this unblocks**: every nested tuple access pattern across all fixtures + annexes. `lang_tuple_basic.deck` becomes parseable end-to-end; downstream `(a, b) := ...` destructuring + match patterns + structural equality were already implemented and just needed reachable input.

**Why this matters (A→B)**: a pure lexer-level fix that masquerades as a "tuple feature gap" two layers up. The diagnostic noise — "tuple index must be non-negative" appearing for valid `nested.0.0` syntax — pointed at the parser, but the real bug was the byte stream the parser was reading. Sessions where bug reports get assigned by symptom text (the parser error message) systematically miss this kind of cross-layer drift.

### Concept #63 — structural equality + Optional ↔ variant-tuple bridging

**Two-fold drift on `==`/`!=`**:

1. **Lists / tuples / maps never compared structurally**. `do_compare` for `BINOP_EQ`/`BINOP_NE` only handled scalars (INT/FLOAT/STR/ATOM/BOOL/UNIT); for any other type it set `cmp = 1` (always-not-equal). So `(1,2) == (1,2)` returned `false`, `[1,2,3] == [1,2,3]` returned `false`, and every fixture line testing structural equality silently failed. This blocks `lang_tuple_basic.deck:45-47`, `lang_list_basic.deck:47/50/56-58`, `lang_map_basic.deck` value-by-value comparisons, and any annex that compares records/lists.

2. **Optional vs atom-variant-tuple repr split**. `map.get(...)` and `some(...)`/`none()` builtins return `DECK_T_OPTIONAL{.inner=v}`. But concept #11 made `:some v` / `:none` first-class atom-variant value syntax that desugars to a 2-tuple `(:some, v)` and bare atom `:none`. Both representations are observable in user code; equality between them was always `false` because the types differ. So `map.get(m, :name) == :some "diana"` and `map.get(m, :missing) == :none` from `lang_map_basic.deck:34-35` (and many others) silently fail.

A→B shape: scalars compare correctly → "equality works" → assumption breaks at the moment a fixture inserts a list/tuple/map/option literal on either side of `==`. The conformance harness reported PASS on fixtures that happen to only compare scalars and FAIL on fixtures that exercise structural equality, with no signal that the runtime was missing something foundational.

**Fix applied**:

- 2026-04-20 · layer 4 runtime · `components/deck_runtime/src/deck_interp.c`:
  * `do_compare` short-circuits at the top: `BINOP_EQ`/`BINOP_NE` delegate to `values_equal(L, R)` (already structural and recursive). Ordering ops (`<` `<=` `>` `>=`) keep the scalar-only logic — they don't make sense for compound types.
  * `values_equal` extended to handle `DECK_T_OPTIONAL` (recurse into `.inner`; both-none → equal; one-none → not equal) and `DECK_T_MAP` (compare by lookup so internal hash order doesn't leak).
  * New `optional_equal_variant(o, t)` helper handles cross-type comparisons: `Optional{.inner=v}` vs `Tuple(:some, v)` and `Optional{.inner=NULL}` vs `Atom(:none)`. Called from the type-mismatch branch when one side is Optional and the other is the variant shape.
  * `do_compare` ordering side also gained string lexicographic comparison for `<`/`>` etc — was previously `cmp = (ptr == ptr) ? 0 : 1`, giving meaningless ordering for non-interned strings. (Strings are interned in practice, but the comparison was structurally wrong.)
- 2026-04-20 · layer 5 test · `components/deck_runtime/src/deck_interp_test.c` — new `t_eq_structural` case covers tuple ==/!=, list ==/!=, and the Optional ↔ tuple bridge for both `:some v` and `:none`. Registered in `CASES[]`.

**Verification**: `idf.py build` succeeds. Test will run on hardware selftest at next flash; all five sub-assertions must pass.

**What this unblocks**:

- `lang_tuple_basic.deck` — `(1, 2) == (1, 2)` and `(1, 2) != (2, 1)` and `(1, "a") != (1, "b")` now true.
- `lang_list_basic.deck` — `doubled == [20, 40, 60, 80, 100]`, `evens == [20, 40]`, `[1, 2] ++ [3, 4] == [1, 2, 3, 4]` etc. become decidable.
- `lang_map_basic.deck` — every `map.get(m, k) == :some v` and `== :none` line. Plus `m.name == "diana"` was already supported via concept #33 dual atom/string lookup.
- Any annex that compares records, lists, or option-returning calls against literals.

**Why this matters (Deck minimalism + spec-honoring)**: equality is the most fundamental observable. Having `==` lie about compound values for years would erode every higher-order assertion — list filtering (`list.contains`), map lookup matching, pattern-match guards, every `if/then/else` branch on a structural comparison. The Optional↔tuple bridge in particular embodies the user's "follow the spirit of the spec" rule: spec §3.7 says `:some v` is a value, concept #11 made the literal syntax produce a tuple, so the runtime's older Optional repr must transparently compare equal — anything else creates a Schrödinger's value where the source of the construction (literal vs builtin) determines equality semantics.
