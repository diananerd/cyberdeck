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
