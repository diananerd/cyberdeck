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

### Layer 1 / 2 open items (deferred, not blocking)

- `@capability system.shell` in `09-deck-shell.md §7` still exports `set_status_bar`/`set_status_bar_style`/`set_navigation_bar` methods. Per `10-deck-bridge-ui §3.2-3.4`, the bridge renders both unconditionally. These capability methods are either redundant (apps never need them) or are for special modes (e.g. fullscreen game/media). Decision: leave for now; separate audit of §07-shell-capability consistency is a follow-up session. Noting here so it isn't lost.
- `@app icon:` appears in `13-deck-cyberdeck-platform.md §6.1` as an app-identity field. Not in `02-deck-app §3` (identity). Need to confirm `icon:` is part of `@app` — likely yes given it's referenced in launcher content inference as the source for card icons. Not a bug; just incomplete doc in §02 §3. Follow-up audit.
- `§10-deck-bridge-ui §4.1` still contains rich layout inference prose that's correct — the *bridge's* internal vocabulary (`DVC_GROUP`, `DVC_LIST`, etc.) is a separate catalog from the *app's* content primitives. The invariant "apps write §12, bridge reads DVC" is crisp; the overlap word `list`/`group` is not a conflict because the bridge maps app-`list` → internal `DVC_LIST` at runtime.
- `01-deck-lang.md §7` (lines 524-543) uses `list\n items: posts\n p ->` (mixed named `items:` with positional `p ->`). This appears to be a third variant shape. §02 §12.1 shape is positional `list expr\n p ->`. Decision: treat `list items: X\n p ->` as a syntactic alternative (named head + positional iter body) consistent with the two-form convention of other §12 primitives. Not fixing — noting. If the parser only supports one shape, the parser must grow to support both, OR §01 §7 gets normalised to positional and §02 §12 becomes the sole form.
- Testing discipline: "done = hardware verified" (flash + monitor + visual confirmation). Compile-pass ≠ done.
